#include "AiAimHostContext.hpp"
#include "AiAimTargetOverlay.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {

void require(const bool condition, const char* message) {
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

void testBuilderMapsExistingRecognitionFields() {
    pubg::AiAimHostCombatSnapshot host;
    host.weapon = "M416";
    host.weapon_supported = true;
    host.stance = "squat";
    host.ads = true;
    host.firing = true;
    host.lean_left = true;
    host.equipment.emplace(1, equipment("M416", "4", "vertical"));

    const auto context = pubg::buildAiAimCombatContext(host);
    require(context.weapon == "M416", "supported weapon was removed");
    require(context.scope == "4" && context.scope_zoom == 4.0F,
            "recognized scope was not mapped");
    require(context.grip == "vertical", "recognized attachment was not mapped");
    require(context.crouched && context.ads && context.firing && context.lean_left,
            "host input state was not mapped");
}

void testUnsupportedAndVariableHighZoomFailClosed() {
    pubg::AiAimHostCombatSnapshot unsupported;
    unsupported.weapon = "Unknown";
    unsupported.weapon_supported = false;
    require(pubg::buildAiAimCombatContext(unsupported).weapon.empty(),
            "unsupported weapon was allowed");

    pubg::AiAimHostCombatSnapshot high_zoom;
    high_zoom.weapon = "M416";
    high_zoom.weapon_supported = true;
    high_zoom.equipment.emplace(1, equipment("M416", "6"));
    require(pubg::buildAiAimCombatContext(high_zoom).weapon.empty(),
            "variable high zoom was not blocked");

    high_zoom.disable_variable_high_zoom = false;
    const auto explicitly_allowed = pubg::buildAiAimCombatContext(high_zoom);
    require(explicitly_allowed.weapon == "M416" &&
            explicitly_allowed.scope_zoom == 6.0F,
            "explicit high-zoom override was not honored");
}

void testTargetOutlinesGatesCoordinatesColorsAndExpiry() {
    using namespace std::chrono_literals;
    const auto now = pubg::ai_aim::Clock::now();
    pubg::ai_aim::DetectionFrame frame;
    frame.sequence = 7;
    frame.timestamp = now;
    frame.roi_left = -1200;
    frame.roi_top = 400;
    frame.roi_width = frame.roi_height = 320;
    frame.detections = {
        {{10, 20, 40, 80}, 0.0F, 0},
        {{20, 30, 50, 90}, 0.5F, 1},
        {{30, 40, 60, 100}, 1.0F, 1},
    };
    pubg::AiAimTargetOverlayState state{true, true, true, -1920, 200, 1920, 1080, 80};
    auto boxes = pubg::aiAimTargetOutlines(&frame, state, now);
    require(boxes.size() == 3, "valid target outlines were lost");
    require(boxes[0].left == 730 && boxes[0].top == 220 && boxes[0].right == 760,
            "ROI-to-selected-monitor physical coordinate conversion is wrong");
    require(boxes[0].red == 255 && boxes[0].green == 0 &&
            boxes[1].red == 255 && boxes[1].green == 255 &&
            boxes[2].red == 0 && boxes[2].green == 255,
            "confidence must map continuously red through yellow to green");
    require(boxes[0].alpha < 128 && boxes[0].width == 1, "outlines should remain faint and thin");
    state.display_enabled = false;
    require(pubg::aiAimTargetOutlines(&frame, state, now).empty(), "display-off leaked boxes");
    state.display_enabled = true;
    state.boxes_enabled = false;
    require(pubg::aiAimTargetOutlines(&frame, state, now).empty(), "box toggle ignored");
    state.boxes_enabled = true;
    state.ai_enabled = false;
    require(pubg::aiAimTargetOutlines(&frame, state, now).empty(), "AI-off leaked boxes");
    state.ai_enabled = true;
    require(pubg::aiAimTargetOutlines(&frame, state, now + 81ms).empty(), "stale frame persisted");
    state.max_frame_age_ms = 10000;
    require(pubg::aiAimTargetOutlines(&frame, state, now + 151ms).empty(), "long motion TTL retained stale boxes");
    require(pubg::aiAimTargetOutlines(&frame, state, now - 1ms).empty(), "future frame accepted");
    frame.detections = {{{-20, -20, 350, 350}, .75F, 1}};
    boxes = pubg::aiAimTargetOutlines(&frame, state, now);
    require(boxes.size() == 1 && boxes[0].left == 720 && boxes[0].right == 1040,
            "boxes were not clipped to capture ROI");
    frame.detections[0].box.left = std::numeric_limits<float>::quiet_NaN();
    require(pubg::aiAimTargetOutlines(&frame, state, now).empty(), "non-finite box reached rendering");
    frame.detections.clear();
    require(pubg::aiAimTargetOutlines(&frame, state, now).empty(), "lost target retained old outline");
}

} // namespace

int main() {
    try {
        testBuilderMapsExistingRecognitionFields();
        testUnsupportedAndVariableHighZoomFailClosed();
        testTargetOutlinesGatesCoordinatesColorsAndExpiry();
        std::cout << "AI aim host context tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "AI aim host context tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
