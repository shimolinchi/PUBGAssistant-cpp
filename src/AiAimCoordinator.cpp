#include "AiAimCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace pubg {
namespace detail {

class AiAimMotionSinkPermit final {
public:
    void authorize() noexcept {
        std::lock_guard lock(mutex_);
        authorized_ = true;
    }

    void revoke() noexcept {
        std::lock_guard lock(mutex_);
        authorized_ = false;
    }

    void setMaxAge(std::chrono::milliseconds age) noexcept {
        std::lock_guard lock(mutex_);
        max_age_ = std::max(std::chrono::milliseconds{1}, age);
    }

    template <typename Submit>
    void submitIfAuthorized(Submit&& submit) {
        std::lock_guard lock(mutex_);
        if (authorized_) {
            submit(max_age_);
        }
    }

private:
    std::mutex mutex_;
    bool authorized_ = false;
    std::chrono::milliseconds max_age_{80};
};

} // namespace detail

namespace {

using namespace pubg::ai_aim;

thread_local const AiAimCoordinator* active_status_callback = nullptr;

bool sameCombatContext(const CombatContext& a, const CombatContext& b) noexcept {
    return a.weapon == b.weapon && a.scope == b.scope && a.grip == b.grip &&
           a.muzzle == b.muzzle && a.stock == b.stock && a.stance == b.stance &&
           a.scope_zoom == b.scope_zoom && a.ads == b.ads && a.firing == b.firing &&
           a.crouched == b.crouched && a.lean_left == b.lean_left &&
           a.lean_right == b.lean_right;
}

bool sameActivation(const ActivationState& a, const ActivationState& b) noexcept {
    return a.enabled == b.enabled && a.aim_primary == b.aim_primary &&
           a.aim_secondary == b.aim_secondary && a.lead_boost == b.lead_boost &&
           a.manual_lead == b.manual_lead && a.paused == b.paused &&
           a.game_foreground == b.game_foreground &&
           a.blocking_ui_open == b.blocking_ui_open;
}

std::string backendStatusDetail(const AimSnapshot& snapshot) {
    if (snapshot.detector_backend == "DirectML") {
        return "DML";
    }
    if (snapshot.detector_backend == "CPU") {
        return "CPU";
    }
    return "加载";
}

void notifyStatusCallback(
    const AiAimCoordinator* owner,
    const AiAimCoordinator::StatusCallback& callback,
    const AiAimRuntimeStatus& status) noexcept {
    if (!callback) {
        return;
    }
    const AiAimCoordinator* previous_callback = active_status_callback;
    active_status_callback = owner;
    try {
        callback(status);
    } catch (...) {
        // Status observers must not be able to interrupt lifecycle or safety gates.
    }
    active_status_callback = previous_callback;
}

class DispatcherAimMotionSink final : public IMotionSink {
public:
    DispatcherAimMotionSink(
        std::shared_ptr<MouseMotionChannel> channel,
        std::shared_ptr<detail::AiAimMotionSinkPermit> permit)
        : channel_(std::move(channel)),
          permit_(std::move(permit)) {}

    void submitAimMotion(const MotionCommand& command) override {
        permit_->submitIfAuthorized([&](std::chrono::milliseconds max_age) {
            (void)channel_->submit(
                command.dx,
                command.dy,
                command.frame_sequence,
                command.timestamp,
                max_age);
        });
    }

private:
    std::shared_ptr<MouseMotionChannel> channel_;
    std::shared_ptr<detail::AiAimMotionSinkPermit> permit_;
};

} // namespace

AiAimCoordinator::AiAimCoordinator(
    std::shared_ptr<MouseMotionDispatcher> dispatcher,
    ModuleFactory module_factory)
    : dispatcher_(std::move(dispatcher)),
      module_factory_(std::move(module_factory)) {
    if (!dispatcher_) {
        throw std::invalid_argument("AiAimCoordinator requires a mouse motion dispatcher");
    }
    if (!module_factory_) {
        throw std::invalid_argument("AiAimCoordinator requires an AI module factory");
    }
    motion_channel_ = dispatcher_->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    motion_channel_->closeAndFence();
    control_worker_ = std::jthread([this](std::stop_token stop) { controlLoop(stop); });
    control_worker_id_ = control_worker_.get_id();
}

