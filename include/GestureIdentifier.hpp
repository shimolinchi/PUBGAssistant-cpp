#pragma once

#include "RegionManager.hpp"
#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

namespace pubg {

// 姿势识别模块，对应 Python 版 modules/gesture_identifier.py。
// 识别站立 stand、蹲下 squat、趴下 lie，用于压枪姿势倍率。
class GestureIdentifier {
public:
    // 回调参数：姿势名、匹配分数。姿势名为空表示未稳定识别。
    using Callback = std::function<void(const std::string&, double)>;

    // config 提供姿势模板和缩放尺寸，regions 提供 stance_region。
    GestureIdentifier(Config& config, RegionManager& regions, int fps = 30, double threshold = 0.65);

    // 停止后台线程。
    ~GestureIdentifier();

    // 开关姿势识别线程。cb 非空时更新回调。
    void setEnabled(bool enabled, Callback cb = {});

    // 返回最近一次稳定识别到的姿势名。
    [[nodiscard]] std::string currentGesture() const;

private:
    // 截取 stance_region 并完成一次灰度模板匹配。
    std::pair<std::string, double> identifyOnce(ScreenCapture& capture);

    // 后台识别循环，按 fps 更新姿势状态。
    void run();

    // 线程安全地复制当前回调，避免后台线程和 UI 线程同时访问 std::function。
    Callback callbackCopy() const;

    Config& config_;
    RegionManager& regions_;
    int fps_ = 30;
    double threshold_ = 0.65;
    int target_w_ = 0;
    int target_h_ = 0;
    std::unordered_map<std::string, std::vector<cv::Mat>> templates_;

    mutable std::mutex mutex_;
    std::atomic_bool enabled_{false};
    std::atomic_bool stop_{false};
    std::thread worker_;
    Callback callback_;
    std::string current_gesture_;
};

} // namespace pubg
