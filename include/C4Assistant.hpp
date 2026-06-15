#pragma once

#include "MinimapRadar.hpp"
#include "OverlayWindow.hpp"

namespace pubg {

// C4 助手，对应 Python 版 modules/c4_assistant.py 的计时/速度提示逻辑。
// 当前通过鼠标左键开始安装计时，根据爆炸剩余时间、距离和目标速度给出起步/跳车提示。
class C4Assistant {
public:
    C4Assistant(Config& config, RegionManager& regions, MinimapRadar& minimap);
    ~C4Assistant();

    void setEnabled(bool enabled);
    void setSelectedColor(const std::string& color);
    void setMarkerHex(std::unordered_map<std::string, std::string> hex);
    void onWeaponDetected(const std::string& weapon, double score);
    void onMouseLeftPress();
    void onMouseRightClick(bool pressed);
    void shutdown();

private:
    void updateLoop();
    void draw(double countdown, double recommended_speed, bool start_prompt, bool jump_prompt,
              const std::string& selected_color, const std::unordered_map<std::string, std::string>& hex);
    void reset();

    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    std::atomic_bool running_{true};
    std::atomic_bool enabled_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::string selected_color_ = "Yellow";
    std::unordered_map<std::string, std::string> hex_;
    bool has_c4_ = false;
    bool installing_ = false;
    bool installed_ = false;
    double install_start_ = 0.0;
    bool cancel_right_pressed_ = false;
    double cancel_right_press_time_ = 0.0;
    double cancel_hold_seconds_ = 0.35;
    double explosion_time_ = 0.0;
    double explosion_margin_ = 2.0;
    double target_speed_ = 60.0;
    double jump_distance_threshold_ = 20.0;
    double distance_ = 0.0;
    bool start_prompt_shown_ = false;
    bool jump_prompt_shown_ = false;
    OverlayWindow overlay_;
};

} // namespace pubg
