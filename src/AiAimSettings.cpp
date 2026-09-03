#include "AiAimSettings.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace pubg {
namespace {

using Json = nlohmann::json;
using namespace ai_aim;

struct ParseContext {
    std::string error;

    void fail(const std::string& path, const std::string_view expected) {
        if (error.empty()) {
            error = path + " must be " + std::string(expected);
        }
    }
};

template <typename T>
bool hasExpectedType(const Json& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value.is_boolean();
    } else if constexpr (std::is_integral_v<T>) {
        return value.is_number_integer();
    } else if constexpr (std::is_floating_point_v<T>) {
        return value.is_number();
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value.is_string();
    } else {
        static_assert(!sizeof(T), "Unsupported AI settings scalar type");
    }
}

template <typename T>
bool readIntegralValue(const Json& value, T& destination) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if (value.is_number_unsigned()) {
        const auto raw = value.get<Json::number_unsigned_t>();
        if (raw > static_cast<Json::number_unsigned_t>(std::numeric_limits<T>::max())) {
            return false;
        }
        destination = static_cast<T>(raw);
        return true;
    }
    if (!value.is_number_integer()) {
        return false;
    }

    const auto raw = value.get<Json::number_integer_t>();
    if constexpr (std::is_signed_v<T>) {
        if (raw < static_cast<Json::number_integer_t>(std::numeric_limits<T>::min()) ||
            raw > static_cast<Json::number_integer_t>(std::numeric_limits<T>::max())) {
            return false;
        }
    } else {
        if (raw < 0 ||
            static_cast<Json::number_unsigned_t>(raw) >
                static_cast<Json::number_unsigned_t>(std::numeric_limits<T>::max())) {
            return false;
        }
    }
    destination = static_cast<T>(raw);
    return true;
}

template <typename T>
void readScalar(
    const Json& object,
    const char* key,
    T& destination,
    const std::string& object_path,
    ParseContext& context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return;
    }

    const std::string path = object_path + "." + key;
    if (!hasExpectedType<T>(*it)) {
        context.fail(path, "the expected scalar type");
        return;
    }
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!readIntegralValue(*it, destination)) {
            context.fail(path, "representable by the destination type");
        }
    } else {
        try {
            destination = it->get<T>();
        } catch (...) {
            context.fail(path, "representable by the destination type");
        }
    }
}

const Json* readObject(
    const Json& parent,
    const char* key,
    const std::string& parent_path,
    ParseContext& context) {
    const auto it = parent.find(key);
    if (it == parent.end()) {
        return nullptr;
    }
    if (!it->is_object()) {
        context.fail(parent_path + "." + key, "an object");
        return nullptr;
    }
    return &*it;
}

void rejectUnknownKeys(
    const Json& object,
    const std::initializer_list<std::string_view> allowed,
    const std::string& object_path,
    ParseContext& context) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        const bool supported = std::any_of(
            allowed.begin(), allowed.end(),
            [&](const std::string_view key) { return it.key() == key; });
        if (!supported) {
            context.fail(object_path + "." + it.key(), "a supported field");
        }
    }
}

template <typename T, std::size_t N>
void readArray(
    const Json& object,
    const char* key,
    std::array<T, N>& destination,
    const std::string& object_path,
    ParseContext& context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return;
    }

    const std::string path = object_path + "." + key;
    if (!it->is_array()) {
        context.fail(path, "an array");
        return;
    }
    if (it->size() > N) {
        context.fail(path, "an array with at most " + std::to_string(N) + " entries");
    }

    const std::size_t count = std::min<std::size_t>(N, it->size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& value = (*it)[index];
        if (!hasExpectedType<T>(value)) {
            context.fail(path + "[" + std::to_string(index) + "]", "the expected scalar type");
            continue;
        }
        try {
            destination[index] = value.get<T>();
        } catch (...) {
            context.fail(path + "[" + std::to_string(index) + "]", "representable by the destination type");
        }
    }
}

AimConfig hostAlgorithmDefaults(const CompatibilityMode compatibility) {
    AimConfig config = compatibility == CompatibilityMode::Original
        ? AimConfig::legacyDefaults()
        : AimConfig::correctedDefaults();
    config.target_fps = 120;
    config.tracking.max_frame_age_ms = 80;
    config.auto_fire.enabled = false;
    config.auto_fire.mode = AutoFireMode::MoveOnly;
    config.sanitize();
    return config;
}

} // namespace

AiAimSettings::AiAimSettings()
    : algorithm(hostAlgorithmDefaults(CompatibilityMode::Corrected)) {}

