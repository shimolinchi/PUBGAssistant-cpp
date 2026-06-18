#pragma once

#include "Config.hpp"
#include "OverlayWindow.hpp"
#include "RegionManager.hpp"
#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

#include <condition_variable>

namespace pubg {

// 大地图一次性测距模块，对应 Python 版 modules/largemap_radar.py。
// F4 触发后进入等待点击/计算模式，截取 largemap_region，识别大地图标点并把结果显示在小地图上方。
class LargeMapRadar {
public:
    LargeMapRadar(Config& config, RegionManager& regions);
    ~LargeMapRadar();

    // 开关结果 HUD 显示。通常跟 F2 瞄准辅助同步。
    void setDisplay(bool visible);

    // 更新色盲模式下的标点颜色阈值。
    void setMarkerColors(std::vector<MarkerColor> colors);

    // 进入/退出一次性测距模式。主窗口 F4 会调用。
    void toggleMode();

    // 取消当前等待/计算状态。
    void cancel();

    // 鼠标点击回调。等待模式下记录玩家在大地图上的位置，并启动单帧计算。
    void onMouseClick(int x, int y, bool pressed);

    // 最近一次测得的各颜色标点距离。
    [[nodiscard]] DistanceMap measuredDistance() const;
    [[nodiscard]] bool isBusy() const;

private:
    // 常驻工作线程循环：等待点击投递的测距任务并执行，避免在输入钩子线程上做重活。
    void workerLoop();

    // 截取大地图并匹配各颜色标点。
    void processSingleFrame(int player_x, int player_y);

    // 在 HUD 上显示等待点击、计算中或结果卡片。
    void renderHud(const std::string& prompt = {});

    Config& config_;
    RegionManager& regions_;
    std::vector<MarkerColor> colors_;
    std::vector<cv::Mat> point_templates_;
    int max_tpl_w_ = 1;
    int max_tpl_h_ = 1;
    cv::Mat kernel_ = cv::Mat::ones(3, 3, CV_8U);
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic_bool display_{false};
    std::atomic_bool stop_{false};
    bool waiting_click_ = false;
    bool calculating_ = false;
    std::atomic_bool cancel_requested_{false};
    // 待处理的一次性测距任务：点击在钩子线程写入，工作线程取出执行。
    bool job_pending_ = false;
    int job_x_ = 0;
    int job_y_ = 0;
    DistanceMap distances_;
    std::thread worker_;
    OverlayWindow overlay_;
};

} // namespace pubg
