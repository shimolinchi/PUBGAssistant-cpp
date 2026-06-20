#include "MortarAutoAim.hpp"

#include "InputController.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace pubg {

MortarAutoAim::MortarAutoAim(Config& config, RegionManager& regions, MinimapRadar& minimap, LargeMapRadar& large_map,
                             ElevationRadar& elevation, MessageCallback message_callback)
    : config_(config), regions_(regions), minimap_(minimap), large_map_(large_map), elevation_(elevation),
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

int MortarAutoAim::directionMaxMs() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_max_ms", 1800);
    });
}

double MortarAutoAim::directionTolerancePx() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_tolerance_px", 4.0);
    });
}

double MortarAutoAim::directionKp() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_kp", 0.045);
    });
}

double MortarAutoAim::directionKi() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_ki", 0.0);
    });
}

double MortarAutoAim::directionKd() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_kd", 0.012);
    });
}

int MortarAutoAim::directionStepDelayMs() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_step_delay_ms", 2);
    });
}

double MortarAutoAim::directionMaxStepPx() const {
    return config_.read([](const Json& data) {
        return data.value("mortar_config", Json::object()).value("direction_auto_aim_max_step_px", 80.0);
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

std::optional<double> MortarAutoAim::selectedMarkerOffset(const std::string& selected_color) const {
    const auto rect = regions_.getRealRegion("compass_region");
    if (!rect || !rect->valid()) {
        return std::nullopt;
    }
    const auto colors = config_.markerColors();
    auto color_it = std::find_if(colors.begin(), colors.end(), [&](const MarkerColor& c) {
        return c.name == selected_color;
    });
    if (color_it == colors.end()) {
        return std::nullopt;
    }

    ScreenCapture capture;
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return std::nullopt;
    }
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, color_it->lower_hsv, color_it->upper_hsv, mask);
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, cv::Mat::ones(2, 2, CV_8U));
    cv::dilate(mask, mask, cv::Mat::ones(3, 3, CV_8U), {-1, -1}, 1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    double best_area = 0.0;
    cv::Rect best_rect;
    for (const auto& contour : contours) {
        const double area = cv::contourArea(contour);
        if (area < 4.0 || area < best_area) {
            continue;
        }
        const cv::Rect bound = cv::boundingRect(contour);
        if (bound.width > rect->width / 3 || bound.height > rect->height / 2) {
            continue;
        }
        best_area = area;
        best_rect = bound;
    }
    if (best_area <= 0.0) {
        return std::nullopt;
    }
    const double marker_x = rect->left + best_rect.x + best_rect.width * 0.5;
    return marker_x - regions_.screenWidth() * 0.5;
}

void MortarAutoAim::alignDirection(const std::string& selected_color, std::atomic_bool& stop_requested) {
    const int max_ms = directionMaxMs();
    const int step_delay = std::max(1, directionStepDelayMs());
    const double tolerance = directionTolerancePx();
    const double kp = directionKp();
    const double ki = directionKi();
    const double kd = directionKd();
    const double max_step = std::clamp(directionMaxStepPx(), 1.0, 500.0);
    const double deadline = nowSeconds() + max_ms / 1000.0;
    double integral = 0.0;
    double previous_error = 0.0;
    double last_time = nowSeconds();
    bool have_previous = false;

    while (running_ && !stop_requested.load() && nowSeconds() < deadline) {
        const auto offset = selectedMarkerOffset(selected_color);
        if (!offset) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step_delay));
            continue;
        }
        const double error = *offset;
        if (std::abs(error) <= tolerance) {
            break;
        }
        const double now = nowSeconds();
        const double dt = std::max(0.001, now - last_time);
        last_time = now;
        integral = std::clamp(integral + error * dt, -200.0, 200.0);
        const double derivative = have_previous ? (error - previous_error) / dt : 0.0;
        previous_error = error;
        have_previous = true;
        const double output = kp * error + ki * integral + kd * derivative;
        int dx = static_cast<int>(std::round(std::clamp(output, -max_step, max_step)));
        if (dx == 0) {
            dx = error > 0 ? 1 : -1;
        }
        InputController::moveMouseRelative(dx, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(step_delay));
    }
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

    std::atomic_bool stop_direction{false};
    std::thread direction_worker([this, selected_color, &stop_direction] {
        alignDirection(selected_color, stop_direction);
    });

    try {
        holdKey('W', reset_hold_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        for (int i = 0; i < step.presses; ++i) {
            wheelDown(delay);
        }
    } catch (...) {
        stop_direction = true;
        if (direction_worker.joinable()) {
            direction_worker.join();
        }
        finish();
        throw;
    }
    if (direction_worker.joinable()) {
        direction_worker.join();
    }
    finish();
}

} // namespace pubg
