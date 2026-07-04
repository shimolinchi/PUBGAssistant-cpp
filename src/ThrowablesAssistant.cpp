#include "ThrowablesAssistant.hpp"

#include "BuildConfig.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>

namespace pubg {

namespace {
std::string oneDecimal(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

double displayValue(const Config& config, const char* key, double fallback) {
    return config.read([&](const Json& data) {
        return data.value("ui_state", Json::object()).value("display", Json::object()).value(key, fallback);
    });
}

int assistTextSize(const Config& config, int fallback) {
    return std::clamp(static_cast<int>(std::round(displayValue(config, "assist_text_size", fallback))), 12, 28);
}

int assistTextAlpha(const Config& config) {
    return std::clamp(static_cast<int>(std::round(displayValue(config, "assist_text_alpha", 1.0) * 255.0)), 0, 255);
}

int configuredKey(const Config& config, const char* name, const char* fallback) {
    return InputController::parseVirtualKey(config.read([&](const Json& root) {
        return root.value("hotkeys", Json::object()).value(name, std::string(fallback));
    }));
}
} // namespace

ThrowablesAssistant::ThrowablesAssistant(Config& config, RegionManager& regions, MinimapRadar& minimap)
    : config_(config), regions_(regions), minimap_(minimap), ballistics_(config), hex_(config.markerHex()) {
    regions_.createOverlay(overlay_, L"PUBGAssistant Throwables", true);
    worker_ = std::thread(&ThrowablesAssistant::workerLoop, this);
}

ThrowablesAssistant::~ThrowablesAssistant() {
    shutdown();
}

void ThrowablesAssistant::shutdown() {
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true)) {
        return;
    }
    enabled_ = false;
    {
        std::lock_guard lock(mutex_);
        sequence_active_ = false;
        pull_done_ = false;
        run_started_ = false;
        mouse_released_ = false;
        jumped_ = false;
        run_keys_released_ = true;
        player_w_was_down_ = false;
        player_sprint_was_down_ = false;
        warning_text_.clear();
        warning_until_ = 0.0;
    }
    running_ = false;
    if (worker_.joinable()) worker_.join();
    InputController::mouseLeftUp();
    releaseRunJumpKeys();
    overlay_.clear();
    overlay_.pumpMessages();
    overlay_.close();
}

void ThrowablesAssistant::setEnabled(bool enabled) {
    if (shutting_down_) return;
    enabled_ = enabled;
    overlay_.show(enabled);
    if (!enabled) {
        {
            std::lock_guard lock(mutex_);
            sequence_active_ = false;
            pull_done_ = false;
            run_started_ = false;
            mouse_released_ = false;
            jumped_ = false;
            run_keys_released_ = true;
            player_w_was_down_ = false;
            player_sprint_was_down_ = false;
            warning_text_.clear();
            warning_until_ = 0.0;
        }
        releaseRunJumpKeys();
        overlay_.clear();
    }
}

void ThrowablesAssistant::setSelectedColor(const std::string& color) {
    if (shutting_down_) return;
    std::lock_guard lock(mutex_);
    selected_color_ = color;
}

void ThrowablesAssistant::setMarkerHex(std::unordered_map<std::string, std::string> hex) {
    if (shutting_down_) return;
    std::lock_guard lock(mutex_);
    hex_ = std::move(hex);
}

void ThrowablesAssistant::setPhysicalKeyQuery(std::function<bool(int)> query) {
    std::lock_guard lock(mutex_);
    physical_key_down_ = std::move(query);
}

ThrowablesAssistant::ThrowMode ThrowablesAssistant::throwModeForDistance(double distance) const {
    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    const double normal_min = cfg.value("normal_min_dist", 20.0);
    const double normal_max = cfg.value("normal_max_dist", 50.0);
    const double jump_min = cfg.value("jump_min_dist", normal_max);
    const double jump_max = cfg.value("jump_max_dist", 65.0);
    const double run_jump_min = cfg.value("run_jump_min_dist", jump_max);
    const double run_jump_max = cfg.value("run_jump_max_dist", 90.0);
    if (distance >= run_jump_min && distance <= run_jump_max) return ThrowMode::RunJump;
    if (distance >= jump_min && distance <= jump_max) return ThrowMode::Jump;
    if (distance >= normal_min && distance <= normal_max) return ThrowMode::Normal;
    return distance > jump_max ? ThrowMode::RunJump : ThrowMode::Normal;
}

