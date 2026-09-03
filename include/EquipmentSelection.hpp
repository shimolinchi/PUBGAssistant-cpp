#pragma once

#include "WeaponSlotInfo.hpp"

#include <map>
#include <optional>
#include <string>

namespace pubg {

// Shared host rule for choosing attachments for the currently recognized
// weapon. When both slots contain the same weapon, preserve the established
// recoil convention of preferring slot 2.
[[nodiscard]] inline std::optional<WeaponSlotInfo> recognizedEquipmentForWeapon(
    const std::map<int, WeaponSlotInfo>& equipment,
    const std::string& weapon) {
    if (weapon.empty()) {
        return std::nullopt;
    }

    const auto slot1 = equipment.find(1);
    const auto slot2 = equipment.find(2);
    if (slot1 != equipment.end() && slot2 != equipment.end() &&
        slot1->second.name == weapon && slot2->second.name == weapon) {
        return slot2->second;
    }
    for (const auto& [_, info] : equipment) {
        if (info.name == weapon) {
            return info;
        }
    }
    return std::nullopt;
}

} // namespace pubg
