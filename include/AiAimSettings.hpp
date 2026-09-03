#pragma once

#include "pubg_ai_aim/AimConfig.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace pubg {

inline constexpr std::string_view kAiAimSettingsSchema = "pubgassistant.ai_aim";
inline constexpr int kAiAimSettingsSchemaVersion = 1;

struct AiAimRuntimeSettings {
    bool prefer_directml = true;
    bool allow_cpu_fallback = false;
    int dml_device_index = 0;
    unsigned capture_adapter_index = 0;
    unsigned capture_output_index = 0;
    bool operator==(const AiAimRuntimeSettings&) const = default;
};

struct AiAimActivationPolicy {
    bool disable_variable_high_zoom = true;
};

struct AiAimDisplaySettings {
    bool target_boxes = false;
};

// Host-owned snapshot; saved parameters are applied via the coordinator.
// The reusable AI module deliberately stays JSON-free.
struct AiAimSettings {
    AiAimSettings();

    ai_aim::AimConfig algorithm;
    AiAimRuntimeSettings runtime;
    AiAimActivationPolicy activation;
    AiAimDisplaySettings display;
    std::string validation_error;

    [[nodiscard]] bool valid() const noexcept { return validation_error.empty(); }
    [[nodiscard]] std::chrono::milliseconds motionMaxAge() const noexcept {
        return std::chrono::milliseconds{algorithm.tracking.max_frame_age_ms};
    }
};

// Missing/empty settings use safe host defaults. A non-empty document must identify
// the host schema and version; ordinary validation failures return a diagnostic.
[[nodiscard]] AiAimSettings parseAiAimSettings(const nlohmann::json& document);

// Serialize only the host schema's supported fields; module-internal parameters
// and the non-configurable move-only safety gates are never exposed in JSON.
[[nodiscard]] nlohmann::json serializeAiAimSettings(const AiAimSettings& settings);

} // namespace pubg
