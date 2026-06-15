#include "HotkeyManager.hpp"

#include <algorithm>
#include <cwctype>

namespace pubg {

#ifdef _WIN32
HotkeyManager* HotkeyManager::s_active = nullptr;
#endif

HotkeyManager::~HotkeyManager() {
    stop();
}

void HotkeyManager::addHotkey(std::string name, int vk, Callback cb) {
    addHotkey(std::move(name), vk, std::move(cb), false);
}

void HotkeyManager::addHotkey(std::string name, int vk, Callback cb, bool intercept) {
    addHotkey(std::move(name), HotkeyCombo{{}, vk}, std::move(cb), intercept);
}

void HotkeyManager::addHotkey(std::string name, HotkeyCombo combo, Callback cb) {
    addHotkey(std::move(name), std::move(combo), std::move(cb), false);
}

void HotkeyManager::addHotkey(std::string name, HotkeyCombo combo, Callback cb, bool intercept) {
    Hotkey hk;
    hk.name = std::move(name);
    hk.mouse = isMouseKey(combo.key);
    hk.combo = std::move(combo);
    hk.callback = std::move(cb);
    // 鼠标键无法被键盘钩子拦截，强制不拦截。
    hk.intercept = intercept && !hk.mouse;
    std::lock_guard lock(mutex_);
    hotkeys_.push_back(std::move(hk));
}

void HotkeyManager::addStateWatcher(std::string name, int vk, StateCallback cb) {
    Hotkey hk;
    hk.name = std::move(name);
    hk.combo = HotkeyCombo{{}, vk};
    hk.state_callback = std::move(cb);
    hk.state_mode = true;
    hk.mouse = isMouseKey(vk);
    hk.intercept = false;
    std::lock_guard lock(mutex_);
    hotkeys_.push_back(std::move(hk));
}

void HotkeyManager::setPassthroughProcess(std::string exe) {
    std::lock_guard lock(mutex_);
    passthrough_exe_ = std::move(exe);
}

void HotkeyManager::start() {
    if (running_) {
        return;
    }
    running_ = true;
    dispatch_worker_ = std::thread(&HotkeyManager::dispatchLoop, this);
    poll_worker_ = std::thread(&HotkeyManager::pollLoop, this);
#ifdef _WIN32
    hook_worker_ = std::thread(&HotkeyManager::hookLoop, this);
#endif
}

void HotkeyManager::stop() {
    if (!running_) {
        // 仍可能有线程残留（极少见），保险起见走 join 流程。
    }
    running_ = false;
#ifdef _WIN32
    if (hook_thread_id_ != 0) {
        PostThreadMessage(hook_thread_id_, WM_QUIT, 0, 0);
    }
#endif
    queue_cv_.notify_all();
    if (poll_worker_.joinable()) poll_worker_.join();
    if (dispatch_worker_.joinable()) dispatch_worker_.join();
#ifdef _WIN32
    if (hook_worker_.joinable()) hook_worker_.join();
    hook_thread_id_ = 0;
#endif
    // 清空尚未执行的回调。
    std::lock_guard lock(queue_mutex_);
    queue_.clear();
}

bool HotkeyManager::isRunning() const {
    return running_;
}

void HotkeyManager::clear() {
    stop();
    std::lock_guard lock(mutex_);
    hotkeys_.clear();
}

bool HotkeyManager::isModifier(int vk) {
    return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU;
}

bool HotkeyManager::isMouseKey(int vk) const {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
           vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

bool HotkeyManager::isFunctionKey(int vk) {
    // F1~F12：程序运行期间这些键一律被钩子吞掉，仅本程序可见。
    return vk >= VK_F1 && vk <= VK_F12;
}

void HotkeyManager::enqueue(std::function<void()> fn) {
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(std::move(fn));
    }
    queue_cv_.notify_one();
}

void HotkeyManager::dispatchLoop() {
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) {
                return;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        if (fn) fn();
    }
}

