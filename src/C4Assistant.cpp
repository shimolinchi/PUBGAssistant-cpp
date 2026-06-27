#include "C4Assistant.hpp"

#include <iomanip>
#include <sstream>

namespace pubg {

namespace {
std::string oneDecimal(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
} // namespace

C4Assistant::C4Assistant(Config& config, RegionManager& regions, MinimapRadar& minimap)
    : config_(config), regions_(regions), minimap_(minimap), hex_(config.markerHex()) {
    const auto cfg = config_.read([](const Json& root) {
        return root.value("c4_config", Json::object());
    });
    target_speed_ = cfg.value("target_speed", target_speed_);
    jump_distance_threshold_ = cfg.value("jump_distance_threshold", jump_distance_threshold_);
    regions_.createOverlay(overlay_, L"PUBGAssistant C4", true);
    worker_ = std::thread(&C4Assistant::updateLoop, this);
}

C4Assistant::~C4Assistant() {
    shutdown();
}

void C4Assistant::shutdown() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void C4Assistant::setEnabled(bool enabled) {
    enabled_ = enabled;
    overlay_.show(enabled);
    if (!enabled) reset();
}

void C4Assistant::setSelectedColor(const std::string& color) {
    std::lock_guard lock(mutex_);
    selected_color_ = color;
}

void C4Assistant::setMarkerHex(std::unordered_map<std::string, std::string> hex) {
    std::lock_guard lock(mutex_);
    hex_ = std::move(hex);
}

void C4Assistant::onWeaponDetected(const std::string& weapon, double score) {
    std::lock_guard lock(mutex_);
    if (installing_ || installed_) return;
    has_c4_ = weapon == "C4" && score >= 0.5;
    if (!has_c4_) {
        installing_ = false;
        installed_ = false;
        distance_ = 0.0;
        overlay_.clear();
    }
}

void C4Assistant::onMouseLeftPress() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || !has_c4_ || installed_ || installing_) return;
    installing_ = true;
    install_start_ = nowSeconds();
    cancel_right_pressed_ = false;
    cancel_right_press_time_ = 0.0;
}

void C4Assistant::onMouseRightClick(bool pressed) {
    std::lock_guard lock(mutex_);
    if (!installing_ || installed_) {
        cancel_right_pressed_ = false;
        cancel_right_press_time_ = 0.0;
        return;
    }
    cancel_right_pressed_ = pressed;
    cancel_right_press_time_ = pressed ? nowSeconds() : 0.0;
}

void C4Assistant::reset() {
    std::lock_guard lock(mutex_);
    installing_ = false;
    installed_ = false;
    install_start_ = 0.0;
    cancel_right_pressed_ = false;
    cancel_right_press_time_ = 0.0;
    explosion_time_ = 0.0;
    distance_ = 0.0;
    start_prompt_shown_ = false;
    jump_prompt_shown_ = false;
    overlay_.clear();
}

