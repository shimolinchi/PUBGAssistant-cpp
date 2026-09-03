#include "pubg_ai_aim/pubg_ai_aim.hpp"

#include "pubg_ai_aim/Algorithms.hpp"
#include "pubg_ai_aim/detail/RecognitionFrequency.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aim = pubg::ai_aim;

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
    }
}

void near(float actual, float expected, float tolerance, const char* expression, int line) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        ++failures;
        std::cerr << "FAILED line " << line << ": " << expression
                  << " actual=" << actual << " expected=" << expected << '\n';
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)
#define NEAR(actual, expected, tolerance) near((actual), (expected), (tolerance), #actual, __LINE__)

bool waitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout) {
    const auto deadline = aim::Clock::now() + timeout;
    while (aim::Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

void testRecognitionFrequencyMeasurement() {
    using namespace std::chrono_literals;
    aim::detail::RecognitionFrequency frequency;
    const aim::Clock::time_point start{};
    CHECK(frequency.hertz(start) == 0.0);
    frequency.record(start);
    CHECK(frequency.hertz(start) == 0.0);
    // 100 actual completions/s, read by a 20Hz host. Reads never add samples.
    for (int i = 1; i <= 100; ++i) {
        const auto now = start + i * 10ms;
        frequency.record(now);
        if (i < 50) CHECK(frequency.hertz(now) == 0.0);
        if (i >= 50 && i % 5 == 0) {
            CHECK(frequency.hertz(now) == 100.0);
            CHECK(frequency.hertz(now) == 100.0);
        }
    }
    // A slower next window is measured without changing any target FPS.
    for (int i = 1; i <= 10; ++i) frequency.record(start + 1s + i * 50ms);
    CHECK(frequency.hertz(start + 1500ms) == 20.0);
    CHECK(frequency.hertz(start + 1499ms) == 0.0); // Invalid read time.
    CHECK(frequency.hertz(start + 2499ms) == 20.0);
    CHECK(frequency.hertz(start + 2500ms) == 0.0); // No frames, not a frozen rate.
    frequency.record(start + 3s);
    CHECK(frequency.hertz(start + 3s) == 0.0); // Resume starts a fresh window.
    frequency.record(start + 3500ms);
    CHECK(frequency.hertz(start + 3500ms) == 2.0); // Low rates still measurable.
    frequency.reset();
    CHECK(frequency.hertz(start + 3500ms) == 0.0);
    frequency.record(start + 4s);
    frequency.record(start + 4500ms);
    CHECK(frequency.hertz(start + 4500ms) == 2.0);
    frequency.record(start); // A new clock/session cannot retain the old rate.
    CHECK(frequency.hertz(start) == 0.0);
}

void testActivationAllowsProcessing() {
    aim::ActivationState valid;
    valid.enabled = true;
    valid.game_foreground = true;
    valid.aim_primary = true;
    CHECK(aim::activationAllowsProcessing(valid));

    auto state = valid;
    state.aim_primary = false;
    state.aim_secondary = true;
    CHECK(aim::activationAllowsProcessing(state));

    state = valid;
    state.enabled = false;
    CHECK(!aim::activationAllowsProcessing(state));

    state = valid;
    state.game_foreground = false;
    CHECK(!aim::activationAllowsProcessing(state));

    state = valid;
    state.paused = true;
    CHECK(!aim::activationAllowsProcessing(state));

    state = valid;
    state.blocking_ui_open = true;
    CHECK(!aim::activationAllowsProcessing(state));

    state = valid;
    state.aim_primary = false;
    state.aim_secondary = false;
    CHECK(!aim::activationAllowsProcessing(state));

    state.lead_boost = true;
    state.manual_lead = true;
    CHECK(!aim::activationAllowsProcessing(state));

    state = valid;
    state.lead_boost = true;
    state.manual_lead = true;
    CHECK(aim::activationAllowsProcessing(state));
}

void setAnchor(
    std::vector<float>& raw,
    int anchors,
    int anchor,
    float cx,
    float cy,
    float width,
    float height,
    float class0,
    float class1) {
    raw[anchor] = cx;
    raw[anchors + anchor] = cy;
    raw[2 * anchors + anchor] = width;
    raw[3 * anchors + anchor] = height;
    raw[4 * anchors + anchor] = class0;
    raw[5 * anchors + anchor] = class1;
}

void testIoU() {
    const aim::Box a = aim::Box::fromCenter(0.0F, 0.0F, 10.0F, 10.0F);
    NEAR(aim::intersectionOverUnion(a, a), 1.0F, 1e-6F);
    NEAR(aim::intersectionOverUnion(a, aim::Box::fromCenter(20.0F, 0.0F, 10.0F, 10.0F)), 0.0F, 1e-6F);
    NEAR(aim::intersectionOverUnion(a, aim::Box::fromCenter(5.0F, 0.0F, 10.0F, 10.0F)), 1.0F / 3.0F, 1e-6F);
    NEAR(aim::intersectionOverUnion(a, aim::Box::fromCenter(0.0F, 0.0F, 4.0F, 4.0F)), 0.16F, 1e-6F);
    NEAR(aim::intersectionOverUnion({}, {}), 0.0F, 1e-6F);
}

void testDecoderLayoutAndRounding() {
    aim::DetectorConfig config;
    config.anchor_count = 8;
    config.class_count = 2;
    config.confidence = 0.404F;
    config.iou_threshold = 1.0F;
    std::vector<float> raw(6 * config.anchor_count, 0.0F);
    setAnchor(raw, config.anchor_count, 7, 100.25F, 120.75F, 20.4F, 40.6F, 0.7F, 0.7F);
    setAnchor(raw, config.anchor_count, 3, 10.0F, 10.0F, 2.0F, 2.0F, 0.404F, 0.1F);

    const auto corrected = aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Corrected);
    CHECK(corrected.size() == 1);
    CHECK(corrected[0].class_id == 0);
    NEAR(corrected[0].confidence, 0.7F, 1e-6F);
    NEAR(corrected[0].box.left, 90.05F, 1e-4F);
    NEAR(corrected[0].box.top, 100.45F, 1e-4F);
    NEAR(corrected[0].box.right, 110.45F, 1e-4F);
    NEAR(corrected[0].box.bottom, 141.05F, 1e-4F);

    const auto original = aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Original);
    CHECK(original.size() == 1);
    NEAR(original[0].box.center().x, 100.0F, 1e-6F);
    NEAR(original[0].box.center().y, 121.0F, 1e-6F);
    NEAR(original[0].box.width(), 20.0F, 1e-6F);
    NEAR(original[0].box.height(), 41.0F, 1e-6F);

    config.anchor_count = 1;
    raw.assign(6, 0.0F);
    setAnchor(raw, 1, 0, 10.5F, 11.5F, 20.5F, 21.5F, 0.9F, 0.1F);
    const auto ties_even = aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Original);
    CHECK(ties_even.size() == 1);
    NEAR(ties_even[0].box.center().x, 10.0F, 1e-6F);
    NEAR(ties_even[0].box.center().y, 12.0F, 1e-6F);
    NEAR(ties_even[0].box.width(), 20.0F, 1e-6F);
    NEAR(ties_even[0].box.height(), 22.0F, 1e-6F);

    std::vector<aim::Detection> scaled{
        {aim::Box::fromCenter(160.0F, 80.0F, 80.0F, 40.0F), 0.9F, 1}};
    aim::scaleDetectionBoxes(scaled, 2.0F, 0.5F);
    NEAR(scaled[0].box.left, 240.0F, 1e-6F);
    NEAR(scaled[0].box.top, 30.0F, 1e-6F);
    NEAR(scaled[0].box.right, 400.0F, 1e-6F);
    NEAR(scaled[0].box.bottom, 50.0F, 1e-6F);
}

