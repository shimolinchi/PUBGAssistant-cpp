#include "AiAimSettings.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(const float actual, const float expected) {
    return std::abs(actual - expected) < 0.0001F;
}

Json validDocument() {
    return Json{
        {"schema", std::string(pubg::kAiAimSettingsSchema)},
        {"schema_version", pubg::kAiAimSettingsSchemaVersion},
    };
}

void testEmptyDocumentUsesHostDefaults() {
    const auto settings = pubg::parseAiAimSettings(Json::object());

    require(settings.valid(), "empty AI settings should use defaults");
    require(settings.algorithm.target_fps == 120, "host target FPS default drifted");
    require(settings.algorithm.tracking.max_frame_age_ms == 80,
            "host frame-age default drifted");
    require(settings.motionMaxAge() == 80ms,
            "motion age must derive from the algorithm frame age");
    require(settings.runtime.prefer_directml, "DirectML should be preferred by default");
    require(!settings.runtime.allow_cpu_fallback,
            "CPU fallback should require an explicit opt-in");
    require(settings.activation.disable_variable_high_zoom,
            "variable high zoom should be blocked by default");
    require(!settings.algorithm.auto_fire.enabled,
            "host defaults must never enable auto fire");
    require(settings.algorithm.auto_fire.mode == pubg::ai_aim::AutoFireMode::MoveOnly,
            "host defaults must remain move-only");
}

void testCheckedInHostConfigUsesTheSameContract() {
    const std::filesystem::path path =
        std::filesystem::path(PUBG_TEST_SOURCE_DIR) / "config" / "ai_aim_settings.json";
    std::ifstream input(path);
    require(input.good(), "checked-in AI settings file is missing");

    Json document;
    input >> document;
    const auto settings = pubg::parseAiAimSettings(document);

    require(settings.valid(), "checked-in AI settings failed typed validation");
    require(settings.algorithm.target_fps == 120, "target FPS was not mapped");
    require(settings.algorithm.detector.input_width == 320, "detector width was not mapped");
    require(settings.algorithm.detector.anchor_count == 2100, "fixed model contract drifted");
    require(near(settings.algorithm.detector.confidence, 0.404F),
            "detector confidence was not mapped");
    require(settings.algorithm.targeting.enabled_classes[1],
            "enabled target classes were not mapped");
    require(near(settings.algorithm.controller.scope_sensitivity[2], 42.0F),
            "scope sensitivity was not mapped");
    require(near(settings.algorithm.prediction.predict_factor, 2.195F),
            "prediction settings were not mapped");
    require(settings.motionMaxAge() ==
                std::chrono::milliseconds{settings.algorithm.tracking.max_frame_age_ms},
            "algorithm and dispatcher frame ages diverged");
}

