#pragma once

#include "MinimapRadar.hpp"

namespace pubg {

// 垂直测高模块，对应 Python 版 modules/elevation_radar.py 的核心检测逻辑。
// 它截取 elevation_region，找各颜色标点在垂直条中的 y 比例，用于迫击炮高低差修正。
class ElevationRadar {
public:
    // config 提供颜色阈值和模板路径，regions 提供 elevation_region。
    ElevationRadar(Config& config, RegionManager& regions, int fps = 30);

    // 停止后台线程。
    ~ElevationRadar();

    // 开关测高检测线程。
    void setEnabled(bool enabled);

    // 开关测高线 overlay 显示。通常主程序会检测开启但显示关闭。
    void setDisplay(bool display);

    // 更新色盲模式下的 HSV 阈值。
    void setMarkerColors(std::vector<MarkerColor> colors);

    // 返回各颜色标点的高度比例，0.0 表示未检测到。
    [[nodiscard]] ElevationMap measuredElevations() const;

private:
    struct Match {
        double y = -1.0;
        double score = 0.0;
    };

    [[nodiscard]] Match matchMarkerInMask(const cv::Mat& mask) const;

    // 后台循环：截图、HSV 阈值、轮廓定位、计算 y/height 比例。
    void run();

    // 外部依赖：配置提供颜色阈值和模板路径，RegionManager 提供 elevation_region。
    Config& config_;
    RegionManager& regions_;

    // 检测节奏、颜色阈值和标点模板缓存。
    int fps_ = 30;
    std::vector<MarkerColor> colors_;
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

    // 最近一次各颜色标点的高度比例，供迫击炮距离修正使用。
    ElevationMap elevations_;

    // 可选测高辅助线 overlay，默认主程序会关闭显示但保留检测。
    OverlayWindow overlay_;
};

} // namespace pubg
