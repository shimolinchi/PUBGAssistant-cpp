#include "MapPointAssistant.hpp"

namespace pubg {

namespace {

struct PointCategoryStyle {
    const char* name;
    cv::Scalar color;
};

PointCategoryStyle styleForCategory(const std::string& key) {
    if (key == "vehicles") return {"载具", cv::Scalar(255, 229, 0)};
    if (key == "planes") return {"飞机", cv::Scalar(0, 176, 255)};
    if (key == "rooms") return {"密室", cv::Scalar(64, 255, 0)};
    if (key == "bear_caves") return {"熊洞", cv::Scalar(255, 0, 170)};
    if (key == "crowbar_rooms") return {"撬棍房", cv::Scalar(230, 0, 255)};
    if (key == "lab_camps") return {"实验营地", cv::Scalar(179, 255, 0)};
    if (key == "safty_doors") return {"安全门", cv::Scalar(26, 26, 255)};
    return {"其他", cv::Scalar(0, 255, 255)};
}

} // namespace

MapPointAssistant::MapPointAssistant(Config& config, RegionManager& regions)
    : config_(config), regions_(regions) {
    overlay_.create(L"PUBGAssistant MapPoints", regions_.screenWidth(), regions_.screenHeight(), true);
    overlay_.show(false);
}

void MapPointAssistant::setEnabled(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        enabled_ = enabled;
    }
    overlay_.show(enabled);
    render();
}

void MapPointAssistant::setMap(const std::string& map_name) {
    {
        std::lock_guard lock(mutex_);
        current_map_ = map_name;
    }
    render();
}

void MapPointAssistant::setMarkerSize(const std::string& size) {
    {
        std::lock_guard lock(mutex_);
        marker_size_ = size;
    }
    render();
}

void MapPointAssistant::setCategoryEnabled(const std::string& group_key, bool enabled) {
    {
        std::lock_guard lock(mutex_);
        groups_[group_key] = enabled;
    }
    render();
}

std::vector<std::string> MapPointAssistant::categoryKeysForGroup(const std::string& group) const {
    if (group == "vehicles") return {"vehicles"};
    if (group == "planes") return {"planes"};
    if (group == "rooms") return {"rooms"};
    return {"bear_caves", "crowbar_rooms", "lab_camps", "safty_doors", "other"};
}

void MapPointAssistant::render() {
    bool enabled = false;
    std::string current_map;
    std::string marker_size;
    std::unordered_map<std::string, bool> groups;
    {
        std::lock_guard lock(mutex_);
        enabled = enabled_;
        current_map = current_map_;
        marker_size = marker_size_;
        groups = groups_;
    }
    if (!enabled) {
        overlay_.clear();
        return;
    }
    auto rect = regions_.getRealRegion("largemap_region");
    if (!rect) {
        return;
    }

    const Json data = config_.read([&](const Json& root) -> Json {
        if (!root.contains("map_data")) {
            return Json();
        }
        const auto& maps = root["map_data"];
        return maps.contains(current_map) ? maps[current_map] : Json();
    });
    if (!data.is_object()) {
        return;
    }

    const int radius = marker_size == "small" ? 2 : (marker_size == "large" ? 4 : 3);
    const int line_width = marker_size == "large" ? 3 : 1;
    std::vector<OverlayCommand> cmds;
    std::vector<std::pair<std::string, cv::Scalar>> legend;
    const std::vector<std::string> group_order{"vehicles", "planes", "rooms", "other"};
    for (const auto& group : group_order) {
        const auto enabled_it = groups.find(group);
        const bool group_enabled = enabled_it != groups.end() && enabled_it->second;
        if (!group_enabled) {
            continue;
        }
        for (const auto& key : categoryKeysForGroup(group)) {
            if (!data.contains(key) || !data[key].is_array()) {
                continue;
            }
            const auto style = styleForCategory(key);
            bool has_any = false;
            for (const auto& pt : data[key]) {
                if (!pt.is_array() || pt.size() < 2) {
                    continue;
                }
                const double nx = pt[0].get<double>();
                const double ny = pt[1].get<double>();
                const double x = rect->left + rect->width * (0.5 + nx / 2.0);
                const double y = rect->top + rect->height * (0.5 - ny / 2.0);
                cmds.push_back({OverlayCommand::Type::Circle, x, y, 0, 0, static_cast<double>(radius), "", style.color, line_width});
                has_any = true;
            }
            if (has_any) {
                legend.emplace_back(style.name, style.color);
            }
        }
    }

    double legend_y = 42.0;
    for (const auto& [name, color] : legend) {
        cmds.push_back({OverlayCommand::Type::Circle, 40.0, legend_y, 0, 0, 5.0, "", color, 1});
        cmds.push_back({OverlayCommand::Type::Text, 53.0, legend_y - 8.0, 0, 0, 0, name, color, 1, 13});
        legend_y += 22.0;
    }
    overlay_.setCommands(std::move(cmds));
    overlay_.pumpMessages();
}

} // namespace pubg