AiAimCoordinator::~AiAimCoordinator() {
    {
        std::lock_guard callback_lock(status_callback_mutex_);
        std::lock_guard lock(mutex_);
        status_callback_ = {};
    }
    shutdown();
}

void AiAimCoordinator::setStatusCallback(StatusCallback callback) {
    std::unique_lock callback_lock(status_callback_mutex_);
    {
        std::lock_guard lock(mutex_);
        status_callback_ = std::move(callback);
        ++status_callback_epoch_;
        callback_snapshot_delivered_ = false;
    }
    drainStatusCallbacksLocked(callback_lock);
}

void AiAimCoordinator::setEnabled(bool enabled) {
    std::shared_ptr<AimModule> module;
    std::shared_ptr<BuildAttempt> build_to_stop;
    ActivationState activation;
    CombatContext combat;
    std::uint64_t generation = 0;
    bool resume_ready_module = false;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (shutdown_ || desired_enabled_ == enabled) {
                return;
            }
            desired_enabled_ = enabled;
            generation = ++generation_;
            activation_.enabled = enabled;
            activation = activation_;
            module = module_;
            combat = combat_;
            build_to_stop = active_build_attempt_;
            resume_ready_module = enabled && module_ready_ && module != nullptr;
        }
        if (!enabled) {
            reconcileMotionGateLocked();
        }
        if (module) {
            module->updateActivation(activation);
        }
        if (enabled) {
            reconcileMotionGateLocked();
        }
    }

    if (build_to_stop) {
        build_to_stop->stop_source.request_stop();
    }

    if (!enabled) {
        publishTransitionStatus(generation, false, AiAimRuntimeState::Off, "关闭");
    } else if (resume_ready_module) {
        const bool waiting_for_weapon = combat.weapon.empty();
        publishModuleStatus(
            generation,
            combat.revision,
            module,
            waiting_for_weapon ? AiAimRuntimeState::Paused : AiAimRuntimeState::Ready,
            waiting_for_weapon ? "等待武器" : backendStatusDetail(module->snapshot()));
    } else {
        publishTransitionStatus(generation, true, AiAimRuntimeState::Loading, "加载");
    }
    condition_.notify_all();
}

bool AiAimCoordinator::desiredEnabled() const {
    std::lock_guard lock(mutex_);
    return desired_enabled_ && !shutdown_;
}

void AiAimCoordinator::updateCombatContext(
    CombatContext context,
    const std::uint64_t source_revision) {
    std::shared_ptr<AimModule> module;
    std::uint64_t generation = 0;
    std::uint64_t combat_revision = 0;
    const bool waiting_for_weapon = context.weapon.empty();
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (source_revision != 0 &&
                source_revision <= combat_source_revision_) {
                return;
            }
            if (source_revision != 0) {
                combat_source_revision_ = source_revision;
            }
            if (sameCombatContext(combat_, context)) {
                return;
            }
            context.revision = combat_.revision + 1;
            combat_ = context;
            module = module_;
            generation = generation_;
            combat_revision = context.revision;
        }
        if (waiting_for_weapon) {
            reconcileMotionGateLocked();
        }
        if (module) {
            module->updateCombatContext(context);
        }
        if (!waiting_for_weapon) {
            reconcileMotionGateLocked();
        }
    }
    if (module) {
        const std::string ready_detail = backendStatusDetail(module->snapshot());
        publishModuleStatus(
            generation,
            combat_revision,
            module,
            waiting_for_weapon ? AiAimRuntimeState::Paused : AiAimRuntimeState::Ready,
            waiting_for_weapon ? "等待武器" : ready_detail);
    }
}

