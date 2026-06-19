#pragma once

#include "RegionManager.hpp"
#include "ScreenCapture.hpp"

namespace pubg {

// 迫击炮上炮检测。检测屏幕上的 F 下炮标志，逻辑与装备栏编号模板匹配保持一致。
class MortarMountDetector {
public:
    MortarMountDetector(Config& config, RegionManager& regions);

    [[nodiscard]] bool detectMounted();

private:
    void loadTemplates();

    Config& config_;
    RegionManager& regions_;
    std::vector<cv::Mat> templates_;
};

} // namespace pubg
