#include "pubg_ai_aim/AimModule.hpp"

#include "pubg_ai_aim/AimEngine.hpp"
#include "pubg_ai_aim/detail/RecognitionFrequency.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pubg::ai_aim {
namespace {

double elapsedMilliseconds(Clock::time_point begin, Clock::time_point end) noexcept {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

} // namespace

class AimModule::Impl {
public:
    Impl(
        AimConfig config,
        std::unique_ptr<IFrameSource> frame_source,
        std::unique_ptr<IObjectDetector> detector,
        std::shared_ptr<IMotionSink> motion_sink,
        std::shared_ptr<ITriggerSink> trigger_sink)
        : config_(std::move(config)),
          frame_source_(std::move(frame_source)),
          detector_(std::move(detector)),
          motion_sink_(std::move(motion_sink)),
          trigger_sink_(std::move(trigger_sink)) {
        config_.sanitize();
        if (!motion_sink_) {
            throw std::invalid_argument("AimModule requires a motion sink");
        }
        if ((frame_source_ == nullptr) != (detector_ == nullptr)) {
            throw std::invalid_argument("frame source and detector must be provided together");
        }
    }

    ~Impl() {
        stop();
    }

    bool start() {
        if (active_worker_ == this) {
            return false;
        }
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return false;
        }
        if (worker_.joinable()) {
            worker_.request_stop();
            queue_cv_.notify_all();
            worker_.join();
        }
        {
            std::scoped_lock queue_lock(queue_mutex_);
            latest_frame_.reset();
            newest_submitted_sequence_ = 0;
            newest_submitted_timestamp_ = {};
        }
        running_.store(true, std::memory_order_release);
        {
            std::scoped_lock state_lock(state_mutex_);
            snapshot_.running = true;
            snapshot_.model_ready = detector_ == nullptr;
            snapshot_.target_locked = false;
            snapshot_.detection_frame.reset();
            recognition_frequency_.reset();
            snapshot_.detector_backend = detector_
                ? std::string(detector_->backendName())
                : std::string("external");
            snapshot_.last_error.clear();
        }
        worker_ = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
        return true;
    }

    void stop() noexcept {
        if (active_worker_ == this) {
            running_.store(false, std::memory_order_release);
            std::unique_lock output_lock(output_gate_);
            OutputFence fence(*this);
            {
                std::scoped_lock state_lock(state_mutex_);
                activation_.enabled = false;
                ++safety_epoch_;
                reset_requested_ = true;
                snapshot_.running = false;
                snapshot_.target_locked = false;
                snapshot_.detection_frame.reset();
                recognition_frequency_.reset();
            }
            queue_cv_.notify_all();
            return;
        }

        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        {
            std::unique_lock output_lock(output_gate_);
            OutputFence fence(*this);
            {
                std::scoped_lock state_lock(state_mutex_);
                activation_.enabled = false;
                ++safety_epoch_;
                reset_requested_ = true;
                recognition_frequency_.reset();
            }
            waitForOutputQuiescence(output_lock);
        }
        if (worker_.joinable()) {
            worker_.request_stop();
            queue_cv_.notify_all();
            worker_.join();
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            snapshot_.running = false;
            snapshot_.target_locked = false;
            snapshot_.detection_frame.reset();
        }
    }

    void setEnabled(bool enabled) {
        std::unique_lock output_lock(output_gate_);
        OutputFence fence(*this);
        {
            std::scoped_lock state_lock(state_mutex_);
            activation_.enabled = enabled;
            ++safety_epoch_;
            if (!enabled) {
                reset_requested_ = true;
                snapshot_.target_locked = false;
                snapshot_.detection_frame.reset();
                recognition_frequency_.reset();
            }
        }
        waitForOutputQuiescence(output_lock);
        queue_cv_.notify_all();
    }

