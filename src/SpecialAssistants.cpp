#include "SpecialAssistants.hpp"

#include <algorithm>
#include <numbers>

namespace pubg {

namespace {
struct AssistHudLayout {
    double x = 0.0;
    double large_y = 0.0;
    double mortar_y = 0.0;
    double box_w = 90.0;
    double box_h = 25.0;
    double spacing = 10.0;
    double total_w = 390.0;
};

AssistHudLayout assistHudLayout(const RegionManager& regions) {
    AssistHudLayout layout;
    if (const auto minimap = regions.getRealRegion("minimap_region")) {
        layout.box_h = 25.0;
        layout.spacing = 10.0;
        const double panel_w = std::floor(minimap->width * 0.82);
        layout.box_w = std::floor((panel_w - 3.0 * layout.spacing) / 4.0);
        layout.total_w = 4.0 * layout.box_w + 3.0 * layout.spacing;
        layout.x = minimap->left + minimap->width - layout.total_w;
        layout.large_y = minimap->top - layout.box_h - layout.spacing - layout.box_h - 30.0;
        layout.large_y = std::max(0.0, layout.large_y);
        layout.mortar_y = layout.large_y + layout.box_h + layout.spacing;
    } else {
        layout.box_w = 90.0;
        layout.box_h = 25.0;
        layout.spacing = 15.0;
        layout.total_w = 4.0 * layout.box_w + 3.0 * layout.spacing;
        layout.x = std::max(0.0, regions.screenWidth() - layout.total_w - 25.0);
        layout.large_y = regions.screenHeight() * 0.465;
        layout.mortar_y = layout.large_y + layout.box_h + layout.spacing;
    }
    return layout;
}

int scaledOddKernel(double base, double scale, int min_value) {
    int value = std::max(min_value, static_cast<int>(std::round(base * scale)));
    if (value % 2 == 0) {
        ++value;
    }
    return value;
}
} // namespace

std::optional<std::pair<double, double>> SpecialAssistants::detectCrosshairCenter(ScreenCapture& capture) const {
    const auto rect = regions_.getRealRegion("crosshair_region");
    if (!rect || !rect->valid()) {
        return std::nullopt;
    }
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return std::nullopt;
    }
    constexpr double kMaxProcessSide = 180.0;
    const double max_side = static_cast<double>(std::max(bgr.cols, bgr.rows));
    const double scale = max_side > kMaxProcessSide ? kMaxProcessSide / max_side : 1.0;
    cv::Mat work;
    if (scale < 1.0) {
        cv::resize(bgr, work, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        work = bgr;
    }
    cv::Mat gray;
    cv::cvtColor(work, gray, cv::COLOR_BGR2GRAY);
    cv::Mat thresh;
    cv::threshold(gray, thresh, 25, 255, cv::THRESH_BINARY);
    const int open_size = scaledOddKernel(5.0, scale, 3);
    const int close_size = scaledOddKernel(21.0, scale, 7);
    const cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {open_size, open_size});
    const cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {close_size, close_size});
    cv::morphologyEx(thresh, thresh, cv::MORPH_OPEN, open_kernel);
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, close_kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        return std::nullopt;
    }
    const auto best = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    const double area = cv::contourArea(*best);
    const double roi_area = static_cast<double>(thresh.cols) * thresh.rows;
    if (area <= roi_area * 0.08 || area >= roi_area * 0.85) {
        return std::nullopt;
    }
    cv::Point2f center;
    float radius = 0.0f;
    cv::minEnclosingCircle(*best, center, radius);
    const double circle_area = std::numbers::pi * radius * radius;
    if (circle_area <= 0.0 || area / circle_area <= 0.7) {
        return std::nullopt;
    }
    const double inv_scale = 1.0 / scale;
    return std::pair<double, double>{rect->left + center.x * inv_scale, rect->top + center.y * inv_scale};
}

std::optional<std::pair<double, double>> SpecialAssistants::cachedCrosshairCenter() {
    const double now = nowSeconds();
    if (cached_crosshair_center_ && now - last_crosshair_sample_ < 0.08) {
        return cached_crosshair_center_;
    }
    last_crosshair_sample_ = now;
    ScreenCapture capture;
    if (auto detected = detectCrosshairCenter(capture)) {
        cached_crosshair_center_ = detected;
        return cached_crosshair_center_;
    }
    cached_crosshair_center_.reset();
    return std::nullopt;
}

