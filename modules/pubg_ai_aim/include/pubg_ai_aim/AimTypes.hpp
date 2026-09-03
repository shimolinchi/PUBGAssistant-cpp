#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pubg::ai_aim {

using Clock = std::chrono::steady_clock;

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;

    constexpr Vec2& operator+=(const Vec2& other) noexcept {
        x += other.x;
        y += other.y;
        return *this;
    }

    constexpr Vec2& operator-=(const Vec2& other) noexcept {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    constexpr Vec2& operator*=(float scale) noexcept {
        x *= scale;
        y *= scale;
        return *this;
    }
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 lhs, const Vec2& rhs) noexcept {
    return lhs += rhs;
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 lhs, const Vec2& rhs) noexcept {
    return lhs -= rhs;
}

[[nodiscard]] constexpr Vec2 operator*(Vec2 value, float scale) noexcept {
    return value *= scale;
}

[[nodiscard]] constexpr Vec2 operator*(float scale, Vec2 value) noexcept {
    return value *= scale;
}

struct Box {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;

    [[nodiscard]] constexpr float width() const noexcept { return right - left; }
    [[nodiscard]] constexpr float height() const noexcept { return bottom - top; }
    [[nodiscard]] constexpr Vec2 center() const noexcept {
        return {(left + right) * 0.5F, (top + bottom) * 0.5F};
    }

    [[nodiscard]] static constexpr Box fromCenter(
        float center_x,
        float center_y,
        float width,
        float height) noexcept {
        return {
            center_x - width * 0.5F,
            center_y - height * 0.5F,
            center_x + width * 0.5F,
            center_y + height * 0.5F,
        };
    }
};

struct Detection {
    Box box{};                    // ROI-local xyxy coordinates.
    float confidence = 0.0F;
    int class_id = -1;
};

enum class PixelFormat {
    Bgra8,
    Bgr8,
    Rgb8,
};

struct Frame {
    std::uint64_t sequence = 0;
    Clock::time_point timestamp{};
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::Bgra8;
    std::vector<std::byte> pixels;

    // Desktop coordinates of the ROI and the selected output's crosshair.
    int desktop_left = 0;
    int desktop_top = 0;
    int screen_width = 0;
    int screen_height = 0;
    int screen_center_x = 0;
    int screen_center_y = 0;
};

struct DetectionFrame {
    std::uint64_t sequence = 0;
    Clock::time_point timestamp{};
    int roi_left = 0;
    int roi_top = 0;
    int roi_width = 0;
    int roi_height = 0;
    int screen_width = 0;
    int screen_height = 0;
    int screen_center_x = 0;
    int screen_center_y = 0;
    std::vector<Detection> detections;
};

struct CombatContext {
    std::uint64_t revision = 0;
    std::string weapon;
    std::string scope;
    std::string grip;
    std::string muzzle;
    std::string stock;
    std::string stance;
    float scope_zoom = 1.0F;
    bool ads = false;
    bool firing = false;
    bool crouched = false;
    bool lean_left = false;
    bool lean_right = false;
};

struct ActivationState {
    bool enabled = false;
    bool aim_primary = false;
    bool aim_secondary = false;
    bool lead_boost = false;
    bool manual_lead = false;
    bool paused = false;
    bool game_foreground = false;
    bool blocking_ui_open = false;
};

[[nodiscard]] constexpr bool activationAllowsProcessing(
    const ActivationState& state) noexcept {
    return state.enabled && state.game_foreground && !state.paused &&
           !state.blocking_ui_open && (state.aim_primary || state.aim_secondary);
}

struct MotionCommand {
    double dx = 0.0;
    double dy = 0.0;
    std::uint64_t frame_sequence = 0;
    Clock::time_point timestamp{};
};

struct AimSnapshot {
    // Immutable, already-decoded current frame for host visualizations. No
    // capture pixels are retained. Cleared on reset, unsafe activation or error.
    std::shared_ptr<const DetectionFrame> detection_frame;
    bool running = false;
    bool model_ready = false;
    bool target_locked = false;
    int target_class = -1;
    float confidence = 0.0F;
    Vec2 target{};
    Vec2 velocity{};
    Vec2 lead{};
    Vec2 raw_error{};
    double capture_ms = 0.0;
    double inference_ms = 0.0;
    // Actual completed valid detection frames/s, averaged every 500ms. Zero
    // during warmup, unsafe activation, errors or after 1s without completion.
    double recognition_hz = 0.0;
    std::uint64_t frame_sequence = 0;
    std::string detector_backend;
    std::string last_error;
};

struct AimDecision {
    bool has_motion = false;
    MotionCommand motion{};
    bool request_click = false;
    AimSnapshot snapshot{};
};

} // namespace pubg::ai_aim