    void updateConfig(AimConfig config) {
        config.sanitize();
        std::unique_lock output_lock(output_gate_);
        OutputFence fence(*this);
        {
            std::scoped_lock state_lock(state_mutex_);
            config_ = std::move(config);
            ++config_revision_;
            ++safety_epoch_;
            reset_requested_ = true;
            snapshot_.target_locked = false;
            snapshot_.detection_frame.reset();
            recognition_frequency_.reset();
        }
        waitForOutputQuiescence(output_lock);
        queue_cv_.notify_all();
    }

    void updateCombatContext(CombatContext context) {
        std::unique_lock output_lock(output_gate_);
        OutputFence fence(*this);
        {
            std::scoped_lock state_lock(state_mutex_);
            combat_ = std::move(context);
            ++safety_epoch_;
            snapshot_.detection_frame.reset();
        }
        waitForOutputQuiescence(output_lock);
    }

    void updateActivation(ActivationState state) {
        std::unique_lock output_lock(output_gate_);
        OutputFence fence(*this);
        {
            std::scoped_lock state_lock(state_mutex_);
            activation_ = state;
            ++safety_epoch_;
            if (!activationAllowsProcessing(activation_)) {
                reset_requested_ = true;
                snapshot_.target_locked = false;
                snapshot_.detection_frame.reset();
                recognition_frequency_.reset();
            }
        }
        waitForOutputQuiescence(output_lock);
        queue_cv_.notify_all();
    }

    bool submitDetections(DetectionFrame frame) {
        if (frame_source_ || detector_) {
            return false;
        }
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        if (frame.sequence == 0 || frame.roi_width <= 0 || frame.roi_height <= 0 ||
            frame.screen_width <= 0 || frame.screen_height <= 0) {
            return false;
        }
        if (frame.timestamp == Clock::time_point{}) {
            return false;
        }
        const Clock::time_point now = Clock::now();
        int max_frame_age_ms = 0;
        {
            std::scoped_lock state_lock(state_mutex_);
            if (!activationAllowsProcessing(activation_)) {
                return false;
            }
            max_frame_age_ms = config_.tracking.max_frame_age_ms;
        }
        if (frame.timestamp > now ||
            frame.timestamp < now - std::chrono::milliseconds(max_frame_age_ms)) {
            return false;
        }
        std::scoped_lock queue_lock(queue_mutex_);
        if (frame.sequence <= newest_submitted_sequence_ ||
            (newest_submitted_timestamp_ != Clock::time_point{} &&
             frame.timestamp <= newest_submitted_timestamp_)) {
            return false;
        }
        newest_submitted_sequence_ = frame.sequence;
        newest_submitted_timestamp_ = frame.timestamp;
        latest_frame_ = std::make_shared<const DetectionFrame>(std::move(frame));
        queue_cv_.notify_one();
        return true;
    }

    void resetTracking() {
        std::unique_lock output_lock(output_gate_);
        OutputFence fence(*this);
        {
            std::scoped_lock state_lock(state_mutex_);
            reset_requested_ = true;
            ++safety_epoch_;
            snapshot_.target_locked = false;
            snapshot_.detection_frame.reset();
            recognition_frequency_.reset();
        }
        waitForOutputQuiescence(output_lock);
        queue_cv_.notify_all();
    }

    AimSnapshot snapshot() const {
        std::scoped_lock state_lock(state_mutex_);
        auto result = snapshot_;
        result.recognition_hz = recognition_frequency_.hertz(Clock::now());
        return result;
    }

private:
    class OutputFence {
    public:
        explicit OutputFence(Impl& owner) noexcept : owner_(owner) {
            ++owner_.output_fence_count_;
        }
        ~OutputFence() {
            --owner_.output_fence_count_;
            owner_.output_cv_.notify_all();
        }
        OutputFence(const OutputFence&) = delete;
        OutputFence& operator=(const OutputFence&) = delete;
    private:
        Impl& owner_;
    };

    class WorkerIdentity {
    public:
        explicit WorkerIdentity(Impl& owner) noexcept
            : previous_(active_worker_) {
            active_worker_ = &owner;
        }
        ~WorkerIdentity() { active_worker_ = previous_; }
        WorkerIdentity(const WorkerIdentity&) = delete;
        WorkerIdentity& operator=(const WorkerIdentity&) = delete;
    private:
        Impl* previous_;
    };