bool ThrowablesAssistant::usesJumpCurve(ThrowMode mode) const {
    return mode == ThrowMode::Jump || mode == ThrowMode::RunJump;
}

int ThrowablesAssistant::throwModeIndex(ThrowMode mode) const {
    if (mode == ThrowMode::RunJump) return 2;
    if (mode == ThrowMode::Jump) return 1;
    return 0;
}

void ThrowablesAssistant::releaseRunJumpKeys() {
#if PUBG_ENABLE_INPUT_CONTROL
    bool release_w = false;
    bool release_sprint = false;
    int sprint_vk = 0;
    std::function<bool(int)> physical_key_down;
    {
        std::lock_guard lock(mutex_);
        release_w = injected_w_down_;
        release_sprint = injected_sprint_down_;
        sprint_vk = injected_sprint_vk_;
        physical_key_down = physical_key_down_;
        injected_w_down_ = false;
        injected_sprint_down_ = false;
        injected_sprint_vk_ = 0;
        player_w_was_down_ = false;
        player_sprint_was_down_ = false;
    }
    if (physical_key_down) {
        if (release_w && physical_key_down(InputController::parseVirtualKey("w"))) {
            release_w = false;
        }
        if (release_sprint && sprint_vk && physical_key_down(sprint_vk)) {
            release_sprint = false;
        }
    }
    if (release_sprint && sprint_vk) InputController::releaseVirtualKey(sprint_vk);
    if (release_w) InputController::keyUp(InputController::parseVirtualKey("w"));
#endif
}

