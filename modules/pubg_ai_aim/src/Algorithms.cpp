#include "pubg_ai_aim/Algorithms.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>

namespace pubg::ai_aim {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float squaredDistance(Vec2 lhs, Vec2 rhs) noexcept {
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    return dx * dx + dy * dy;
}

bool classAllowed(int class_id, const TargetingConfig& targeting, bool force_body_class) noexcept {
    if (force_body_class) {
        return class_id == 1;
    }
    if (class_id < 0 || class_id >= static_cast<int>(targeting.enabled_classes.size())) {
        return false;
    }
    return targeting.enabled_classes[static_cast<std::size_t>(class_id)];
}

std::string compactLower(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char character : input) {
        if (std::isalnum(character) != 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return result;
}

float nmsIoU(const Detection& lhs, const Detection& rhs, bool original_coordinates) noexcept {
    if (!original_coordinates) {
        return intersectionOverUnion(lhs.box, rhs.box);
    }

    // Original LOAI passed rounded cx/cy as cv::Rect left/top to NMSBoxes.
    const Vec2 lhs_center = lhs.box.center();
    const Vec2 rhs_center = rhs.box.center();
    const float lhs_width = std::round(lhs.box.width());
    const float lhs_height = std::round(lhs.box.height());
    const float rhs_width = std::round(rhs.box.width());
    const float rhs_height = std::round(rhs.box.height());
    const Box lhs_wrong{
        std::round(lhs_center.x),
        std::round(lhs_center.y),
        std::round(lhs_center.x) + lhs_width,
        std::round(lhs_center.y) + lhs_height,
    };
    const Box rhs_wrong{
        std::round(rhs_center.x),
        std::round(rhs_center.y),
        std::round(rhs_center.x) + rhs_width,
        std::round(rhs_center.y) + rhs_height,
    };
    return intersectionOverUnion(lhs_wrong, rhs_wrong);
}

} // namespace

MotionCommand FractionalMotionAccumulator::consume(const MotionCommand& command) noexcept {
    residual_x_ += command.dx;
    residual_y_ += command.dy;

    MotionCommand result = command;
    result.dx = std::round(residual_x_);
    result.dy = std::round(residual_y_);
    residual_x_ -= result.dx;
    residual_y_ -= result.dy;
    return result;
}

void FractionalMotionAccumulator::reset() noexcept {
    residual_x_ = 0.0;
    residual_y_ = 0.0;
}

float intersectionOverUnion(const Box& lhs, const Box& rhs) noexcept {
    const float intersection_width =
        std::max(0.0F, std::min(lhs.right, rhs.right) - std::max(lhs.left, rhs.left));
    const float intersection_height =
        std::max(0.0F, std::min(lhs.bottom, rhs.bottom) - std::max(lhs.top, rhs.top));
    const float intersection = intersection_width * intersection_height;
    const float lhs_area = std::max(0.0F, lhs.width()) * std::max(0.0F, lhs.height());
    const float rhs_area = std::max(0.0F, rhs.width()) * std::max(0.0F, rhs.height());
    const float union_area = lhs_area + rhs_area - intersection;
    return union_area > 0.0F ? intersection / union_area : 0.0F;
}

float centerBoxIoU(const Box& lhs, const Box& rhs) noexcept {
    return intersectionOverUnion(lhs, rhs);
}

std::vector<Detection> decodeYoloV8(
    std::span<const float> channel_first_output,
    const DetectorConfig& raw_config,
    CompatibilityMode compatibility) {
    DetectorConfig config = raw_config;
    config.anchor_count = std::clamp(config.anchor_count, 1, 1'000'000);
    config.class_count = std::clamp(config.class_count, 1, 256);
    config.confidence = std::clamp(config.confidence, 0.0F, 1.0F);
    config.iou_threshold = std::clamp(config.iou_threshold, 0.0F, 1.0F);

    const std::size_t anchors = static_cast<std::size_t>(config.anchor_count);
    const std::size_t channels = static_cast<std::size_t>(config.class_count + 4);
    if (channel_first_output.size() < anchors * channels) {
        return {};
    }

    std::vector<Detection> candidates;
    candidates.reserve(128);
    for (std::size_t anchor = 0; anchor < anchors; ++anchor) {
        int best_class = 0;
        float best_score = channel_first_output[4 * anchors + anchor];
        for (int class_id = 1; class_id < config.class_count; ++class_id) {
            const float score = channel_first_output[
                static_cast<std::size_t>(4 + class_id) * anchors + anchor];
            if (score > best_score) {
                best_score = score;
                best_class = class_id;
            }
        }
        if (!std::isfinite(best_score) || best_score <= config.confidence) {
            continue;
        }

        float center_x = channel_first_output[anchor];
        float center_y = channel_first_output[anchors + anchor];
        float width = channel_first_output[2 * anchors + anchor];
        float height = channel_first_output[3 * anchors + anchor];
        if (!std::isfinite(center_x) || !std::isfinite(center_y) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F) {
            continue;
        }
        if (compatibility == CompatibilityMode::Original) {
            // cvRound/cvtss2si follows the active FP rounding mode; the default
            // mode is ties-to-even, unlike std::round's half-away-from-zero.
            center_x = std::nearbyint(center_x);
            center_y = std::nearbyint(center_y);
            width = std::nearbyint(width);
            height = std::nearbyint(height);
        }
        candidates.push_back({Box::fromCenter(center_x, center_y, width, height), best_score, best_class});
    }

    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return candidates[lhs].confidence > candidates[rhs].confidence;
    });

    const bool original_coordinates = compatibility == CompatibilityMode::Original;
    const bool class_aware = config.class_aware_nms && !original_coordinates;
    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<Detection> result;
    result.reserve(candidates.size());

    for (std::size_t position = 0; position < order.size(); ++position) {
        const std::size_t candidate_index = order[position];
        if (suppressed[candidate_index]) {
            continue;
        }
        result.push_back(candidates[candidate_index]);
        for (std::size_t other_position = position + 1; other_position < order.size(); ++other_position) {
            const std::size_t other_index = order[other_position];
            if (suppressed[other_index]) {
                continue;
            }
            if (class_aware && candidates[candidate_index].class_id != candidates[other_index].class_id) {
                continue;
            }
            if (nmsIoU(candidates[candidate_index], candidates[other_index], original_coordinates) >
                config.iou_threshold) {
                suppressed[other_index] = true;
            }
        }
    }
    return result;
}