    struct StateCopy {
        AimConfig config;
        CombatContext combat;
        ActivationState activation;
        std::uint64_t config_revision = 0;
        std::uint64_t safety_epoch = 0;
        bool reset_requested = false;
    };

    StateCopy copyState() {
        std::scoped_lock state_lock(state_mutex_);
        StateCopy copy{
            config_, combat_, activation_, config_revision_, safety_epoch_, reset_requested_};
        reset_requested_ = false;
        return copy;
    }

    std::shared_ptr<const DetectionFrame> waitForExternalFrame(std::stop_token stop) {
        std::unique_lock queue_lock(queue_mutex_);
        queue_cv_.wait(queue_lock, stop, [&] {
            return latest_frame_ != nullptr || hasResetRequest();
        });
        if (stop.stop_requested()) {
            return {};
        }
        auto frame = std::move(latest_frame_);
        latest_frame_.reset();
        return frame;
    }

    bool hasResetRequest() const {
        std::scoped_lock state_lock(state_mutex_);
        return reset_requested_;
    }

    void waitWhileOutputBlocked(
        std::stop_token stop,
        std::uint64_t expected_config_revision) {
        std::unique_lock queue_lock(queue_mutex_);
        latest_frame_.reset();
        queue_cv_.wait(queue_lock, stop, [&] {
            std::scoped_lock state_lock(state_mutex_);
            return !running_.load(std::memory_order_acquire) ||
                   activationAllowsProcessing(activation_) || reset_requested_ ||
                   config_revision_ != expected_config_revision;
        });
    }

    bool beginOutput(std::uint64_t expected_epoch) {
        std::unique_lock output_lock(output_gate_);
        if (output_fence_count_ != 0 || output_in_flight_) {
            return false;
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            if (!running_.load(std::memory_order_acquire) ||
                safety_epoch_ != expected_epoch ||
                !activationAllowsProcessing(activation_)) {
                return false;
            }
        }
        output_in_flight_ = true;
        output_owner_ = std::this_thread::get_id();
        return true;
    }

    void endOutput() noexcept {
        {
            std::scoped_lock output_lock(output_gate_);
            output_in_flight_ = false;
            output_owner_ = {};
        }
        output_cv_.notify_all();
    }

    void waitForOutputQuiescence(std::unique_lock<std::mutex>& output_lock) {
        if (output_in_flight_ && output_owner_ != std::this_thread::get_id()) {
            output_cv_.wait(output_lock, [&] { return !output_in_flight_; });
        }
    }

    struct CaptureResult {
        CaptureStatus status = CaptureStatus::NoFrame;
        std::shared_ptr<const DetectionFrame> frame;
        std::string timing_error;
    };