SpecialAssistants::SpecialAssistants(Config& config, RegionManager& regions, MinimapRadar& minimap,
                                     ElevationRadar& elevation, LargeMapRadar& large_map)
    : config_(config), regions_(regions), minimap_(minimap), elevation_(elevation), large_map_(large_map),
      ballistics_(config), hex_(config.markerHex()) {
    overlay_.create(L"PUBGAssistant Special", regions_.screenWidth(), regions_.screenHeight(), true);
    worker_ = std::thread(&SpecialAssistants::run, this);
}

SpecialAssistants::~SpecialAssistants() {
    shutdown();
}

void SpecialAssistants::shutdown() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    overlay_.clear();
}

void SpecialAssistants::setDisplayEnabled(bool enabled) {
    display_enabled_ = enabled;
    overlay_.show(enabled);
}

void SpecialAssistants::setCurrentWeapon(const std::string& weapon) {
    std::lock_guard lock(mutex_);
    current_weapon_ = weapon;
}

void SpecialAssistants::setManualEnabled(const std::string& key, bool enabled) {
    std::lock_guard lock(mutex_);
    manual_[key] = enabled;
}

void SpecialAssistants::setMarkerHex(std::unordered_map<std::string, std::string> hex) {
    std::lock_guard lock(mutex_);
    hex_ = std::move(hex);
}

void SpecialAssistants::drawRocket(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                                   std::vector<OverlayCommand>& cmds) {
    const double cx = regions_.screenWidth() / 2.0;
    const double cy = regions_.screenHeight() / 2.0;
    const double line_len = regions_.screenHeight() * 0.42;
    cmds.push_back({OverlayCommand::Type::Line, cx, cy, cx, regions_.screenHeight() * 0.9,
                    0, "", hexToBgr("#FFFFFF"), 1, 18, 255});
    for (const auto& [color, dist] : dists) {
        if (dist <= 0.0 || dist > 175.0) {
            continue;
        }
        const double ratio = ballistics_.rocketRatio(dist);
        if (!std::isfinite(ratio) || ratio <= 0.0) {
            continue;
        }
        const double y = std::min(cy + ratio * line_len, regions_.screenHeight() - 10.0);
        const auto bgr = hexToBgr(hex.count(color) ? hex.at(color) : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 28, y, cx + 28, y, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Text, cx + 36, y - 10, 0, 0, 0, std::to_string(static_cast<int>(std::round(dist))) + "m", bgr, 1, 18});
    }
}

void SpecialAssistants::drawVss(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                                std::vector<OverlayCommand>& cmds) {
    const auto tracked = cachedCrosshairCenter();
    if (!tracked) {
        drawCenterNotice("未找到VSS准星", "#E74C3C", cmds);
        return;
    }
    const double cx = tracked->first;
    const double cy = tracked->second;
    for (const auto& [color, dist] : dists) {
        if (dist < 100.0 || dist > 420.0) {
            continue;
        }
        const double y = cy + ballistics_.vssDropRatio(dist) * regions_.screenHeight();
        const auto bgr = hexToBgr(hex.count(color) ? hex.at(color) : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 22, y, cx + 22, y, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Text, cx + 30, y - 9, 0, 0, 0, std::to_string(static_cast<int>(std::round(dist))) + "m", bgr, 1, 16});
    }
}

void SpecialAssistants::drawCrossbow(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                                     std::vector<OverlayCommand>& cmds) {
    const auto tracked = cachedCrosshairCenter();
    if (!tracked) {
        drawCenterNotice("未找到十字弩准星", "#E74C3C", cmds);
        return;
    }
    const double cx = tracked->first;
    const double cy = tracked->second;
    const auto black = hexToBgr("#000000");
    cmds.push_back({OverlayCommand::Type::Line, cx - 7, cy - 7, cx + 7, cy + 7, 0, "", black, 2});
    cmds.push_back({OverlayCommand::Type::Line, cx - 7, cy + 7, cx + 7, cy - 7, 0, "", black, 2});
    for (const auto& [color, dist] : dists) {
        if (dist <= 30.0 || dist > 350.0) {
            continue;
        }
        const double y = cy + ballistics_.crossbowDropRatio(dist) * regions_.screenHeight();
        const auto bgr = hexToBgr(hex.count(color) ? hex.at(color) : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 28, y, cx + 28, y, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Text, cx + 36, y - 10, 0, 0, 0, std::to_string(static_cast<int>(std::round(dist))) + "m", bgr, 1, 16});
    }
}

