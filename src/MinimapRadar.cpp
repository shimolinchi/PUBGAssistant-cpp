#include "MinimapRadar.hpp"

namespace pubg {

MinimapRadar::MinimapRadar(Config& config, RegionManager& regions, int fps)
    : config_(config), regions_(regions), fps_(fps), colors_(config.markerColors()) {
    auto preferred = config_.paths().templatePath("pnt/minimap");
    point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(preferred);
    if (point_templates_.empty()) {
        point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt"));
    }
    for (const auto& tpl : point_templates_) {
        max_tpl_w_ = std::max(max_tpl_w_, tpl.cols);
        max_tpl_h_ = std::max(max_tpl_h_, tpl.rows);
    }
    for (const auto& c : colors_) {
        distances_[c.name] = 0.0;
    }
    overlay_.create(L"PUBGAssistant Minimap", regions_.screenWidth(), regions_.screenHeight(), true, true);
}

MinimapRadar::~MinimapRadar() {
    setEnabled(false);
}

void MinimapRadar::setEnabled(bool enabled) {
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        worker_ = std::thread(&MinimapRadar::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        overlay_.clear();
    }
}

void MinimapRadar::setDisplay(bool display) {
    display_ = display;
    overlay_.show(display);
}

void MinimapRadar::setMarkerColors(std::vector<MarkerColor> colors) {
    std::lock_guard lock(mutex_);
    colors_ = std::move(colors);
    for (const auto& c : colors_) {
        distances_.try_emplace(c.name, 0.0);
    }
}

std::vector<TargetPoint> MinimapRadar::latestTargets() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

DistanceMap MinimapRadar::measuredDistance() const {
    std::lock_guard lock(mutex_);
    return distances_;
}

std::vector<TargetPoint> MinimapRadar::matchColorCandidates(const cv::Mat& mask, const MarkerColor& color) {
    std::vector<TargetPoint> out;
    if (point_templates_.empty() || cv::countNonZero(mask) == 0) {
        return out;
    }
    cv::Mat search;
    cv::dilate(mask, search, kernel_, {-1, -1}, 1);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(search, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : contours) {
        cv::Rect bound = cv::boundingRect(contour);
        if (bound.area() < 4) {
            continue;
        }
        const int x1 = std::max(0, bound.x - max_tpl_w_);
        const int y1 = std::max(0, bound.y - max_tpl_h_);
        const int x2 = std::min(mask.cols, bound.x + bound.width + max_tpl_w_);
        const int y2 = std::min(mask.rows, bound.y + bound.height + max_tpl_h_);
        cv::Mat roi = mask(cv::Rect(x1, y1, x2 - x1, y2 - y1));

        for (const auto& tpl : point_templates_) {
            if (roi.rows < tpl.rows || roi.cols < tpl.cols) {
                continue;
            }
            cv::Mat res;
            cv::matchTemplate(roi, tpl, res, cv::TM_CCOEFF_NORMED);
            double max_val = 0.0;
            cv::Point max_loc;
            cv::minMaxLoc(res, nullptr, &max_val, nullptr, &max_loc);
            if (max_val >= 0.75) {
                TargetPoint pt;
                pt.color_name = color.name;
                pt.hex = color.hex;
                pt.x = x1 + max_loc.x + tpl.cols / 2.0;
                pt.y = y1 + max_loc.y + tpl.rows;
                pt.confidence = max_val;
                out.push_back(pt);
            }
        }
    }
    return out;
}

void MinimapRadar::draw(const std::vector<TargetPoint>& points, const Rect& rect) {
    if (!display_) {
        return;
    }
    std::vector<OverlayCommand> cmds;
    const double cx = rect.left + rect.width / 2.0;
    const double cy = rect.top + rect.height / 2.0;
    cmds.push_back({OverlayCommand::Type::Line, cx - 6, cy, cx + 6, cy, 0, "", {0, 0, 0}, 4});
    cmds.push_back({OverlayCommand::Type::Line, cx, cy - 6, cx, cy + 6, 0, "", {0, 0, 0}, 4});
    cmds.push_back({OverlayCommand::Type::Line, cx - 6, cy, cx + 6, cy, 0, "", {255, 255, 255}, 2});
    cmds.push_back({OverlayCommand::Type::Line, cx, cy - 6, cx, cy + 6, 0, "", {255, 255, 255}, 2});
    for (const auto& p : points) {
        auto color = hexToBgr(p.hex);
        const double ax = rect.left + p.x;
        const double ay = rect.top + p.y;
        cmds.push_back({OverlayCommand::Type::Circle, ax, ay, 0, 0, 3, "", color, 2});
        cmds.push_back({OverlayCommand::Type::Rect, ax - 10, ay - 20, ax + 10, ay, 0, "", color, 2});
        cmds.push_back({OverlayCommand::Type::Text, ax - 18, ay + 8, 0, 0, 0, std::to_string(static_cast<int>(std::round(p.distance_m))) + "m", color, 1, 16});
    }
    overlay_.setCommands(std::move(cmds));
}

void MinimapRadar::run() {
    ScreenCapture capture;
    while (!stop_) {
        const double start = nowSeconds();
        auto rect = regions_.getRealRegion("minimap_region");
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
        const double center = rect->width / 2.0;

        std::vector<MarkerColor> colors;
        {
            std::lock_guard lock(mutex_);
            colors = colors_;
        }

        std::vector<TargetPoint> found;
        DistanceMap distances;
        const double now = nowSeconds();
        for (const auto& c : colors) {
            cv::Mat mask;
            cv::inRange(hsv, c.lower_hsv, c.upper_hsv, mask);
            auto candidates = matchColorCandidates(mask, c);
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return a.confidence > b.confidence;
            });
            std::vector<TargetPoint> nms;
            for (auto item : candidates) {
                bool too_close = false;
                for (const auto& existing : nms) {
                    if (std::hypot(item.x - existing.x, item.y - existing.y) < 8.0) {
                        too_close = true;
                        break;
                    }
                }
                if (!too_close) {
                    const double px = std::hypot(item.x - center, item.y - center);
                    item.distance_m = (px / std::max(1, rect->width)) * 700.0;
                    nms.push_back(item);
                }
            }
            if (!nms.empty()) {
                auto item = nms.front();
                if (auto it = stable_targets_.find(c.name); it != stable_targets_.end()) {
                    item.x = it->second.x * 0.70 + item.x * 0.30;
                    item.y = it->second.y * 0.70 + item.y * 0.30;
                    item.distance_m = it->second.distance_m * 0.70 + item.distance_m * 0.30;
                    item.confidence = std::max(item.confidence, it->second.confidence);
                }
                stable_targets_[c.name] = item;
                stable_seen_times_[c.name] = now;
                distances[c.name] = item.distance_m;
                found.push_back(item);
            } else if (auto seen = stable_seen_times_.find(c.name);
                       seen != stable_seen_times_.end() && now - seen->second <= 0.25 &&
                       stable_targets_.contains(c.name)) {
                const auto item = stable_targets_[c.name];
                distances[c.name] = item.distance_m;
                found.push_back(item);
            } else {
                stable_targets_.erase(c.name);
                stable_seen_times_.erase(c.name);
                distances[c.name] = 0.0;
            }
        }
        {
            std::lock_guard lock(mutex_);
            latest_ = found;
            distances_ = distances;
        }
        draw(found, *rect);
        overlay_.pumpMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

} // namespace pubg