    CaptureResult captureAndInfer(
        std::stop_token stop,
        const StateCopy& expected_state,
        std::uint64_t last_sequence,
        Clock::time_point last_timestamp,
        double& capture_ms,
        double& inference_ms) {
        Frame frame;
        const auto capture_begin = Clock::now();
        const CaptureStatus status = frame_source_->capture(frame, stop);
        const auto capture_end = Clock::now();
        capture_ms = elapsedMilliseconds(capture_begin, capture_end);
        if (status == CaptureStatus::Stopped || stop.stop_requested()) {
            return {CaptureStatus::Stopped, {}, {}};
        }
        if (status != CaptureStatus::FrameReady) {
            return {status, {}, {}};
        }
        if (frame.timestamp == Clock::time_point{}) {
            frame.timestamp = capture_end;
        }
        // A capture already in progress cannot always be cancelled, but a
        // gate transition must prevent it from starting an expensive infer.
        if (!processingAllowed(
                expected_state.config_revision, expected_state.safety_epoch)) {
            return {CaptureStatus::NoFrame, {}, {}};
        }
        std::string timing_error;
        if (!validateFrameTiming(
                frame.sequence,
                frame.timestamp,
                expected_state.config.tracking,
                last_sequence,
                last_timestamp,
                timing_error)) {
            return {CaptureStatus::FrameReady, {}, std::move(timing_error)};
        }

        const auto inference_begin = Clock::now();
        std::vector<Detection> detections = detector_->infer(frame, stop);
        const auto inference_end = Clock::now();
        inference_ms = elapsedMilliseconds(inference_begin, inference_end);
        if (stop.stop_requested()) {
            return {CaptureStatus::Stopped, {}, {}};
        }

        DetectionFrame detection_frame;
        detection_frame.sequence = frame.sequence;
        detection_frame.timestamp = frame.timestamp;
        detection_frame.roi_left = frame.desktop_left;
        detection_frame.roi_top = frame.desktop_top;
        detection_frame.roi_width = frame.width;
        detection_frame.roi_height = frame.height;
        detection_frame.screen_width = frame.screen_width;
        detection_frame.screen_height = frame.screen_height;
        detection_frame.screen_center_x = frame.screen_center_x;
        detection_frame.screen_center_y = frame.screen_center_y;
        detection_frame.detections = std::move(detections);
        return {
            CaptureStatus::FrameReady,
            std::make_shared<const DetectionFrame>(std::move(detection_frame)),
            {},
        };
    }

    void applyState(
        AimEngine& engine,
        const StateCopy& state,
        std::uint64_t& applied_config_revision) {
        if (state.config_revision != applied_config_revision) {
            if (detector_) {
                (void)detector_->updateDecoderConfig(
                    state.config.detector, state.config.compatibility);
            }
            engine.updateConfig(state.config);
            applied_config_revision = state.config_revision;
        } else if (state.reset_requested) {
            engine.reset();
        }
    }

    bool validateFrameTiming(
        std::uint64_t sequence,
        Clock::time_point timestamp,
        const TrackingConfig& tracking,
        std::uint64_t last_sequence,
        Clock::time_point last_timestamp,
        std::string& error) const {
        if (sequence == 0 || timestamp == Clock::time_point{}) {
            error = "frame sequence and timestamp must be non-zero";
            return false;
        }
        if (sequence <= last_sequence ||
            (last_timestamp != Clock::time_point{} && timestamp <= last_timestamp)) {
            error = "non-monotonic detection frame was rejected";
            return false;
        }
        const Clock::time_point now = Clock::now();
        if (timestamp > now) {
            error = "future-dated detection frame was rejected";
            return false;
        }
        if (timestamp <
            now - std::chrono::milliseconds(tracking.max_frame_age_ms)) {
            error = "stale detection frame was rejected";
            return false;
        }
        return true;
    }

    void waitForFailureBackoff(std::stop_token stop, int failure_streak) {
        const int exponent = std::clamp(failure_streak - 1, 0, 6);
        const int delay_ms = std::min(1000, 20 * (1 << exponent));
        std::unique_lock queue_lock(queue_mutex_);
        queue_cv_.wait_for(
            queue_lock,
            stop,
            std::chrono::milliseconds(delay_ms),
            [&] { return hasResetRequest() ||
                         !running_.load(std::memory_order_acquire); });
    }

    std::string detectorBackendName() const {
        return detector_ ? std::string(detector_->backendName()) : std::string("external");
    }

    bool processingAllowed(
        std::uint64_t expected_config_revision,
        std::uint64_t expected_safety_epoch) const {
        std::scoped_lock state_lock(state_mutex_);
        return running_.load(std::memory_order_acquire) &&
               activationAllowsProcessing(activation_) &&
               config_revision_ == expected_config_revision &&
               safety_epoch_ == expected_safety_epoch;
    }

