#pragma once

#include "Config.hpp"
#include "OverlayWindow.hpp"
#include "RegionManager.hpp"
#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

namespace pubg {

// 小地图实时测距模块，对应 Python 版 modules/minimap_radar.py。
// 它截取 minimap_region，按 HSV 颜色阈值和标点模板检测队友标点，并换算到游戏距离。
class MinimapRadar {
public:
    // config 提供 pnt_colors 和模板路径，regions 提供 minimap_region。
    MinimapRadar(Config& config, RegionManager& regions, int fps = 60);

    // 停止后台线程。
    ~MinimapRadar();

    // 开关小地图检测线程。
    void setEnabled(bool enabled);

    // 开关小地图检测结果 HUD 显示；检测本身可继续运行。
    void setDisplay(bool display);

    // 更新标点颜色阈值，后续 UI 切换色盲模式时调用。
    void setMarkerColors(std::vector<MarkerColor> colors);

    // 返回最近检测到的标点列表。
    [[nodiscard]] std::vector<TargetPoint> latestTargets() const;

    // 返回各颜色标点到玩家的距离，供特殊武器和迫击炮模块使用。
    [[nodiscard]] DistanceMap measuredDistance() const;

private:
    // 在单个颜色 mask 中用标点 alpha 模板找候选点。
    std::vector<TargetPoint> matchColorCandidates(const cv::Mat& mask, const MarkerColor& color);

    // 后台检测循环：截图、HSV 阈值、模板匹配、NMS、距离换算。
    void run();

    // 将检测结果画到 overlay 上，主要用于调试和实时距离显示。
    void draw(const std::vector<TargetPoint>& points, const Rect& rect);

    // 外部依赖：配置提供颜色/模板路径，RegionManager 提供 minimap_region 和屏幕尺寸。
    Config& config_;
    RegionManager& regions_;

    // 检测节奏和当前色盲模式颜色配置。
    int fps_ = 60;
    std::vector<MarkerColor> colors_;

    // 标点形状模板缓存，以及模板最大尺寸，用于缩小局部搜索区域。
    std::vector<cv::Mat> point_templates_;
    int max_tpl_w_ = 1;
    int max_tpl_h_ = 1;
    cv::Mat kernel_ = cv::Mat::ones(3, 3, CV_8U);

    // 后台线程和显示开关。
    mutable std::mutex mutex_;
    std::atomic_bool enabled_{false};
    std::atomic_bool display_{true};
    std::atomic_bool stop_{false};
    std::thread worker_;

    // 最近一次检测结果。latest_ 给调试/HUD，distances_ 给特殊武器计算。
    std::vector<TargetPoint> latest_;
    DistanceMap distances_;
    std::unordered_map<std::string, TargetPoint> stable_targets_;
    std::unordered_map<std::string, double> stable_seen_times_;

    // 小地图标点和距离文字的透明 overlay。
    OverlayWindow overlay_;
};

} // namespace pubg
