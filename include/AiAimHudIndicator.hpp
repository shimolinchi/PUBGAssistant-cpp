#pragma once

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace pubg {

enum class AiAimHudState {
    Off,
    WaitingForWeapon,
    WeaponRecognized,
};

inline constexpr const char* kAiAimHudLabel = "自瞄";

[[nodiscard]] inline std::string aiAimRecognitionFrequencyText(bool enabled, double hertz) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "fps: " << std::fixed << std::setprecision(1)
         << (enabled && std::isfinite(hertz) && hertz > 0.0 ? hertz : 0.0);
    return text.str();
}

// The existing weapon detector represents no recognized weapon with an empty
// name. This indicator describes the switch and recognition, not backend health
// or whether the current weapon is supported for automatic aiming.
[[nodiscard]] constexpr AiAimHudState aiAimHudState(
    bool enabled, std::string_view recognized_weapon) noexcept {
    if (!enabled) {
        return AiAimHudState::Off;
    }
    return recognized_weapon.empty() ? AiAimHudState::WaitingForWeapon
                                     : AiAimHudState::WeaponRecognized;
}

// Keep status colors independent of the configurable map-marker palette.
[[nodiscard]] constexpr const char* aiAimHudColorHex(AiAimHudState state) noexcept {
    switch (state) {
    case AiAimHudState::WeaponRecognized:
        return "#109166";
    case AiAimHudState::WaitingForWeapon:
        return "#E3D43C";
    case AiAimHudState::Off:
    default:
        return "#E74C3C";
    }
}

} // namespace pubg
