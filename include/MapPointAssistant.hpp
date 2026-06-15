#pragma once

#include "Config.hpp"
#include "OverlayWindow.hpp"
#include "RegionManager.hpp"

namespace pubg {

// 大地图资源点位显示模块，对应 Python 版 modules/map_assistant.py。
// 使用 config.json 的 map_data，在 largemap_region 内按归一化坐标绘制载具、飞机、密室等点位。
class MapPointAssistant {
public:
    MapPointAssistant(Config& config, RegionManager& regions);

    // 开关点位 overlay。
    void setEnabled(bool enabled);

    // 选择当前地图，例如 "艾伦格 (Erangel)"。
    void setMap(const std::string& map_name);

    // 设置点位尺寸：small/medium/large。
    void setMarkerSize(const std::string& size);

    // 开关点位类别：vehicles/planes/rooms/other。
    void setCategoryEnabled(const std::string& group_key, bool enabled);

    // 重新读取配置并重画当前点位。
    void render();

private:
    // 将 config.json 中的分类 key 映射到 UI 上的四组按钮。
    [[nodiscard]] std::vector<std::string> categoryKeysForGroup(const std::string& group) const;

    Config& config_;
    RegionManager& regions_;
    std::mutex mutex_;
    bool enabled_ = false;
    std::string current_map_ = "艾伦格 (Erangel)";
    std::string marker_size_ = "medium";
    std::unordered_map<std::string, bool> groups_{
        {"vehicles", true}, {"planes", true}, {"rooms", true}, {"other", true}
    };
    OverlayWindow overlay_;
};

} // namespace pubg