void scaleDetectionBoxes(
    std::span<Detection> detections,
    float scale_x,
    float scale_y) noexcept {
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        scale_x <= 0.0F || scale_y <= 0.0F) {
        return;
    }
    for (Detection& detection : detections) {
        detection.box.left *= scale_x;
        detection.box.right *= scale_x;
        detection.box.top *= scale_y;
        detection.box.bottom *= scale_y;
    }
}

std::optional<std::size_t> selectTarget(
    std::span<const Detection> detections,
    const DetectionFrame& frame,
    const TargetingConfig& targeting,
    const TrackingConfig& tracking,
    const TrackingReference& previous,
    bool force_body_class) noexcept {
    const Vec2 screen_center{
        static_cast<float>(frame.screen_center_x),
        static_cast<float>(frame.screen_center_y),
    };
    const float track_iou = std::clamp(tracking.iou_threshold, 0.0F, 0.95F);
    float best_score = std::numeric_limits<float>::max();
    std::optional<std::size_t> best;

    for (std::size_t index = 0; index < detections.size(); ++index) {
        const Detection& detection = detections[index];
        if (!classAllowed(detection.class_id, targeting, force_body_class)) {
            continue;
        }
        if (!std::isfinite(detection.confidence) ||
            !std::isfinite(detection.box.left) || !std::isfinite(detection.box.top) ||
            !std::isfinite(detection.box.right) || !std::isfinite(detection.box.bottom) ||
            detection.box.width() <= 0.0F || detection.box.height() <= 0.0F) {
            continue;
        }
        const Vec2 local_center = detection.box.center();
        const Vec2 absolute_center{
            local_center.x + static_cast<float>(frame.roi_left),
            local_center.y + static_cast<float>(frame.roi_top),
        };
        float score = squaredDistance(absolute_center, screen_center);
        if (previous.valid) {
            const float distance_squared = squaredDistance(absolute_center, previous.absolute_center);
            const float distance = std::sqrt(std::max(0.0F, distance_squared));
            const float height_delta = std::abs(detection.box.height() - previous.height);
            const float iou = centerBoxIoU(detection.box, previous.box);
            if (iou >= track_iou) {
                score *= 0.18F;
            } else if (distance <= std::max(previous.height * 0.90F, 28.0F)) {
                score *= 0.35F;
            } else {
                score += distance_squared * 0.18F;
            }
            score += height_delta * height_delta * 0.08F;
        }
        if (score < best_score) {
            best_score = score;
            best = index;
        }
    }
    return best;
}

