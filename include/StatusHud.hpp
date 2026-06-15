#pragma once

#include "Config.hpp"
#include "OverlayWindow.hpp"
#include "RegionManager.hpp"

namespace pubg {

// 左下角持久状态 HUD，对应 Python main.py 中 TransparentHudWindow 状态层。
// 显示标点颜色、识别/测距/压枪开关、装备栏状态、两个武器槽和当前姿势。
class StatusHud {
public:
    StatusHud(Config& config, RegionManager& regions);

    // 主开关状态变化时调用，更新“识别/测距/压枪”的颜色。
    void setSwitches(bool weapon_detection, bool display, bool recoil);

    // 装备栏状态变化时调用，status 使用 opened/closed/confirming。
    void setEquipmentStatus(const std::string& status);

    // 装备栏扫描完成时调用，保存 1/2 号武器和配件显示。
    void setEquipment(const std::map<int, WeaponSlotInfo>& equipment_slots);

    // 当前手持武器和姿势变化时调用。
    void setCurrentWeapon(const std::string& weapon);
    void setStance(const std::string& stance);

    // 标点颜色和色盲模式变化时调用。
    void setMarkerColor(const std::string& color);
    void reloadMarkerHex();

    // 手动/自动特殊助手状态变化时调用，决定是否显示“当前使用标点”。
    void setMarkerIndicatorVisible(bool visible);

    // 重新绘制 HUD。
    void render();

private:
    [[nodiscard]] std::string formatWeapon(const WeaponSlotInfo& info) const;
    [[nodiscard]] std::string displayWeaponName(const std::string& name) const;
    [[nodiscard]] cv::Scalar statusColor(const std::string& name) const;

    Config& config_;
    RegionManager& regions_;
    OverlayWindow overlay_;
    std::mutex mutex_;

    bool weapon_detection_enabled_ = true;
    bool display_enabled_ = false;
    bool recoil_enabled_ = false;
    bool marker_indicator_visible_ = false;
    std::string equipment_status_ = "closed";
    std::string current_weapon_;
    std::string current_stance_;
    std::string marker_color_ = "Yellow";
    std::unordered_map<std::string, std::string> marker_hex_;
    std::map<int, WeaponSlotInfo> equipment_;
};

} // namespace pubg
