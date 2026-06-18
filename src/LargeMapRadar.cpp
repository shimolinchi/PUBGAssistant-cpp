#include "LargeMapRadar.hpp"

namespace pubg {

namespace {
struct AssistHudLayout {
    double x = 0.0;
    double large_y = 0.0;
    double mortar_y = 0.0;
    double box_w = 90.0;
    double box_h = 25.0;
    double spacing = 10.0;
    double total_w = 390.0;
};

AssistHudLayout assistHudLayout(const RegionManager& regions) {
    AssistHudLayout layout;
    if (const auto minimap = regions.getRealRegion("minimap_region")) {
        layout.box_h = 25.0;
        layout.spacing = 10.0;
        const double panel_w = std::floor(minimap->width * 0.82);
        layout.box_w = std::floor((panel_w - 3.0 * layout.spacing) / 4.0);
        layout.total_w = 4.0 * layout.box_w + 3.0 * layout.spacing;
        layout.x = minimap->left + minimap->width - layout.total_w;
        layout.large_y = minimap->top - layout.box_h - layout.spacing - layout.box_h - 30.0;
        layout.large_y = std::max(0.0, layout.large_y);
        layout.mortar_y = layout.large_y + layout.box_h + layout.spacing;
    } else {
        layout.box_w = 90.0;
        layout.box_h = 25.0;
        layout.spacing = 15.0;
        layout.total_w = 4.0 * layout.box_w + 3.0 * layout.spacing;
        layout.x = std::max(0.0, regions.screenWidth() - layout.total_w - 25.0);
        layout.large_y = regions.screenHeight() * 0.465;
        layout.mortar_y = layout.large_y + layout.box_h + layout.spacing;
    }
    return layout;
}
} // namespace

LargeMapRadar::LargeMapRadar(Config& config, RegionManager& regions)
    : config_(config), regions_(regions), colors_(config.markerColors()) {
    point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt/largemap"));
    if (point_templates_.empty()) {
        point_templates_ = TemplateMatcher::loadAlphaBinaryTemplates(config_.paths().templatePath("pnt"));
    }
    overlay_.create(L"PUBGAssistant LargeMap", regions_.screenWidth(), regions_.screenHeight(), true);
    worker_ = std::thread(&LargeMapRadar::workerLoop, this);
}

LargeMapRadar::~LargeMapRadar() {
    {
        std::lock_guard lock(mutex_);
        waiting_click_ = false;
        calculating_ = false;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    overlay_.clear();
}

void LargeMapRadar::setDisplay(bool visible) {
    display_ = visible;
    overlay_.show(visible);
    renderHud();
}

void LargeMapRadar::setMarkerColors(std::vector<MarkerColor> colors) {
    bool waiting = false;
    bool calculating = false;
    {
        std::lock_guard lock(mutex_);
        colors_ = std::move(colors);
        waiting = waiting_click_;
        calculating = calculating_;
    }
    // 切换色盲模式后立即重绘，让大地图测距的四个圆角矩形和迫击炮助手一样即时变色。
    // renderHud 内部会重新加锁，所以必须在释放 mutex_ 之后再调用；同时保留等待/计算中的提示文案。
    if (waiting) {
        renderHud("点击大地图中的自己位置");
    } else if (calculating) {
        renderHud("计算中...");
    } else {
        renderHud();
    }
}

void LargeMapRadar::toggleMode() {
    bool waiting = false;
    {
        std::lock_guard lock(mutex_);
        waiting_click_ = !waiting_click_;
        calculating_ = false;
        waiting = waiting_click_;
    }
    renderHud(waiting ? "点击大地图中的自己位置" : "");
}

void LargeMapRadar::cancel() {
    {
        std::lock_guard lock(mutex_);
        waiting_click_ = false;
        calculating_ = false;
    }
    renderHud();
}

void LargeMapRadar::onMouseClick(int x, int y, bool pressed) {
    if (!pressed) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (!waiting_click_) {
            return;
        }
        waiting_click_ = false;
        calculating_ = true;
        job_x_ = x;
        job_y_ = y;
        job_pending_ = true;
    }
    renderHud("计算中...");
    // 仅唤醒常驻工作线程，绝不在输入钩子线程上做截图/匹配或 join，避免鼠标卡顿。
    cv_.notify_one();
}

void LargeMapRadar::workerLoop() {
    while (true) {
        int x = 0;
        int y = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return job_pending_ || stop_; });
            if (stop_) {
                return;
            }
            x = job_x_;
            y = job_y_;
            job_pending_ = false;
        }
        processSingleFrame(x, y);
    }
}

DistanceMap LargeMapRadar::measuredDistance() const {
    std::lock_guard lock(mutex_);
    return distances_;
}

