#pragma once

#include "pubg_ai_aim/AimConfig.hpp"
#include "pubg_ai_aim/AimTypes.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pubg::ai_aim {

struct TrackingReference {
    bool valid = false;
    Box box{};
    Vec2 absolute_center{};
    float height = 0.0F;
};

struct LegacyPidState {
    float setpoint = 0.0F;
    float previous_error = 0.0F;
    float previous_i_output = 0.0F;
};

class FractionalMotionAccumulator {
public:
    [[nodiscard]] MotionCommand consume(const MotionCommand& command) noexcept;
    void reset() noexcept;

private:
    double residual_x_ = 0.0;
    double residual_y_ = 0.0;
};

[[nodiscard]] float intersectionOverUnion(const Box& lhs, const Box& rhs) noexcept;
[[nodiscard]] float centerBoxIoU(const Box& lhs, const Box& rhs) noexcept;

[[nodiscard]] std::vector<Detection> decodeYoloV8(
    std::span<const float> channel_first_output,
    const DetectorConfig& config,
    CompatibilityMode compatibility);

void scaleDetectionBoxes(
    std::span<Detection> detections,
    float scale_x,
    float scale_y) noexcept;

[[nodiscard]] std::optional<std::size_t> selectTarget(
    std::span<const Detection> detections,
    const DetectionFrame& frame,
    const TargetingConfig& targeting,
    const TrackingConfig& tracking,
    const TrackingReference& previous,
    bool force_body_class = false) noexcept;

[[nodiscard]] bool associationPasses(
    const Detection& candidate,
    Vec2 candidate_absolute_center,
    const TrackingReference& previous,
    const TargetingConfig& targeting,
    const TrackingConfig& tracking) noexcept;

[[nodiscard]] Vec2 classAimPoint(
    const Detection& detection,
    const TargetingConfig& targeting) noexcept;

[[nodiscard]] float softDeadzone(float value, float deadzone) noexcept;
[[nodiscard]] float fovConvertAxis(
    float pixel_error,
    float fov_degrees,
    int game_pixels,
    int axis_resolution,
    float angle_span_degrees) noexcept;

[[nodiscard]] float legacyPidStep(
    LegacyPidState& state,
    float input,
    float p,
    float i,
    float d) noexcept;

[[nodiscard]] std::string canonicalWeaponName(std::string_view name);
[[nodiscard]] std::string canonicalScopeName(std::string_view name);
[[nodiscard]] bool isDmrWeapon(std::string_view canonical_name) noexcept;
[[nodiscard]] bool isSmgWeapon(std::string_view canonical_name) noexcept;
[[nodiscard]] bool isAutomaticWeapon(std::string_view canonical_name) noexcept;
[[nodiscard]] float scopePredictionZoom(std::string_view scope) noexcept;
[[nodiscard]] float scopeFireScale(
    std::string_view scope,
    const FireOffsetConfig& config) noexcept;
[[nodiscard]] float fireDistanceScale(
    float box_height,
    const FireOffsetConfig& config) noexcept;
[[nodiscard]] float scopeSensitivity(
    std::string_view scope,
    const ControllerConfig& config,
    CompatibilityMode compatibility) noexcept;
[[nodiscard]] float controllerPidGainScale(
    std::string_view scope,
    float scope_zoom,
    const ControllerConfig& config,
    CompatibilityMode compatibility,
    float global_scale) noexcept;
[[nodiscard]] float resolveWeaponLeadScale(
    std::string_view weapon,
    const PredictionConfig& config) noexcept;
[[nodiscard]] float resolveManualWeaponScale(
    std::string_view weapon,
    const PredictionConfig& config) noexcept;

} // namespace pubg::ai_aim