void AiAimCoordinator::updateActivation(ActivationState activation) {
    std::shared_ptr<AimModule> module;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            activation.enabled = activation.enabled && desired_enabled_ && !shutdown_;
            if (sameActivation(activation_, activation)) {
                return;
            }
            activation_ = activation;
            module = module_;
        }

        if (!activationAllowsProcessing(activation)) {
            reconcileMotionGateLocked();
        }
        if (module) {
            module->updateActivation(activation);
        }
        if (activationAllowsProcessing(activation)) {
            reconcileMotionGateLocked();
        }
    }
}

void AiAimCoordinator::resetTracking() {
    std::shared_ptr<AimModule> module;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            module = module_;
        }
        closeMotionGateLocked();
        if (module) {
            module->resetTracking();
        }
        reconcileMotionGateLocked();
    }
}

bool AiAimCoordinator::updateConfiguration(
    ModuleFactory module_factory, AimConfig config,
    std::chrono::milliseconds motion_max_age, bool recreate_runtime) {
    if (!module_factory) throw std::invalid_argument("AI module factory is required");
    config.sanitize();
    std::shared_ptr<BuildAttempt> build_to_stop;
    std::shared_ptr<AimModule> module;
    CombatContext combat;
    std::uint64_t generation = 0;
    bool enabled = false;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return false;
            module_factory_ = std::move(module_factory);
            generation = ++generation_;
            enabled = desired_enabled_;
            build_to_stop = active_build_attempt_;
            module_restart_requested_ = module_restart_requested_ || (recreate_runtime && module_);
            if (!module_restart_requested_) module = module_;
            combat = combat_;
        }
        // Fence already queued/in-flight output before publishing a new config.
        closeMotionGateLocked();
        if (module) {
            module->updateConfig(std::move(config));
            module_motion_permit_->setMaxAge(motion_max_age);
        }
        reconcileMotionGateLocked();
    }
    if (build_to_stop) build_to_stop->stop_source.request_stop();
    if (!enabled) {
        publishTransitionStatus(generation, false, AiAimRuntimeState::Off, "关闭");
    } else if (module) {
        publishModuleStatus(generation, combat.revision, module,
            combat.weapon.empty() ? AiAimRuntimeState::Paused : AiAimRuntimeState::Ready,
            combat.weapon.empty() ? "等待武器" : backendStatusDetail(module->snapshot()));
    } else {
        publishTransitionStatus(generation, true, AiAimRuntimeState::Loading, "加载");
    }
    condition_.notify_all();
    return true;
}

void AiAimCoordinator::shutdown() noexcept {
    std::shared_ptr<AimModule> module;
    std::shared_ptr<BuildAttempt> build_to_stop;
    ActivationState disabled_activation;
    bool initiate_shutdown = false;
    const bool called_from_worker = std::this_thread::get_id() == control_worker_id_;
    const bool called_from_status_callback = active_status_callback == this;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (shutdown_complete_) {
                return;
            }
            if (!shutdown_) {
                shutdown_ = true;
                desired_enabled_ = false;
                activation_.enabled = false;
                disabled_activation = activation_;
                ++generation_;
                module = module_;
                build_to_stop = active_build_attempt_;
                initiate_shutdown = true;
            }
        }
        if (initiate_shutdown) {
            reconcileMotionGateLocked();
            if (module) {
                try {
                    module->updateActivation(disabled_activation);
                } catch (...) {
                }
            }
        }
    }

    if (initiate_shutdown) {
        if (build_to_stop) {
            build_to_stop->stop_source.request_stop();
        }
        control_worker_.request_stop();
        condition_.notify_all();
    }

    // A status callback may request shutdown from either the control worker or
    // an App thread. It initiates cancellation but must not wait while callback
    // delivery is serialized; the control worker completes cleanup afterward.
    if (called_from_worker || called_from_status_callback) {
        return;
    }

    if (initiate_shutdown && control_worker_.joinable()) {
        control_worker_.join();
    }

    if (!initiate_shutdown) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return shutdown_complete_; });
    }
}

AiAimRuntimeStatus AiAimCoordinator::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