void testNmsModes() {
    aim::DetectorConfig config;
    config.anchor_count = 2;
    config.class_count = 2;
    config.confidence = 0.1F;
    config.iou_threshold = 0.075F;
    config.class_aware_nms = true;
    std::vector<float> raw(12, 0.0F);
    setAnchor(raw, 2, 0, 50.0F, 50.0F, 100.0F, 100.0F, 0.9F, 0.1F);
    setAnchor(raw, 2, 1, 100.0F, 50.0F, 10.0F, 100.0F, 0.8F, 0.1F);
    CHECK(aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Corrected).size() == 2);
    CHECK(aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Original).size() == 1);

    config.iou_threshold = 0.5F;
    raw.assign(12, 0.0F);
    setAnchor(raw, 2, 0, 100.0F, 100.0F, 80.0F, 80.0F, 0.9F, 0.1F);
    setAnchor(raw, 2, 1, 100.0F, 100.0F, 80.0F, 80.0F, 0.1F, 0.85F);
    const auto corrected = aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Corrected);
    const auto original = aim::decodeYoloV8(raw, config, aim::CompatibilityMode::Original);
    CHECK(corrected.size() == 2);
    CHECK(original.size() == 1);
}

void testTargetSelectionAndAssociation() {
    aim::DetectionFrame frame;
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 320;
    frame.screen_height = 320;
    frame.screen_center_x = 160;
    frame.screen_center_y = 160;
    frame.detections = {
        {aim::Box::fromCenter(159.0F, 160.0F, 40.0F, 40.0F), 0.9F, 0},
        {aim::Box::fromCenter(105.0F, 100.0F, 40.0F, 40.0F), 0.8F, 0},
    };
    aim::TargetingConfig targeting;
    targeting.enabled_classes = {true, false, false, false};
    aim::TrackingConfig tracking;
    tracking.iou_threshold = 0.3F;
    aim::TrackingReference previous{
        true,
        aim::Box::fromCenter(100.0F, 100.0F, 40.0F, 40.0F),
        {100.0F, 100.0F},
        40.0F,
    };
    const auto selected = aim::selectTarget(
        frame.detections, frame, targeting, tracking, previous, false);
    CHECK(selected.has_value());
    CHECK(*selected == 1);

    frame.detections = {
        {aim::Box::fromCenter(150.0F, 160.0F, 40.0F, 40.0F), 0.9F, 0},
        {aim::Box::fromCenter(140.0F, 140.0F, 40.0F, 40.0F), 0.8F, 0},
    };
    const auto no_history = aim::selectTarget(
        frame.detections, frame, targeting, tracking, {}, false);
    CHECK(no_history.has_value() && *no_history == 0);

    targeting.target_stickiness = 3.0F;
    tracking.iou_threshold = 0.455F;
    const aim::Detection near_candidate{
        aim::Box::fromCenter(100.0F, 0.0F, 40.0F, 40.0F), 1.0F, 0};
    const aim::Detection far_candidate{
        aim::Box::fromCenter(120.0F, 0.0F, 40.0F, 40.0F), 1.0F, 0};
    const aim::TrackingReference origin{
        true, aim::Box::fromCenter(0.0F, 0.0F, 40.0F, 40.0F), {0.0F, 0.0F}, 40.0F};
    CHECK(aim::associationPasses(near_candidate, {100.0F, 0.0F}, origin, targeting, tracking));
    CHECK(!aim::associationPasses(far_candidate, {120.0F, 0.0F}, origin, targeting, tracking));
    const aim::Detection tall_candidate{
        aim::Box::fromCenter(0.0F, 0.0F, 40.0F, 75.0F), 1.0F, 0};
    CHECK(!aim::associationPasses(tall_candidate, {0.0F, 0.0F}, origin, targeting, tracking));
}

