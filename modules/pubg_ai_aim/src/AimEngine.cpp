#include "pubg_ai_aim/AimEngine.hpp"

#include "pubg_ai_aim/Algorithms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <random>
#include <utility>

namespace pubg::ai_aim {
namespace {

float clampAxis(float value, float maximum) noexcept {
    return std::clamp(value, -maximum, maximum);
}

float length(Vec2 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y);
}

float lerp(float previous, float current, float alpha) noexcept {
    return previous + (current - previous) * alpha;
}

Vec2 lerp(Vec2 previous, Vec2 current, float alpha) noexcept {
    return {lerp(previous.x, current.x, alpha), lerp(previous.y, current.y, alpha)};
}

float classValue(const std::array<float, 4>& values, int class_id, float fallback) noexcept {
    if (class_id < 0 || class_id >= static_cast<int>(values.size())) {
        return fallback;
    }
    return values[static_cast<std::size_t>(class_id)];
}

} // namespace

class AimEngine::Impl {
public:
    explicit Impl(AimConfig config) : config_(std::move(config)) {
        config_.sanitize();
    }

    void updateConfig(AimConfig config) {
        config.sanitize();
        config_ = std::move(config);
        reset();
    }

    void reset() noexcept {
        previous_ = {};
        lost_frames_ = 0;
        track_age_ = 0;
        have_previous_aim_ = false;
        previous_absolute_aim_ = {};
        filtered_position_ = {};
        velocity_ = {};
        previous_lead_ = {};
        held_velocity_ = {};
        motion_hold_frames_ = 0;
        previous_timestamp_ = {};
        pid_x_ = {};
        pid_y_ = {};
        next_click_at_ = {};
    }