    void workerLoop(std::stop_token stop) noexcept {
        WorkerIdentity worker_identity(*this);
        AimEngine engine(initialConfig());
        std::uint64_t applied_config_revision = 0;
        std::uint64_t last_processed_sequence = 0;
        Clock::time_point last_processed_timestamp{};
        int failure_streak = 0;

        while (running_.load(std::memory_order_acquire) && !stop.stop_requested()) {
            try {
                StateCopy before_capture = copyState();
                if (!activationAllowsProcessing(before_capture.activation)) {
                    engine.reset();
                    waitWhileOutputBlocked(stop, before_capture.config_revision);
                    continue;
                }
                applyState(engine, before_capture, applied_config_revision);

                double capture_ms = 0.0;
                double inference_ms = 0.0;
                std::shared_ptr<const DetectionFrame> frame;
                if (frame_source_) {
                    CaptureResult captured = captureAndInfer(
                        stop,
                        before_capture,
                        last_processed_sequence,
                        last_processed_timestamp,
                        capture_ms,
                        inference_ms);
                    if (captured.status == CaptureStatus::Stopped) {
                        break;
                    }
                    if (captured.status == CaptureStatus::RecoverableError) {
                        ++failure_streak;
                        publishError("frame capture reported a recoverable error", false);
                        waitForFailureBackoff(stop, failure_streak);
                        continue;
                    }
                    if (captured.status == CaptureStatus::NoFrame) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    if (!captured.timing_error.empty()) {
                        ++failure_streak;
                        publishError(std::move(captured.timing_error), false);
                        waitForFailureBackoff(stop, failure_streak);
                        continue;
                    }
                    frame = std::move(captured.frame);
                } else {
                    frame = waitForExternalFrame(stop);
                }

                StateCopy state = copyState();
                if (!frame) {
                    continue;
                }
                if (state.config_revision != before_capture.config_revision ||
                    state.safety_epoch != before_capture.safety_epoch ||
                    !activationAllowsProcessing(state.activation)) {
                    engine.reset();
                    continue;
                }
                applyState(engine, state, applied_config_revision);

                std::string timing_error;
                if (!validateFrameTiming(
                        frame->sequence,
                        frame->timestamp,
                        state.config.tracking,
                        last_processed_sequence,
                        last_processed_timestamp,
                        timing_error)) {
                    ++failure_streak;
                    publishError(std::move(timing_error), false);
                    waitForFailureBackoff(stop, failure_streak);
                    continue;
                }
                last_processed_sequence = frame->sequence;
                last_processed_timestamp = frame->timestamp;

                AimDecision decision = engine.process(*frame, state.combat, state.activation);
                decision.snapshot.running = true;
                decision.snapshot.model_ready = true;
                decision.snapshot.capture_ms = capture_ms;
                decision.snapshot.inference_ms = inference_ms;
                decision.snapshot.detector_backend = detectorBackendName();
                decision.snapshot.detection_frame = frame;
                {
                    std::scoped_lock state_lock(state_mutex_);
                    // A completed old inference must not republish boxes after
                    // config changes, focus loss, disable or tracking reset.
                    if (config_revision_ != state.config_revision ||
                        safety_epoch_ != state.safety_epoch ||
                        !running_.load(std::memory_order_acquire) ||
                        !activationAllowsProcessing(activation_)) {
                        continue;
                    }
                    // Count once even when no target was found; sequence gaps
                    // and host polling must not inflate or cap this rate.
                    recognition_frequency_.record(Clock::now());
                    snapshot_ = decision.snapshot;
                }

                if (!decision.has_motion && !decision.request_click) {
                    failure_streak = 0;
                    continue;
                }

                if (decision.has_motion && beginOutput(state.safety_epoch)) {
                    try {
                        motion_sink_->submitAimMotion(decision.motion);
                    } catch (...) {
                        endOutput();
                        throw;
                    }
                    endOutput();
                }
                if (decision.request_click && trigger_sink_ && beginOutput(state.safety_epoch)) {
                    try {
                        trigger_sink_->click(frame->timestamp);
                    } catch (...) {
                        endOutput();
                        throw;
                    }
                    endOutput();
                }
                failure_streak = 0;
            } catch (const std::exception& error) {
                if (stop.stop_requested()) {
                    break;
                }
                ++failure_streak;
                publishError(error.what(), true);
                waitForFailureBackoff(stop, failure_streak);
            } catch (...) {
                if (stop.stop_requested()) {
                    break;
                }
                ++failure_streak;
                publishError("unknown pubg_ai_aim worker error", true);
                waitForFailureBackoff(stop, failure_streak);
            }
        }
        std::scoped_lock state_lock(state_mutex_);
        snapshot_.running = false;
        snapshot_.target_locked = false;
        snapshot_.detection_frame.reset();
        recognition_frequency_.reset();
    }

