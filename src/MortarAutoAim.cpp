#include "MortarAutoAim.hpp"

#include "InputController.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace pubg {

MortarAutoAim::MortarAutoAim(Config& config, MinimapRadar& minimap, LargeMapRadar& large_map,
                             ElevationRadar& elevation, MessageCallback message_callback)
    : config_(config), minimap_(minimap), large_map_(large_map), elevation_(elevation),
      ballistics_(config), message_callback_(std::move(message_callback)) {}

MortarAutoAim::~MortarAutoAim() {
    shutdown();
}

void MortarAutoAim::trigger(const std::string& selected_color) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread(&MortarAutoAim::run, this, selected_color);
}

void MortarAutoAim::shutdown() {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
}

std::optional<double> MortarAutoAim::targetDistance(const std::string& selected_color) const {
    const auto elevations = elevation_.measuredElevations();
    const auto compensate = [this, &elevations, &selected_color](double distance) {
        if (auto elev = elevations.find(selected_color); elev != elevations.end() && elev->second > 0.0) {
            return ballistics_.mortarTrueDistance(distance, elev->second);
        }
        return distance;
    };
    const auto minimap = minimap_.measuredDistance();
    if (auto it = minimap.find(selected_color); it != minimap.end() && it->second > 0.0) {
        return compensate(it->second);
    }
    const auto large_map = large_map_.measuredDistance();
    if (auto it = large_map.find(selected_color); it != large_map.end() && it->second > 0.0) {
        return compensate(it->second);
    }
    return std::nullopt;
}

std::vector<MortarAutoAim::Step> MortarAutoAim::loadSteps() const {
    return config_.read([](const Json& data) {
        std::vector<Step> steps;
        const auto cfg = data.value("mortar_config", Json::object());
        if (cfg.contains("auto_aim_distances") && cfg["auto_aim_distances"].is_array()) {
            int presses = 0;
            for (const auto& item : cfg["auto_aim_distances"]) {
                if (item.is_number()) {
                    steps.push_back({presses, item.get<double>()});
                    ++presses;
                }
            }
        }
        if (steps.empty() && cfg.contains("auto_aim_steps") && cfg["auto_aim_steps"].is_array()) {
            for (const auto& item : cfg["auto_aim_steps"]) {
                if (!item.is_object()) continue;
                steps.push_back({
                    item.value("presses", static_cast<int>(steps.size())),
                    item.value("distance", 121.0)
                });
            }
        }
        if (steps.empty()) {
            steps = {
                {0, 121.0}, {1, 123.0}, {2, 127.0}, {3, 134.0}, {4, 140.0},
                {5, 150.0}, {6, 160.0}, {7, 170.0}, {8, 180.0}, {9, 190.0},
                {10, 200.0}, {11, 210.0}, {12, 220.0}, {13, 230.0}, {14, 240.0}
            };
        }
        std::sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
            if (a.distance == b.distance) return a.presses < b.presses;
            return a.distance < b.distance;
        });
        return steps;
    });
}

int MortarAutoAim::resetHoldMs() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("auto_aim_reset_w_hold_ms", 1500);
    });
}

int MortarAutoAim::keyDelayMs() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("auto_aim_key_delay_ms", 3);
    });
}

MortarAutoAim::Step MortarAutoAim::nearestStep(double distance) const {
    const auto steps = loadSteps();
    return *std::min_element(steps.begin(), steps.end(), [distance](const Step& a, const Step& b) {
        return std::abs(a.distance - distance) < std::abs(b.distance - distance);
    });
}

void MortarAutoAim::tapKey(int vk, int delay_ms) {
    InputController::keyDown(vk);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    InputController::keyUp(vk);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void MortarAutoAim::holdKey(int vk, int hold_ms) {
    InputController::keyDown(vk);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    InputController::keyUp(vk);
}

void MortarAutoAim::wheelDown(int delay_ms) {
    InputController::mouseWheel(-1);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void MortarAutoAim::run(std::string selected_color) {
    const auto finish = [this] {
        running_ = false;
    };

    const auto distance = targetDistance(selected_color);
    if (!distance) {
        std::cout << "[mortar-auto] no distance for marker " << selected_color << "\n";
        if (message_callback_) {
            message_callback_("未找到此颜色标点，无法瞄准", selected_color);
        }
        finish();
        return;
    }

    const auto steps = loadSteps();
    if (!steps.empty()) {
        const auto [min_it, max_it] = std::minmax_element(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
            return a.distance < b.distance;
        });
        if (*distance < min_it->distance) {
            if (message_callback_) {
                message_callback_("此颜色标点太近，无法瞄准", selected_color);
            }
            finish();
            return;
        }
        if (*distance > max_it->distance) {
            if (message_callback_) {
                message_callback_("此颜色标点太远，无法瞄准", selected_color);
            }
            finish();
            return;
        }
    }

    const Step step = nearestStep(*distance);
    const int delay = keyDelayMs();
    const int reset_hold_ms = resetHoldMs();
    std::cout << "[mortar-auto] marker=" << selected_color
              << " distance=" << static_cast<int>(std::round(*distance))
              << " target=" << step.distance
              << " reset_w_ms=" << reset_hold_ms
              << " wheel_down=" << step.presses << "\n";

    holdKey('W', reset_hold_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    for (int i = 0; i < step.presses; ++i) {
        wheelDown(delay);
    }
    finish();
}

} // namespace pubg