    AimDecision process(
        const DetectionFrame& frame,
        const CombatContext& combat,
        const ActivationState& activation) {
        AimDecision decision;
        decision.snapshot.frame_sequence = frame.sequence;

        if (!activationAllowsProcessing(activation)) {
            reset();
            return decision;
        }
        if (frame.roi_width <= 0 || frame.roi_height <= 0 ||
            (config_.controller.fov_enabled &&
             (frame.screen_width <= 0 || frame.screen_height <= 0))) {
            reset();
            decision.snapshot.last_error =
                "invalid frame geometry: full screen dimensions are required for FOV conversion";
            return decision;
        }

        const std::string weapon = canonicalWeaponName(combat.weapon);
        const std::string scope = canonicalScopeName(combat.scope);
        const bool force_body = config_.targeting.model_classes &&
            config_.prediction.lead_boost_hotkey_enabled && activation.lead_boost &&
            isDmrWeapon(weapon);
        const auto selected = selectTarget(
            frame.detections, frame, config_.targeting, config_.tracking, previous_, force_body);

        if (!selected.has_value()) {
            ++lost_frames_;
            if (lost_frames_ > config_.tracking.max_lost_frames) {
                reset();
            }
            return decision;
        }

        const Detection& target = frame.detections[*selected];
        Vec2 target_center = target.box.center();
        Vec2 absolute_center{
            target_center.x + static_cast<float>(frame.roi_left),
            target_center.y + static_cast<float>(frame.roi_top),
        };

        if (previous_.valid && !associationPasses(
                target, absolute_center, previous_, config_.targeting, config_.tracking)) {
            reset();
        }

        lost_frames_ = 0;
        track_age_ = std::min(track_age_ + 1, 1000);

        Vec2 aim = classAimPoint(target, config_.targeting);
        if (config_.targeting.advanced) {
            const float width = std::max(target.box.width(), 1.0F);
            const float height = std::max(target.box.height(), 1.0F);
            const float aspect = width / height;
            const float lean_ratio = std::clamp((aspect - 0.44F) / 0.22F, 0.0F, 1.0F);
            const float absolute_target_x = aim.x + static_cast<float>(frame.roi_left);
            const float direction = absolute_target_x >= static_cast<float>(frame.screen_center_x)
                ? 1.0F
                : -1.0F;
            aim.x += direction * width * config_.targeting.advanced_multiple *
                     config_.targeting.lean_x_adjust * lean_ratio;
        }

        if (combat.firing && config_.fire_offset.enabled && !isDmrWeapon(weapon)) {
            float offset = classValue(
                config_.fire_offset.class_offset,
                target.class_id,
                config_.fire_offset.default_offset);
            offset *= scopeFireScale(scope, config_.fire_offset);
            if (config_.fire_offset.distance_scale_enabled) {
                offset *= fireDistanceScale(target.box.height(), config_.fire_offset);
            }
            if (combat.lean_left || combat.lean_right) {
                offset *= config_.fire_offset.lean_scale;
            }
            if (isSmgWeapon(weapon)) {
                offset *= config_.fire_offset.smg_scale;
            }
            aim.y += offset;
        }

        const Vec2 absolute_aim{
            aim.x + static_cast<float>(frame.roi_left),
            aim.y + static_cast<float>(frame.roi_top),
        };
        const Vec2 screen_center{
            static_cast<float>(frame.screen_center_x),
            static_cast<float>(frame.screen_center_y),
        };

        const float global_scale = config_.controller.global_scale_enabled
            ? config_.controller.global_scale
            : 1.0F;
        const float sqrt_scale = std::sqrt(global_scale);
        const float inverse_sqrt_scale = 1.0F / sqrt_scale;
        const float relative_height = target.box.height() /
            static_cast<float>(std::max(frame.roi_height, 1));
        const float small = std::clamp((0.28F - relative_height) / 0.22F, 0.0F, 1.0F);

        Vec2 lead{};
        const bool legacy_motion_reference =
            config_.compatibility == CompatibilityMode::Original;
        const Vec2 motion_reference = legacy_motion_reference
            ? absolute_aim
            : absolute_center;
        const Vec2 aim_offset = legacy_motion_reference
            ? Vec2{}
            : absolute_aim - absolute_center;
        if (config_.prediction.enabled) {
            updatePrediction(frame, motion_reference, target, weapon, scope, activation,
                             small, global_scale, sqrt_scale, inverse_sqrt_scale, lead);
        } else {
            filtered_position_ = motion_reference;
            velocity_ = {};
            previous_lead_ = {};
            lead = {};
        }

        Vec2 error = (config_.prediction.enabled ? filtered_position_ : motion_reference) +
                     aim_offset + lead - screen_center;
        decision.snapshot.raw_error = error;

        const float effective_range = config_.targeting.effective_range * global_scale * 0.5F;
        if (std::abs(error.x) > effective_range || std::abs(error.y) > effective_range) {
            updateTrackingReference(target, absolute_center);
            fillSnapshot(decision.snapshot, target, absolute_aim, lead, false);
            return decision;
        }

        if (config_.controller.fov_enabled) {
            error.x = fovConvertAxis(
                error.x,
                std::min(config_.controller.horizontal_fov * global_scale, 179.0F),
                config_.controller.game_x_pixels,
                frame.screen_width,
                360.0F);
            error.y = fovConvertAxis(
                error.y,
                std::min(config_.controller.vertical_fov * global_scale, 179.0F),
                config_.controller.game_y_pixels,
                frame.screen_height,
                180.0F);
        }

        if (config_.controller.pid_enabled) {
            const float pid_gain_scale = controllerPidGainScale(
                scope,
                combat.scope_zoom,
                config_.controller,
                config_.compatibility,
                global_scale);
            error.x = legacyPidStep(
                pid_x_, error.x,
                config_.controller.p_x * pid_gain_scale,
                config_.controller.i_x * pid_gain_scale,
                config_.controller.d_x * pid_gain_scale);
            if (config_.controller.fire_no_y_pid && combat.firing) {
                error.y = 0.0F;
            } else {
                error.y = legacyPidStep(
                    pid_y_, error.y,
                    config_.controller.p_y * pid_gain_scale,
                    config_.controller.i_y * pid_gain_scale,
                    config_.controller.d_y * pid_gain_scale);
            }
        } else {
            error *= global_scale;
        }

        const float max_pixels = std::max(config_.controller.max_pixels * global_scale, 1.0F);
        error.x = clampAxis(error.x, max_pixels);
        error.y = clampAxis(error.y, max_pixels);

        if (config_.controller.debounce_enabled) {
            const float debounce = config_.controller.debounce_pixels * inverse_sqrt_scale;
            if (std::abs(error.x) < debounce && std::abs(error.y) < debounce) {
                updateTrackingReference(target, absolute_center);
                fillSnapshot(decision.snapshot, target, absolute_aim, lead, true);
                return decision;
            }
        }

        static constexpr std::array<float, 5> acquisition_gain{0.0F, 0.45F, 0.65F, 0.82F, 1.0F};
        const float gain = acquisition_gain[
            static_cast<std::size_t>(std::clamp(track_age_, 0, 4))];
        error *= gain;

        if (!std::isfinite(error.x) || !std::isfinite(error.y)) {
            reset();
            decision.snapshot.last_error = "non-finite motion was rejected";
            return decision;
        }

        const bool trigger_inside =
            std::abs(decision.snapshot.raw_error.x) <
                target.box.width() * 0.5F * config_.auto_fire.trigger_width &&
            std::abs(decision.snapshot.raw_error.y) <
                target.box.height() * 0.5F * config_.auto_fire.trigger_height;

        const bool trigger_only =
            config_.auto_fire.enabled && config_.auto_fire.mode == AutoFireMode::TriggerOnly;
        if (!trigger_only && (error.x != 0.0F || error.y != 0.0F)) {
            decision.has_motion = true;
            decision.motion = {
                static_cast<double>(error.x),
                static_cast<double>(error.y),
                frame.sequence,
                frame.timestamp,
            };
        }

        if (config_.auto_fire.enabled &&
            config_.auto_fire.mode != AutoFireMode::MoveOnly && trigger_inside && clickReady(frame.timestamp)) {
            decision.request_click = true;
            scheduleNextClick(frame.timestamp);
        }

        updateTrackingReference(target, absolute_center);
        fillSnapshot(decision.snapshot, target, absolute_aim, lead, true);
        return decision;
    }

private:
    void updatePrediction(
        const DetectionFrame& frame,
        Vec2 absolute_aim,
        const Detection& target,
        std::string_view weapon,
        std::string_view scope,
        const ActivationState& activation,
        float small,
        float global_scale,
        float sqrt_scale,
        float inverse_sqrt_scale,
        Vec2& lead) {
        Vec2 raw_delta{};
        if (have_previous_aim_) {
            raw_delta = absolute_aim - previous_absolute_aim_;
            if (config_.compatibility == CompatibilityMode::Corrected &&
                previous_timestamp_ != Clock::time_point{} && frame.timestamp > previous_timestamp_) {
                const float elapsed = std::chrono::duration<float>(
                    frame.timestamp - previous_timestamp_).count();
                const float nominal = 1.0F / static_cast<float>(std::max(config_.target_fps, 1));
                const float normalization = std::clamp(nominal / std::max(elapsed, 0.0001F), 0.25F, 4.0F);
                raw_delta *= normalization;
            }
        }

        if (!have_previous_aim_ || length(raw_delta) > config_.prediction.jump_reset_pixels) {
            raw_delta = {};
            velocity_ = {};
            held_velocity_ = {};
            motion_hold_frames_ = 0;
            filtered_position_ = absolute_aim;
        } else {
            const float position_alpha = std::clamp(0.82F - 0.27F * small, 0.55F, 0.82F);
            filtered_position_ = lerp(filtered_position_, absolute_aim, position_alpha);
            const float velocity_alpha = std::clamp(0.35F - 0.23F * small, 0.08F, 0.35F);
            velocity_ = lerp(velocity_, raw_delta, velocity_alpha);
        }

        float base_deadzone = config_.prediction.velocity_deadzone;
        float predict_factor = config_.prediction.predict_factor;
        if (config_.prediction.lead_boost_hotkey_enabled) {
            if (activation.lead_boost) {
                base_deadzone = config_.prediction.lead_boost_velocity_deadzone;
                predict_factor = config_.prediction.lead_boost_predict_factor;
            } else {
                base_deadzone = config_.prediction.normal_velocity_deadzone;
                predict_factor = config_.prediction.normal_predict_factor;
            }
        }
        base_deadzone *= inverse_sqrt_scale;
        const float dynamic_deadzone = std::clamp(
            (1.0F + small * config_.prediction.small_target_deadzone_scale) * base_deadzone,
            0.0F,
            30.0F);

        Vec2 usable_velocity{
            softDeadzone(velocity_.x, dynamic_deadzone),
            softDeadzone(velocity_.y, dynamic_deadzone),
        };
        if (usable_velocity.x == 0.0F && usable_velocity.y == 0.0F) {
            if (motion_hold_frames_ < config_.prediction.lead_hold_frames) {
                ++motion_hold_frames_;
                usable_velocity = held_velocity_;
            } else {
                held_velocity_ = {};
            }
        } else {
            held_velocity_ = usable_velocity;
            motion_hold_frames_ = 0;
        }

        const float predict_factor_limit =
            config_.prediction.lead_boost_hotkey_enabled && activation.lead_boost
            ? 30.0F
            : 20.0F;
        predict_factor = std::clamp(
            predict_factor * inverse_sqrt_scale, 0.0F, predict_factor_limit);
        float scale = predict_factor;
        scale *= resolveWeaponLeadScale(weapon, config_.prediction);
        scale *= scopePredictionZoom(scope);

        const float target_size_scale = std::clamp(
            config_.prediction.small_target_base_height /
                std::max(target.box.height(), 1.0F),
            1.0F,
            config_.prediction.small_target_max_scale);
        scale *= 1.0F + (target_size_scale - 1.0F) *
                          config_.prediction.small_target_predict_scale;

        if (config_.prediction.dmr_boost_enabled && isDmrWeapon(weapon)) {
            scale *= config_.prediction.dmr_scale;
        } else if (config_.prediction.automatic_boost_enabled && isAutomaticWeapon(weapon)) {
            scale *= config_.prediction.automatic_scale;
        }
        if (config_.prediction.manual_scale_enabled && activation.manual_lead) {
            scale *= resolveManualWeaponScale(weapon, config_.prediction);
        }

        Vec2 desired_lead{
            usable_velocity.x * scale,
            usable_velocity.y * scale * config_.prediction.lead_y_scale,
        };
        desired_lead.x = clampAxis(desired_lead.x, config_.prediction.max_lead_pixels);
        desired_lead.y = clampAxis(desired_lead.y, config_.prediction.max_lead_pixels);

        const float smoothing = std::clamp(
            1.0F - (1.0F - config_.prediction.lead_smooth) * sqrt_scale,
            0.0F,
            0.95F);
        Vec2 smoothed = desired_lead * (1.0F - smoothing) + previous_lead_ * smoothing;
        const float max_step = std::clamp(
            config_.prediction.lead_step * global_scale, 0.0F, 60.0F);
        smoothed.x = previous_lead_.x + clampAxis(smoothed.x - previous_lead_.x, max_step);
        smoothed.y = previous_lead_.y + clampAxis(smoothed.y - previous_lead_.y, max_step);

        previous_lead_ = smoothed;
        lead = smoothed;
        previous_absolute_aim_ = absolute_aim;
        previous_timestamp_ = frame.timestamp;
        have_previous_aim_ = true;
    }

