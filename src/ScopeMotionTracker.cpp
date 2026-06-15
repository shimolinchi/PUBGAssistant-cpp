#include "ScopeMotionTracker.hpp"

namespace pubg {

ScopeMotionTracker::ScopeMotionTracker(RegionManager& regions, std::string region_name)
    : regions_(regions), region_name_(std::move(region_name)) {}

void ScopeMotionTracker::setRegionName(std::string region_name) {
    region_name_ = std::move(region_name);
    reset();
}

void ScopeMotionTracker::reset() {
    previous_profile_.release();
}

std::tuple<double, double, bool> ScopeMotionTracker::detectMotion(ScreenCapture& capture) {
    auto rect = regions_.getRealRegion(region_name_);
    if (!rect) return {0.0, 0.0, false};
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) return {0.0, 0.0, false};
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Mat grad;
    cv::Sobel(gray, grad, CV_32F, 0, 1, 3);
    cv::Mat abs_grad;
    cv::convertScaleAbs(grad, abs_grad);
    cv::Mat profile;
    cv::reduce(abs_grad, profile, 1, cv::REDUCE_AVG, CV_32F);
    double confidence = 0.0;
    cv::minMaxLoc(profile, nullptr, &confidence);
    if (confidence < 8.0) {
        return {0.0, confidence / 255.0, false};
    }
    if (previous_profile_.empty()) {
        previous_profile_ = profile.clone();
        return {0.0, confidence / 255.0, false};
    }
    int best_shift = 0;
    double best_score = -1.0;
    for (int shift = -12; shift <= 12; ++shift) {
        double score = 0.0;
        int count = 0;
        for (int y = 0; y < profile.rows; ++y) {
            int py = y + shift;
            if (py < 0 || py >= previous_profile_.rows) continue;
            score -= std::abs(profile.at<float>(y, 0) - previous_profile_.at<float>(py, 0));
            ++count;
        }
        if (count > 0 && score > best_score) {
            best_score = score;
            best_shift = shift;
        }
    }
    previous_profile_ = profile.clone();
    return {static_cast<double>(best_shift), std::min(1.0, confidence / 255.0), true};
}

} // namespace pubg
