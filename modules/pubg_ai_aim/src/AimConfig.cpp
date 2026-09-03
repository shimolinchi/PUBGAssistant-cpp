#include "pubg_ai_aim/AimConfig.hpp"

#include <algorithm>
#include <cmath>

namespace pubg::ai_aim {
namespace {

float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

float bounded(float value, float fallback, float low, float high) noexcept {
    return std::clamp(finiteOr(value, fallback), low, high);
}

} // namespace

void AimConfig::sanitize() noexcept {
    switch (compatibility) {
    case CompatibilityMode::Original:
    case CompatibilityMode::Corrected:
        break;
    default:
        compatibility = CompatibilityMode::Corrected;
        break;
    }
    target_fps = std::clamp(target_fps, 1, 1000);

    detector.input_width = std::clamp(detector.input_width, 32, 4096);
    detector.input_height = std::clamp(detector.input_height, 32, 4096);
    detector.anchor_count = std::clamp(detector.anchor_count, 1, 1'000'000);
    detector.class_count = std::clamp(detector.class_count, 1, 256);
    detector.confidence = bounded(detector.confidence, 0.404F, 0.0F, 1.0F);
    detector.iou_threshold = bounded(detector.iou_threshold, 0.5F, 0.0F, 1.0F);

    targeting.location = bounded(targeting.location, 0.5F, 0.0F, 1.0F);
    for (float& value : targeting.class_location) {
        value = bounded(value, targeting.location, 0.0F, 1.0F);
    }
    targeting.effective_range = bounded(targeting.effective_range, 320.0F, 1.0F, 8192.0F);
    targeting.target_stickiness = bounded(targeting.target_stickiness, 3.0F, 0.5F, 10.0F);
    targeting.advanced_multiple = bounded(targeting.advanced_multiple, 0.307F, 0.0F, 1.0F);
    targeting.lean_x_adjust = bounded(targeting.lean_x_adjust, 0.22F, 0.0F, 0.6F);

    tracking.iou_threshold = bounded(tracking.iou_threshold, 0.455F, 0.0F, 0.95F);
    tracking.max_lost_frames = std::clamp(tracking.max_lost_frames, 0, 120);
    tracking.max_frame_age_ms = std::clamp(tracking.max_frame_age_ms, 1, 10'000);

    prediction.predict_factor = bounded(prediction.predict_factor, 2.195F, 0.0F, 20.0F);
    prediction.velocity_deadzone = bounded(prediction.velocity_deadzone, 2.013F, 0.0F, 30.0F);
    prediction.lead_smooth = bounded(prediction.lead_smooth, 0.0F, 0.0F, 0.95F);
    prediction.lead_step = bounded(prediction.lead_step, 13.657F, 0.0F, 60.0F);
    prediction.lead_y_scale = bounded(prediction.lead_y_scale, 0.0F, 0.0F, 5.0F);
    prediction.lead_hold_frames = std::clamp(prediction.lead_hold_frames, 0, 240);
    prediction.small_target_deadzone_scale = bounded(
        prediction.small_target_deadzone_scale, 1.5F, 0.0F, 3.0F);
    prediction.small_target_predict_scale = bounded(
        prediction.small_target_predict_scale, 0.6F, 0.0F, 10.0F);
    prediction.small_target_base_height = bounded(
        prediction.small_target_base_height, 85.0F, 1.0F, 4096.0F);
    prediction.small_target_max_scale = bounded(
        prediction.small_target_max_scale, 20.0F, 1.0F, 20.0F);
    prediction.dmr_scale = bounded(prediction.dmr_scale, 2.0F, 0.0F, 3.0F);
    prediction.automatic_scale = bounded(prediction.automatic_scale, 1.4F, 0.0F, 2.5F);
    prediction.lead_boost_predict_factor = bounded(
        prediction.lead_boost_predict_factor, 20.0F, 0.0F, 30.0F);
    prediction.normal_predict_factor = bounded(
        prediction.normal_predict_factor, 2.0F, 0.0F, 20.0F);
    prediction.lead_boost_velocity_deadzone = bounded(
        prediction.lead_boost_velocity_deadzone, 1.0F, 0.0F, 30.0F);
    prediction.normal_velocity_deadzone = bounded(
        prediction.normal_velocity_deadzone, 5.0F, 0.0F, 30.0F);
    prediction.unknown_automatic_scale = bounded(
        prediction.unknown_automatic_scale, 1.0F, 0.05F, 5.0F);
    prediction.unknown_dmr_scale = bounded(
        prediction.unknown_dmr_scale, 1.0F, 0.05F, 5.0F);
    prediction.weapon_scale_min = bounded(prediction.weapon_scale_min, 0.05F, 0.05F, 5.0F);
    prediction.weapon_scale_max = bounded(prediction.weapon_scale_max, 5.0F, 0.05F, 5.0F);
    if (prediction.weapon_scale_min > prediction.weapon_scale_max) {
        std::swap(prediction.weapon_scale_min, prediction.weapon_scale_max);
    }
    for (auto& [name, value] : prediction.weapon_scales) {
        (void)name;
        value = bounded(
            value,
            prediction.unknown_automatic_scale,
            prediction.weapon_scale_min,
            prediction.weapon_scale_max);
    }
    prediction.manual_default_scale = bounded(
        prediction.manual_default_scale, 1.0F, 0.1F, 5.0F);
    for (auto& [name, value] : prediction.manual_weapon_scales) {
        (void)name;
        value = bounded(value, prediction.manual_default_scale, 0.1F, 5.0F);
    }
    prediction.jump_reset_pixels = bounded(
        prediction.jump_reset_pixels, 100.0F, 1.0F, 4096.0F);
    prediction.max_lead_pixels = bounded(
        prediction.max_lead_pixels, 120.0F, 1.0F, 4096.0F);

    fire_offset.default_offset = bounded(fire_offset.default_offset, 0.0F, -1000.0F, 1000.0F);
    for (float& value : fire_offset.class_offset) {
        value = bounded(value, 0.0F, -1000.0F, 1000.0F);
    }
    fire_offset.base_box_height = bounded(fire_offset.base_box_height, 85.0F, 1.0F, 4096.0F);
    fire_offset.minimum_distance_scale = bounded(
        fire_offset.minimum_distance_scale, 0.8F, 0.1F, 1.0F);
    for (float& value : fire_offset.scope_scale) {
        value = bounded(value, 1.0F, 0.0F, 10.0F);
    }
    fire_offset.lean_scale = bounded(fire_offset.lean_scale, 0.8F, 0.0F, 2.0F);
    fire_offset.smg_scale = bounded(fire_offset.smg_scale, 0.45F, 0.0F, 2.0F);

    controller.horizontal_fov = bounded(controller.horizontal_fov, 115.0F, 1.0F, 179.0F);
    controller.vertical_fov = bounded(controller.vertical_fov, 100.0F, 1.0F, 179.0F);
    controller.game_x_pixels = std::clamp(controller.game_x_pixels, 1, 100'000);
    controller.game_y_pixels = std::clamp(controller.game_y_pixels, 1, 100'000);
    controller.p_x = bounded(controller.p_x, 0.881F, -100.0F, 100.0F);
    controller.i_x = bounded(controller.i_x, 0.151F, -100.0F, 100.0F);
    controller.d_x = bounded(controller.d_x, 0.0F, -100.0F, 100.0F);
    controller.p_y = bounded(controller.p_y, 0.456F, -100.0F, 100.0F);
    controller.i_y = bounded(controller.i_y, 0.0F, -100.0F, 100.0F);
    controller.d_y = bounded(controller.d_y, 0.0F, -100.0F, 100.0F);
    controller.ads_sensitivity_base = bounded(
        controller.ads_sensitivity_base, 30.0F, 0.01F, 1000.0F);
    controller.ads_sensitivity_current = bounded(
        controller.ads_sensitivity_current, 42.0F, 0.01F, 1000.0F);
    controller.ads_sensitivity_scale_min = bounded(
        controller.ads_sensitivity_scale_min, 0.3F, 0.01F, 100.0F);
    controller.ads_sensitivity_scale_max = bounded(
        controller.ads_sensitivity_scale_max, 3.0F, 0.01F, 100.0F);
    if (controller.ads_sensitivity_scale_min > controller.ads_sensitivity_scale_max) {
        std::swap(
            controller.ads_sensitivity_scale_min,
            controller.ads_sensitivity_scale_max);
    }
    for (float& sensitivity : controller.scope_sensitivity) {
        sensitivity = bounded(sensitivity, 30.0F, 0.01F, 1000.0F);
    }
    controller.max_pixels = bounded(controller.max_pixels, 40.0F, 1.0F, 8192.0F);
    controller.debounce_pixels = bounded(controller.debounce_pixels, 2.0F, 0.0F, 8192.0F);
    controller.global_scale = bounded(controller.global_scale, 1.0F, 0.1F, 5.0F);

    auto_fire.trigger_width = bounded(auto_fire.trigger_width, 0.75F, 0.0F, 10.0F);
    auto_fire.trigger_height = bounded(auto_fire.trigger_height, 0.75F, 0.0F, 10.0F);
    auto_fire.interval_ms = std::clamp(auto_fire.interval_ms, 0, 60'000);
    auto_fire.random_ms = std::clamp(auto_fire.random_ms, 0, 60'000);
    switch (auto_fire.mode) {
    case AutoFireMode::MoveOnly:
    case AutoFireMode::TriggerOnly:
    case AutoFireMode::MoveAndTrigger:
        break;
    default:
        auto_fire.mode = AutoFireMode::MoveOnly;
        break;
    }
}

AimConfig AimConfig::legacyDefaults() {
    AimConfig config;
    config.compatibility = CompatibilityMode::Original;
    config.detector.class_aware_nms = false;

    config.prediction.weapon_scales = {
        {"aug", 1.0F},       {"m762", 1.4F},      {"groza", 0.9F},
        {"qbz", 0.95F},      {"scarl", 0.2F},     {"k2", 1.0F},
        {"akm", 0.7F},       {"m416", 0.9F},      {"m16a4", 1.0F},
        {"g36c", 0.9F},      {"mk47", 0.75F},     {"ace32", 0.75F},
        {"vss", 0.0F},       {"mk14", 0.0F},      {"slr", 1.0F},
        {"mini14", 0.8F},    {"qbu", 1.0F},       {"mk12", 1.0F},
        {"sks", 1.2F},       {"famas", 1.0F},     {"mp5k", 1.3F},
        {"tommygun", 1.85F}, {"ump45", 1.85F},    {"vector", 1.85F},
        {"js9", 1.85F},      {"mp9", 1.85F},      {"p90", 1.55F},
        {"uzi", 1.85F},      {"m24", 2.8F},       {"kar98k", 2.85F},
        {"win94", 1.1F},     {"dragunov", 1.1F},  {"m249", 1.2F},
        {"mg3", 1.2F},      {"rpd", 1.2F},
    };

    config.prediction.manual_weapon_scales = {
        {"aug", 0.6F},       {"m762", 1.25F},     {"groza", 0.95F},
        {"qbz", 0.6F},       {"scarl", 1.3F},     {"k2", 0.65F},
        {"akm", 1.15F},      {"m416", 0.55F},     {"m16a4", 0.85F},
        {"g36c", 0.75F},     {"mk47", 0.9F},      {"ace32", 1.05F},
        {"vss", 0.1F},       {"mk14", 0.1F},      {"slr", 0.1F},
        {"mini14", 0.1F},    {"qbu", 0.1F},       {"mk12", 0.1F},
        {"sks", 0.1F},       {"famas", 0.85F},    {"mp5k", 0.95F},
        {"tommygun", 0.95F}, {"ump45", 0.95F},    {"vector", 0.95F},
        {"js9", 0.95F},      {"mp9", 0.95F},      {"p90", 0.95F},
        {"uzi", 0.95F},      {"m24", 1.1F},       {"kar98k", 1.1F},
        {"win94", 1.1F},     {"dragunov", 0.1F},  {"m249", 1.25F},
        {"mg3", 1.15F},     {"rpd", 1.25F},
    };

    config.sanitize();
    return config;
}

AimConfig AimConfig::correctedDefaults() {
    AimConfig config = legacyDefaults();
    config.compatibility = CompatibilityMode::Corrected;
    config.detector.class_aware_nms = true;
    config.sanitize();
    return config;
}

} // namespace pubg::ai_aim
