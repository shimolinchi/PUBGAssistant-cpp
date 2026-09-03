#include "AiAimHudIndicator.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testDisabledAlwaysShowsRed() {
    for (const std::string_view weapon : {"", "M416", "MK47", "VSS"}) {
        const auto state = pubg::aiAimHudState(false, weapon);
        require(state == pubg::AiAimHudState::Off,
                "disabled AI indicator depended on the recognized weapon");
        require(std::string_view(pubg::aiAimHudColorHex(state)) == "#E74C3C",
                "disabled AI indicator is not red");
    }
}

void testEnabledWithoutWeaponShowsYellow() {
    const auto state = pubg::aiAimHudState(true, "");
    require(state == pubg::AiAimHudState::WaitingForWeapon,
            "enabled AI without a recognized weapon did not wait for weapon");
    require(std::string_view(pubg::aiAimHudColorHex(state)) == "#E3D43C",
            "enabled AI without a recognized weapon is not yellow");
}

void testEnabledWithWeaponShowsGreen() {
    // Special weapons are also recognition results. The HUD must not duplicate
    // the separate AI weapon-support gate or infer recognition from backend text.
    for (const std::string_view weapon : {"M416", "MK47", "VSS", "Grenade", "Mortar"}) {
        const auto state = pubg::aiAimHudState(true, weapon);
        require(state == pubg::AiAimHudState::WeaponRecognized,
                "enabled AI ignored an existing weapon-recognition result");
        require(std::string_view(pubg::aiAimHudColorHex(state)) == "#109166",
                "enabled AI with a recognized weapon is not green");
    }
}

void testLabelAlwaysUsesTwoCharacters() {
    require(std::string_view(pubg::kAiAimHudLabel) == "自瞄",
            "AI indicator no longer uses the fixed two-character label");
}

void testRecognitionFrequencyFormatting() {
    require(pubg::aiAimRecognitionFrequencyText(true, 59.96) == "fps: 60.0",
            "frequency must show lowercase fps and one rounded decimal without a Hz suffix");
    require(pubg::aiAimRecognitionFrequencyText(true, 123.24) == "fps: 123.2",
            "frequency was capped at the HUD refresh rate");
    require(pubg::aiAimRecognitionFrequencyText(false, 123.24) == "fps: 0.0",
            "disabled HUD retained the previous frequency");
    for (const double invalid : {-1.0, -0.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        require(pubg::aiAimRecognitionFrequencyText(true, invalid) == "fps: 0.0",
                "invalid frequency reached the HUD");
    }
}

} // namespace

int main() {
    try {
        testDisabledAlwaysShowsRed();
        testEnabledWithoutWeaponShowsYellow();
        testEnabledWithWeaponShowsGreen();
        testLabelAlwaysUsesTwoCharacters();
        testRecognitionFrequencyFormatting();
        std::cout << "AI aim HUD indicator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AI aim HUD indicator tests failed: " << error.what() << '\n';
        return 1;
    }
}
