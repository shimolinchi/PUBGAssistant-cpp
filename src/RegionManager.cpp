#include "RegionManager.hpp"

#include <iostream>

namespace pubg {

RegionManager::RegionManager(Config& config, DisplayInfo display) : config_(config) {
    screen_left_ = display.left;
    screen_top_ = display.top;
    screen_w_ = std::max(1, display.width);
    screen_h_ = std::max(1, display.height);
    qt_screen_left_ = display.qt_left;
    qt_screen_top_ = display.qt_top;
    qt_screen_w_ = std::max(1, display.qt_width);
    qt_screen_h_ = std::max(1, display.qt_height);
    device_pixel_ratio_ = display.device_pixel_ratio > 0.0 ? display.device_pixel_ratio : 1.0;
    syncCrosshairRegion();
    syncCompassRegion();
}

std::optional<Rect> RegionManager::getRealRegion(const std::string& name) const {
    return config_.read([&](const Json& data) -> std::optional<Rect> {
        if (!data.contains("real_regions") || !data["real_regions"].contains(name)) {
            return std::nullopt;
        }
        const auto& r = data["real_regions"][name];
        Rect rect;
        rect.left = static_cast<int>(std::round(r.value("left", 0.0)));
        rect.top = static_cast<int>(std::round(r.value("top", 0.0)));
        rect.width = static_cast<int>(std::round(r.value("width", 0.0)));
        rect.height = static_cast<int>(std::round(r.value("height", 0.0)));
        if (!rect.valid()) {
            return std::nullopt;
        }
        return rect;
    });
}

double RegionManager::getRealScale(const std::string& name, double fallback) const {
    return config_.read([&](const Json& data) -> double {
        if (data.contains("real_scales") && data["real_scales"].contains(name) &&
            data["real_scales"][name].is_number()) {
            return data["real_scales"][name].get<double>();
        }
        return fallback;
    });
}

void RegionManager::setRealRegion(const std::string& name, Rect rect) {
    config_.write([&](Json& data) {
        data["real_regions"][name] = Json{
            {"left", rect.left},
            {"top", rect.top},
            {"width", rect.width},
            {"height", rect.height},
        };
    });
    config_.save();
}

void RegionManager::setRealScale(const std::string& name, double value) {
    config_.write([&](Json& data) {
        data["real_scales"][name] = value;
    });
    config_.save();
}

bool RegionManager::createOverlay(OverlayWindow& overlay, const std::wstring& title, bool click_through,
                                  bool exclude_from_capture) const {
    return overlay.createAt(title, screen_left_, screen_top_, screen_w_, screen_h_, click_through, exclude_from_capture);
}

void RegionManager::syncCrosshairRegion() {
    const int side = std::max(1, std::min({screen_w_, screen_h_, static_cast<int>(std::round(screen_h_ / 1.5))}));
    Rect rect;
    rect.left = screen_left_ + static_cast<int>(std::round((screen_w_ - side) / 2.0));
    rect.top = screen_top_ + static_cast<int>(std::round((screen_h_ - side) / 2.0));
    rect.width = side;
    rect.height = side;
    setRealRegion("crosshair_region", rect);
}

void RegionManager::syncCompassRegion() {
    const bool has_compass = config_.read([](const Json& data) {
        return data.contains("real_regions") &&
               data["real_regions"].contains("compass_region") &&
               data["real_regions"]["compass_region"].is_object();
    });
    if (has_compass) {
        return;
    }

    Rect rect;
    rect.width = std::max(300, static_cast<int>(std::round(screen_w_ * 0.42)));
    rect.height = std::max(50, static_cast<int>(std::round(screen_h_ * 0.07)));
    rect.left = screen_left_ + static_cast<int>(std::round((screen_w_ - rect.width) / 2.0));
    rect.top = screen_top_;
    setRealRegion("compass_region", rect);
}

} // namespace pubg
