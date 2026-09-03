#pragma once

#include "pubg_ai_aim/AimTypes.hpp"
#include "pubg_ai_aim/AimConfig.hpp"

#include <stop_token>
#include <string_view>
#include <vector>

namespace pubg::ai_aim {

enum class CaptureStatus {
    FrameReady,
    NoFrame,
    RecoverableError,
    Stopped,
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual CaptureStatus capture(Frame& frame, std::stop_token stop) = 0;
};

class IObjectDetector {
public:
    virtual ~IObjectDetector() = default;
    virtual std::vector<Detection> infer(const Frame& frame, std::stop_token stop) = 0;

    // Detectors that own a YOLO decoder may accept the module's sanitized
    // decoder settings. Custom/external detectors can keep the default no-op.
    virtual bool updateDecoderConfig(
        const DetectorConfig& config,
        CompatibilityMode compatibility) {
        (void)config;
        (void)compatibility;
        return false;
    }

    // Reports the provider that is actually active, not merely requested.
    [[nodiscard]] virtual std::string_view backendName() const noexcept {
        return "custom";
    }

    std::vector<Detection> infer(const Frame& frame) {
        return infer(frame, {});
    }
};

class IMotionSink {
public:
    virtual ~IMotionSink() = default;
    virtual void submitAimMotion(const MotionCommand& command) = 0;
};

class ITriggerSink {
public:
    virtual ~ITriggerSink() = default;
    virtual void click(Clock::time_point timestamp) = 0;
};

} // namespace pubg::ai_aim
