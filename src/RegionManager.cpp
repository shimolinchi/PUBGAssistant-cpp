#include "RegionManager.hpp"

#include <iostream>

namespace pubg {

RegionManager::RegionManager(Config& config) : config_(config) {
#ifdef _WIN32
    screen_w_ = GetSystemMetrics(SM_CXSCREEN);
    screen_h_ = GetSystemMetrics(SM_CYSCREEN);
#endif
    syncCrosshairRegion();
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
        if (data.contains("map_scales") && data["map_scales"].contains(name) &&
            data["map_scales"][name].is_number()) {
            return data["map_scales"][name].get<double>();
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

void RegionManager::syncCrosshairRegion() {
    const int side = std::max(1, std::min({screen_w_, screen_h_, static_cast<int>(std::round(screen_h_ / 1.5))}));
    Rect rect;
    rect.left = static_cast<int>(std::round((screen_w_ - side) / 2.0));
    rect.top = static_cast<int>(std::round((screen_h_ - side) / 2.0));
    rect.width = side;
    rect.height = side;
    setRealRegion("crosshair_region", rect);
}

} // namespace pubg