void testSchemaAndTypeErrorsAreRejectedWithoutThrowing() {
    Json wrong_schema = validDocument();
    wrong_schema["schema"] = "pubg_ai_aim.example";
    require(!pubg::parseAiAimSettings(wrong_schema).valid(),
            "foreign AI settings schema was accepted");

    Json future_version = validDocument();
    future_version["schema_version"] = 2;
    require(!pubg::parseAiAimSettings(future_version).valid(),
            "future AI settings schema version was accepted");

    Json wrapped_version = validDocument();
    wrapped_version["schema_version"] = std::uint64_t{4'294'967'297ULL};
    require(!pubg::parseAiAimSettings(wrapped_version).valid(),
            "out-of-range schema version wrapped to version 1");

    Json wrong_type = validDocument();
    wrong_type["runtime"] = Json{{"target_fps", "fast"}};
    const auto typed = pubg::parseAiAimSettings(wrong_type);
    require(!typed.valid(), "wrong scalar type was accepted");
    require(typed.algorithm.target_fps == 120,
            "wrong scalar type did not retain the safe default");

    Json oversized_integer = validDocument();
    oversized_integer["runtime"] = Json{
        {"target_fps", std::numeric_limits<std::uint64_t>::max()},
    };
    const auto oversized = pubg::parseAiAimSettings(oversized_integer);
    require(!oversized.valid(), "out-of-range integer was silently narrowed");
    require(oversized.algorithm.target_fps == 120,
            "out-of-range integer did not retain the safe default");

    Json typo = validDocument();
    typo["runtime"] = Json{{"target_fpss", 120}};
    require(!pubg::parseAiAimSettings(typo).valid(),
            "unknown field typo was silently accepted");

    Json fixed_model_shape = validDocument();
    fixed_model_shape["detector"] = Json{{"anchors", 999}};
    require(!pubg::parseAiAimSettings(fixed_model_shape).valid(),
            "fixed model structure was exposed as a tunable field");
}

void testSanitizeAndSafetyInvariants() {
    Json document = validDocument();
    document["runtime"] = Json{
        {"target_fps", 5000},
        {"dml_device_index", -4},
        {"capture_adapter_index", -3},
        {"capture_output_index", -2},
    };
    document["tracking"] = Json{{"max_frame_age_ms", -20}};
    document["activation"] = Json{
        {"disable_variable_high_zoom", false},
        {"auto_fire", true},
        {"require_game_foreground", false},
        {"block_on_inventory_map_menu", false},
    };

    const auto settings = pubg::parseAiAimSettings(document);
    require(!settings.valid(), "removed safety-policy fields were silently accepted");
    require(settings.algorithm.target_fps == 1000, "target FPS was not sanitized");
    require(settings.algorithm.tracking.max_frame_age_ms == 1,
            "frame age was not sanitized");
    require(settings.motionMaxAge() == 1ms,
            "sanitized frame age was not reused for output expiry");
    require(settings.runtime.dml_device_index == 0 &&
                settings.runtime.capture_adapter_index == 0 &&
                settings.runtime.capture_output_index == 0,
            "negative runtime indices were not clamped");
    require(!settings.activation.disable_variable_high_zoom,
            "supported high-zoom policy was not mapped");
    require(!settings.algorithm.auto_fire.enabled,
            "configuration bypassed the move-only safety invariant");
    require(settings.algorithm.auto_fire.mode == pubg::ai_aim::AutoFireMode::MoveOnly,
            "configuration changed the move-only safety invariant");
}

void testArraysOnlyOverrideProvidedValidSlots() {
    Json document = validDocument();
    document["targeting"] = Json{{"enabled_classes", Json::array({true, false})}};
    document["controller"] = Json{{"scope_sensitivity", Json::array({11.0, 22.0})}};

    const auto settings = pubg::parseAiAimSettings(document);
    require(settings.valid(), "partial arrays should preserve unspecified defaults");
    require(settings.algorithm.targeting.enabled_classes[0] &&
                !settings.algorithm.targeting.enabled_classes[1] &&
                !settings.algorithm.targeting.enabled_classes[2],
            "partial class array did not preserve typed defaults");
    require(near(settings.algorithm.controller.scope_sensitivity[0], 11.0F) &&
                near(settings.algorithm.controller.scope_sensitivity[1], 22.0F) &&
                near(settings.algorithm.controller.scope_sensitivity[2], 42.0F),
            "partial scope array did not preserve typed defaults");

    Json oversized = validDocument();
    oversized["targeting"] = Json{
        {"enabled_classes", Json::array({true, false, false, false, true})},
    };
    require(!pubg::parseAiAimSettings(oversized).valid(),
            "oversized fixed-capacity array was silently truncated");
}

void testSerializationCoversEverySupportedFieldAndBothModes() {
    Json document{
        {"schema", std::string(pubg::kAiAimSettingsSchema)},
        {"schema_version", pubg::kAiAimSettingsSchemaVersion},
        {"compatibility", "original"},
        {"runtime", {
            {"prefer_directml", false}, {"allow_cpu_fallback", true},
            {"dml_device_index", 2}, {"capture_adapter_index", 3},
            {"capture_output_index", 4}, {"target_fps", 144},
        }},
        {"detector", {
            {"confidence", 0.625}, {"iou", 0.75}, {"class_aware_nms", false},
        }},
        {"targeting", {
            {"model_classes", false}, {"enabled_classes", {true, false, true, false}},
            {"class_location", {0.25, 0.75, 0.0, 0.0}},
            {"location", 0.25}, {"effective_range", 256.0}, {"target_stickiness", 2.0},
        }},
        {"tracking", {{"max_frame_age_ms", 90}, {"iou_threshold", 0.5}, {"max_lost_frames", 9}}},
        {"controller", {
            {"p_x", 1.25}, {"i_x", 0.25}, {"d_x", 0.125},
            {"p_y", 1.5}, {"i_y", 0.125}, {"d_y", 0.25},
            {"fire_no_y_pid", true}, {"debounce_enabled", true}, {"debounce_pixels", 3.5},
            {"horizontal_fov", 105.0}, {"vertical_fov", 95.0},
            {"max_pixels", 25.0}, {"global_scale", 0.75},
            {"ads_sensitivity_scale_enabled", false}, {"ads_sensitivity_base", 35.0},
            {"ads_sensitivity_current", 45.0}, {"auto_scope_sensitivity", false},
            {"scope_sensitivity", {31.0, 32.0, 33.0, 34.0, 35.0, 36.0}},
        }},
        {"prediction", {
            {"enabled", false}, {"predict_factor", 1.5}, {"velocity_deadzone", 1.25},
            {"automatic_boost_enabled", false}, {"automatic_scale", 1.75},
            {"dmr_boost_enabled", false}, {"dmr_scale", 2.5}, {"max_lead_pixels", 85.0},
            {"normal_predict_factor", 3.5}, {"normal_velocity_deadzone", 4.25},
            {"lead_smooth", 0.25}, {"lead_step", 12.5}, {"lead_y_scale", 0.5},
            {"lead_hold_frames", 15}, {"jump_reset_pixels", 110.0},
        }},
        {"fire_offset", {
            {"enabled", false}, {"class_offset", {12.5, -25.0, 0.0, 0.0}},
            {"distance_scale_enabled", false}, {"base_box_height", 100.0},
            {"minimum_distance_scale", 0.5}, {"scope_scale", {0.5, 1.0, 1.5, 2.0}},
            {"lean_scale", 0.75}, {"smg_scale", 0.25},
        }},
        {"activation", {{"disable_variable_high_zoom", false}}},
        {"display", {{"target_boxes", true}}},
    };
    for (const char* mode : {"original", "corrected"}) {
        document["compatibility"] = mode;
        const auto settings = pubg::parseAiAimSettings(document);
        require(settings.valid(), "full supported settings document failed validation");
        const Json serialized = pubg::serializeAiAimSettings(settings);
        require(serialized == document,
                "serialization omitted or changed a supported schema field");
        const auto restored = pubg::parseAiAimSettings(serialized);
        require(restored.valid() && pubg::serializeAiAimSettings(restored) == serialized,
                "AI settings serialization was not round-trip stable");
        require(!restored.algorithm.auto_fire.enabled &&
                    restored.algorithm.auto_fire.mode == pubg::ai_aim::AutoFireMode::MoveOnly,
                "round-trip changed the move-only safety invariant");
    }

    const Json defaults = pubg::serializeAiAimSettings(pubg::AiAimSettings{});
    require(pubg::parseAiAimSettings(defaults).valid(), "serialized defaults are invalid");
    require(!defaults.contains("auto_fire") &&
                !defaults["activation"].contains("require_game_foreground") &&
                !defaults["detector"].contains("input_width"),
            "serializer exposed non-configurable module internals or safety gates");
}

void testNewParametersKeepLegacyDefaultsAndValidateTypes() {
    const auto legacy = pubg::parseAiAimSettings(validDocument());
    const auto defaults = pubg::AiAimSettings{};
    require(legacy.valid() && pubg::serializeAiAimSettings(legacy) == pubg::serializeAiAimSettings(defaults),
            "old documents no longer retain the original algorithm defaults");
    require(!legacy.display.target_boxes, "legacy settings unexpectedly enabled target boxes");
    require(near(legacy.algorithm.controller.p_x, 0.881F) &&
            near(legacy.algorithm.controller.p_y, 0.456F) && legacy.algorithm.fire_offset.enabled,
            "exposing settings silently changed speed or firing defaults");

    const char* numeric_paths[]{
        "/controller/p_x", "/controller/i_x", "/controller/d_x", "/controller/p_y",
        "/controller/i_y", "/controller/d_y", "/controller/debounce_pixels",
        "/tracking/iou_threshold", "/tracking/max_lost_frames",
        "/targeting/class_location/0", "/prediction/normal_predict_factor",
        "/prediction/normal_velocity_deadzone", "/prediction/lead_smooth",
        "/prediction/lead_step", "/prediction/lead_y_scale", "/prediction/lead_hold_frames",
        "/prediction/jump_reset_pixels", "/fire_offset/class_offset/1",
        "/fire_offset/base_box_height", "/fire_offset/minimum_distance_scale",
        "/fire_offset/scope_scale/2", "/fire_offset/lean_scale", "/fire_offset/smg_scale",
    };
    for (const char* path : numeric_paths) {
        auto invalid = pubg::serializeAiAimSettings(defaults);
        invalid[Json::json_pointer(path)] = "invalid";
        require(!pubg::parseAiAimSettings(invalid).valid(), "new numeric parameter accepted a string");
    }
    for (const char* path : {"/controller/fire_no_y_pid", "/controller/debounce_enabled",
                             "/fire_offset/enabled", "/fire_offset/distance_scale_enabled", "/display/target_boxes"}) {
        auto invalid = pubg::serializeAiAimSettings(defaults);
        invalid[Json::json_pointer(path)] = 1;
        require(!pubg::parseAiAimSettings(invalid).valid(), "new checkbox parameter accepted an integer");
    }
    auto partial = validDocument();
    partial["targeting"]["class_location"] = Json::array({0.25});
    partial["fire_offset"]["class_offset"] = Json::array({5.0});
    partial["fire_offset"]["scope_scale"] = Json::array({0.75});
    const auto parsed = pubg::parseAiAimSettings(partial);
    require(parsed.valid() && near(parsed.algorithm.targeting.class_location[1], 0.723F) &&
            near(parsed.algorithm.fire_offset.class_offset[1], 120.0F) &&
            near(parsed.algorithm.fire_offset.scope_scale[2], 1.5F),
            "new partial arrays did not preserve unspecified defaults");
}

} // namespace

int main() {
    try {
        testEmptyDocumentUsesHostDefaults();
        testCheckedInHostConfigUsesTheSameContract();
        testSchemaAndTypeErrorsAreRejectedWithoutThrowing();
        testSanitizeAndSafetyInvariants();
        testArraysOnlyOverrideProvidedValidSlots();
        testSerializationCoversEverySupportedFieldAndBothModes();
        testNewParametersKeepLegacyDefaultsAndValidateTypes();
        std::cout << "AiAimSettings tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AiAimSettings tests failed: " << error.what() << '\n';
        return 1;
    }
}
