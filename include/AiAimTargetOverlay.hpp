#pragma once

#include "pubg_ai_aim/AimTypes.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pubg {

struct AiAimTargetOverlayState {
    bool display_enabled = false;
    bool ai_enabled = false;
    bool boxes_enabled = false;
    int screen_left = 0;
    int screen_top = 0;
    int screen_width = 0;
    int screen_height = 0;
    int max_frame_age_ms = 80;
};

struct AiAimTargetOutline {
    double left, top, right, bottom;
    int red, green;
    static constexpr int alpha = 85;
    static constexpr int width = 1;
};

// Pure presentation policy: reuse immutable detector results, no extra capture,
// inference, tracking, or window implementation.
[[nodiscard]] inline std::vector<AiAimTargetOutline> aiAimTargetOutlines(
    const ai_aim::DetectionFrame* frame, const AiAimTargetOverlayState& state,
    ai_aim::Clock::time_point now = ai_aim::Clock::now()) {
    std::vector<AiAimTargetOutline> result;
    if (!frame || !state.display_enabled || !state.ai_enabled || !state.boxes_enabled ||
        frame->sequence == 0 || frame->roi_width <= 0 || frame->roi_height <= 0 ||
        state.screen_width <= 0 || state.screen_height <= 0 ||
        frame->timestamp == ai_aim::Clock::time_point{} || frame->timestamp > now ||
        now - frame->timestamp > std::chrono::milliseconds(std::clamp(state.max_frame_age_ms, 1, 150))) {
        return result;
    }
    const double offset_x = static_cast<double>(frame->roi_left) - state.screen_left;
    const double offset_y = static_cast<double>(frame->roi_top) - state.screen_top;
    for (const auto& detection : frame->detections) {
        const auto& box = detection.box;
        if ((detection.class_id != 0 && detection.class_id != 1) ||
            !std::isfinite(detection.confidence) || detection.confidence < 0 || detection.confidence > 1 ||
            !std::isfinite(box.left) || !std::isfinite(box.top) ||
            !std::isfinite(box.right) || !std::isfinite(box.bottom) ||
            box.right <= box.left || box.bottom <= box.top) continue;
        const auto local_x = [&](float value) {
            return std::clamp(offset_x + std::clamp(static_cast<double>(value), 0.0,
                static_cast<double>(frame->roi_width)), 0.0, static_cast<double>(state.screen_width));
        };
        const auto local_y = [&](float value) {
            return std::clamp(offset_y + std::clamp(static_cast<double>(value), 0.0,
                static_cast<double>(frame->roi_height)), 0.0, static_cast<double>(state.screen_height));
        };
        const double left = local_x(box.left), right = local_x(box.right);
        const double top = local_y(box.top), bottom = local_y(box.bottom);
        if (right <= left || bottom <= top) continue;
        const double confidence = detection.confidence;
        result.push_back({left, top, right, bottom,
            static_cast<int>(std::lround(255 * std::min(1.0, 2 * (1 - confidence)))),
            static_cast<int>(std::lround(255 * std::min(1.0, 2 * confidence)))});
    }
    return result;
}

} // namespace pubg
