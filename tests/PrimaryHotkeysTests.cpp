#include "PrimaryHotkeys.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testPrimaryDefaultsFollowF1ThroughF5() {
    constexpr std::array<std::string_view, 5> expected_actions{
        "toggle_weapon_detection",
        "toggle_display",
        "toggle_recoil",
        "toggle_ai_aim",
        "measure_map",
    };
    constexpr std::array<std::string_view, 5> expected_keys{
        "<f1>", "<f2>", "<f3>", "<f4>", "<f5>",
    };

    require(pubg::kPrimaryHotkeyDefaults.size() == expected_actions.size(),
            "primary hotkey count changed unexpectedly");
    for (std::size_t index = 0; index < expected_actions.size(); ++index) {
        const auto& binding = pubg::kPrimaryHotkeyDefaults[index];
        require(binding.action == expected_actions[index],
                "primary action order does not match the four switches plus map measurement");
        require(binding.key == expected_keys[index],
                "primary action is assigned to the wrong default function key");
        require(std::string_view(pubg::primaryHotkeyDefault(binding.action)) == binding.key,
                "primary hotkey lookup disagrees with the canonical table");
    }

    for (std::size_t left = 0; left < expected_keys.size(); ++left) {
        for (std::size_t right = left + 1; right < expected_keys.size(); ++right) {
            require(expected_keys[left] != expected_keys[right],
                    "two primary actions share the same default key");
        }
    }
}

void testEveryExistingBindingRemainsAuthoritative() {
    const pubg::HotkeyStringMap configured{
        {"toggle_weapon_detection", "<f2>"},
        {"toggle_display", "<ctrl>+<shift>+<space>"},
        {"toggle_recoil", "<ctrl>+<shift>+<tab>"},
        {"toggle_ai_aim", "<f5>"},
        {"measure_map", "<f4>"},
        {"custom_action", "<alt>+m"},
    };

    const auto resolved = pubg::withAvailablePrimaryHotkeyDefaults(configured);
    require(resolved == configured,
            "existing user bindings were overwritten by program defaults");
}

void testOnlyMissingBindingsReceiveDefaults() {
    const pubg::HotkeyStringMap configured{
        {"toggle_ai_aim", "<f9>"},
        {"custom_action", "<f10>"},
    };
    const auto resolved = pubg::withAvailablePrimaryHotkeyDefaults(configured);

    require(resolved.at("toggle_weapon_detection") == "<f1>",
            "missing weapon-detection key did not receive its default");
    require(resolved.at("toggle_display") == "<f2>",
            "missing display key did not receive its default");
    require(resolved.at("toggle_recoil") == "<f3>",
            "missing recoil key did not receive its default");
    require(resolved.at("toggle_ai_aim") == "<f9>",
            "configured AI key was replaced by the F4 default");
    require(resolved.at("measure_map") == "<f5>",
            "missing map-measure key did not receive its default");
    require(resolved.at("custom_action") == "<f10>",
            "an unrelated custom binding was changed");
}

void testOccupiedDefaultLeavesMissingActionUnbound() {
    const pubg::HotkeyStringMap legacy_config{
        {"measure_map", "<F4>"},
        {"custom_action", "F1"},
    };
    const auto resolved = pubg::withAvailablePrimaryHotkeyDefaults(legacy_config);

    require(!resolved.contains("toggle_ai_aim"),
            "missing AI action reused the F4 key already assigned to map measurement");
    require(!resolved.contains("toggle_weapon_detection"),
            "missing weapon action reused the F1 key already assigned to a custom action");
    require(resolved.at("measure_map") == "<F4>",
            "legacy map binding was changed while resolving defaults");
    require(resolved.at("custom_action") == "F1",
            "custom binding was changed while resolving defaults");
}

void testEquivalentHotkeysIgnoreStoredFormatting() {
    require(pubg::equivalentHotkey("<F4>", "f4"),
            "F4 formatting differences were treated as different hotkeys");
    require(pubg::equivalentHotkey(" <CTRL> + <F4> ", "ctrl+f4"),
            "modifier formatting differences were treated as different hotkeys");
    require(!pubg::equivalentHotkey("<f4>", "<f5>"),
            "different function keys were treated as equivalent");
}

void testUnknownActionHasNoInventedDefault() {
    require(std::string_view(pubg::primaryHotkeyDefault("unknown_action")).empty(),
            "unknown action unexpectedly received a default hotkey");
}

} // namespace

int main() {
    try {
        testPrimaryDefaultsFollowF1ThroughF5();
        testEveryExistingBindingRemainsAuthoritative();
        testOnlyMissingBindingsReceiveDefaults();
        testOccupiedDefaultLeavesMissingActionUnbound();
        testEquivalentHotkeysIgnoreStoredFormatting();
        testUnknownActionHasNoInventedDefault();
        std::cout << "Primary hotkey tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Primary hotkey tests failed: " << error.what() << '\n';
        return 1;
    }
}
