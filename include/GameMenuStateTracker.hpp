#pragma once

#include <optional>

namespace pubg {

// Tracks the game menu conservatively from Escape transitions. App owns the
// surrounding lock; this type only contains the small, deterministic policy so
// the AI and mortar gates share the same interpretation.
class GameMenuStateTracker final {
public:
    struct KnownUiState final {
        bool equipment_open = false;
        bool large_map_open = false;
        bool manual_minimap_open = false;
        bool app_minimap_open = false;

        [[nodiscard]] bool anyOpen() const noexcept {
            return equipment_open || large_map_open ||
                   manual_minimap_open || app_minimap_open;
        }
    };

    struct EscapeTransition final {
        bool close_app_minimap = false;
    };

    // If a known overlay is open, Escape is assumed to close that overlay.
    // Otherwise Escape can only make the persistent game-menu assumption more
    // conservative. Releasing Escape deliberately does not clear it.
    [[nodiscard]] EscapeTransition onEscapePressed(
        const KnownUiState known_ui) noexcept {
        hidden_cursor_samples_ = 0;
        cursor_confirmed_hidden_ = false;
        if (!known_ui.anyOpen()) {
            active_ = true;
        }
        return {.close_app_minimap = known_ui.app_minimap_open};
    }

    // Cursor visibility is polled, so evidence that arrives during debounce is
    // not lost like a one-shot detector callback. Unknown/visible cursor state,
    // background focus, and known UI all fail closed and reset confirmation.
    [[nodiscard]] bool observeCursorVisibility(
        const std::optional<bool> cursor_visible,
        const KnownUiState known_ui,
        const bool game_foreground,
        const bool debounce_elapsed) noexcept {
        const bool previously_blocked = aimInputBlocked();
        if (!cursor_visible.has_value() || *cursor_visible ||
            !game_foreground || known_ui.anyOpen() || !debounce_elapsed) {
            hidden_cursor_samples_ = 0;
            cursor_confirmed_hidden_ = false;
            return previously_blocked != aimInputBlocked();
        }
        if (hidden_cursor_samples_ < kRequiredHiddenCursorSamples) {
            ++hidden_cursor_samples_;
        }
        if (hidden_cursor_samples_ >= kRequiredHiddenCursorSamples) {
            cursor_confirmed_hidden_ = true;
            active_ = false;
        }
        return previously_blocked != aimInputBlocked();
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool aimInputBlocked() const noexcept {
        return active_ || !cursor_confirmed_hidden_;
    }

private:
    static constexpr int kRequiredHiddenCursorSamples = 3;
    bool active_ = false;
    bool cursor_confirmed_hidden_ = false;
    int hidden_cursor_samples_ = 0;
};

} // namespace pubg
