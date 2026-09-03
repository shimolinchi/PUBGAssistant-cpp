#include "pubg_ai_aim/pubg_ai_aim.hpp"

#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <utility>

namespace aim = pubg::ai_aim;

class ConsoleMotionSink final : public aim::IMotionSink {
public:
    void submitAimMotion(const aim::MotionCommand& command) override {
        std::cout << "aim dx=" << command.dx << " dy=" << command.dy
                  << " sequence=" << command.frame_sequence << '\n';
    }
};

int main() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;

    auto sink = std::make_shared<ConsoleMotionSink>();
    aim::AimModule module(config, sink);

    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    module.updateActivation(activation);
    if (!module.start()) {
        std::cerr << "failed to start aim module\n";
        return 1;
    }

    aim::DetectionFrame frame;
    frame.sequence = 1;
    frame.timestamp = aim::Clock::now();
    frame.roi_left = 800;
    frame.roi_top = 380;
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 1920;
    frame.screen_height = 1080;
    frame.screen_center_x = 960;
    frame.screen_center_y = 540;
    frame.detections.push_back({aim::Box::fromCenter(180.0F, 160.0F, 40.0F, 80.0F), 0.9F, 1});
    if (!module.submitDetections(std::move(frame))) {
        std::cerr << "detection frame was rejected\n";
        module.stop();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    module.stop();
}
