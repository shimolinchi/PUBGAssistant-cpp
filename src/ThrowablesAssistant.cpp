#include "ThrowablesAssistant.hpp"

#include "BuildConfig.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>

namespace pubg {

namespace {
// 把秒数格式化为固定一位小数，避免 std::to_string + substr 截出位数不稳定的文本。
std::string oneDecimal(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
} // namespace

ThrowablesAssistant::ThrowablesAssistant(Config& config, RegionManager& regions, MinimapRadar& minimap)
    : config_(config), regions_(regions), minimap_(minimap), ballistics_(config), hex_(config.markerHex()) {
    overlay_.create(L"PUBGAssistant Throwables", regions_.screenWidth(), regions_.screenHeight(), true);
    worker_ = std::thread(&ThrowablesAssistant::workerLoop, this);
}

ThrowablesAssistant::~ThrowablesAssistant() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void ThrowablesAssistant::setEnabled(bool enabled) {
    enabled_ = enabled;
    overlay_.show(enabled);
    if (!enabled) overlay_.clear();
}

void ThrowablesAssistant::setSelectedColor(const std::string& color) {
    std::lock_guard lock(mutex_);
    selected_color_ = color;
}

void ThrowablesAssistant::setMarkerHex(std::unordered_map<std::string, std::string> hex) {
    std::lock_guard lock(mutex_);
    hex_ = std::move(hex);
}

bool ThrowablesAssistant::shouldJumpThrow(double distance) const {
    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    return distance >= cfg.value("jump_min_dist", 50.0) && distance <= cfg.value("jump_max_dist", 80.0);
}

void ThrowablesAssistant::showWarning(const std::string& text) {
    warning_text_ = text;
    warning_until_ = nowSeconds() + 2.0;
}

void ThrowablesAssistant::drawWarningBox(const std::string& text) {
    const double box_w = 300.0;
    const double box_h = 42.0;
    const double box_x = regions_.screenWidth() / 2.0 - box_w / 2.0;
    const double box_y = regions_.screenHeight() * 0.8 - box_h;
    const auto bgr = hexToBgr("#E74C3C");
    overlay_.setCommands({
        OverlayCommand{OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                       10, "", bgr, 0, 18, 89},
        OverlayCommand{OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                       10, "", bgr, 2, 18, 204},
        OverlayCommand{OverlayCommand::Type::TextCenter, box_x, box_y, box_x + box_w, box_y + box_h,
                       0, text, hexToBgr("#FFFFFF"), 1, 18, 255}
    });
    overlay_.pumpMessages();
}

void ThrowablesAssistant::onThrowKey(bool pressed) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    if (!pressed) {
        return;
    }
    if (!InputController::isLeftMouseDown()) {
        showWarning("[ 左键未按下，无法触发瞬爆 ]");
        return;
    }
    const auto dists = minimap_.measuredDistance();
    const double dist = dists.count(selected_color_) ? dists.at(selected_color_) : 0.0;
    if (dist <= 0.0) {
        showWarning("[ 未检测到 " + selected_color_ + " 标点 ]");
        return;
    }
    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    const double jump_min = cfg.value("jump_min_dist", 50.0);
    const double jump_max = cfg.value("jump_max_dist", 80.0);
    if (dist > jump_max) {
        showWarning("[ 目标距离 " + std::to_string(static_cast<int>(std::round(dist))) + "m 太远 ]");
        return;
    }
    jump_throw_ = shouldJumpThrow(dist);
    if (dist >= jump_min && jump_throw_) {
        const auto jump_dists = cfg.value("jump_calib_dists", Json::array());
        const auto jump_times = cfg.value("jump_calib_times", Json::array());
        if (jump_dists.empty() || jump_times.empty()) {
            showWarning("[ 跳投参数未配置 ]");
            return;
        }
    }
    const double cook = ballistics_.throwableCookTime(dist, jump_throw_);
    cooking_ = true;
    throw_at_ = nowSeconds() + std::max(0.0, cook);
#if PUBG_ENABLE_INPUT_CONTROL
    const int pull_key = InputController::parseVirtualKey("r");
    InputController::keyDown(pull_key);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    InputController::keyUp(pull_key);
    InputController::mouseLeftDown();
#endif
}

void ThrowablesAssistant::executeThrow(bool jump_throw) {
#if PUBG_ENABLE_INPUT_CONTROL
    InputController::mouseLeftUp();
    if (jump_throw) {
        const auto cfg = config_.read([](const Json& root) {
            return root.value("throwables_config", Json::object());
        });
        const int delay_ms = static_cast<int>(cfg.value("jump_delay_after_release", 0.3) * 1000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, delay_ms)));
        InputController::keyDown(VK_SPACE);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        InputController::keyUp(VK_SPACE);
    }
