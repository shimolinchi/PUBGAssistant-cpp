#include "GameMenuStateTracker.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testMenuRemainsActiveAfterKeyReleaseBoundary() {
    pubg::GameMenuStateTracker tracker;
    (void)tracker.onEscapePressed({});
    require(tracker.active(), "Escape without another UI should open the menu gate");

    // Key release has no tracker transition. The caller may retain a short
    // debounce window, but the persistent safety state must remain active.
    require(tracker.active(), "the menu gate was cleared at the key-release boundary");
}

void testRepeatedEscapeCannotClearMenuWithoutGameplayEvidence() {
    pubg::GameMenuStateTracker tracker;
    (void)tracker.onEscapePressed({});
    (void)tracker.onEscapePressed({});
    require(tracker.active(),
            "an unaccounted Escape must not clear the menu gate");
}

void testStableHiddenCursorClearsMenuAfterDebounce() {
    pubg::GameMenuStateTracker tracker;
    (void)tracker.onEscapePressed({});

    for (int sample = 0; sample < 5; ++sample) {
        (void)tracker.observeCursorVisibility(false, {}, true, false);
    }
    require(tracker.active() && tracker.aimInputBlocked(),
            "hidden cursor samples inside the debounce window were accepted");

    (void)tracker.observeCursorVisibility(false, {}, true, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    require(tracker.active(),
            "the menu gate accepted fewer than three stable samples");
    require(tracker.observeCursorVisibility(false, {}, true, true),
            "stable hidden-cursor evidence did not change the safety state");
    require(!tracker.active(),
            "the menu gate remained active after gameplay was confirmed");
    require(!tracker.aimInputBlocked(),
            "aim input remained blocked after gameplay was confirmed");
}

void testVisibleOrUnknownCursorCannotClearMenu() {
    pubg::GameMenuStateTracker tracker;
    (void)tracker.onEscapePressed({});

    for (int sample = 0; sample < 5; ++sample) {
        (void)tracker.observeCursorVisibility(true, {}, true, true);
        (void)tracker.observeCursorVisibility(std::nullopt, {}, true, true);
    }
    require(tracker.active() && tracker.aimInputBlocked(),
            "visible or unknown cursor state cleared the menu gate");
}

void testOtherSafetyGatesResetHiddenCursorConfirmation() {
    pubg::GameMenuStateTracker tracker;
    (void)tracker.onEscapePressed({});

    (void)tracker.observeCursorVisibility(false, {}, true, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    (void)tracker.observeCursorVisibility(
        false, {.equipment_open = true}, true, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    require(tracker.active(),
            "known UI did not reset hidden-cursor confirmation");

    (void)tracker.observeCursorVisibility(false, {}, false, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    require(tracker.active(),
            "background state did not reset hidden-cursor confirmation");
    (void)tracker.observeCursorVisibility(false, {}, true, true);
    require(!tracker.active(),
            "three new foreground samples did not clear the menu gate");
}

void testEscapeClosesKnownUiWithoutOpeningMenu() {
    const pubg::GameMenuStateTracker::KnownUiState cases[]{
        {.equipment_open = true},
        {.large_map_open = true},
        {.manual_minimap_open = true},
        {.app_minimap_open = true},
    };
    for (const auto state : cases) {
        pubg::GameMenuStateTracker tracker;
        const auto transition = tracker.onEscapePressed(state);
        require(!tracker.active(), "closing a known UI must not assume the game menu opened");
        require(transition.close_app_minimap == state.app_minimap_open,
                "the app-minimap close decision did not match the known UI state");
    }
}

} // namespace

int main() {
    try {
        testMenuRemainsActiveAfterKeyReleaseBoundary();
        testRepeatedEscapeCannotClearMenuWithoutGameplayEvidence();
        testStableHiddenCursorClearsMenuAfterDebounce();
        testVisibleOrUnknownCursorCannotClearMenu();
        testOtherSafetyGatesResetHiddenCursorConfirmation();
        testEscapeClosesKnownUiWithoutOpeningMenu();
        std::cout << "Game menu state tracker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Game menu state tracker tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