void testAimPointFovAndPid() {
    aim::TargetingConfig targeting;
    targeting.enabled_classes = {true, false, false, false};
    targeting.class_location[0] = 0.723F;
    aim::Detection detection{aim::Box::fromCenter(100.0F, 80.0F, 40.0F, 100.0F), 1.0F, 0};
    const aim::Vec2 point = aim::classAimPoint(detection, targeting);
    NEAR(point.x, 100.0F, 1e-6F);
    NEAR(point.y, 57.7F, 1e-4F);

    NEAR(aim::fovConvertAxis(160.0F, 90.0F, 360, 320, 360.0F), 45.0F, 1e-4F);
    NEAR(aim::fovConvertAxis(80.0F, 90.0F, 360, 320, 360.0F), 26.565051F, 1e-4F);
    NEAR(aim::fovConvertAxis(-80.0F, 90.0F, 360, 320, 360.0F), -26.565051F, 1e-4F);

    aim::LegacyPidState pid;
    NEAR(aim::legacyPidStep(pid, 10.0F, 0.8F, 0.1F, 0.2F), 11.0F, 1e-5F);
    NEAR(aim::legacyPidStep(pid, 10.0F, 0.8F, 0.1F, 0.2F), 9.1F, 1e-5F);
    NEAR(aim::legacyPidStep(pid, 5.0F, 0.8F, 0.1F, 0.2F), 3.61F, 1e-5F);
}

void testEngineUsesFullScreenForFov() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;
    config.controller.fov_enabled = true;
    config.controller.horizontal_fov = 90.0F;
    config.controller.vertical_fov = 90.0F;
    config.controller.game_x_pixels = 360;
    config.controller.game_y_pixels = 360;
    config.controller.pid_enabled = false;
    config.controller.global_scale_enabled = false;
    config.controller.max_pixels = 1000.0F;
    config.targeting.effective_range = 2000.0F;
    config.targeting.class_location[1] = 0.5F;

    aim::DetectionFrame frame;
    frame.sequence = 1;
    frame.timestamp = aim::Clock::now();
    frame.roi_left = 800;
    frame.roi_top = 380;
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 1920;
    frame.screen_height = 1080;
    frame.screen_center_x = 960;
    frame.screen_center_y = 540;
    frame.detections.push_back({
        aim::Box::fromCenter(320.0F, 160.0F, 40.0F, 80.0F), 0.9F, 1});

    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    aim::AimEngine engine(config);
    const auto decision = engine.process(frame, {}, activation);
    CHECK(decision.has_motion);
    const float converted = aim::fovConvertAxis(160.0F, 90.0F, 360, 1920, 360.0F);
    NEAR(static_cast<float>(decision.motion.dx), converted * 0.45F, 1e-4F);
}