AimSnapshot AiAimCoordinator::snapshot() const {
    std::shared_ptr<AimModule> module;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        module = module_;
        generation = generation_;
    }
    auto result = module ? module->snapshot() : AimSnapshot{};
    {
        std::lock_guard lock(mutex_);
        if (module != module_ || generation != generation_ || shutdown_ ||
            !desired_enabled_ || !module_ready_ || module_restart_requested_ ||
            combat_.weapon.empty() || !activationAllowsProcessing(activation_)) {
            result.detection_frame.reset();
            result.target_locked = false;
            result.recognition_hz = 0.0;
        }
    }
    return result;
}

void AiAimCoordinator::reconcileMotionGateLocked() {
    bool should_open = false;
    std::shared_ptr<detail::AiAimMotionSinkPermit> permit;
    {
        std::lock_guard lock(mutex_);
        should_open = !shutdown_ && !module_restart_requested_ && desired_enabled_ && module_ready_ && module_ &&
                      !combat_.weapon.empty() &&
                      activationAllowsProcessing(activation_);
        permit = module_motion_permit_;
    }

    if (should_open) {
        if (!motion_channel_->isOpen()) {
            motion_channel_->open();
        }
        if (permit) {
            permit->authorize();
        }
    } else {
        closeMotionGateLocked();
    }
}

void AiAimCoordinator::closeMotionGateLocked() {
    std::shared_ptr<detail::AiAimMotionSinkPermit> permit;
    {
        std::lock_guard lock(mutex_);
        permit = module_motion_permit_;
    }
    if (permit) {
        permit->revoke();
    }
    if (motion_channel_->isOpen()) {
        motion_channel_->closeAndFence();
    }
}

void AiAimCoordinator::finishBuildAttempt(
    const std::shared_ptr<BuildAttempt>& attempt) noexcept {
    std::lock_guard lock(mutex_);
    if (active_build_attempt_ == attempt) {
        active_build_attempt_.reset();
    }
}