Json serializeAiAimSettings(const AiAimSettings& settings) {
    const auto& algorithm = settings.algorithm;
    return Json{
        {"schema", std::string(kAiAimSettingsSchema)},
        {"schema_version", kAiAimSettingsSchemaVersion},
        {"compatibility", algorithm.compatibility == CompatibilityMode::Original
            ? "original" : "corrected"},
        {"runtime", {
            {"prefer_directml", settings.runtime.prefer_directml},
            {"allow_cpu_fallback", settings.runtime.allow_cpu_fallback},
            {"dml_device_index", settings.runtime.dml_device_index},
            {"capture_adapter_index", settings.runtime.capture_adapter_index},
            {"capture_output_index", settings.runtime.capture_output_index},
            {"target_fps", algorithm.target_fps},
        }},
        {"detector", {
            {"confidence", algorithm.detector.confidence},
            {"iou", algorithm.detector.iou_threshold},
            {"class_aware_nms", algorithm.detector.class_aware_nms},
        }},
        {"targeting", {
            {"model_classes", algorithm.targeting.model_classes},
            {"enabled_classes", algorithm.targeting.enabled_classes},
            {"location", algorithm.targeting.location},
            {"class_location", algorithm.targeting.class_location},
            {"effective_range", algorithm.targeting.effective_range},
            {"target_stickiness", algorithm.targeting.target_stickiness},
        }},
        {"tracking", {
            {"max_frame_age_ms", algorithm.tracking.max_frame_age_ms},
            {"iou_threshold", algorithm.tracking.iou_threshold},
            {"max_lost_frames", algorithm.tracking.max_lost_frames},
        }},
        {"controller", {
            {"p_x", algorithm.controller.p_x}, {"i_x", algorithm.controller.i_x},
            {"d_x", algorithm.controller.d_x}, {"p_y", algorithm.controller.p_y},
            {"i_y", algorithm.controller.i_y}, {"d_y", algorithm.controller.d_y},
            {"fire_no_y_pid", algorithm.controller.fire_no_y_pid},
            {"debounce_enabled", algorithm.controller.debounce_enabled},
            {"debounce_pixels", algorithm.controller.debounce_pixels},
            {"horizontal_fov", algorithm.controller.horizontal_fov},
            {"vertical_fov", algorithm.controller.vertical_fov},
            {"max_pixels", algorithm.controller.max_pixels},
            {"global_scale", algorithm.controller.global_scale},
            {"ads_sensitivity_scale_enabled", algorithm.controller.ads_sensitivity_scale_enabled},
            {"ads_sensitivity_base", algorithm.controller.ads_sensitivity_base},
            {"ads_sensitivity_current", algorithm.controller.ads_sensitivity_current},
            {"auto_scope_sensitivity", algorithm.controller.auto_scope_sensitivity},
            {"scope_sensitivity", algorithm.controller.scope_sensitivity},
        }},
        {"prediction", {
            {"enabled", algorithm.prediction.enabled},
            {"predict_factor", algorithm.prediction.predict_factor},
            {"velocity_deadzone", algorithm.prediction.velocity_deadzone},
            {"automatic_boost_enabled", algorithm.prediction.automatic_boost_enabled},
            {"automatic_scale", algorithm.prediction.automatic_scale},
            {"dmr_boost_enabled", algorithm.prediction.dmr_boost_enabled},
            {"dmr_scale", algorithm.prediction.dmr_scale},
            {"max_lead_pixels", algorithm.prediction.max_lead_pixels},
            {"normal_predict_factor", algorithm.prediction.normal_predict_factor},
            {"normal_velocity_deadzone", algorithm.prediction.normal_velocity_deadzone},
            {"lead_smooth", algorithm.prediction.lead_smooth},
            {"lead_step", algorithm.prediction.lead_step},
            {"lead_y_scale", algorithm.prediction.lead_y_scale},
            {"lead_hold_frames", algorithm.prediction.lead_hold_frames},
            {"jump_reset_pixels", algorithm.prediction.jump_reset_pixels},
        }},
        {"fire_offset", {
            {"enabled", algorithm.fire_offset.enabled},
            {"class_offset", algorithm.fire_offset.class_offset},
            {"distance_scale_enabled", algorithm.fire_offset.distance_scale_enabled},
            {"base_box_height", algorithm.fire_offset.base_box_height},
            {"minimum_distance_scale", algorithm.fire_offset.minimum_distance_scale},
            {"scope_scale", algorithm.fire_offset.scope_scale},
            {"lean_scale", algorithm.fire_offset.lean_scale},
            {"smg_scale", algorithm.fire_offset.smg_scale},
        }},
        {"activation", {
            {"disable_variable_high_zoom", settings.activation.disable_variable_high_zoom},
        }},
        {"display", {{"target_boxes", settings.display.target_boxes}}},
    };
}