void testDmrLeadBoostForcesBodyClass() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;
    config.controller.fov_enabled = false;
    config.controller.pid_enabled = false;
    config.controller.global_scale_enabled = false;
    config.targeting.enabled_classes = {true, true, false, false};
    config.targeting.class_location = {0.5F, 0.5F, 0.0F, 0.0F};

    aim::DetectionFrame frame;
    frame.sequence = 1;
    frame.timestamp = aim::Clock::now();
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 320;
    frame.screen_height = 320;
    frame.screen_center_x = 160;
    frame.screen_center_y = 160;
    frame.detections = {
        {aim::Box::fromCenter(160.0F, 160.0F, 30.0F, 30.0F), 0.9F, 0},
        {aim::Box::fromCenter(180.0F, 160.0F, 40.0F, 80.0F), 0.9F, 1},
    };

    aim::CombatContext combat;
    combat.weapon = "mini14";
    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    activation.lead_boost = true;

    aim::AimEngine forced_engine(config);
    CHECK(forced_engine.process(frame, combat, activation).snapshot.target_class == 1);
    activation.lead_boost = false;
    aim::AimEngine normal_engine(config);
    CHECK(normal_engine.process(frame, combat, activation).snapshot.target_class == 0);
}

void testWeaponsScopesAndDeadzone() {
    CHECK(aim::canonicalWeaponName("berry") == "m762");
    CHECK(aim::canonicalWeaponName("SCAR-L") == "scarl");
    CHECK(aim::canonicalWeaponName("kar98") == "kar98k");
    CHECK(aim::canonicalWeaponName("micro-uzi") == "uzi");
    CHECK(aim::isDmrWeapon("mini14"));
    CHECK(aim::isDmrWeapon("mk14"));
    CHECK(aim::isDmrWeapon("vss"));
    CHECK(aim::isDmrWeapon("dragunov"));
    CHECK(aim::isAutomaticWeapon("rpd"));
    CHECK(aim::isSmgWeapon("ump45"));

    const auto config = aim::AimConfig::legacyDefaults();
    CHECK(!config.auto_fire.enabled);
    NEAR(aim::resolveWeaponLeadScale("m762", config.prediction), 1.4F, 1e-6F);
    NEAR(aim::resolveWeaponLeadScale("scarl", config.prediction), 0.2F, 1e-6F);
    NEAR(aim::resolveWeaponLeadScale("vss", config.prediction), 0.05F, 1e-6F);
    NEAR(aim::resolveWeaponLeadScale("rpd", config.prediction), 1.2F, 1e-6F);
    NEAR(aim::resolveManualWeaponScale("m762", config.prediction), 1.25F, 1e-6F);
    NEAR(aim::resolveManualWeaponScale("vss", config.prediction), 0.1F, 1e-6F);
    NEAR(aim::resolveManualWeaponScale("rpd", config.prediction), 1.25F, 1e-6F);

    CHECK(aim::canonicalScopeName("holographic") == "1x");
    CHECK(aim::canonicalScopeName("6x") == "6x");
    NEAR(aim::scopePredictionZoom("1x"), 1.0F, 1e-6F);
    NEAR(aim::scopePredictionZoom("2x"), 2.0F, 1e-6F);
    NEAR(aim::scopePredictionZoom("6x"), 3.0F, 1e-6F);
    NEAR(aim::scopePredictionZoom("8x"), 4.0F, 1e-6F);
    NEAR(aim::fireDistanceScale(170.0F, config.fire_offset), 0.8F, 1e-6F);
    NEAR(aim::fireDistanceScale(42.5F, config.fire_offset), 1.25F, 1e-6F);
    NEAR(aim::scopeSensitivity(
        "6x", config.controller, aim::CompatibilityMode::Original), 42.0F, 1e-6F);
    NEAR(aim::scopeSensitivity(
        "6x", config.controller, aim::CompatibilityMode::Corrected), 44.0F, 1e-6F);
    NEAR(aim::scopeSensitivity(
        "8x", config.controller, aim::CompatibilityMode::Original), 41.0F, 1e-6F);
    NEAR(aim::scopeSensitivity(
        "8x", config.controller, aim::CompatibilityMode::Corrected), 50.0F, 1e-6F);
    const float expected_gain = std::pow(30.0F / 42.0F, 1.36F) / 3.0F;
    NEAR(aim::controllerPidGainScale(
        "6x", 3.0F, config.controller, aim::CompatibilityMode::Original, 1.0F),
        expected_gain,
        1e-6F);

    NEAR(aim::softDeadzone(5.0F, 2.0F), 3.0F, 1e-6F);
    NEAR(aim::softDeadzone(-5.0F, 2.0F), -3.0F, 1e-6F);
    NEAR(aim::softDeadzone(2.0F, 2.0F), 0.0F, 1e-6F);

    auto unsafe = aim::AimConfig::correctedDefaults();
    unsafe.prediction.weapon_scales["m762"] = std::numeric_limits<float>::quiet_NaN();
    unsafe.prediction.manual_weapon_scales["m762"] =
        std::numeric_limits<float>::infinity();
    unsafe.compatibility = static_cast<aim::CompatibilityMode>(99);
    unsafe.auto_fire.mode = static_cast<aim::AutoFireMode>(99);
    unsafe.tracking.max_frame_age_ms = -1;
    unsafe.sanitize();
    CHECK(std::isfinite(unsafe.prediction.weapon_scales["m762"]));
    CHECK(std::isfinite(unsafe.prediction.manual_weapon_scales["m762"]));
    CHECK(unsafe.compatibility == aim::CompatibilityMode::Corrected);
    CHECK(unsafe.auto_fire.mode == aim::AutoFireMode::MoveOnly);
    CHECK(unsafe.tracking.max_frame_age_ms == 1);
}