    bool clickReady(Clock::time_point now) {
        return now != Clock::time_point{} && now >= next_click_at_;
    }

    void scheduleNextClick(Clock::time_point now) {
        const int extra = config_.auto_fire.random_ms > 0
            ? random_(engine_) % (config_.auto_fire.random_ms + 1)
            : 0;
        const auto interval = std::chrono::milliseconds(config_.auto_fire.interval_ms + extra);
        next_click_at_ = now + interval;
    }

    void updateTrackingReference(const Detection& target, Vec2 absolute_center) noexcept {
        previous_.valid = true;
        previous_.box = target.box;
        previous_.absolute_center = absolute_center;
        previous_.height = target.box.height();
    }

    void fillSnapshot(
        AimSnapshot& snapshot,
        const Detection& target,
        Vec2 absolute_aim,
        Vec2 lead,
        bool locked) const {
        snapshot.target_locked = locked;
        snapshot.target_class = target.class_id;
        snapshot.confidence = target.confidence;
        snapshot.target = absolute_aim;
        snapshot.velocity = velocity_;
        snapshot.lead = lead;
    }

    AimConfig config_;
    TrackingReference previous_{};
    int lost_frames_ = 0;
    int track_age_ = 0;
    bool have_previous_aim_ = false;
    Vec2 previous_absolute_aim_{};
    Vec2 filtered_position_{};
    Vec2 velocity_{};
    Vec2 previous_lead_{};
    Vec2 held_velocity_{};
    int motion_hold_frames_ = 0;
    Clock::time_point previous_timestamp_{};
    LegacyPidState pid_x_{};
    LegacyPidState pid_y_{};
    Clock::time_point next_click_at_{};
    std::minstd_rand engine_{0x50414247U};
    std::uniform_int_distribution<int> random_{0, 0x7FFF};
};

AimEngine::AimEngine(AimConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
AimEngine::~AimEngine() = default;
AimEngine::AimEngine(AimEngine&&) noexcept = default;
AimEngine& AimEngine::operator=(AimEngine&&) noexcept = default;

void AimEngine::updateConfig(AimConfig config) {
    impl_->updateConfig(std::move(config));
}

void AimEngine::reset() noexcept {
    impl_->reset();
}

AimDecision AimEngine::process(
    const DetectionFrame& frame,
    const CombatContext& combat,
    const ActivationState& activation) {
    return impl_->process(frame, combat, activation);
}

} // namespace pubg::ai_aim
