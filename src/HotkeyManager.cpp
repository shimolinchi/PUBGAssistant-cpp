#include "HotkeyManager.hpp"

namespace pubg {

HotkeyManager::~HotkeyManager() {
    stop();
}

void HotkeyManager::addHotkey(std::string name, int vk, Callback cb) {
    addHotkey(std::move(name), HotkeyCombo{{}, vk}, std::move(cb));
}

void HotkeyManager::addHotkey(std::string name, HotkeyCombo combo, Callback cb) {
    Hotkey hk;
    hk.name = std::move(name);
    hk.combo = std::move(combo);
    hk.callback = std::move(cb);
    std::lock_guard lock(mutex_);
    hotkeys_.push_back(std::move(hk));
}

void HotkeyManager::addStateWatcher(std::string name, int vk, StateCallback cb) {
    Hotkey hk;
    hk.name = std::move(name);
    hk.combo = HotkeyCombo{{}, vk};
    hk.state_callback = std::move(cb);
    hk.state_mode = true;
    std::lock_guard lock(mutex_);
    hotkeys_.push_back(std::move(hk));
}

void HotkeyManager::start() {
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread(&HotkeyManager::run, this);
}

void HotkeyManager::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
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

bool HotkeyManager::isComboDown(const HotkeyCombo& combo) {
    if (!combo.key) {
        return false;
    }
    for (const int modifier : combo.modifiers) {
        if (modifier && !InputController::isKeyDown(modifier)) {
            return false;
        }
    }
    return InputController::isKeyDown(combo.key);
}

void HotkeyManager::run() {
    while (running_) {
        std::vector<Callback> callbacks;
        std::vector<std::pair<StateCallback, bool>> state_callbacks;
        {
            std::lock_guard lock(mutex_);
            for (auto& hk : hotkeys_) {
                if (!hk.combo.key) {
                    continue;
                }
                const bool down = hk.state_mode ? InputController::isKeyDown(hk.combo.key) : isComboDown(hk.combo);
                if (hk.state_mode && down != hk.was_down && hk.state_callback) {
                    state_callbacks.emplace_back(hk.state_callback, down);
                } else if (!hk.state_mode && down && !hk.was_down && hk.callback) {
                    callbacks.push_back(hk.callback);
                }
                hk.was_down = down;
            }
        }
        for (const auto& [cb, state] : state_callbacks) {
            cb(state);
        }
        for (const auto& cb : callbacks) {
            cb();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace pubg