void testFractionalMotion() {
    aim::FractionalMotionAccumulator accumulator;
    aim::MotionCommand input;
    input.dx = 0.25;
    const double expected[]{0.0, 1.0, 0.0, 0.0};
    for (double value : expected) {
        const auto output = accumulator.consume(input);
        CHECK(output.dx == value);
    }
    accumulator.reset();
    input.dx = -0.25;
    const double negative_expected[]{0.0, -1.0, 0.0, 0.0};
    for (double value : negative_expected) {
        const auto output = accumulator.consume(input);
        CHECK(output.dx == value);
    }
    accumulator.reset();
    input.dx = 0.5;
    CHECK(accumulator.consume(input).dx == 1.0);
    accumulator.reset();
    input.dx = -0.5;
    CHECK(accumulator.consume(input).dx == -1.0);
}

class RecordingSink final : public aim::IMotionSink {
public:
    void submitAimMotion(const aim::MotionCommand& command) override {
        {
            std::scoped_lock lock(mutex_);
            commands_.push_back(command);
        }
        cv_.notify_all();
    }

    bool waitForCount(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return commands_.size() >= count; });
    }

    std::size_t count() const {
        std::scoped_lock lock(mutex_);
        return commands_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<aim::MotionCommand> commands_;
};

class ReentrantSink final : public aim::IMotionSink {
public:
    enum class Action { Disable, Stop };

    explicit ReentrantSink(Action action) : action_(action) {}

    void bind(aim::AimModule& module) noexcept { module_ = &module; }

    void submitAimMotion(const aim::MotionCommand&) override {
        const std::size_t current = count_.fetch_add(1) + 1;
        if (current == 1 && module_) {
            if (action_ == Action::Disable) {
                module_->setEnabled(false);
            } else {
                module_->stop();
            }
        }
        cv_.notify_all();
    }

    bool waitForCount(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return count_.load() >= count; });
    }

    std::size_t count() const noexcept { return count_.load(); }

private:
    Action action_;
    aim::AimModule* module_ = nullptr;
    std::atomic<std::size_t> count_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

