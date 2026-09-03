#pragma once

#include <array>
#include <string>
#include <unordered_map>

namespace pubg::ai_aim {

enum class CompatibilityMode {
    Original,
    Corrected,
};

enum class AutoFireMode {
    MoveOnly = 0,
    TriggerOnly = 1,
    MoveAndTrigger = 2,
};

struct DetectorConfig {
    int input_width = 320;
    int input_height = 320;
    int anchor_count = 2100;
    int class_count = 2;
    float confidence = 0.404F;
    float iou_threshold = 0.5F;
    bool class_aware_nms = true;
};

struct TargetingConfig {
    std::array<bool, 4> enabled_classes{false, true, false, false};
    bool model_classes = true;
    float location = 0.5F;
    std::array<float, 4> class_location{0.536F, 0.723F, 0.0F, 0.0F};
    float effective_range = 320.0F;
    float target_stickiness = 3.0F;
    bool advanced = false;
    float advanced_multiple = 0.307F;
    float lean_x_adjust = 0.22F;
};

struct TrackingConfig {
    float iou_threshold = 0.455F;
    int max_lost_frames = 6;
    int max_frame_age_ms = 250;
};

struct PredictionConfig {
    bool enabled = true;
    float predict_factor = 2.195F;
    float velocity_deadzone = 2.013F;

    float lead_smooth = 0.0F;
    float lead_step = 13.657F;
    float lead_y_scale = 0.0F;
    int lead_hold_frames = 26;

    float small_target_deadzone_scale = 1.5F;
    float small_target_predict_scale = 0.6F;
    float small_target_base_height = 85.0F;
    float small_target_max_scale = 20.0F;

    bool dmr_boost_enabled = true;
    float dmr_scale = 2.0F;
    bool automatic_boost_enabled = true;
    float automatic_scale = 1.4F;

    bool lead_boost_hotkey_enabled = true;
    float lead_boost_predict_factor = 30.0F;
    float normal_predict_factor = 2.0F;
    float lead_boost_velocity_deadzone = 1.0F;
    float normal_velocity_deadzone = 5.0F;

    bool weapon_scale_enabled = true;
    float unknown_automatic_scale = 1.0F;
    float unknown_dmr_scale = 1.0F;
    float weapon_scale_min = 0.05F;
    float weapon_scale_max = 5.0F;
    std::unordered_map<std::string, float> weapon_scales;

    bool manual_scale_enabled = true;
    float manual_default_scale = 1.0F;
    std::unordered_map<std::string, float> manual_weapon_scales;

    float jump_reset_pixels = 100.0F;
    float max_lead_pixels = 120.0F;
};

struct FireOffsetConfig {
    bool enabled = true;
    float default_offset = 0.0F;
    std::array<float, 4> class_offset{30.0F, 120.0F, 0.0F, 0.0F};
    bool distance_scale_enabled = true;
    float base_box_height = 85.0F;
    float minimum_distance_scale = 0.8F;
    std::array<float, 4> scope_scale{0.6F, 1.2F, 1.5F, 1.3F};
    float lean_scale = 0.8F;
    float smg_scale = 0.45F;
};

struct ControllerConfig {
    bool fov_enabled = true;
    float horizontal_fov = 115.0F;
    float vertical_fov = 100.0F;
    int game_x_pixels = 370;
    int game_y_pixels = 370;

    bool pid_enabled = true;
    float p_x = 0.881F;
    float i_x = 0.151F;
    float d_x = 0.0F;
    float p_y = 0.456F;
    float i_y = 0.0F;
    float d_y = 0.0F;
    bool fire_no_y_pid = false;

    bool ads_sensitivity_scale_enabled = true;
    float ads_sensitivity_base = 30.0F;
    float ads_sensitivity_current = 42.0F;
    float ads_sensitivity_scale_min = 0.3F;
    float ads_sensitivity_scale_max = 3.0F;
    bool auto_scope_sensitivity = true;
    std::array<float, 6> scope_sensitivity{
        50.0F, 40.0F, 42.0F, 41.0F, 44.0F, 50.0F};

    float max_pixels = 40.0F;
    bool debounce_enabled = false;
    float debounce_pixels = 2.0F;

    bool global_scale_enabled = true;
    float global_scale = 1.0F;
};

struct AutoFireConfig {
    bool enabled = false;
    AutoFireMode mode = AutoFireMode::MoveOnly;
    float trigger_width = 0.75F;
    float trigger_height = 0.75F;
    int interval_ms = 150;
    int random_ms = 80;
};

struct AimConfig {
    CompatibilityMode compatibility = CompatibilityMode::Corrected;
    int target_fps = 125;
    DetectorConfig detector{};
    TargetingConfig targeting{};
    TrackingConfig tracking{};
    PredictionConfig prediction{};
    FireOffsetConfig fire_offset{};
    ControllerConfig controller{};
    AutoFireConfig auto_fire{};

    void sanitize() noexcept;
    [[nodiscard]] static AimConfig legacyDefaults();
    [[nodiscard]] static AimConfig correctedDefaults();
};

} // namespace pubg::ai_aim
