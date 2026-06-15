#include "ElevationRadar.hpp"

namespace pubg {

ElevationRadar::ElevationRadar(Config& config, RegionManager& regions, int fps)
    : config_(config), regions_(regions), fps_(fps), colors_(config.markerColors()) {
    point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt/elevation"));
    overlay_.create(L"PUBGAssistant Elevation", regions_.screenWidth(), regions_.screenHeight(), true);
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
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            double best_y = -1.0;
            double best_area = 0.0;
            for (const auto& contour : contours) {
                const double area = cv::contourArea(contour);
                if (area > best_area) {
                    auto b = cv::boundingRect(contour);
                    best_area = area;
                    best_y = b.y + b.height / 2.0;
                }
            }
            if (best_y >= 0.0) {
                const double ratio = best_y / std::max(1, rect->height);
                next[c.name] = ratio;
                if (display_) {
                    const double ax = rect->left + rect->width / 2.0;
                    const double ay = rect->top + best_y;
                    const auto color = hexToBgr(c.hex);
                    cmds.push_back({OverlayCommand::Type::Circle, ax, ay, 0, 0, 2.0, "", color, 1});
                    cmds.push_back({OverlayCommand::Type::Line, ax - 8.0, ay + 8.0, ax - 16.0, ay + 16.0, 0, "", color, 1});
                    cmds.push_back({OverlayCommand::Type::Line, ax + 8.0, ay + 8.0, ax + 16.0, ay + 16.0, 0, "", color, 1});
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
