#pragma once

#include "RegionManager.hpp"
#include "ScreenCapture.hpp"

namespace pubg {

// SR 呼吸控制用的镜内边缘运动检测器。
// 对应 Python 版 ScopeMotionTracker/压枪模块中的 scope_top_edge_*_region 检测逻辑。
class ScopeMotionTracker {
public:
    ScopeMotionTracker(RegionManager& regions, std::string region_name, Json config = Json::object());

    void setRegionName(std::string region_name);
    void reset();

    // 检测当前帧相对上一帧的垂直位移。
    // 返回 dy/confidence/found；found=false 表示没找到可靠边缘。
    std::tuple<double, double, bool> detectMotion(ScreenCapture& capture);

private:
    std::tuple<double, double, bool> detectTopEdge(const cv::Mat& frame, const Rect& rect) const;
    static double median(std::vector<double> values);

    RegionManager& regions_;
    std::string region_name_;
    int black_threshold_ = 70;
    double min_bright_ratio_ = 0.35;
    double min_gradient_ = 0.12;
    double max_edge_jump_ = 45.0;
    int min_points_ = 8;
    std::optional<double> last_edge_y_;
    double last_confidence_ = 0.0;
};

} // namespace pubg