void LargeMapRadar::processSingleFrame(int player_x, int player_y) {
    auto rect = regions_.getRealRegion("largemap_region");
    if (!rect) {
        {
            std::lock_guard lock(mutex_);
            calculating_ = false;
        }
        renderHud("未校准大地图");
        return;
    }
    ScreenCapture capture;
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        {
            std::lock_guard lock(mutex_);
            calculating_ = false;
        }
        renderHud("截图失败");
        return;
    }
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    DistanceMap next;
    const double fallback_px = config_.read([](const Json& d) { return d.value("map_1km_pixels", 540.0); });
    const double km_px = regions_.getRealScale("largemap_1km_px", fallback_px);
    std::vector<MarkerColor> colors;
    {
        std::lock_guard lock(mutex_);
        colors = colors_;
    }
    for (const auto& color : colors) {
        cv::Mat mask;
        cv::inRange(hsv, color.lower_hsv, color.upper_hsv, mask);
        double best_score = 0.0;
        cv::Point2d best{-1.0, -1.0};
        for (const auto& tpl : point_templates_) {
            if (tpl.empty() || tpl.rows > mask.rows || tpl.cols > mask.cols) {
                continue;
            }
            cv::Mat res;
            cv::matchTemplate(mask, tpl, res, cv::TM_CCOEFF_NORMED);
            double max_val = 0.0;
            cv::Point max_loc;
            cv::minMaxLoc(res, nullptr, &max_val, nullptr, &max_loc);
            if (std::isfinite(max_val) && max_val >= 0.70 && max_val > best_score) {
                best_score = max_val;
                best = cv::Point2d(max_loc.x + tpl.cols / 2.0,
                                   static_cast<double>(max_loc.y + tpl.rows));
            }
        }
        if (best.x >= 0) {
            const double px = std::hypot((rect->left + best.x) - player_x, (rect->top + best.y) - player_y);
            next[color.name] = (px / std::max(1.0, km_px)) * 1000.0;
        } else {
            next[color.name] = 0.0;
        }
    }
    {
        std::lock_guard lock(mutex_);
        distances_ = next;
        calculating_ = false;
    }
    renderHud();
}

void LargeMapRadar::renderHud(const std::string& prompt) {
    if (!display_) {
        return;
    }
    std::vector<MarkerColor> colors;
    DistanceMap distances;
    {
        std::lock_guard lock(mutex_);
        colors = colors_;
        distances = distances_;
    }
    std::vector<OverlayCommand> cmds;
    const auto layout = assistHudLayout(regions_);
    if (!prompt.empty()) {
        const auto base = [&colors] {
            for (const auto& color : colors) {
                if (color.name == "Blue") return hexToBgr(color.hex);
            }
            return hexToBgr("#017BC2");
        }();
        cmds.push_back({OverlayCommand::Type::RoundedRect, layout.x, layout.large_y,
                        layout.x + layout.total_w, layout.large_y + layout.box_h,
                        10, "", base, 0, 18, 51});
        cmds.push_back({OverlayCommand::Type::RoundedRect, layout.x, layout.large_y,
                        layout.x + layout.total_w, layout.large_y + layout.box_h,
                        10, "", base, 2, 18, 204});
        cmds.push_back({OverlayCommand::Type::TextCenter, layout.x, layout.large_y,
                        layout.x + layout.total_w, layout.large_y + layout.box_h,
                        0, prompt, hexToBgr("#FFFFFF"), 1, 12, 255});
    } else {
        int i = 0;
        for (const auto& color : colors) {
            const double dist = distances.count(color.name) ? distances[color.name] : 0.0;
            auto bgr = hexToBgr(color.hex);
            const double bx = layout.x + i * (layout.box_w + layout.spacing);
            const double by = layout.large_y;
            const bool valid = dist > 0.0;
            const std::string text = valid ? std::to_string(static_cast<int>(std::round(dist))) + "m" : "---";
            cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                            10, "", bgr, 0, 18, valid ? 179 : 51});
            cmds.push_back({OverlayCommand::Type::RoundedRect, bx, by, bx + layout.box_w, by + layout.box_h,
                            10, "", bgr, 2, 18, 204});
            cmds.push_back({OverlayCommand::Type::TextCenter, bx, by, bx + layout.box_w, by + layout.box_h,
                            0, text, valid ? hexToBgr("#FFFFFF") : hexToBgr("#FFFFFF"), 1, valid ? 14 : 13, 255});
            ++i;
            if (i >= 4) break;
        }
    }
    overlay_.setCommands(std::move(cmds));
    overlay_.pumpMessages();
}

} // namespace pubg
