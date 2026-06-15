#include "SpecialAssistants.hpp"

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
} // namespace

SpecialAssistants::SpecialAssistants(Config& config, RegionManager& regions, MinimapRadar& minimap, ElevationRadar& elevation)
    : config_(config), regions_(regions), minimap_(minimap), elevation_(elevation), ballistics_(config), hex_(config.markerHex()) {
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
    for (const auto& [color, dist] : dists) {
        if (dist <= 0.0 || dist > 170.0) {
            continue;
        }
        const double ratio = ballistics_.rocketRatio(dist);
        const double y = std::min(cy + ratio * line_len, regions_.screenHeight() - 10.0);
        const auto bgr = hexToBgr(hex.count(color) ? hex.at(color) : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 28, y, cx + 28, y, 0, "", bgr, 3});
        cmds.push_back({OverlayCommand::Type::Text, cx + 36, y - 10, 0, 0, 0, std::to_string(static_cast<int>(std::round(dist))) + "m", bgr, 1, 18});
    }
}

void SpecialAssistants::drawVss(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                                std::vector<OverlayCommand>& cmds) {
    const double cx = regions_.screenWidth() / 2.0;
    const double cy = regions_.screenHeight() / 2.0;
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
    const double cx = regions_.screenWidth() / 2.0;
    const double cy = regions_.screenHeight() / 2.0;
    for (const auto& [color, dist] : dists) {
        if (dist <= 0.0 || dist > 350.0) {
            continue;
        }
        const double y = cy + ballistics_.crossbowDropRatio(dist) * regions_.screenHeight();
        const auto bgr = hexToBgr(hex.count(color) ? hex.at(color) : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 9, y - 9, cx + 9, y + 9, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Line, cx - 9, y + 9, cx + 9, y - 9, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Text, cx + 24, y - 9, 0, 0, 0, std::to_string(static_cast<int>(std::round(dist))) + "m", bgr, 1, 16});
    }
}

void SpecialAssistants::drawMortar(const DistanceMap& dists, const ElevationMap& elevs,
                                   const std::unordered_map<std::string, std::string>& hex,
                                   std::vector<OverlayCommand>& cmds) {
    const auto layout = assistHudLayout(regions_);
    int i = 0;
    for (const auto& [color, dist] : dists) {
        const std::string color_hex = hex.count(color) ? hex.at(color) : "#FFFFFF";
        const auto bgr = hexToBgr(color_hex);
        const auto elev_it = elevs.find(color);
        const bool has_elevation = elev_it != elevs.end() && elev_it->second > 0.0;
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
        const double by = layout.mortar_y;
        cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                        10, "", bgr, 0, 18, fill_alpha});
        cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                        10, "", bgr, 2, 18, 204});
        cmds.push_back({OverlayCommand::Type::TextCenter, bx, by, bx + layout.box_w, by + layout.box_h,
                        0, text, hexToBgr("#FFFFFF"), 1, font_size, 255});
        ++i;
        if (i >= 4) break;
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
            drawMortar(dists, elevs, hex, cmds);
            if (weapon == "Rocket" || manual["rocket"]) {
                drawRocket(dists, hex, cmds);
            }
            if (weapon == "VSS" || manual["vss"]) {
                drawVss(dists, hex, cmds);
            }
            if (weapon == "Crossbow" || manual["crossbow"]) {
                drawCrossbow(dists, hex, cmds);
            }
        }
        overlay_.setCommands(std::move(cmds));
        overlay_.pumpMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

} // namespace pubg