void AiAimCoordinator::controlLoop(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        bool enable = false;
        bool restart = false;
        std::uint64_t generation = 0;
        ModuleFactory factory;
        std::shared_ptr<AimModule> module_to_stop;
        std::shared_ptr<BuildAttempt> build_attempt;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop, [this] {
                return shutdown_ || module_restart_requested_ || (desired_enabled_ && !module_) ||
                       (!desired_enabled_ && module_ != nullptr);
            });
            if (stop.stop_requested() || shutdown_) {
                break;
            }
            enable = desired_enabled_;
            restart = module_restart_requested_;
            generation = generation_;
            if (enable && !restart) {
                factory = module_factory_;
                build_attempt = std::make_shared<BuildAttempt>();
                active_build_attempt_ = build_attempt;
            }
        }

        if (!enable || restart) {
            {
                std::lock_guard gate_lock(motion_gate_mutex_);
                {
                    std::lock_guard lock(mutex_);
                    if (!desired_enabled_ || module_restart_requested_) {
                        module_restart_requested_ = false;
                        module_ready_ = false;
                        if (module_motion_permit_) {
                            module_motion_permit_->revoke();
                            module_motion_permit_.reset();
                        }
                        module_to_stop = std::move(module_);
                        module_warning_.clear();
                    }
                }
                reconcileMotionGateLocked();
            }
            if (module_to_stop) {
                module_to_stop->stop();
            }
            continue;
        }

        std::stop_callback propagate_worker_stop(stop, [build_attempt] {
            build_attempt->stop_source.request_stop();
        });
        std::shared_ptr<AimModule> created;
        try {
            publishTransitionStatus(
                generation, true, AiAimRuntimeState::Loading, "加载");
            if (stop.stop_requested() || build_attempt->stop_source.stop_requested()) {
                finishBuildAttempt(build_attempt);
                continue;
            }
            auto motion_permit = std::make_shared<detail::AiAimMotionSinkPermit>();
            auto motion_sink = std::make_shared<DispatcherAimMotionSink>(
                motion_channel_, motion_permit);
            ModuleBuild build = factory(
                motion_sink, build_attempt->stop_source.get_token());
            if (!build.module) {
                throw std::runtime_error("AI module factory returned null");
            }
            motion_permit->setMaxAge(build.motion_max_age);
            std::string startup_warning = std::move(build.startup_warning);
            created = std::shared_ptr<AimModule>(std::move(build.module));

            CombatContext combat;
            ActivationState activation;
            bool accepted = false;
            {
                std::lock_guard gate_lock(motion_gate_mutex_);
                {
                    std::lock_guard lock(mutex_);
                    if (!shutdown_ && desired_enabled_ && generation == generation_ &&
                        !module_) {
                        module_motion_permit_ = motion_permit;
                        module_ = created;
                        module_warning_ = std::move(startup_warning);
                        module_ready_ = false;
                        combat = combat_;
                        activation = activation_;
                        activation.enabled = true;
                        accepted = true;
                    }
                }
                if (accepted) {
                    created->updateCombatContext(combat);
                    created->updateActivation(activation);
                    if (!created->start()) {
                        throw std::runtime_error("AI worker failed to start");
                    }
                    {
                        std::lock_guard lock(mutex_);
                        if (!shutdown_ && desired_enabled_ && generation == generation_ &&
                            module_ == created) {
                            module_ready_ = true;
                        } else {
                            motion_permit->revoke();
                            if (module_motion_permit_ == motion_permit) {
                                module_motion_permit_.reset();
                            }
                            module_.reset();
                            module_warning_.clear();
                            module_ready_ = false;
                            accepted = false;
                        }
                    }
                    reconcileMotionGateLocked();
                }
            }
            if (!accepted) {
                created->stop();
                finishBuildAttempt(build_attempt);
                continue;
            }
            finishBuildAttempt(build_attempt);
            const std::string backend_status = backendStatusDetail(created->snapshot());
            publishModuleStatus(
                generation,
                combat.revision,
                created,
                combat.weapon.empty() ? AiAimRuntimeState::Paused : AiAimRuntimeState::Ready,
                combat.weapon.empty() ? "等待武器" : backend_status);
        } catch (const std::exception& error) {
            bool current_failure = false;
            std::uint64_t failure_generation = 0;
            {
                std::lock_guard gate_lock(motion_gate_mutex_);
                {
                    std::lock_guard lock(mutex_);
                    current_failure = generation == generation_ &&
                                      desired_enabled_ && !shutdown_;
                    if (module_ == created) {
                        if (module_motion_permit_) {
                            module_motion_permit_->revoke();
                            module_motion_permit_.reset();
                        }
                        module_.reset();
                        module_warning_.clear();
                        module_ready_ = false;
                    }
                    if (current_failure) {
                        desired_enabled_ = false;
                        activation_.enabled = false;
                        failure_generation = ++generation_;
                    }
                }
                reconcileMotionGateLocked();
            }
            if (created) {
                created->stop();
            }
            if (current_failure) {
                publishTransitionStatus(
                    failure_generation, false, AiAimRuntimeState::Error, error.what());
            }
            finishBuildAttempt(build_attempt);
        } catch (...) {
            bool current_failure = false;
            std::uint64_t failure_generation = 0;
            {
                std::lock_guard gate_lock(motion_gate_mutex_);
                {
                    std::lock_guard lock(mutex_);
                    current_failure = generation == generation_ &&
                                      desired_enabled_ && !shutdown_;
                    if (module_ == created) {
                        if (module_motion_permit_) {
                            module_motion_permit_->revoke();
                            module_motion_permit_.reset();
                        }
                        module_.reset();
                        module_warning_.clear();
                        module_ready_ = false;
                    }
                    if (current_failure) {
                        desired_enabled_ = false;
                        activation_.enabled = false;
                        failure_generation = ++generation_;
                    }
                }
                reconcileMotionGateLocked();
            }
            if (created) {
                created->stop();
            }
            if (current_failure) {
                publishTransitionStatus(
                    failure_generation,
                    false,
                    AiAimRuntimeState::Error,
                    "unknown AI initialization error");
            }
            finishBuildAttempt(build_attempt);
        }
    }

    std::shared_ptr<AimModule> module_to_stop;
    {
        std::lock_guard gate_lock(motion_gate_mutex_);
        {
            std::lock_guard lock(mutex_);
            module_ready_ = false;
            active_build_attempt_.reset();
            module_warning_.clear();
            if (module_motion_permit_) {
                module_motion_permit_->revoke();
                module_motion_permit_.reset();
            }
            module_to_stop = std::move(module_);
        }
        reconcileMotionGateLocked();
    }
    if (module_to_stop) {
        module_to_stop->stop();
    }
    publishStatus(AiAimRuntimeState::Off, "关闭");
    {
        std::lock_guard lock(mutex_);
        shutdown_complete_ = true;
    }
    condition_.notify_all();
}