class BlockingSink final : public aim::IMotionSink {
public:
    void submitAimMotion(const aim::MotionCommand&) override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return released_; });
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return entered_; });
    }

    void release() {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

aim::DetectionFrame makeModuleFrame(std::uint64_t sequence) {
    aim::DetectionFrame frame;
    frame.sequence = sequence;
    frame.timestamp = aim::Clock::now();
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 320;
    frame.screen_height = 320;
    frame.screen_center_x = 160;
    frame.screen_center_y = 160;
    frame.detections.push_back({aim::Box::fromCenter(180.0F, 160.0F, 40.0F, 80.0F), 0.9F, 1});
    return frame;
}

struct InternalProbeState {
    std::atomic<int> capture_calls{0};
    std::atomic<int> infer_calls{0};
    std::atomic<int> config_updates{0};
    std::atomic<int> pending_frames{0};
    std::atomic<bool> continuous_frames{false};
    std::atomic<bool> throw_from_infer{false};
    std::atomic<bool> block_capture{false};
    std::mutex config_mutex;
    aim::DetectorConfig decoder{};
    aim::CompatibilityMode compatibility = aim::CompatibilityMode::Corrected;
    std::mutex capture_mutex;
    std::condition_variable capture_cv;
    bool capture_entered = false;
    bool release_capture = false;
};

class ProbeFrameSource final : public aim::IFrameSource {
public:
    explicit ProbeFrameSource(std::shared_ptr<InternalProbeState> state)
        : state_(std::move(state)) {}

    aim::CaptureStatus capture(aim::Frame& frame, std::stop_token stop) override {
        state_->capture_calls.fetch_add(1);
        if (stop.stop_requested()) {
            return aim::CaptureStatus::Stopped;
        }

        bool emit = state_->continuous_frames.load();
        int pending = state_->pending_frames.load();
        while (!emit && pending > 0) {
            if (state_->pending_frames.compare_exchange_weak(pending, pending - 1)) {
                emit = true;
            }
        }
        if (!emit) {
            return aim::CaptureStatus::NoFrame;
        }
        if (state_->block_capture.load()) {
            std::unique_lock lock(state_->capture_mutex);
            state_->capture_entered = true;
            state_->capture_cv.notify_all();
            state_->capture_cv.wait(lock, [&] {
                return state_->release_capture || stop.stop_requested();
            });
            if (stop.stop_requested()) {
                return aim::CaptureStatus::Stopped;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        frame.sequence = ++sequence_;
        frame.timestamp = aim::Clock::now();
        frame.width = 320;
        frame.height = 320;
        frame.stride = 320 * 4;
        frame.screen_width = 320;
        frame.screen_height = 320;
        frame.screen_center_x = 160;
        frame.screen_center_y = 160;
        return aim::CaptureStatus::FrameReady;
    }

private:
    std::shared_ptr<InternalProbeState> state_;
    std::uint64_t sequence_ = 0;
};

class ProbeDetector final : public aim::IObjectDetector {
public:
    explicit ProbeDetector(std::shared_ptr<InternalProbeState> state)
        : state_(std::move(state)) {}

    std::vector<aim::Detection> infer(
        const aim::Frame&,
        std::stop_token) override {
        state_->infer_calls.fetch_add(1);
        if (state_->throw_from_infer.load()) {
            throw std::runtime_error("probe inference failure");
        }
        return {};
    }

    bool updateDecoderConfig(
        const aim::DetectorConfig& config,
        aim::CompatibilityMode compatibility) override {
        {
            std::scoped_lock lock(state_->config_mutex);
            state_->decoder = config;
            state_->compatibility = compatibility;
        }
        state_->config_updates.fetch_add(1);
        return true;
    }

    std::string_view backendName() const noexcept override { return "probe"; }

private:
    std::shared_ptr<InternalProbeState> state_;
};

void testAimModuleExternalMode() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;
    config.controller.fov_enabled = false;
    config.controller.pid_enabled = false;
    config.controller.global_scale_enabled = false;
    config.targeting.class_location[1] = 0.5F;

    auto sink = std::make_shared<RecordingSink>();
    aim::AimModule module(config, sink);
    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    module.updateActivation(activation);

    CHECK(module.start());
    CHECK(!module.start());
    CHECK(module.submitDetections(makeModuleFrame(1)));
    CHECK(sink->waitForCount(1, std::chrono::milliseconds(1000)));
    CHECK(!module.submitDetections(makeModuleFrame(1)));
    auto missing_timestamp = makeModuleFrame(2);
    missing_timestamp.timestamp = {};
    CHECK(!module.submitDetections(missing_timestamp));

    auto non_monotonic = makeModuleFrame(2);
    non_monotonic.timestamp = aim::Clock::now() - std::chrono::seconds(1);
    CHECK(!module.submitDetections(non_monotonic));
    auto future = makeModuleFrame(2);
    future.timestamp = aim::Clock::now() + std::chrono::seconds(1);
    CHECK(!module.submitDetections(future));

    auto valid = makeModuleFrame(2);
    const auto valid_timestamp = valid.timestamp;
    CHECK(module.submitDetections(valid));
    CHECK(sink->waitForCount(2, std::chrono::milliseconds(1000)));
    auto repeated_timestamp = makeModuleFrame(3);
    repeated_timestamp.timestamp = valid_timestamp;
    CHECK(!module.submitDetections(repeated_timestamp));

    activation.paused = true;
    module.updateActivation(activation);
    CHECK(!module.submitDetections(makeModuleFrame(3)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sink->count() == 2);
    module.stop();
    CHECK(!module.snapshot().running);
    CHECK(!module.submitDetections(makeModuleFrame(3)));
}

aim::ActivationState enabledActivation() {
    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    return activation;
}

aim::AimConfig moduleTestConfig() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;
    config.controller.fov_enabled = false;
    config.controller.pid_enabled = false;
    config.controller.global_scale_enabled = false;
    config.targeting.class_location[1] = 0.5F;
    return config;
}

void testAimModuleReentrantDisable() {
    auto sink = std::make_shared<ReentrantSink>(ReentrantSink::Action::Disable);
    aim::AimModule module(moduleTestConfig(), sink);
    sink->bind(module);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(module.submitDetections(makeModuleFrame(1)));
    CHECK(sink->waitForCount(1, std::chrono::milliseconds(1000)));
    CHECK(!module.submitDetections(makeModuleFrame(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sink->count() == 1);
    module.stop();
}

void testInternalWorkIsActivationGated() {
    auto state = std::make_shared<InternalProbeState>();
    auto sink = std::make_shared<RecordingSink>();
    aim::AimModule module(
        moduleTestConfig(),
        std::make_unique<ProbeFrameSource>(state),
        std::make_unique<ProbeDetector>(state),
        sink);
    CHECK(module.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(state->capture_calls.load() == 0);
    CHECK(state->infer_calls.load() == 0);

    aim::ActivationState activation = enabledActivation();
    module.updateActivation(activation);
    CHECK(waitUntil(
        [&] { return state->capture_calls.load() >= 2; },
        std::chrono::milliseconds(500)));

    activation.blocking_ui_open = true;
    module.updateActivation(activation);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const int ui_blocked_count = state->capture_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(state->capture_calls.load() == ui_blocked_count);

    activation.blocking_ui_open = false;
    activation.game_foreground = false;
    module.updateActivation(activation);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const int background_count = state->capture_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(state->capture_calls.load() == background_count);

    activation.game_foreground = true;
    module.updateActivation(activation);
    CHECK(waitUntil(
        [&] { return state->capture_calls.load() > background_count; },
        std::chrono::milliseconds(500)));
    module.setEnabled(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const int disabled_count = state->capture_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(state->capture_calls.load() == disabled_count);
    module.stop();
}

void testDetectorConfigSyncAndBackendStatus() {
    auto config = moduleTestConfig();
    config.detector.input_width = 352;
    config.detector.confidence = 0.61F;
    auto state = std::make_shared<InternalProbeState>();
    state->pending_frames.store(1);
    auto sink = std::make_shared<RecordingSink>();
    aim::AimModule module(
        config,
        std::make_unique<ProbeFrameSource>(state),
        std::make_unique<ProbeDetector>(state),
        sink);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(waitUntil(
        [&] {
            const auto snapshot = module.snapshot();
            return state->infer_calls.load() >= 1 && snapshot.model_ready;
        },
        std::chrono::milliseconds(1000)));
    CHECK(module.snapshot().detector_backend == "probe");
    CHECK(state->config_updates.load() >= 1);
    {
        std::scoped_lock lock(state->config_mutex);
        CHECK(state->decoder.input_width == 352);
        NEAR(state->decoder.confidence, 0.61F, 1e-6F);
        CHECK(state->compatibility == aim::CompatibilityMode::Corrected);
    }

    config.detector.input_width = 640;
    config.detector.confidence = 0.73F;
    config.compatibility = aim::CompatibilityMode::Original;
    const int previous_updates = state->config_updates.load();
    module.updateConfig(config);
    CHECK(waitUntil(
        [&] { return state->config_updates.load() > previous_updates; },
        std::chrono::milliseconds(1000)));
    {
        std::scoped_lock lock(state->config_mutex);
        CHECK(state->decoder.input_width == 640);
        NEAR(state->decoder.confidence, 0.73F, 1e-6F);
        CHECK(state->compatibility == aim::CompatibilityMode::Original);
    }
    module.stop();
}

void testGateClosingDuringCapturePreventsInference() {
    auto state = std::make_shared<InternalProbeState>();
    state->pending_frames.store(1);
    state->block_capture.store(true);
    auto sink = std::make_shared<RecordingSink>();
    aim::AimModule module(
        moduleTestConfig(),
        std::make_unique<ProbeFrameSource>(state),
        std::make_unique<ProbeDetector>(state),
        sink);
    aim::ActivationState activation = enabledActivation();
    module.updateActivation(activation);
    CHECK(module.start());
    {
        std::unique_lock lock(state->capture_mutex);
        CHECK(state->capture_cv.wait_for(
            lock,
            std::chrono::milliseconds(1000),
            [&] { return state->capture_entered; }));
    }

    activation.blocking_ui_open = true;
    module.updateActivation(activation);
    {
        std::scoped_lock lock(state->capture_mutex);
        state->release_capture = true;
    }
    state->capture_cv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(state->infer_calls.load() == 0);
    module.stop();
}

void testWorkerErrorBackoffAndRecovery() {
    auto state = std::make_shared<InternalProbeState>();
    state->continuous_frames.store(true);
    state->throw_from_infer.store(true);
    auto sink = std::make_shared<RecordingSink>();
    auto config = moduleTestConfig();
    aim::AimModule module(
        config,
        std::make_unique<ProbeFrameSource>(state),
        std::make_unique<ProbeDetector>(state),
        sink);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(waitUntil(
        [&] { return state->infer_calls.load() >= 3; },
        std::chrono::milliseconds(1000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int failed_inferences = state->infer_calls.load();
    CHECK(failed_inferences < 10);
    CHECK(!module.snapshot().model_ready);
    CHECK(module.snapshot().last_error == "probe inference failure");

    state->throw_from_infer.store(false);
    module.updateConfig(config);
    CHECK(waitUntil(
        [&] {
            const auto snapshot = module.snapshot();
            return snapshot.model_ready && snapshot.last_error.empty();
        },
        std::chrono::milliseconds(1000)));
    CHECK(module.snapshot().detector_backend == "probe");
    module.stop();
}

void testAimModuleReentrantStopAndRestart() {
    auto sink = std::make_shared<ReentrantSink>(ReentrantSink::Action::Stop);
    aim::AimModule module(moduleTestConfig(), sink);
    sink->bind(module);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(module.submitDetections(makeModuleFrame(1)));
    CHECK(sink->waitForCount(1, std::chrono::milliseconds(1000)));
    for (int attempt = 0; attempt < 100 && module.snapshot().running; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!module.snapshot().running);

    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(module.submitDetections(makeModuleFrame(1)));
    CHECK(sink->waitForCount(2, std::chrono::milliseconds(1000)));
    module.stop();
}

void testDetectionSnapshotsClearOnConfigurationAndGates() {
    auto sink = std::make_shared<RecordingSink>();
    auto config = moduleTestConfig();
    aim::AimModule module(config, sink);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    std::uint64_t sequence = 0;
    const auto feed = [&] {
        CHECK(waitUntil([&] {
            sequence += 1000; // Capture sequence gaps are not processed-frame counts.
            auto frame = makeModuleFrame(sequence);
            frame.roi_left = -1200;
            frame.roi_top = 400;
            (void)module.submitDetections(std::move(frame));
            const auto snapshot = module.snapshot();
            return snapshot.detection_frame && snapshot.recognition_hz > 0.0;
        }, std::chrono::milliseconds(2000)));
        CHECK(module.snapshot().recognition_hz < 1000.0);
    };
    feed();
    const auto retained = module.snapshot().detection_frame;
    CHECK(retained && retained->roi_left == -1200 && !retained->detections.empty());
    module.updateConfig(config);
    CHECK(!module.snapshot().detection_frame);
    CHECK(module.snapshot().recognition_hz == 0.0);
    // Previously acquired frames remain immutable/valid after runtime reset.
    CHECK(retained && !retained->detections.empty());
    feed();
    module.resetTracking();
    CHECK(!module.snapshot().detection_frame);
    CHECK(module.snapshot().recognition_hz == 0.0);
    feed();
    auto blocked = enabledActivation();
    blocked.blocking_ui_open = true;
    module.updateActivation(blocked);
    CHECK(!module.snapshot().detection_frame);
    CHECK(module.snapshot().recognition_hz == 0.0);
    module.updateActivation(enabledActivation());
    feed();
    module.updateCombatContext({});
    CHECK(!module.snapshot().detection_frame);
    feed();
    module.setEnabled(false);
    CHECK(!module.snapshot().detection_frame);
    CHECK(module.snapshot().recognition_hz == 0.0);
    module.stop();
    CHECK(!module.snapshot().detection_frame);
    CHECK(module.snapshot().recognition_hz == 0.0);
}

void testInternalRecognitionFrequencyWithoutTargetsAndAfterStall() {
    using namespace std::chrono_literals;
    auto state = std::make_shared<InternalProbeState>();
    auto sink = std::make_shared<RecordingSink>();
    auto config = moduleTestConfig();
    config.target_fps = 1; // The measurement must not echo this setting.
    aim::AimModule module(config, std::make_unique<ProbeFrameSource>(state),
                          std::make_unique<ProbeDetector>(state), sink);
    module.updateActivation(enabledActivation());
    state->continuous_frames = true;
    CHECK(module.start());
    CHECK(waitUntil([&] { return module.snapshot().recognition_hz > 1.0; }, 2s));
    const auto snapshot = module.snapshot();
    CHECK(snapshot.detection_frame && snapshot.detection_frame->detections.empty());
    CHECK(!snapshot.target_locked);
    state->continuous_frames = false;
    CHECK(waitUntil([&] { return module.snapshot().recognition_hz == 0.0; }, 2s));
    state->continuous_frames = true;
    CHECK(waitUntil([&] { return module.snapshot().recognition_hz > 1.0; }, 2s));
    state->throw_from_infer = true;
    CHECK(waitUntil([&] { return !module.snapshot().last_error.empty(); }, 1s));
    CHECK(module.snapshot().recognition_hz == 0.0);
    module.stop();
    state->continuous_frames = false;
    state->throw_from_infer = false;
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(module.snapshot().recognition_hz == 0.0);
    module.stop();
}

void testAimModuleForeignDisableFence() {
    auto sink = std::make_shared<BlockingSink>();
    aim::AimModule module(moduleTestConfig(), sink);
    module.updateActivation(enabledActivation());
    CHECK(module.start());
    CHECK(module.submitDetections(makeModuleFrame(1)));
    CHECK(sink->waitUntilEntered(std::chrono::milliseconds(1000)));

    std::atomic<bool> disable_returned = false;
    std::jthread disable_thread([&] {
        module.setEnabled(false);
        disable_returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!disable_returned.load());
    sink->release();
    disable_thread.join();
    CHECK(disable_returned.load());
    module.stop();
}

} // namespace

int main() {
    testRecognitionFrequencyMeasurement();
    testActivationAllowsProcessing();
    testIoU();
    testDecoderLayoutAndRounding();
    testNmsModes();
    testTargetSelectionAndAssociation();
    testAimPointFovAndPid();
    testEngineUsesFullScreenForFov();
    testDmrLeadBoostForcesBodyClass();
    testWeaponsScopesAndDeadzone();
    testFractionalMotion();
    testAimModuleExternalMode();
    testAimModuleReentrantDisable();
    testAimModuleReentrantStopAndRestart();
    testAimModuleForeignDisableFence();
    testDetectionSnapshotsClearOnConfigurationAndGates();
    testInternalRecognitionFrequencyWithoutTargetsAndAfterStall();
    testInternalWorkIsActivationGated();
    testDetectorConfigSyncAndBackendStatus();
    testGateClosingDuringCapturePreventsInference();
    testWorkerErrorBackoffAndRecovery();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "pubg_ai_aim core tests passed\n";
    return EXIT_SUCCESS;
}
