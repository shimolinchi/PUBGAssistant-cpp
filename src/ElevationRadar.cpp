#include "ElevationRadar.hpp"

namespace pubg {

ElevationRadar::ElevationRadar(Config& config, RegionManager& regions, int fps)
    : config_(config), regions_(regions), fps_(fps), colors_(config.markerColors()) {
    point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt/elevation"));
    if (point_templates_.empty()) {
        point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt"));
    }
    for (const auto& tpl : point_templates_) {
        max_tpl_w_ = std::max(max_tpl_w_, tpl.cols);
        max_tpl_h_ = std::max(max_tpl_h_, tpl.rows);
    }
    regions_.createOverlay(overlay_, L"PUBGAssistant Elevation", true);
}

ElevationRadar::~ElevationRadar() {
    setEnabled(false);
}

void ElevationRadar::setEnabled(bool enabled) {
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        worker_ = std::thread(&ElevationRadar::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        overlay_.clear();
    }
}

void ElevationRadar::setDisplay(bool display) {
    display_ = display;
    overlay_.show(display);
}

void ElevationRadar::setMarkerColors(std::vector<MarkerColor> colors) {
    std::lock_guard lock(mutex_);
    colors_ = std::move(colors);
}

ElevationMap ElevationRadar::measuredElevations() const {
    std::lock_guard lock(mutex_);
    return elevations_;
}

ElevationRadar::Match ElevationRadar::matchMarkerInMask(const cv::Mat& mask) const {
    if (point_templates_.empty() || cv::countNonZero(mask) == 0) {
        return {};
    }
    cv::Mat search;
    cv::dilate(mask, search, kernel_, {-1, -1}, 1);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(search, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    Match best;
    for (const auto& contour : contours) {
        const cv::Rect bound = cv::boundingRect(contour);
        if (bound.area() < 4) {
            continue;
        }
        const int x1 = std::max(0, bound.x - max_tpl_w_);
        const int y1 = std::max(0, bound.y - max_tpl_h_);
        const int x2 = std::min(mask.cols, bound.x + bound.width + max_tpl_w_);
        const int y2 = std::min(mask.rows, bound.y + bound.height + max_tpl_h_);
        const cv::Mat roi = mask(cv::Rect(x1, y1, x2 - x1, y2 - y1));

        for (const auto& tpl : point_templates_) {
            if (tpl.empty() || roi.rows < tpl.rows || roi.cols < tpl.cols) {
                continue;
            }
            cv::Mat res;
            cv::matchTemplate(roi, tpl, res, cv::TM_CCOEFF_NORMED);
            double max_val = 0.0;
            cv::Point max_loc;
            cv::minMaxLoc(res, nullptr, &max_val, nullptr, &max_loc);
            if (std::isfinite(max_val) && max_val >= 0.55 && max_val > best.score) {
                best.score = max_val;
                best.left = x1 + max_loc.x;
                best.top = y1 + max_loc.y;
                best.right = best.left + tpl.cols;
                best.bottom = best.top + tpl.rows;
                best.y = best.bottom;
            }
        }
    }
    return best;
}

void ElevationRadar::run() {
    ScreenCapture capture;
    while (!stop_) {
        const double start = nowSeconds();
        auto rect = regions_.getRealRegion("elevation_region");
        if (!rect) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        cv::Mat bgr = capture.grabBgr(*rect);
        if (bgr.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        cv::Mat hsv;
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

        ElevationMap next;
        std::vector<OverlayCommand> cmds;
        std::vector<MarkerColor> colors;
        {
            std::lock_guard lock(mutex_);
            colors = colors_;
        }
        for (const auto& c : colors) {
            cv::Mat mask;
            cv::inRange(hsv, c.lower_hsv, c.upper_hsv, mask);
            const auto match = matchMarkerInMask(mask);
            const double best_y = match.y;
            if (best_y >= 0.0) {
                const double ratio = best_y / std::max(1, rect->height);
                next[c.name] = ratio;
                if (display_) {
                    constexpr double padding = 2.0;
                    const double x1 = rect->left + match.left - padding;
                    const double y1 = rect->top + match.top - padding;
                    const double x2 = rect->left + match.right + padding;
                    const double y2 = rect->top + match.bottom + padding;
                    const auto color = hexToBgr(c.hex);
                    cmds.push_back({OverlayCommand::Type::Rect, x1, y1, x2, y2, 0, "", color, 0, 18, 70});
                    cmds.push_back({OverlayCommand::Type::Rect, x1, y1, x2, y2, 0, "", color, 2, 18, 220});
                }
            } else {
                next[c.name] = 0.0;
            }
        }
        {
            std::lock_guard lock(mutex_);
            elevations_ = next;
        }
        if (display_) {
            overlay_.setCommands(std::move(cmds));
        }
        overlay_.pumpMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

} // namespace pubg