void HotkeyManager::pollLoop() {
    // 仅轮询鼠标按钮型热键/状态监听（键盘键由钩子处理）。
    // 非 Windows 平台没有钩子，这里退化为对所有键的轮询。
    while (running_) {
        std::vector<Callback> callbacks;
        std::vector<std::pair<StateCallback, bool>> state_callbacks;
        {
            std::lock_guard lock(mutex_);
            for (auto& hk : hotkeys_) {
                if (!hk.combo.key) continue;
#ifdef _WIN32
                if (!hk.mouse) continue;  // 键盘键交给钩子。
#endif
                const bool down = InputController::isKeyDown(hk.combo.key);
                if (hk.state_mode && down != hk.was_down && hk.state_callback) {
                    state_callbacks.emplace_back(hk.state_callback, down);
                } else if (!hk.state_mode && down && !hk.was_down && hk.callback) {
                    callbacks.push_back(hk.callback);
                }
                hk.was_down = down;
            }
        }
        for (const auto& [cb, state] : state_callbacks) {
            if (cb) cb(state);
        }
        for (const auto& cb : callbacks) {
            if (cb) cb();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

#ifdef _WIN32

bool HotkeyManager::comboDownFromState(const HotkeyCombo& combo) const {
    if (!combo.key) return false;
    for (const int modifier : combo.modifiers) {
        if (modifier && modifier < 256 && !key_down_[modifier]) {
            return false;
        }
    }
    if (combo.key < 0 || combo.key >= 256) return false;
    return key_down_[combo.key];
}

bool HotkeyManager::foregroundAllowsKeys() {
    // 仅钩子线程调用，按前台 hwnd 缓存，避免每个按键都查询进程。
    HWND fg = GetForegroundWindow();
    if (fg == fg_cache_hwnd_) {
        return fg_cache_allow_;
    }
    fg_cache_hwnd_ = fg;
    bool allow = false;
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            // 本程序自己在前台：放行，保证能录制 F 键、切页。
            allow = true;
        } else if (pid) {
            std::string target;
            {
                std::lock_guard lock(mutex_);
                target = passthrough_exe_;
            }
            if (!target.empty()) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (h) {
                    wchar_t path[MAX_PATH];
                    DWORD size = MAX_PATH;
                    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
                        std::wstring full(path, size);
                        const auto slash = full.find_last_of(L"\\/");
                        const std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
                        const std::wstring want = widen(target);
                        allow = base.size() == want.size() &&
                                std::equal(base.begin(), base.end(), want.begin(), [](wchar_t a, wchar_t b) {
                                    return std::towlower(a) == std::towlower(b);
                                });
                    }
                    CloseHandle(h);
                }
            }
        }
    }
    fg_cache_allow_ = allow;
    return allow;
}

bool HotkeyManager::onKeyboardEvent(int vk, bool down) {
    if (vk < 0 || vk >= 256) return false;

    // 把左右修饰键归并到通用 VK，便于组合匹配。
    auto normalize = [](int code) -> int {
        switch (code) {
            case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
            case VK_LSHIFT:   case VK_RSHIFT:   return VK_SHIFT;
            case VK_LMENU:    case VK_RMENU:    return VK_MENU;
            default: return code;
        }
    };
    const int norm = normalize(vk);

    std::vector<Callback> to_fire;
    std::vector<std::pair<StateCallback, bool>> states_to_fire;
    // F1~F12 默认吞掉，但当前台是游戏或本程序自身时放行，
    // 让游戏能收到 F8 开火、本程序能录制 F 键。
    bool intercept = isFunctionKey(vk) && !foregroundAllowsKeys();
    {
        std::lock_guard lock(mutex_);
        // 更新权威按键状态（通用 + 归并后的）。
        key_down_[vk] = down;
        if (norm != vk && norm < 256) key_down_[norm] = down;

        for (auto& hk : hotkeys_) {
            if (hk.mouse || !hk.combo.key) continue;
            if (hk.combo.key != vk && hk.combo.key != norm) continue;

            if (hk.state_mode) {
                const bool now = comboDownFromState(hk.combo);
                if (now != hk.was_down) {
                    if (hk.state_callback) states_to_fire.emplace_back(hk.state_callback, now);
                    hk.was_down = now;
                }
                if (hk.intercept) intercept = true;
            } else {
                const bool now = comboDownFromState(hk.combo);
                if (now && !hk.was_down && hk.callback) {
                    to_fire.push_back(hk.callback);
                }
                hk.was_down = now;
                if (hk.intercept) intercept = true;
            }
        }
    }
    for (auto& cb : to_fire) enqueue(cb);
    for (auto& [cb, st] : states_to_fire) {
        enqueue([cb, st] { if (cb) cb(st); });
    }
    return intercept;
}

LRESULT CALLBACK HotkeyManager::lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    HotkeyManager* self = s_active;
    if (nCode == HC_ACTION && self) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (down || up) {
            if (self->onKeyboardEvent(static_cast<int>(kb->vkCode), down)) {
                // 吞掉该按键：系统和其它程序都收不到。
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void HotkeyManager::hookLoop() {
    hook_thread_id_ = GetCurrentThreadId();
    s_active = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &HotkeyManager::lowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (running_ && GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    s_active = nullptr;
    key_down_.fill(false);
}

#endif  // _WIN32

} // namespace pubg
