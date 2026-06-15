#pragma once

#include "Config.hpp"

namespace pubg {

// 全局区域管理器，对应 Python 版 modules/region_manager.py 的核心数据部分。
// 负责从 config.json 的 real_regions/real_scales 提供截图区域和比例尺。
class RegionManager {
public:
    // 构造时读取当前屏幕尺寸，并同步 crosshair_region 到配置中。
    explicit RegionManager(Config& config);

    // 按名称读取真实截图区域，例如 "minimap_region"、"weapon_region"。
    // 返回 nullopt 表示配置不存在或宽高无效。
    [[nodiscard]] std::optional<Rect> getRealRegion(const std::string& name) const;

    // 按名称读取真实比例尺，例如 "largemap_1km_px"；找不到时返回 fallback。
    [[nodiscard]] double getRealScale(const std::string& name, double fallback = 0.0) const;

    // 当前主显示器物理宽度，HUD 定位和全屏 overlay 使用。
    [[nodiscard]] int screenWidth() const noexcept { return screen_w_; }

    // 当前主显示器物理高度，HUD 定位和全屏 overlay 使用。
    [[nodiscard]] int screenHeight() const noexcept { return screen_h_; }

    // 写入/更新某个真实截图区域。后续校准 UI 会调用。
    void setRealRegion(const std::string& name, Rect rect);

    // 写入/更新某个比例尺。后续大地图 1km 校准会调用。
    void setRealScale(const std::string& name, double value);

    // 根据当前屏幕尺寸生成准星检测区域，对应 Python 版自动同步 crosshair_region 的逻辑。
    void syncCrosshairRegion();

private:
    Config& config_;
    int screen_w_ = 1920;
    int screen_h_ = 1080;
};

} // namespace pubg
