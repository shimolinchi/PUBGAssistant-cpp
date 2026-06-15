#pragma once

#include "Ballistics.hpp"
#include "ElevationRadar.hpp"
#include "MinimapRadar.hpp"
#include "OverlayWindow.hpp"

namespace pubg {

// 特殊武器 HUD 统筹模块。
// 对应 Python 版 rocket_assistant/vss_assistant/crossbow_assistant/mortar_assistant 的显示计算部分。
// 投掷物自动释放和 C4 状态机分别由 ThrowablesAssistant/C4Assistant 管理。
class SpecialAssistants {
public:
    // minimap 提供各颜色标点距离，elevation 提供测高比例，ballistics 负责弹道插值。
    SpecialAssistants(Config& config, RegionManager& regions, MinimapRadar& minimap, ElevationRadar& elevation);

    // 停止后台线程。
    ~SpecialAssistants();

    // 开关特殊武器总显示层。通常跟 F2 瞄准辅助同步。
    void setDisplayEnabled(bool enabled);

    // 更新当前手持武器。Rocket/VSS/Crossbow 会根据武器名自动显示对应 HUD。
    void setCurrentWeapon(const std::string& weapon);

    // 手动强制开启/关闭某个助手，例如 key="rocket"。
    // 对应 Python 版手动特殊武器按钮的概念。
    void setManualEnabled(const std::string& key, bool enabled);

    // 更新色盲模式颜色。地图页切换 pnt_color_mode 后即时同步 HUD。
    void setMarkerHex(std::unordered_map<std::string, std::string> hex);

    // 显式关闭线程和 overlay。App 退出时调用。
    void shutdown();

private:
    // 后台 HUD 循环：读取 minimap/elevation 数据，按当前武器生成绘制命令。
    void run();

    // 火箭筒 HUD：根据距离和 rocketRatio 绘制水平瞄准线。
    void drawRocket(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                    std::vector<OverlayCommand>& cmds);

    // VSS HUD：根据距离和 vssDropRatio 绘制下坠参考线。
    void drawVss(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                 std::vector<OverlayCommand>& cmds);

    // 十字弩 HUD：根据距离和 crossbowDropRatio 绘制 X 形落点标记。
    void drawCrossbow(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                      std::vector<OverlayCommand>& cmds);

    // 迫击炮 HUD：结合小地图水平距离和垂直测高比例，显示修正后的打击距离。
    void drawMortar(const DistanceMap& dists, const ElevationMap& elevs,
                    const std::unordered_map<std::string, std::string>& hex,
                    std::vector<OverlayCommand>& cmds);

    // 外部依赖：配置/区域/小地图距离/垂直测高。
    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    ElevationRadar& elevation_;

    // 弹道插值器和颜色名到 HUD 颜色的映射。
    Ballistics ballistics_;
    std::unordered_map<std::string, std::string> hex_;

    // 后台 HUD 线程生命周期和显示总开关。
    std::atomic_bool running_{true};
    std::atomic_bool display_enabled_{false};
    std::thread worker_;

    // 当前手持武器和手动强制开关。
    std::mutex mutex_;
    std::string current_weapon_;
    std::unordered_map<std::string, bool> manual_;

    // 所有特殊武器标记共享一个 overlay。
    OverlayWindow overlay_;
};

} // namespace pubg
