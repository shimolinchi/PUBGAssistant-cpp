#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

namespace pubg {

struct PrimaryHotkeyBinding {
    std::string_view action;
    std::string_view key;
};

// 四个主开关和大地图测距的默认值。运行时、设置页和重置逻辑共用此表；
// 配置中已存在的合法字符串始终优先，不会因默认值调整而被覆盖。
inline constexpr std::array<PrimaryHotkeyBinding, 5> kPrimaryHotkeyDefaults{{
    {"toggle_weapon_detection", "<f1>"},
    {"toggle_display", "<f2>"},
    {"toggle_recoil", "<f3>"},
    {"toggle_ai_aim", "<f4>"},
    {"measure_map", "<f5>"},
}};

[[nodiscard]] constexpr const char* primaryHotkeyDefault(
    const std::string_view action) noexcept {
    for (const auto& binding : kPrimaryHotkeyDefaults) {
        if (binding.action == action) {
            return binding.key.data();
        }
    }
    return "";
}

using HotkeyStringMap = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] constexpr bool equivalentHotkey(
    const std::string_view left,
    const std::string_view right) noexcept {
    auto next = [](const std::string_view value, std::size_t& index) constexpr {
        while (index < value.size()) {
            const unsigned char current = static_cast<unsigned char>(value[index++]);
            if (current == '<' || current == '>' || current == ' ' ||
                current == '\t' || current == '\r' || current == '\n') {
                continue;
            }
            return current >= 'A' && current <= 'Z'
                ? static_cast<char>(current - 'A' + 'a')
                : static_cast<char>(current);
        }
        return '\0';
    };

    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (true) {
        const char left_char = next(left, left_index);
        const char right_char = next(right, right_index);
        if (left_char != right_char) {
            return false;
        }
        if (left_char == '\0') {
            return true;
        }
    }
}

[[nodiscard]] inline HotkeyStringMap withAvailablePrimaryHotkeyDefaults(
    const HotkeyStringMap& configured) {
    HotkeyStringMap result = configured;
    for (const auto& binding : kPrimaryHotkeyDefaults) {
        if (result.contains(binding.action)) {
            continue;
        }
        bool occupied = false;
        for (const auto& [other_action, other_key] : result) {
            (void)other_action;
            if (equivalentHotkey(other_key, binding.key)) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            result.emplace(std::string(binding.action), std::string(binding.key));
        }
    }
    return result;
}

} // namespace pubg