#else
    (void)jump_throw;
#endif
}

void ThrowablesAssistant::render() {
    if (!enabled_) return;
    const auto dists = minimap_.measuredDistance();
    std::string color;
    std::unordered_map<std::string, std::string> hex;
    {
        std::lock_guard lock(mutex_);
        color = selected_color_;
        hex = hex_;
        if (!warning_text_.empty() && nowSeconds() <= warning_until_) {
            drawWarningBox(warning_text_);
            return;
        }
        if (!warning_text_.empty() && nowSeconds() > warning_until_) {
            warning_text_.clear();
        }
    }
    const double dist = dists.count(color) ? dists.at(color) : 0.0;
    if (dist <= 0.0) {
        overlay_.clear();
        return;
    }
    const bool jump = shouldJumpThrow(dist);
    const double ratio = ballistics_.throwableElevationRatio(dist, jump);
    const double cook = ballistics_.throwableCookTime(dist, jump);
    if (!std::isfinite(ratio) || ratio <= 0.0 || !std::isfinite(cook) || cook <= 0.0) {
        overlay_.clear();
        return;
    }
    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    const double total_time = std::max(0.1, cfg.value("grenade_total_time", 5.0));
    const double arc_radius = regions_.screenWidth() * cfg.value("arc_radius_ratio", 0.097);
    const double cx = regions_.screenWidth() / 2.0;
    const double cy = regions_.screenHeight() / 2.0;
    const double y = regions_.screenHeight() * ratio;
    auto bgr = hexToBgr(hex.count(color) ? hex[color] : "#FFFFFF");
    std::vector<OverlayCommand> cmds;
    cmds.push_back({OverlayCommand::Type::Line, cx, cy, cx, regions_.screenHeight() * 0.9, 0, "", hexToBgr("#FFFFFF"), 1, 18, 255});
    cmds.push_back({OverlayCommand::Type::Line, cx - 30, y, cx + 30, y, 0, "", bgr, 1});
    cmds.push_back({OverlayCommand::Type::Text, cx + 34, y - 10, 0, 0, 0,
                    std::to_string(static_cast<int>(std::round(dist))) + "m",
                    bgr, 1, 16});
    constexpr double arc_span = 50.0 * std::numbers::pi / 180.0;
    const double clamped_cook = std::clamp(cook, 0.0, total_time);
    const double angle = arc_span * 0.5 - (clamped_cook / total_time) * arc_span;
    const double mark_x = cx + std::cos(angle) * arc_radius;
    const double mark_y = cy + std::sin(angle) * arc_radius;
    const double pointer_len = 18.0;
    const double vx = cx - mark_x;
    const double vy = cy - mark_y;
    const double vlen = std::max(1.0, std::hypot(vx, vy));
    cmds.push_back({OverlayCommand::Type::Line,
                    mark_x - 2.0, mark_y, mark_x - 2.0 + vx / vlen * pointer_len,
                    mark_y + vy / vlen * pointer_len, 0, "", bgr, 2});
    cmds.push_back({OverlayCommand::Type::Text, mark_x + 10, mark_y - 7, 0, 0, 0,
                    oneDecimal(cook) + "s", bgr, 1, 16});
    overlay_.setCommands(std::move(cmds));
    overlay_.pumpMessages();
}

void ThrowablesAssistant::workerLoop() {
    while (running_) {
        bool fire = false;
        bool jump = false;
        {
            std::lock_guard lock(mutex_);
            if (cooking_ && nowSeconds() >= throw_at_) {
                cooking_ = false;
                fire = true;
                jump = jump_throw_;
            }
        }
        if (fire) executeThrow(jump);
        render();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace pubg