void ThrowablesAssistant::pullThrowable() {
#if PUBG_ENABLE_INPUT_CONTROL
    if (shutting_down_ || !running_) return;
    const int pull_key = InputController::parseVirtualKey("r");
    InputController::keyDown(pull_key);
    for (int i = 0; i < 10 && !shutting_down_ && running_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    InputController::keyUp(pull_key);
    if (shutting_down_ || !running_) return;
    InputController::mouseLeftDown();
#endif
}

void ThrowablesAssistant::startRunJumpKeys() {
#if PUBG_ENABLE_INPUT_CONTROL
    int sprint_key = configuredKey(config_, "sprint_key", "shift");
    const int w_key = InputController::parseVirtualKey("w");
    const bool player_w_down = InputController::isKeyDown(w_key);
    const bool player_sprint_down = sprint_key != 0 && InputController::isKeyDown(sprint_key);
    {
        std::lock_guard lock(mutex_);
        if (!sequence_active_ || throw_mode_ != ThrowMode::RunJump || run_keys_released_) {
            return;
        }
        player_w_was_down_ = player_w_down;
        player_sprint_was_down_ = player_sprint_down;
        injected_w_down_ = !player_w_down;
        injected_sprint_down_ = sprint_key != 0 && !player_sprint_down;
        injected_sprint_vk_ = sprint_key;
    }
    if (!player_w_down) InputController::keyDown(w_key);
    if (sprint_key && !player_sprint_down) InputController::pressVirtualKey(sprint_key);
#endif
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
    if (!enabled_ || shutting_down_) return;
    std::lock_guard lock(mutex_);
    if (!pressed) return;

    if (!InputController::isLeftMouseDown()) {
        showWarning("左键未按下，无法触发瞬爆");
        return;
    }

    const auto dists = minimap_.measuredDistance();
    const double dist = dists.count(selected_color_) ? dists.at(selected_color_) : 0.0;
    if (dist <= 0.0) {
        showWarning("未检测到标点，无法瞬爆");
        return;
    }

    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    const double normal_min = cfg.value("normal_min_dist", 20.0);
    const double run_jump_max = cfg.value("run_jump_max_dist", 90.0);
    if (dist < normal_min) {
        showWarning("距离太近，无法瞬爆");
        return;
    }
    if (dist > run_jump_max) {
        showWarning("距离太远，无法瞬爆");
        return;
    }

    throw_mode_ = throwModeForDistance(dist);
    {
        const int mode_index = throwModeIndex(throw_mode_);
        const char* dist_key = mode_index == 2 ? "run_jump_calib_dists" : mode_index == 1 ? "jump_calib_dists" : "calib_dists";
        const char* time_key = mode_index == 2 ? "run_jump_calib_times" : mode_index == 1 ? "jump_calib_times" : "calib_times";
        const auto dists = cfg.value(dist_key, Json::array());
        const auto times = cfg.value(time_key, Json::array());
        if (dists.empty() || times.empty()) {
            showWarning("投掷参数未配置");
            return;
        }
    }

    const double cook = ballistics_.throwableCookTime(dist, throwModeIndex(throw_mode_));
    if (!std::isfinite(cook) || cook <= 0.0) {
        showWarning("当前距离缺少瞬爆时间");
        return;
    }

    const double start_time = nowSeconds();
    const double run_seconds = std::max(0.0, cfg.value("run_jump_run_seconds", 2.0));
    const double jump_delay = std::max(0.0, cfg.value("jump_delay_after_release", 0.3));
    sequence_active_ = true;
    pull_done_ = false;
    run_started_ = false;
    mouse_released_ = false;
    jumped_ = false;
    run_keys_released_ = throw_mode_ != ThrowMode::RunJump;

    if (throw_mode_ == ThrowMode::RunJump) {
        if (run_seconds < cook) {
            pull_at_ = start_time;
            run_start_at_ = start_time + (cook - run_seconds);
        } else {
            run_start_at_ = start_time;
            pull_at_ = start_time + (run_seconds - cook);
        }
        release_at_ = start_time + std::max(run_seconds, cook);
        jump_at_ = release_at_ + jump_delay;
        run_keys_up_at_ = jump_at_;
    } else {
        pull_at_ = start_time;
        run_start_at_ = 0.0;
        release_at_ = start_time + cook;
        jump_at_ = usesJumpCurve(throw_mode_) ? release_at_ + jump_delay : 0.0;
        run_keys_up_at_ = 0.0;
    }
}

void ThrowablesAssistant::render() {
    if (!enabled_ || shutting_down_) return;
    const auto dists = minimap_.measuredDistance();
    std::unordered_map<std::string, std::string> hex;
    std::string warning;
    {
        std::lock_guard lock(mutex_);
        hex = hex_;
        if (!warning_text_.empty() && nowSeconds() <= warning_until_) {
            warning = warning_text_;
        }
        if (!warning_text_.empty() && nowSeconds() > warning_until_) {
            warning_text_.clear();
        }
    }
    if (!warning.empty()) {
        drawWarningBox(warning);
        return;
    }
    const auto cfg = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    const double total_time = std::max(0.1, cfg.value("grenade_total_time", 5.0));
    const double arc_radius = regions_.screenWidth() * cfg.value("arc_radius_ratio", 0.097);
    const double cx = regions_.screenWidth() / 2.0;
    const double cy = regions_.screenHeight() / 2.0;
    std::vector<OverlayCommand> cmds;
    cmds.push_back({OverlayCommand::Type::Line, cx, cy, cx, regions_.screenHeight() * 0.9, 0, "", hexToBgr("#FFFFFF"), 1, 18, 255});
    constexpr double arc_span = 50.0 * std::numbers::pi / 180.0;
    const std::vector<std::string> color_order{"Yellow", "Orange", "Blue", "Green"};
    int drawn = 0;
    for (const auto& color : color_order) {
        const double dist = dists.count(color) ? dists.at(color) : 0.0;
        if (dist <= 0.0) continue;

        const ThrowMode mode = throwModeForDistance(dist);
        const int mode_index = throwModeIndex(mode);
        const double ratio = ballistics_.throwableElevationRatio(dist, mode_index);
        const double cook = ballistics_.throwableCookTime(dist, mode_index);
        if (!std::isfinite(ratio) || ratio <= 0.0 || !std::isfinite(cook) || cook <= 0.0) {
            continue;
        }

        const double y = regions_.screenHeight() * ratio;
        const auto bgr = hexToBgr(hex.count(color) ? hex[color] : "#FFFFFF");
        cmds.push_back({OverlayCommand::Type::Line, cx - 30, y, cx + 30, y, 0, "", bgr, 1});
        cmds.push_back({OverlayCommand::Type::Text, cx + 34, y - 10, 0, 0, 0,
                        std::to_string(static_cast<int>(std::round(dist))) + "m",
                        bgr, 1, assistTextSize(config_, 16), assistTextAlpha(config_)});

        const double clamped_cook = std::clamp(cook, 0.0, total_time);
        const double angle = arc_span * 0.5 - (clamped_cook / total_time) * arc_span;
        const double mark_x = cx + std::cos(angle) * arc_radius;
        const double mark_y = cy + std::sin(angle) * arc_radius + drawn * 14.0;
        const double pointer_len = 18.0;
        const double vx = cx - mark_x;
        const double vy = cy - mark_y;
        const double vlen = std::max(1.0, std::hypot(vx, vy));
        cmds.push_back({OverlayCommand::Type::Line,
                        mark_x - 2.0, mark_y, mark_x - 2.0 + vx / vlen * pointer_len,
                        mark_y + vy / vlen * pointer_len, 0, "", bgr, 2});
        cmds.push_back({OverlayCommand::Type::Text, mark_x + 10, mark_y - 7, 0, 0, 0,
                        oneDecimal(cook) + "s", bgr, 1, assistTextSize(config_, 16), assistTextAlpha(config_)});
        ++drawn;
    }
    if (drawn > 0) {
        overlay_.setCommands(std::move(cmds));
        overlay_.pumpMessages();
    } else {
        overlay_.clear();
    }
}

void ThrowablesAssistant::workerLoop() {
    while (running_) {
        std::vector<ThrowAction> actions;
        {
            std::lock_guard lock(mutex_);
            const double now = nowSeconds();
            if (sequence_active_) {
                if (!pull_done_ && now >= pull_at_) {
                    pull_done_ = true;
                    actions.push_back(ThrowAction::Pull);
                }
                if (!run_started_ && throw_mode_ == ThrowMode::RunJump && now >= run_start_at_) {
                    run_started_ = true;
                    actions.push_back(ThrowAction::StartRun);
                }
                if (!mouse_released_ && now >= release_at_) {
                    mouse_released_ = true;
                    actions.push_back(ThrowAction::ReleaseMouse);
                }
                if (!jumped_ && usesJumpCurve(throw_mode_) && now >= jump_at_) {
                    jumped_ = true;
                    actions.push_back(ThrowAction::Jump);
                }
                if (throw_mode_ == ThrowMode::RunJump && jumped_ && !run_keys_released_ && now >= run_keys_up_at_) {
                    run_keys_released_ = true;
                    actions.push_back(ThrowAction::ReleaseRunKeys);
                }
                const bool done = mouse_released_ && (!usesJumpCurve(throw_mode_) || jumped_) &&
                                  (throw_mode_ != ThrowMode::RunJump || run_keys_released_);
                if (done) {
                    sequence_active_ = false;
                    run_keys_up_at_ = 0.0;
                }
            }
        }
        for (const auto action : actions) {
            if (shutting_down_ || !running_) {
                break;
            }
            switch (action) {
                case ThrowAction::Pull:
                    pullThrowable();
                    break;
                case ThrowAction::StartRun:
                    startRunJumpKeys();
                    break;
                case ThrowAction::ReleaseMouse:
                    InputController::mouseLeftUp();
                    break;
                case ThrowAction::Jump:
                    InputController::keyDown(VK_SPACE);
                    for (int i = 0; i < 55 && !shutting_down_ && running_; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    InputController::keyUp(VK_SPACE);
                    if (throw_mode_ == ThrowMode::RunJump) {
                        releaseRunJumpKeys();
                        std::lock_guard lock(mutex_);
                        run_keys_released_ = true;
                    }
                    break;
                case ThrowAction::ReleaseRunKeys:
                    releaseRunJumpKeys();
                    break;
            }
        }
        render();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace pubg