void SpecialAssistants::drawCenterNotice(const std::string& text, const std::string& color_hex,
                                         std::vector<OverlayCommand>& cmds) const {
    const double box_w = 300.0;
    const double box_h = 42.0;
    const double box_x = regions_.screenWidth() / 2.0 - box_w / 2.0;
    const double box_y = regions_.screenHeight() * 0.8 - box_h;
    const auto bgr = hexToBgr(color_hex);
    cmds.push_back({OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                    10, "", bgr, 0, 18, 89});
    cmds.push_back({OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                    10, "", bgr, 2, 18, 204});
    cmds.push_back({OverlayCommand::Type::TextCenter, box_x, box_y, box_x + box_w, box_y + box_h,
                    0, text, hexToBgr("#FFFFFF"), 1, 18, 255});
}

void SpecialAssistants::drawMortar(const DistanceMap& dists, const ElevationMap& elevs,
                                   const std::unordered_map<std::string, std::string>& hex,
                                   std::vector<OverlayCommand>& cmds) {
    const auto layout = assistHudLayout(regions_);
    drawDistancePanelRow(dists, elevs, hex, cmds, layout.mortar_y, true);
}

void SpecialAssistants::drawLargeMapDistances(const DistanceMap& dists, const ElevationMap& elevs,
                                              const std::unordered_map<std::string, std::string>& hex,
                                              std::vector<OverlayCommand>& cmds) {
    const auto layout = assistHudLayout(regions_);
    drawDistancePanelRow(dists, elevs, hex, cmds, layout.large_y, true);
}

void SpecialAssistants::drawDistancePanelRow(const DistanceMap& dists, const ElevationMap& elevs,
                                             const std::unordered_map<std::string, std::string>& hex,
                                             std::vector<OverlayCommand>& cmds, double y,
                                             bool apply_elevation) const {
    const auto layout = assistHudLayout(regions_);
    const std::vector<std::string> color_order{"Yellow", "Orange", "Blue", "Green"};
    int i = 0;
    for (const auto& color : color_order) {
        const auto dist_it = dists.find(color);
        const double dist = dist_it != dists.end() ? dist_it->second : 0.0;
        const std::string color_hex = hex.count(color) ? hex.at(color) : "#FFFFFF";
        const auto bgr = hexToBgr(color_hex);
        const auto elev_it = elevs.find(color);
        const bool has_elevation = apply_elevation && elev_it != elevs.end() && elev_it->second > 0.0;
        const double elev = has_elevation ? elev_it->second : 0.0;
        const double true_dist = dist > 0.0 && has_elevation ? ballistics_.mortarTrueDistance(dist, elev) : 0.0;
        std::string text = "---";
        int fill_alpha = 51;
        int font_size = 13;
        if (dist > 0.0 && dist < 121.0) {
            text = "距离过近";
            fill_alpha = 51;
        } else if (dist > 0.0 && !has_elevation) {
            text = std::to_string(static_cast<int>(std::round(dist))) + "m";
            fill_alpha = 128;
            font_size = 14;
        } else if (true_dist > 0.0) {
            text = std::to_string(static_cast<int>(std::round(true_dist))) + "m";
            fill_alpha = 179;
            font_size = 14;
        }
        const double bx = layout.x + i * (layout.box_w + layout.spacing);
        const double by = y;
        cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                        10, "", bgr, 0, 18, fill_alpha});
        cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                        10, "", bgr, 2, 18, 204});
        cmds.push_back({OverlayCommand::Type::TextCenter, bx, by, bx + layout.box_w, by + layout.box_h,
                        0, text, hexToBgr("#FFFFFF"), 1, font_size, 255});
        ++i;
    }
}

void SpecialAssistants::run() {
    while (running_) {
        std::vector<OverlayCommand> cmds;
        if (display_enabled_) {
            const auto dists = minimap_.measuredDistance();
            const auto elevs = elevation_.measuredElevations();
            std::string weapon;
            std::unordered_map<std::string, bool> manual;
            std::unordered_map<std::string, std::string> hex;
            {
                std::lock_guard lock(mutex_);
                weapon = current_weapon_;
                manual = manual_;
                hex = hex_;
            }
            if (manual["mortar"]) {
                drawMortar(dists, elevs, hex, cmds);
            }
            if (manual["mortar"] && !large_map_.isBusy()) {
                drawLargeMapDistances(large_map_.measuredDistance(), elevs, hex, cmds);
            }
            if (manual["rocket"]) {
                drawRocket(dists, hex, cmds);
            }
            if (manual["vss"]) {
                drawVss(dists, hex, cmds);
            }
            if (manual["crossbow"]) {
                drawCrossbow(dists, hex, cmds);
            }
        }
        overlay_.setCommands(std::move(cmds));
        overlay_.pumpMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

} // namespace pubg