AiAimSettings parseAiAimSettings(const nlohmann::json& document) {
    AiAimSettings settings;
    ParseContext context;

    try {
        if (!document.is_object()) {
            context.fail("ai_aim", "an object");
            settings.validation_error = std::move(context.error);
            return settings;
        }
        if (document.empty()) {
            return settings;
        }

        const auto schema = document.find("schema");
        if (schema == document.end()) {
            context.fail("ai_aim.schema", "present");
        } else if (!schema->is_string()) {
            context.fail("ai_aim.schema", "a string");
        } else if (schema->get<std::string_view>() != kAiAimSettingsSchema) {
            context.fail("ai_aim.schema", "pubgassistant.ai_aim");
        }

        const auto schema_version = document.find("schema_version");
        if (schema_version == document.end()) {
            context.fail("ai_aim.schema_version", "present");
        } else if (!schema_version->is_number_integer()) {
            context.fail("ai_aim.schema_version", "an integer");
        } else {
            int parsed_version = 0;
            if (!readIntegralValue(*schema_version, parsed_version)) {
                context.fail("ai_aim.schema_version", "representable by an integer");
            } else if (parsed_version != kAiAimSettingsSchemaVersion) {
                context.fail("ai_aim.schema_version", "the supported version 1");
            }
        }
        if (!context.error.empty()) {
            settings.validation_error = std::move(context.error);
            return settings;
        }

        rejectUnknownKeys(
            document,
            {"schema", "schema_version", "compatibility", "runtime", "detector",
             "targeting", "tracking", "controller", "prediction", "fire_offset", "activation", "display"},
            "ai_aim",
            context);

        std::string compatibility = "corrected";
        readScalar(document, "compatibility", compatibility, "ai_aim", context);
        CompatibilityMode compatibility_mode = CompatibilityMode::Corrected;
        if (compatibility == "original") {
            compatibility_mode = CompatibilityMode::Original;
        } else if (compatibility != "corrected") {
            context.fail("ai_aim.compatibility", "original or corrected");
        }
        settings.algorithm = hostAlgorithmDefaults(compatibility_mode);

        if (const Json* runtime = readObject(document, "runtime", "ai_aim", context)) {
            rejectUnknownKeys(
                *runtime,
                {"prefer_directml", "allow_cpu_fallback", "dml_device_index",
                 "capture_adapter_index", "capture_output_index", "target_fps"},
                "ai_aim.runtime",
                context);
            readScalar(*runtime, "prefer_directml", settings.runtime.prefer_directml,
                       "ai_aim.runtime", context);
            readScalar(*runtime, "allow_cpu_fallback", settings.runtime.allow_cpu_fallback,
                       "ai_aim.runtime", context);
            readScalar(*runtime, "dml_device_index", settings.runtime.dml_device_index,
                       "ai_aim.runtime", context);

            int capture_adapter_index = static_cast<int>(settings.runtime.capture_adapter_index);
            int capture_output_index = static_cast<int>(settings.runtime.capture_output_index);
            readScalar(*runtime, "capture_adapter_index", capture_adapter_index,
                       "ai_aim.runtime", context);
            readScalar(*runtime, "capture_output_index", capture_output_index,
                       "ai_aim.runtime", context);
            settings.runtime.dml_device_index = std::max(0, settings.runtime.dml_device_index);
            settings.runtime.capture_adapter_index = static_cast<unsigned>(
                std::max(0, capture_adapter_index));
            settings.runtime.capture_output_index = static_cast<unsigned>(
                std::max(0, capture_output_index));

            readScalar(*runtime, "target_fps", settings.algorithm.target_fps,
                       "ai_aim.runtime", context);
        }

        if (const Json* tracking = readObject(document, "tracking", "ai_aim", context)) {
            rejectUnknownKeys(
                *tracking, {"max_frame_age_ms", "iou_threshold", "max_lost_frames"}, "ai_aim.tracking", context);
            readScalar(*tracking, "max_frame_age_ms", settings.algorithm.tracking.max_frame_age_ms,
                       "ai_aim.tracking", context);
            readScalar(*tracking, "iou_threshold", settings.algorithm.tracking.iou_threshold,
                       "ai_aim.tracking", context);
            readScalar(*tracking, "max_lost_frames", settings.algorithm.tracking.max_lost_frames,
                       "ai_aim.tracking", context);
        }

        if (const Json* detector = readObject(document, "detector", "ai_aim", context)) {
            rejectUnknownKeys(
                *detector,
                {"confidence", "iou", "class_aware_nms"},
                "ai_aim.detector",
                context);
            readScalar(*detector, "confidence", settings.algorithm.detector.confidence,
                       "ai_aim.detector", context);
            readScalar(*detector, "iou", settings.algorithm.detector.iou_threshold,
                       "ai_aim.detector", context);
            readScalar(*detector, "class_aware_nms", settings.algorithm.detector.class_aware_nms,
                       "ai_aim.detector", context);
        }

        if (const Json* targeting = readObject(document, "targeting", "ai_aim", context)) {
            rejectUnknownKeys(
                *targeting,
                {"model_classes", "enabled_classes", "location", "class_location", "effective_range",
                 "target_stickiness"},
                "ai_aim.targeting",
                context);
            readScalar(*targeting, "model_classes", settings.algorithm.targeting.model_classes,
                       "ai_aim.targeting", context);
            readArray(*targeting, "enabled_classes", settings.algorithm.targeting.enabled_classes,
                      "ai_aim.targeting", context);
            readScalar(*targeting, "location", settings.algorithm.targeting.location,
                       "ai_aim.targeting", context);
            readArray(*targeting, "class_location", settings.algorithm.targeting.class_location,
                      "ai_aim.targeting", context);
            readScalar(*targeting, "effective_range", settings.algorithm.targeting.effective_range,
                       "ai_aim.targeting", context);
            readScalar(*targeting, "target_stickiness", settings.algorithm.targeting.target_stickiness,
                       "ai_aim.targeting", context);
        }

        if (const Json* controller = readObject(document, "controller", "ai_aim", context)) {
            rejectUnknownKeys(
                *controller,
                {"p_x", "i_x", "d_x", "p_y", "i_y", "d_y", "fire_no_y_pid",
                 "debounce_enabled", "debounce_pixels",
                 "horizontal_fov", "vertical_fov", "max_pixels", "global_scale",
                 "ads_sensitivity_scale_enabled", "ads_sensitivity_base",
                 "ads_sensitivity_current", "auto_scope_sensitivity", "scope_sensitivity"},
                "ai_aim.controller",
                context);
            readScalar(*controller, "p_x", settings.algorithm.controller.p_x, "ai_aim.controller", context);
            readScalar(*controller, "i_x", settings.algorithm.controller.i_x, "ai_aim.controller", context);
            readScalar(*controller, "d_x", settings.algorithm.controller.d_x, "ai_aim.controller", context);
            readScalar(*controller, "p_y", settings.algorithm.controller.p_y, "ai_aim.controller", context);
            readScalar(*controller, "i_y", settings.algorithm.controller.i_y, "ai_aim.controller", context);
            readScalar(*controller, "d_y", settings.algorithm.controller.d_y, "ai_aim.controller", context);
            readScalar(*controller, "fire_no_y_pid", settings.algorithm.controller.fire_no_y_pid,
                       "ai_aim.controller", context);
            readScalar(*controller, "debounce_enabled", settings.algorithm.controller.debounce_enabled,
                       "ai_aim.controller", context);
            readScalar(*controller, "debounce_pixels", settings.algorithm.controller.debounce_pixels,
                       "ai_aim.controller", context);
            readScalar(*controller, "horizontal_fov", settings.algorithm.controller.horizontal_fov,
                       "ai_aim.controller", context);
            readScalar(*controller, "vertical_fov", settings.algorithm.controller.vertical_fov,
                       "ai_aim.controller", context);
            readScalar(*controller, "max_pixels", settings.algorithm.controller.max_pixels,
                       "ai_aim.controller", context);
            readScalar(*controller, "global_scale", settings.algorithm.controller.global_scale,
                       "ai_aim.controller", context);
            readScalar(*controller, "ads_sensitivity_scale_enabled",
                       settings.algorithm.controller.ads_sensitivity_scale_enabled,
                       "ai_aim.controller", context);
            readScalar(*controller, "ads_sensitivity_base",
                       settings.algorithm.controller.ads_sensitivity_base,
                       "ai_aim.controller", context);
            readScalar(*controller, "ads_sensitivity_current",
                       settings.algorithm.controller.ads_sensitivity_current,
                       "ai_aim.controller", context);
            readScalar(*controller, "auto_scope_sensitivity",
                       settings.algorithm.controller.auto_scope_sensitivity,
                       "ai_aim.controller", context);
            readArray(*controller, "scope_sensitivity",
                      settings.algorithm.controller.scope_sensitivity,
                      "ai_aim.controller", context);
        }

        if (const Json* prediction = readObject(document, "prediction", "ai_aim", context)) {
            rejectUnknownKeys(
                *prediction,
                {"enabled", "predict_factor", "velocity_deadzone", "automatic_boost_enabled",
                 "automatic_scale", "dmr_boost_enabled", "dmr_scale", "max_lead_pixels",
                 "normal_predict_factor", "normal_velocity_deadzone", "lead_smooth", "lead_step",
                 "lead_y_scale", "lead_hold_frames", "jump_reset_pixels"},
                "ai_aim.prediction",
                context);
            readScalar(*prediction, "enabled", settings.algorithm.prediction.enabled,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "predict_factor", settings.algorithm.prediction.predict_factor,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "velocity_deadzone",
                       settings.algorithm.prediction.velocity_deadzone,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "automatic_boost_enabled",
                       settings.algorithm.prediction.automatic_boost_enabled,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "automatic_scale",
                       settings.algorithm.prediction.automatic_scale,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "dmr_boost_enabled",
                       settings.algorithm.prediction.dmr_boost_enabled,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "dmr_scale", settings.algorithm.prediction.dmr_scale,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "max_lead_pixels",
                       settings.algorithm.prediction.max_lead_pixels,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "normal_predict_factor", settings.algorithm.prediction.normal_predict_factor,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "normal_velocity_deadzone", settings.algorithm.prediction.normal_velocity_deadzone,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "lead_smooth", settings.algorithm.prediction.lead_smooth,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "lead_step", settings.algorithm.prediction.lead_step,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "lead_y_scale", settings.algorithm.prediction.lead_y_scale,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "lead_hold_frames", settings.algorithm.prediction.lead_hold_frames,
                       "ai_aim.prediction", context);
            readScalar(*prediction, "jump_reset_pixels", settings.algorithm.prediction.jump_reset_pixels,
                       "ai_aim.prediction", context);
        }

        if (const Json* offset = readObject(document, "fire_offset", "ai_aim", context)) {
            rejectUnknownKeys(*offset,
                {"enabled", "class_offset", "distance_scale_enabled", "base_box_height",
                 "minimum_distance_scale", "scope_scale", "lean_scale", "smg_scale"},
                "ai_aim.fire_offset", context);
            readScalar(*offset, "enabled", settings.algorithm.fire_offset.enabled, "ai_aim.fire_offset", context);
            readArray(*offset, "class_offset", settings.algorithm.fire_offset.class_offset, "ai_aim.fire_offset", context);
            readScalar(*offset, "distance_scale_enabled", settings.algorithm.fire_offset.distance_scale_enabled,
                       "ai_aim.fire_offset", context);
            readScalar(*offset, "base_box_height", settings.algorithm.fire_offset.base_box_height,
                       "ai_aim.fire_offset", context);
            readScalar(*offset, "minimum_distance_scale", settings.algorithm.fire_offset.minimum_distance_scale,
                       "ai_aim.fire_offset", context);
            readArray(*offset, "scope_scale", settings.algorithm.fire_offset.scope_scale, "ai_aim.fire_offset", context);
            readScalar(*offset, "lean_scale", settings.algorithm.fire_offset.lean_scale, "ai_aim.fire_offset", context);
            readScalar(*offset, "smg_scale", settings.algorithm.fire_offset.smg_scale, "ai_aim.fire_offset", context);
        }

        if (const Json* activation = readObject(document, "activation", "ai_aim", context)) {
            rejectUnknownKeys(
                *activation,
                {"disable_variable_high_zoom"},
                "ai_aim.activation",
                context);
            readScalar(*activation, "disable_variable_high_zoom",
                       settings.activation.disable_variable_high_zoom,
                       "ai_aim.activation", context);
        }

        if (const Json* display = readObject(document, "display", "ai_aim", context)) {
            rejectUnknownKeys(*display, {"target_boxes"}, "ai_aim.display", context);
            readScalar(*display, "target_boxes", settings.display.target_boxes, "ai_aim.display", context);
        }

        // Host safety invariants are not configurable in JSON.
        settings.algorithm.auto_fire.enabled = false;
        settings.algorithm.auto_fire.mode = AutoFireMode::MoveOnly;
        settings.algorithm.sanitize();
        settings.validation_error = std::move(context.error);
    } catch (const std::exception& error) {
        settings.validation_error = std::string("ai_aim parse failed: ") + error.what();
    } catch (...) {
        settings.validation_error = "ai_aim parse failed: unknown error";
    }
    return settings;
}

} // namespace pubg
