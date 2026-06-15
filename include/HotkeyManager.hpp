#pragma once

#include "InputController.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace pubg {

// 全局热键管理器。
// 键盘键走 WH_KEYBOARD_LL 低级钩子，可对指定热键“吞掉”按键，使其只被本程序捕捉、
// 系统和其它程序（含游戏）都收不到；鼠标按钮无法被键盘钩子捕获，仍用轮询监听。
// 非 Windows 平台退化为纯轮询，仅用于语法检查。
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

    // 析构时停止所有后台线程并卸载钩子。
    ~HotkeyManager();

    // 注册一个热键。name 只用于调试识别，vk 是 Win32 虚拟键码。
    // intercept=true 时该键会被钩子吞掉，仅本程序可见（仅对键盘键有效）。
    void addHotkey(std::string name, int vk, Callback cb);
    void addHotkey(std::string name, int vk, Callback cb, bool intercept);

    // 注册组合热键，例如 <ctrl>+<shift>+m。只在主键刚按下时触发。
    void addHotkey(std::string name, HotkeyCombo combo, Callback cb);
    void addHotkey(std::string name, HotkeyCombo combo, Callback cb, bool intercept);

    // 注册一个按下/松开状态监听。投掷键、鼠标键桥接等需要按下和释放两个边沿。
    // 状态监听一律放行按键（不拦截），避免影响游戏内同名按键。
    void addStateWatcher(std::string name, int vk, StateCallback cb);

    // 设置“放行进程”可执行名（如 "TslGame.exe"）。当该进程或本程序自身处于前台时，
    // F 键不被吞掉，从而游戏能正常收到 F8 等按键、本程序也能录制 F 键；
    // 前台是其它程序时 F 键仍被吞，系统/其它程序收不到。留空表示无条件吞 F 键。
    void setPassthroughProcess(std::string exe);

    // 启动后台线程并安装键盘钩子。
    void start();

    // 停止后台线程并卸载钩子。
    void stop();

    // 当前是否处于运行状态。
    [[nodiscard]] bool isRunning() const;

    // 清空所有已注册热键。重载配置时在 stop() 后调用。
    void clear();

private:
    static bool isModifier(int vk);
    static bool isFunctionKey(int vk);
    bool isMouseKey(int vk) const;

    // 单个热键状态。
    struct Hotkey {
        std::string name;
        HotkeyCombo combo;
        Callback callback;
        StateCallback state_callback;
        bool was_down = false;
        bool state_mode = false;
        bool intercept = false;  // 触发时吞掉按键（仅键盘键有效）。
        bool mouse = false;      // 主键是鼠标按钮，需走轮询。
    };

    // 把回调投递到 dispatch 线程执行，避免在钩子线程里做重活。
    void enqueue(std::function<void()> fn);
    void dispatchLoop();

    // 鼠标按钮轮询线程（以及非 Windows 平台的全量轮询回退）。
    void pollLoop();

#ifdef _WIN32
    // 钩子线程：安装钩子并跑消息循环。
    void hookLoop();
    static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    // 处理单个键盘事件，返回 true 表示吞掉该键。
    bool onKeyboardEvent(int vk, bool down);
    // 在当前已知按键状态下判断组合是否成立。
    bool comboDownFromState(const HotkeyCombo& combo) const;
    // 前台窗口是否属于“放行进程”或本程序自身（此时 F 键不吞）。按 hwnd 缓存结果。
    bool foregroundAllowsKeys();
#endif

    std::vector<Hotkey> hotkeys_;
    std::mutex mutex_;
    std::atomic_bool running_{false};

    // dispatch：串行执行排队的回调。
    std::thread dispatch_worker_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> queue_;

    // 鼠标轮询线程。
    std::thread poll_worker_;

    // 前台为该进程或本程序自身时放行 F 键（受 mutex_ 保护）。
    std::string passthrough_exe_;

#ifdef _WIN32
    std::thread hook_worker_;
    HHOOK hook_ = nullptr;
    unsigned long hook_thread_id_ = 0;
    // 前台窗口判定缓存（仅钩子线程访问）。
    HWND fg_cache_hwnd_ = nullptr;
    bool fg_cache_allow_ = false;
    // 由钩子事件维护的权威按键状态（含 Ctrl/Shift/Alt 的左右归并到通用键）。
    std::array<bool, 256> key_down_{};
    // 当前持有钩子、供静态钩子过程回调的实例。
    static HotkeyManager* s_active;
#endif
};

} // namespace pubg