bool associationPasses(
    const Detection& candidate,
    Vec2 candidate_absolute_center,
    const TrackingReference& previous,
    const TargetingConfig& targeting,
    const TrackingConfig& tracking) noexcept {
    if (!std::isfinite(candidate_absolute_center.x) ||
        !std::isfinite(candidate_absolute_center.y) ||
        !std::isfinite(candidate.box.left) || !std::isfinite(candidate.box.top) ||
        !std::isfinite(candidate.box.right) || !std::isfinite(candidate.box.bottom) ||
        candidate.box.width() <= 0.0F || candidate.box.height() <= 0.0F) {
        return false;
    }
    if (!previous.valid) {
        return true;
    }
    const float stickiness = std::clamp(targeting.target_stickiness, 0.5F, 10.0F);
    const float track_iou = std::clamp(tracking.iou_threshold, 0.0F, 0.95F);
    const float iou_threshold =
        std::clamp(0.8F - 0.1F * stickiness, 0.2F, 0.8F) * track_iou;
    const float position_threshold =
        std::max(previous.height * 1.35F, 45.0F) * (1.0F + 0.35F * stickiness);
    const float height_threshold =
        std::max(previous.height * (0.1F * stickiness + 0.55F), 26.0F);

    const float distance = std::sqrt(std::max(
        0.0F, squaredDistance(candidate_absolute_center, previous.absolute_center)));
    const float height_delta = std::abs(candidate.box.height() - previous.height);
    const float iou = centerBoxIoU(candidate.box, previous.box);
    return !((iou < iou_threshold && distance > position_threshold) ||
             height_delta > height_threshold);
}

Vec2 classAimPoint(const Detection& detection, const TargetingConfig& targeting) noexcept {
    float location = targeting.location;
    if (detection.class_id >= 0 &&
        detection.class_id < static_cast<int>(targeting.class_location.size())) {
        location = targeting.class_location[static_cast<std::size_t>(detection.class_id)];
    }
    location = std::clamp(location, 0.0F, 1.0F);
    const Vec2 center = detection.box.center();
    return {center.x, detection.box.bottom - detection.box.height() * location};
}

float softDeadzone(float value, float deadzone) noexcept {
    deadzone = std::max(deadzone, 0.0F);
    if (std::abs(value) <= deadzone) {
        return 0.0F;
    }
    return value - std::copysign(deadzone, value);
}

float fovConvertAxis(
    float pixel_error,
    float fov_degrees,
    int game_pixels,
    int axis_resolution,
    float angle_span_degrees) noexcept {
    if (axis_resolution <= 0 || game_pixels <= 0 || angle_span_degrees <= 0.0F) {
        return 0.0F;
    }
    fov_degrees = std::clamp(fov_degrees, 1.0F, 179.0F);
    const float focal = (static_cast<float>(axis_resolution) * 0.5F) /
                        std::tan(fov_degrees * kPi / 360.0F);
    if (!std::isfinite(focal) || focal == 0.0F) {
        return 0.0F;
    }
    const float angle = std::atan(pixel_error / focal);
    return angle * static_cast<float>(game_pixels) /
           (angle_span_degrees * kPi / 180.0F);
}

float legacyPidStep(
    LegacyPidState& state,
    float input,
    float p,
    float i,
    float d) noexcept {
    const float error = input - state.setpoint;
    const float p_output = error * p;
    const float i_output = (error + state.previous_i_output) * i;
    const float d_output = (error - state.previous_error) * d;
    state.previous_error = error;
    state.previous_i_output = i_output;
    return p_output + i_output + d_output;
}

std::string canonicalWeaponName(std::string_view name) {
    const std::string compact = compactLower(name);
    if (compact == "berry" || compact == "beryl" || compact == "berylm762") return "m762";
    if (compact == "scar" || compact == "scarl") return "scarl";
    if (compact == "m16") return "m16a4";
    if (compact == "mk47" || compact == "mutant") return "mk47";
    if (compact == "ace" || compact == "ace32") return "ace32";
    if (compact == "mini" || compact == "mini14") return "mini14";
    if (compact == "mp5") return "mp5k";
    if (compact == "tom" || compact == "tommy") return "tommygun";
    if (compact == "ump") return "ump45";
    if (compact == "microuzi") return "uzi";
    if (compact == "98k" || compact == "k98" || compact == "kar98") return "kar98k";
    if (compact == "winchester") return "win94";
    if (compact == "dlg" || compact == "dragunov") return "dragunov";
    return compact;
}

std::string canonicalScopeName(std::string_view name) {
    const std::string compact = compactLower(name);
    if (compact == "reddot" || compact == "red" || compact == "holographic" ||
        compact == "holo" || compact == "quanxi" || compact == "1" || compact == "1x") {
        return "1x";
    }
    if (compact == "2" || compact == "2x") return "2x";
    if (compact == "3" || compact == "3x") return "3x";
    if (compact == "4" || compact == "4x") return "4x";
    if (compact == "6" || compact == "6x") return "6x";
    if (compact == "8" || compact == "8x") return "8x";
    return compact;
}