    void publishError(std::string message, bool model_unavailable) {
        std::scoped_lock state_lock(state_mutex_);
        snapshot_.running = running_.load(std::memory_order_acquire);
        if (model_unavailable) {
            snapshot_.model_ready = false;
        }
        snapshot_.detector_backend = detectorBackendName();
        snapshot_.last_error = std::move(message);
        snapshot_.target_locked = false;
        snapshot_.detection_frame.reset();
        recognition_frequency_.reset();
    }

    AimConfig initialConfig() const {
        std::scoped_lock state_lock(state_mutex_);
        return config_;
    }

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex queue_mutex_;
    mutable std::mutex output_gate_;
    std::condition_variable output_cv_;
    std::condition_variable_any queue_cv_;
    std::jthread worker_;
    std::atomic<bool> running_{false};

    AimConfig config_;
    CombatContext combat_{};
    ActivationState activation_{};
    AimSnapshot snapshot_{};
    detail::RecognitionFrequency recognition_frequency_;
    std::uint64_t config_revision_ = 1;
    std::uint64_t safety_epoch_ = 1;
    bool reset_requested_ = true;
    bool output_in_flight_ = false;
    std::thread::id output_owner_{};
    std::size_t output_fence_count_ = 0;

    std::unique_ptr<IFrameSource> frame_source_;
    std::unique_ptr<IObjectDetector> detector_;
    std::shared_ptr<IMotionSink> motion_sink_;
    std::shared_ptr<ITriggerSink> trigger_sink_;

    std::shared_ptr<const DetectionFrame> latest_frame_;
    std::uint64_t newest_submitted_sequence_ = 0;
    Clock::time_point newest_submitted_timestamp_{};

    inline static thread_local Impl* active_worker_ = nullptr;
};

AimModule::AimModule(
    AimConfig config,
    std::shared_ptr<IMotionSink> motion_sink,
    std::shared_ptr<ITriggerSink> trigger_sink)
    : impl_(std::make_unique<Impl>(
          std::move(config), nullptr, nullptr, std::move(motion_sink), std::move(trigger_sink))) {}

AimModule::AimModule(
    AimConfig config,
    std::unique_ptr<IFrameSource> frame_source,
    std::unique_ptr<IObjectDetector> detector,
    std::shared_ptr<IMotionSink> motion_sink,
    std::shared_ptr<ITriggerSink> trigger_sink)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(frame_source), std::move(detector),
          std::move(motion_sink), std::move(trigger_sink))) {}

AimModule::~AimModule() = default;

bool AimModule::start() { return impl_->start(); }
void AimModule::stop() noexcept { impl_->stop(); }
void AimModule::setEnabled(bool enabled) { impl_->setEnabled(enabled); }
void AimModule::updateConfig(AimConfig config) { impl_->updateConfig(std::move(config)); }
void AimModule::updateCombatContext(CombatContext context) {
    impl_->updateCombatContext(std::move(context));
}
void AimModule::updateActivation(ActivationState state) {
    impl_->updateActivation(state);
}
bool AimModule::submitDetections(DetectionFrame frame) {
    return impl_->submitDetections(std::move(frame));
}
void AimModule::resetTracking() { impl_->resetTracking(); }
AimSnapshot AimModule::snapshot() const { return impl_->snapshot(); }

} // namespace pubg::ai_aim
