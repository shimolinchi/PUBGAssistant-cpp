#pragma once

#include "EquipmentSelection.hpp"
#include "pubg_ai_aim/AimTypes.hpp"

#include <map>
#include <optional>
#include <string>

namespace pubg {

// Snapshot of recognition/input state supplied by the host application. The AI
// library remains independent of Config, EquipmentDetector, and App.
struct AiAimHostCombatSnapshot final {
    std::string weapon;
    std::string stance;
    std::map<int, WeaponSlotInfo> equipment;
    bool weapon_supported = false;
    bool firing = false;
    bool ads = false;
    bool lean_left = false;
    bool lean_right = false;
    bool disable_variable_high_zoom = true;
};

[[nodiscard]] pubg::ai_aim::CombatContext buildAiAimCombatContext(
    const AiAimHostCombatSnapshot& host);

} // namespace pubg
