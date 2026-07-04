#pragma once

#include "Ballistics.hpp"
#include "InputController.hpp"
#include "MinimapRadar.hpp"
#include "OverlayWindow.hpp"

namespace pubg {

// 投掷物助手，对应 Python 版 modules/throwables_assistant.py。
// 负责显示手雷抬高/瞬爆时间，并在按下投掷快捷键后按距离计时自动松开左键，可选跳投。
class ThrowablesAssistant {
public:
    ThrowablesAssistant(Config& config, RegionManager& regions, MinimapRadar& minimap);
    ~ThrowablesAssistant();

    void shutdown();

    // 开关投掷物助手。
    void setEnabled(bool enabled);

    // 选择目标标点颜色，例如 Yellow/Orange/Blue/Green。
    void setSelectedColor(const std::string& color);

    // 更新色盲模式颜色，用于 HUD 绘制。
    void setMarkerHex(std::unordered_map<std::string, std::string> hex);

    void setPhysicalKeyQuery(std::function<bool(int)> query);

    // 投掷快捷键按下/松开事件。按下时开始捏雷倒计时，松开时取消或交给自动释放。
    void onThrowKey(bool pressed);

    // 绘制当前距离对应的抬高参考。
    void render();

private:
    enum class ThrowMode {
        Normal,
        Jump,
        RunJump,
    };

    void workerLoop();
    enum class ThrowAction {
        Pull,
        StartRun,
        ReleaseMouse,
        Jump,
        ReleaseRunKeys,
    };

    ThrowMode throwModeForDistance(double distance) const;
    bool usesJumpCurve(ThrowMode mode) const;
    int throwModeIndex(ThrowMode mode) const;
    void pullThrowable();
    void startRunJumpKeys();
    void releaseRunJumpKeys();
    void showWarning(const std::string& text);
    void drawWarningBox(const std::string& text);

    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    Ballistics ballistics_;
    std::function<bool(int)> physical_key_down_;
    std::atomic_bool running_{true};
    std::atomic_bool shutting_down_{false};
    std::atomic_bool enabled_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::string selected_color_ = "Yellow";
    std::unordered_map<std::string, std::string> hex_;
    bool sequence_active_ = false;
    bool pull_done_ = false;
    bool run_started_ = false;
    bool mouse_released_ = false;
    bool jumped_ = false;
    bool run_keys_released_ = false;
    bool injected_w_down_ = false;
    bool injected_sprint_down_ = false;
    int injected_sprint_vk_ = 0;
    bool player_w_was_down_ = false;
    bool player_sprint_was_down_ = false;
    double pull_at_ = 0.0;
    double run_start_at_ = 0.0;
    double release_at_ = 0.0;
    double jump_at_ = 0.0;
    double run_keys_up_at_ = 0.0;
    ThrowMode throw_mode_ = ThrowMode::Normal;
    std::string warning_text_;
    double warning_until_ = 0.0;
    OverlayWindow overlay_;
};

} // namespace pubg
