#pragma once

#include "pubg_ai_aim/AimConfig.hpp"
#include "pubg_ai_aim/AimTypes.hpp"

#include <memory>

namespace pubg::ai_aim {

class AimEngine final {
public:
    explicit AimEngine(AimConfig config = AimConfig::correctedDefaults());
    ~AimEngine();

    AimEngine(AimEngine&&) noexcept;
    AimEngine& operator=(AimEngine&&) noexcept;
    AimEngine(const AimEngine&) = delete;
    AimEngine& operator=(const AimEngine&) = delete;

    void updateConfig(AimConfig config);
    void reset() noexcept;

    [[nodiscard]] AimDecision process(
        const DetectionFrame& frame,
        const CombatContext& combat,
        const ActivationState& activation);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pubg::ai_aim
