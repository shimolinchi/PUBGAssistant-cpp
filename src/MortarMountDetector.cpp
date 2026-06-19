#include "MortarMountDetector.hpp"

#include <iostream>

namespace pubg {

MortarMountDetector::MortarMountDetector(Config& config, RegionManager& regions)
    : config_(config), regions_(regions) {
    loadTemplates();
}

void MortarMountDetector::loadTemplates() {
    templates_.clear();
    auto loadFrom = [this](const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir)) {
            return;
        }
        for (const auto& file : std::filesystem::directory_iterator(dir)) {
            if (!file.is_regular_file() || file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_GRAYSCALE);
            if (img.empty()) {
                continue;
            }
            cv::Mat resized;
            cv::resize(img, resized, {30, 30});
            templates_.push_back(resized);
        }
    };

    const auto others = config_.paths().templatePath("others");
    loadFrom(others / "motar_sign");
    if (templates_.empty()) {
        loadFrom(others / "mortar_sign");
    }
    if (templates_.empty()) {
        loadFrom(others);
    }
    std::cerr << "[templates] mortar mount templates: " << templates_.size() << "\n";
}

bool MortarMountDetector::detectMounted() {
    const auto rect = regions_.getRealRegion("mortar_mount_region");
    if (!rect || templates_.empty()) {
        std::cerr << "[mortar-mount] missing region or templates\n";
        return false;
    }

    ScreenCapture capture;
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return false;
    }

    cv::Mat resized;
    cv::resize(bgr, resized, {35, 35});
    cv::Mat gray;
    cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);

    double best_score = 0.0;
    for (const auto& tpl : templates_) {
        if (tpl.rows > gray.rows || tpl.cols > gray.cols) {
            continue;
        }
        cv::Mat res;
        cv::matchTemplate(gray, tpl, res, cv::TM_CCOEFF_NORMED);
        double max_val = 0.0;
        cv::minMaxLoc(res, nullptr, &max_val);
        if (std::isfinite(max_val) && max_val > best_score) {
            best_score = max_val;
        }
        if (std::isfinite(max_val) && max_val >= 0.5) {
            std::cerr << "[mortar-mount] matched score=" << max_val << "\n";
            return true;
        }
    }
    std::cerr << "[mortar-mount] no match best=" << best_score << "\n";
    return false;
}

} // namespace pubg
