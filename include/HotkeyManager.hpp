#pragma once

#include "InputController.hpp"
#include <mutex>

namespace pubg {

// 简单热键轮询器。
// 目前不用全局 Hook，而是后台线程轮询 GetAsyncKeyState，减少第一版 Windows 调试复杂度。
class HotkeyManager {
public:
    // 热键触发回调。只在“刚按下”的边沿触发一次。
    using Callback = std::function<void()>;
    using StateCallback = std::function<void(bool)>;

    // 一个全局热键组合。modifiers 保存 Ctrl/Shift/Alt，key 保存主键。
    struct HotkeyCombo {
        std::vector<int> modifiers;
        int key = 0;
    };

    // 析构时停止轮询线程。
    ~HotkeyManager();

    // 注册一个热键。name 只用于调试识别，vk 是 Win32 虚拟键码。
    void addHotkey(std::string name, int vk, Callback cb);

    // 注册组合热键，例如 <ctrl>+<shift>+m。只在主键刚按下时触发。
    void addHotkey(std::string name, HotkeyCombo combo, Callback cb);

    // 注册一个按下/松开状态监听。投掷键、鼠标键桥接等需要按下和释放两个边沿。
    void addStateWatcher(std::string name, int vk, StateCallback cb);

    // 启动后台轮询线程。
    void start();

    // 停止后台轮询线程。
    void stop();

    // 当前轮询线程是否处于运行状态。
    [[nodiscard]] bool isRunning() const;

    // 清空所有已注册热键。重载配置时在 stop() 后调用。
    void clear();

private:
    static bool isModifier(int vk);
    static bool isComboDown(const HotkeyCombo& combo);

    // 轮询所有注册热键，检测 false -> true 的按下边沿。
    void run();

    // 单个热键状态。
    struct Hotkey {
        std::string name;
        HotkeyCombo combo;
        Callback callback;
        StateCallback state_callback;
        bool was_down = false;
        bool state_mode = false;
    };

    std::vector<Hotkey> hotkeys_;
    std::mutex mutex_;
    std::atomic_bool running_{false};
    std::thread worker_;
};

} // namespace pubg