void C4Assistant::draw(double countdown, double recommended_speed, bool start_prompt, bool jump_prompt,
                       const std::string& selected_color, const std::unordered_map<std::string, std::string>& hex) {
    auto bgr = hexToBgr(hex.count(selected_color) ? hex.at(selected_color) : "#FFFFFF");
    const double cx = regions_.screenWidth() / 2.0;
    const double y = regions_.screenHeight() * 0.62;
    std::vector<OverlayCommand> cmds;
    const int font_size = countdown <= 6.0 ? 48 : 35;
    const cv::Scalar countdown_color = countdown <= 2.0 ? hexToBgr("#E74C3C")
        : countdown <= 4.0 ? hexToBgr("#F39C12")
        : countdown <= 6.0 ? hexToBgr("#F1C40F")
        : hexToBgr("#FFFFFF");
    cmds.push_back({OverlayCommand::Type::Text, cx - 70, y - 60, 0, 0, 0,
                    oneDecimal(std::max(0.0, countdown)) + " s",
                    countdown_color, 1, font_size});
    std::string speed_text = "N/A";
    if (recommended_speed > 0.0 && recommended_speed <= 160.0) {
        speed_text = oneDecimal(recommended_speed) + " km/h";
    } else if (recommended_speed > 160.0) {
        speed_text = ">160 km/h";
    }
    cmds.push_back({OverlayCommand::Type::Text, cx - 88, y + 5, 0, 0, 0, "建议车速: " + speed_text, hexToBgr("#3498DB"), 1, 15});
    cmds.push_back({OverlayCommand::Type::Text, regions_.screenWidth() / 4.0 - 65.0, y + 40, 0, 0, 0, "当前使用标点：", bgr, 1, 15});
    cmds.push_back({OverlayCommand::Type::Circle, regions_.screenWidth() / 4.0 + 70.0, y + 49, 0, 0, 6.0, "", bgr, 2});
    if (start_prompt) cmds.push_back({OverlayCommand::Type::Text, cx - 55, y + 50, 0, 0, 0, "推荐起步", hexToBgr("#2ECC71"), 1, 15});
    if (jump_prompt) cmds.push_back({OverlayCommand::Type::Text, cx - 55, y + 100, 0, 0, 0, "推荐跳车", hexToBgr("#E74C3C"), 1, 15});
    overlay_.setCommands(std::move(cmds));
    overlay_.pumpMessages();
}

void C4Assistant::updateLoop() {
    while (running_) {
        if (enabled_) {
            bool draw_now = false;
            double countdown = 0;
            double speed = 0;
            bool start = false;
            bool jump = false;
            bool installing_now = false;
            std::string selected_color;
            std::unordered_map<std::string, std::string> hex;
            double new_dist = 0.0;
            bool installed_snapshot = false;
            {
                std::lock_guard lock(mutex_);
                selected_color = selected_color_;
                installed_snapshot = installed_;
            }
            if (installed_snapshot) {
                const auto dists = minimap_.measuredDistance();
                new_dist = dists.count(selected_color) ? dists.at(selected_color) : 0.0;
            }
            {
                std::lock_guard lock(mutex_);
                hex = hex_;
                const double now = nowSeconds();
                if (installing_ && cancel_right_pressed_ && now - cancel_right_press_time_ >= cancel_hold_seconds_) {
                    installing_ = false;
                    cancel_right_pressed_ = false;
                    cancel_right_press_time_ = 0.0;
                    overlay_.clear();
                }
                if (installing_ && now - install_start_ >= 4.0) {
                    installing_ = false;
                    installed_ = true;
                    explosion_time_ = now + (16.0 - explosion_margin_);
                    start_prompt_shown_ = false;
                    jump_prompt_shown_ = false;
                    distance_ = 0.0;
                }
                installing_now = installing_;
                if (installed_) {
                    countdown = explosion_time_ - now;
                    if (new_dist > 0.0) distance_ = new_dist;
                    speed = countdown > 0.1 ? distance_ / countdown * 3.6 : 0.0;
                    if (!start_prompt_shown_ && speed <= target_speed_ && distance_ > 0.0) {
                        start = true;
                        start_prompt_shown_ = true;
                    }
                    if (!jump_prompt_shown_ && distance_ > 0.0 && distance_ <= jump_distance_threshold_) {
                        jump = true;
                        jump_prompt_shown_ = true;
                    }
                    draw_now = countdown > 0.0;
                    if (!draw_now) installed_ = false;
                }
            }
            if (installing_now) {
                const double cx = regions_.screenWidth() / 2.0;
                const double y = regions_.screenHeight() * 0.62;
                overlay_.setCommands({OverlayCommand{OverlayCommand::Type::Text, cx - 135.0, y, 0, 0, 0,
                                                      "C4安装中，长按鼠标右键取消", hexToBgr("#F39C12"), 1, 15}});
                overlay_.pumpMessages();
            }
            if (draw_now) draw(countdown, speed, start, jump, selected_color, hex);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace pubg
