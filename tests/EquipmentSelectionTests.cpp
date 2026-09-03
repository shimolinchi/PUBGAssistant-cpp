#include "EquipmentSelection.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

pubg::WeaponSlotInfo equipment(
    std::string name,
    std::string scope,
    std::string grip = {}) {
    pubg::WeaponSlotInfo info;
    info.name = std::move(name);
    info.scope = std::move(scope);
    info.grip = std::move(grip);
    return info;
}

void testDuplicateWeaponPrefersSecondSlot() {
    const std::map<int, pubg::WeaponSlotInfo> slots{
        {1, equipment("M416", "2", "vertical")},
        {2, equipment("M416", "4", "half")},
    };
    const auto selected = pubg::recognizedEquipmentForWeapon(slots, "M416");
    require(selected.has_value(), "matching equipment was not selected");
    require(selected->scope == "4" && selected->grip == "half",
            "duplicate weapon did not prefer slot 2");
}

void testSingleMatchAndMissingWeapon() {
    const std::map<int, pubg::WeaponSlotInfo> slots{
        {1, equipment("AKM", "3")},
        {2, equipment("M416", "4")},
    };
    const auto selected = pubg::recognizedEquipmentForWeapon(slots, "AKM");
    require(selected.has_value() && selected->scope == "3",
            "single matching slot was not selected");
    require(!pubg::recognizedEquipmentForWeapon(slots, "").has_value(),
            "empty current weapon must not select equipment");
    require(!pubg::recognizedEquipmentForWeapon(slots, "AUG").has_value(),
            "unknown current weapon must not select unrelated equipment");
}

} // namespace

int main() {
    try {
        testDuplicateWeaponPrefersSecondSlot();
        testSingleMatchAndMissingWeapon();
        std::cout << "EquipmentSelection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "EquipmentSelection tests failed: " << error.what() << '\n';
        return 1;
    }
}