std::uint64_t AiAimCoordinator::updateStatusLocked(
    AiAimRuntimeState state,
    std::string detail,
    std::string warning) {
    if (status_.state == state && status_.detail == detail &&
        status_.warning == warning &&
        status_.lifecycle_generation == generation_) {
        return 0;
    }
    const std::uint64_t revision = ++status_revision_;
    status_ = AiAimRuntimeStatus{
        state,
        std::move(detail),
        std::move(warning),
        generation_,
        revision};
    return revision;
}

void AiAimCoordinator::publishStatus(AiAimRuntimeState state, std::string detail) {
    std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_);
        revision = updateStatusLocked(state, std::move(detail));
    }
    if (revision != 0) {
        dispatchStatus(revision);
    }
}

void AiAimCoordinator::publishTransitionStatus(
    std::uint64_t generation,
    bool expected_enabled,
    AiAimRuntimeState state,
    std::string detail) {
    std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_ || generation_ != generation ||
            desired_enabled_ != expected_enabled) {
            return;
        }
        revision = updateStatusLocked(state, std::move(detail));
    }
    if (revision != 0) {
        dispatchStatus(revision);
    }
}

void AiAimCoordinator::publishModuleStatus(
    std::uint64_t generation,
    std::uint64_t combat_revision,
    const std::shared_ptr<AimModule>& expected_module,
    AiAimRuntimeState state,
    std::string detail) {
    std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_ || !desired_enabled_ || generation_ != generation ||
            combat_.revision != combat_revision || !module_ready_ || module_restart_requested_ ||
            module_ != expected_module) {
            return;
        }
        revision = updateStatusLocked(
            state, std::move(detail), module_warning_);
    }
    if (revision != 0) {
        dispatchStatus(revision);
    }
}

void AiAimCoordinator::drainStatusCallbacksLocked(
    std::unique_lock<std::recursive_mutex>& callback_lock) noexcept {
    while (callback_lock.owns_lock()) {
        StatusCallback callback;
        AiAimRuntimeStatus current;
        std::uint64_t callback_epoch = 0;
        {
            std::unique_lock lock(mutex_);
            const bool current_snapshot_delivered =
                callback_snapshot_delivered_ &&
                delivered_callback_epoch_ == status_callback_epoch_ &&
                delivered_status_revision_ == status_revision_;
            if (current_snapshot_delivered) {
                // Release the callback mutex while status publication is still
                // excluded. A later revision can then acquire callback ownership
                // instead of observing a false "busy" result and being lost.
                callback_lock.unlock();
                return;
            }
            current = status_;
            callback = status_callback_;
            callback_epoch = status_callback_epoch_;
        }

        notifyStatusCallback(this, callback, current);

        std::lock_guard lock(mutex_);
        if (callback_epoch == status_callback_epoch_) {
            delivered_callback_epoch_ = callback_epoch;
            delivered_status_revision_ = current.revision;
            callback_snapshot_delivered_ = true;
        }
    }
}

void AiAimCoordinator::dispatchStatus(std::uint64_t /*revision*/) noexcept {
    if (active_status_callback == this) {
        // The outer delivery loop will observe and drain the new revision.
        return;
    }
    std::unique_lock callback_lock(
        status_callback_mutex_, std::try_to_lock);
    if (!callback_lock.owns_lock()) {
        // Another publisher owns delivery and is responsible for draining the
        // latest revision before it releases the callback mutex.
        return;
    }
    drainStatusCallbacksLocked(callback_lock);
}

} // namespace pubg
