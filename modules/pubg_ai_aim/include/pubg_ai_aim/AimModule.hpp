#pragma once

#include "pubg_ai_aim/AimConfig.hpp"
#include "pubg_ai_aim/AimTypes.hpp"
#include "pubg_ai_aim/Interfaces.hpp"

#include <memory>

namespace pubg::ai_aim {

class AimModule final {
public:
    AimModule(
        AimConfig config,
        std::shared_ptr<IMotionSink> motion_sink,
        std::shared_ptr<ITriggerSink> trigger_sink = {});

    AimModule(
        AimConfig config,
        std::unique_ptr<IFrameSource> frame_source,
        std::unique_ptr<IObjectDetector> detector,
        std::shared_ptr<IMotionSink> motion_sink,
        std::shared_ptr<ITriggerSink> trigger_sink = {});
    ~AimModule();

    AimModule(const AimModule&) = delete;
    AimModule& operator=(const AimModule&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;
    void setEnabled(bool enabled);
    void updateConfig(AimConfig config);
    void updateCombatContext(CombatContext context);
    void updateActivation(ActivationState state);
    [[nodiscard]] bool submitDetections(DetectionFrame frame);
    void resetTracking();
    [[nodiscard]] AimSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pubg::ai_aim
