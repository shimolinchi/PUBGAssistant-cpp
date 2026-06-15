#include "GestureIdentifier.hpp"

namespace pubg {

GestureIdentifier::GestureIdentifier(Config& config, RegionManager& regions, int fps, double threshold)
    : config_(config), regions_(regions), fps_(fps), threshold_(threshold) {
    templates_ = TemplateMatcher::loadGrayTemplates(config_.paths().templatesDir() / "gestures");
    const auto scaling = config_.read([](const Json& data) {
        return data.value("region_scaling_settings", Json::object())
            .value("stance_region", Json::object());
    });
    target_w_ = scaling.value("width", 0);
    target_h_ = scaling.value("height", 0);
    if ((target_w_ <= 0 || target_h_ <= 0) && !templates_.empty()) {
        const auto& first = templates_.begin()->second.front();
        target_w_ = first.cols;
        target_h_ = first.rows;
    }
}

GestureIdentifier::~GestureIdentifier() {
    setEnabled(false);
}

void GestureIdentifier::setEnabled(bool enabled, Callback cb) {
    if (cb) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(cb);
    }
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        worker_ = std::thread(&GestureIdentifier::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

std::string GestureIdentifier::currentGesture() const {
    std::lock_guard lock(mutex_);
    return current_gesture_;
}

GestureIdentifier::Callback GestureIdentifier::callbackCopy() const {
    std::lock_guard lock(mutex_);
    return callback_;
}

std::pair<std::string, double> GestureIdentifier::identifyOnce(ScreenCapture& capture) {
    const auto rect = regions_.getRealRegion("stance_region");
    if (!rect || templates_.empty() || target_w_ <= 0 || target_h_ <= 0) {
        return {"", 0.0};
    }
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return {"", 0.0};
    }
    if (bgr.cols != target_w_ || bgr.rows != target_h_) {
        cv::resize(bgr, bgr, {target_w_, target_h_});
    }
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return TemplateMatcher::matchGray(gray, templates_);
}

void GestureIdentifier::run() {
    ScreenCapture capture;
    while (!stop_) {
        const double start = nowSeconds();
        auto [gesture, score] = identifyOnce(capture);
        std::string notify_gesture;
        double notify_score = 0.0;
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            if (score >= threshold_) {
                if (gesture != current_gesture_) {
                    current_gesture_ = gesture;
                    notify_gesture = current_gesture_;
                    notify_score = score;
                    notify = true;
                }
            } else if (!current_gesture_.empty()) {
                current_gesture_.clear();
                notify_gesture.clear();
                notify_score = 0.0;
                notify = true;
            }
        }
        if (notify) {
            if (auto cb = callbackCopy()) {
                cb(notify_gesture, notify_score);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

} // namespace pubg