bool isDmrWeapon(std::string_view canonical_name) noexcept {
    return canonical_name == "mini14" || canonical_name == "sks" ||
           canonical_name == "mk12" || canonical_name == "slr" ||
           canonical_name == "qbu" || canonical_name == "vss" ||
           canonical_name == "mk14" || canonical_name == "dragunov";
}

bool isSmgWeapon(std::string_view canonical_name) noexcept {
    return canonical_name == "mp5k" || canonical_name == "tommygun" ||
           canonical_name == "ump45" || canonical_name == "vector" ||
           canonical_name == "js9" || canonical_name == "mp9" ||
           canonical_name == "p90" || canonical_name == "uzi";
}

bool isAutomaticWeapon(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 26> weapons{
        "aug", "m762", "groza", "qbz", "scarl", "k2", "akm", "m416",
        "m16a4", "g36c", "mk47", "ace32", "famas", "mp5k", "tommygun",
        "ump45", "vector", "js9", "mp9", "p90", "uzi", "m249", "mg3",
        "rpd", "mk14", "vss",
    };
    return std::find(weapons.begin(), weapons.end(), name) != weapons.end();
}

float scopePredictionZoom(std::string_view scope) noexcept {
    if (scope == "2x") return 2.0F;
    if (scope == "3x" || scope == "6x") return 3.0F;
    if (scope == "4x" || scope == "8x") return 4.0F;
    return 1.0F;
}

float scopeFireScale(std::string_view scope, const FireOffsetConfig& config) noexcept {
    if (scope == "2x") return config.scope_scale[1];
    if (scope == "3x" || scope == "6x") return config.scope_scale[2];
    if (scope == "4x" || scope == "8x") return config.scope_scale[3];
    return config.scope_scale[0];
}

float fireDistanceScale(float box_height, const FireOffsetConfig& config) noexcept {
    const float minimum = std::clamp(config.minimum_distance_scale, 0.1F, 1.0F);
    const float ratio = config.base_box_height / std::max(box_height, 1.0F);
    return std::clamp(ratio, minimum, 1.0F / minimum);
}

float scopeSensitivity(
    std::string_view scope,
    const ControllerConfig& config,
    CompatibilityMode compatibility) noexcept {
    if (scope == "1x") return config.scope_sensitivity[0];
    if (scope == "2x") return config.scope_sensitivity[1];
    if (scope == "3x") return config.scope_sensitivity[2];
    if (scope == "4x") return config.scope_sensitivity[3];
    if (scope == "6x") {
        return compatibility == CompatibilityMode::Original
            ? config.scope_sensitivity[2]
            : config.scope_sensitivity[4];
    }
    if (scope == "8x") {
        return compatibility == CompatibilityMode::Original
            ? config.scope_sensitivity[3]
            : config.scope_sensitivity[5];
    }
    return config.ads_sensitivity_current;
}

float controllerPidGainScale(
    std::string_view scope,
    float scope_zoom,
    const ControllerConfig& config,
    CompatibilityMode compatibility,
    float global_scale) noexcept {
    float sensitivity_factor = 1.0F;
    if (config.ads_sensitivity_scale_enabled) {
        const float current = config.auto_scope_sensitivity
            ? scopeSensitivity(scope, config, compatibility)
            : config.ads_sensitivity_current;
        const float ratio = std::clamp(
            config.ads_sensitivity_base / std::max(current, 0.01F),
            config.ads_sensitivity_scale_min,
            config.ads_sensitivity_scale_max);
        sensitivity_factor = std::pow(ratio, 1.36F);
    }
    if (!std::isfinite(scope_zoom)) {
        scope_zoom = 1.0F;
    }
    return global_scale * sensitivity_factor / std::clamp(scope_zoom, 1.0F, 10.0F);
}

float resolveWeaponLeadScale(std::string_view weapon, const PredictionConfig& config) noexcept {
    if (!config.weapon_scale_enabled) {
        return 1.0F;
    }
    const auto found = config.weapon_scales.find(std::string(weapon));
    const float fallback = isDmrWeapon(weapon)
        ? config.unknown_dmr_scale
        : config.unknown_automatic_scale;
    const float value = found != config.weapon_scales.end() ? found->second : fallback;
    return std::clamp(value, config.weapon_scale_min, config.weapon_scale_max);
}

float resolveManualWeaponScale(std::string_view weapon, const PredictionConfig& config) noexcept {
    const auto found = config.manual_weapon_scales.find(std::string(weapon));
    const float value = found != config.manual_weapon_scales.end()
        ? found->second
        : config.manual_default_scale;
    return std::clamp(value, 0.1F, 5.0F);
}

} // namespace pubg::ai_aim
