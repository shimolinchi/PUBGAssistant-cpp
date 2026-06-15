#include "App.hpp"

#include "BuildConfig.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <tuple>
#include <QApplication>
#include <QMetaObject>

namespace pubg {

App::App()
    : paths_(),
      config_(paths_) {
    config_.load();
    migrateLegacyDefaultHotkeys();
    regions_ = std::make_unique<RegionManager>(config_);
    minimap_ = std::make_unique<MinimapRadar>(config_, *regions_, 60);
    elevation_ = std::make_unique<ElevationRadar>(config_, *regions_, 30);
    weapon_detector_ = std::make_unique<WeaponDetector>(config_, *regions_, 30, 0.65);
    equipment_detector_ = std::make_unique<EquipmentDetector>(config_, *regions_, 15, 10.0);
    gesture_identifier_ = std::make_unique<GestureIdentifier>(config_, *regions_, 30, 0.65);
    recoil_ = std::make_unique<RecoilControl>(config_, regions_.get());
    special_ = std::make_unique<SpecialAssistants>(config_, *regions_, *minimap_, *elevation_);
    map_points_ = std::make_unique<MapPointAssistant>(config_, *regions_);
    large_map_ = std::make_unique<LargeMapRadar>(config_, *regions_);
    throwables_ = std::make_unique<ThrowablesAssistant>(config_, *regions_, *minimap_);
    c4_ = std::make_unique<C4Assistant>(config_, *regions_, *minimap_);
    status_hud_ = std::make_unique<StatusHud>(config_, *regions_);
    manual_assistants_ = {
        {"mortar", false}, {"rocket", false}, {"throwables", false},
        {"vss", false}, {"crossbow", false}, {"c4", false},
    };
    wireCallbacks();
}

void App::wireCallbacks() {
    weapon_detector_->setEnabled(false, [this](const std::string& weapon, double score) {
        c4_->onWeaponDetected(weapon, score);
        updateWeaponFromDetectors(weapon, score);
    });
    equipment_detector_->setEnabled(false, [this](const std::map<int, WeaponSlotInfo>& equipment_slots) {
        updateEquipment(equipment_slots);
    });
    equipment_detector_->setStatusCallback([this](const std::string& status) {
        if (status_hud_) {
            status_hud_->setEquipmentStatus(status);
        }
    });
    gesture_identifier_->setEnabled(false, [this](const std::string& stance, double) {
        {
            std::lock_guard lock(state_mutex_);
            current_stance_ = stance;
        }
        recoil_->updateStance(stance);
        if (status_hud_) {
            status_hud_->setStance(stance);
        }
    });

    registerHotkeys();
}

std::string App::currentMarkerColor() const {
    std::lock_guard lock(state_mutex_);
    return current_marker_color_;
}

void App::registerHotkeys() {
    // F1~F12 被键盘钩子吞掉、不传给系统；但当游戏（默认 TslGame.exe，可在 config.json 的
    // game_process 改）或本程序自身处于前台时放行，使游戏能收到 F8 开火、本程序能录制 F 键。
    hotkeys_.setPassthroughProcess(config_.read([](const Json& data) {
        return data.value("game_process", std::string("TslGame.exe"));
    }));
    // Home 等键照常放行，只触发对应功能。
    hotkeys_.addHotkey("toggle_weapon_detection", hotkeyCombo("toggle_weapon_detection", "<f1>"), [this] { toggleWeaponDetection(); });
    hotkeys_.addHotkey("toggle_display", hotkeyCombo("toggle_display", "<f2>"), [this] { toggleDisplay(); });
#if PUBG_ENABLE_INPUT_CONTROL
    hotkeys_.addHotkey("toggle_recoil", hotkeyCombo("toggle_recoil", "<f3>"), [this] { toggleRecoil(); });
#endif
    hotkeys_.addHotkey("toggle_display_n", InputController::parseVirtualKey("n"), [this] { toggleDisplay(); });
    hotkeys_.addHotkey("measure_map", hotkeyCombo("measure_map", "<f4>"), [this] {
        large_map_->toggleMode();
    });
    hotkeys_.addHotkey("toggle_window", hotkeyCombo("toggle_window", "<home>"), [this] {
        if (main_window_) {
            QMetaObject::invokeMethod(main_window_, [this] { main_window_->toggleWindowVisible(); }, Qt::QueuedConnection);
        }
    });
    hotkeys_.addHotkey("tab_left", VK_LEFT, [this] {
        if (main_window_) {
            QMetaObject::invokeMethod(main_window_, [this] { main_window_->switchTab(-1); }, Qt::QueuedConnection);
        }
    });
    hotkeys_.addHotkey("tab_right", VK_RIGHT, [this] {
        if (main_window_) {
            QMetaObject::invokeMethod(main_window_, [this] { main_window_->switchTab(1); }, Qt::QueuedConnection);
        }
    });
    hotkeys_.addHotkey("tab", hotkeyCombo("toggle_equipment", "tab"), [this] {
        const bool enabled = [this] {
            std::lock_guard lock(state_mutex_);
            return weapon_detection_enabled_;
        }();
        if (enabled) {
            equipment_detector_->onTabPress();
        }
    });
    hotkeys_.addHotkey("marker_prev", hotkeyCombo("marker_prev", "q"), [this] { cycleMarkerColor(-1); });
    hotkeys_.addHotkey("marker_next", hotkeyCombo("marker_next", "e"), [this] { cycleMarkerColor(1); });
    hotkeys_.addStateWatcher("throw", hotkeyVk("throw", "b"), [this](bool pressed) {
        const bool should_throw = [this] {
            std::lock_guard lock(state_mutex_);
            return display_enabled_ && current_weapon_ == "Grenade";
        }();
        if (should_throw) {
            throwables_->onThrowKey(pressed);
        }
    });
    hotkeys_.addStateWatcher("left_mouse", VK_LBUTTON, [this](bool pressed) {
        bool show_map_points = false;
        {
            std::lock_guard lock(state_mutex_);
            left_pressed_ = pressed;
            show_map_points = left_pressed_ && middle_pressed_ && current_weapon_.empty();
        }
        if (pressed) {
            const auto [x, y] = InputController::cursorPosition();
            large_map_->onMouseClick(x, y, true);
            c4_->onMouseLeftPress();
        }
        if (show_map_points) {
            map_points_->setEnabled(true);
        }
    });
    hotkeys_.addStateWatcher("middle_mouse", VK_MBUTTON, [this](bool pressed) {
        bool show_map_points = false;
        {
            std::lock_guard lock(state_mutex_);
            middle_pressed_ = pressed;
            show_map_points = left_pressed_ && middle_pressed_ && current_weapon_.empty();
        }
        if (show_map_points) {
            map_points_->setEnabled(true);
        }
    });
    hotkeys_.addStateWatcher("right_mouse", VK_RBUTTON, [this](bool pressed) {
        bool hide_map_points = false;
        {
            std::lock_guard lock(state_mutex_);
            right_pressed_ = pressed;
            hide_map_points = pressed && !alt_pressed_;
        }
        c4_->onMouseRightClick(pressed);
        if (hide_map_points && map_points_) {
            map_points_->setEnabled(false);
        }
    });
    hotkeys_.addStateWatcher("alt", VK_MENU, [this](bool pressed) {
        std::lock_guard lock(state_mutex_);
        alt_pressed_ = pressed;
    });
}

void App::reloadHotkeys() {
    std::lock_guard control_lock(control_mutex_);
    const bool was_running = hotkeys_.isRunning();
    hotkeys_.clear();
    registerHotkeys();
    recoil_->reloadConfig();
    if (was_running) {
        hotkeys_.start();
    }
}

void App::shutdown() {
    std::lock_guard control_lock(control_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
    }
    hotkeys_.stop();
    equipment_detector_->setEnabled(false);
    weapon_detector_->setEnabled(false);
    gesture_identifier_->setEnabled(false);
    recoil_->setEnabled(false);
    minimap_->setEnabled(false);
    elevation_->setEnabled(false);
    large_map_->setDisplay(false);
    throwables_->setEnabled(false);
    c4_->setEnabled(false);
    special_->shutdown();
    c4_->shutdown();
}

int App::hotkeyVk(const std::string& name, const std::string& fallback) const {
    const auto hotkeys = config_.hotkeys();
    const auto it = hotkeys.find(name);
    const auto& value = it != hotkeys.end() ? it->second : fallback;
    const int vk = InputController::parseVirtualKey(value);
    return vk ? vk : InputController::parseVirtualKey(fallback);
}

HotkeyManager::HotkeyCombo App::hotkeyCombo(const std::string& name, const std::string& fallback) const {
    const auto hotkeys = config_.hotkeys();
    const auto it = hotkeys.find(name);
    const std::string value = it != hotkeys.end() ? it->second : fallback;
    HotkeyManager::HotkeyCombo combo;
    auto parse = [](std::string part) {
        part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c) { return std::isspace(c) != 0; }), part.end());
        return InputController::parseVirtualKey(part);
    };
    size_t start = 0;
    while (start <= value.size()) {
        const size_t plus = value.find('+', start);
        const std::string part = value.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        const int vk = parse(part);
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) {
            combo.modifiers.push_back(vk);
        } else if (vk) {
            combo.key = vk;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!combo.key) {
        combo.key = InputController::parseVirtualKey(fallback);
        combo.modifiers.clear();
    }
    return combo;
}

void App::migrateLegacyDefaultHotkeys() {
    std::vector<std::tuple<std::string, std::vector<std::string>, std::string>> migrations{
        {"toggle_weapon_detection", {"<f2>"}, "<f1>"},
        {"toggle_display", {"<ctrl>+<shift>+<space>"}, "<f2>"},
        {"toggle_recoil", {"<ctrl>+<shift>+<tab>"}, "<f3>"},
        {"measure_map", {"<f1>", "<ctrl>+<shift>+m"}, "<f4>"},
    };
    bool changed = false;
    config_.write([&](Json& data) {
        if (!data.contains("hotkeys") || !data["hotkeys"].is_object()) {
            return;
        }
        for (const auto& [action, old_values, next] : migrations) {
            if (!data["hotkeys"].contains(action) || !data["hotkeys"][action].is_string()) {
                continue;
            }
            const std::string current = data["hotkeys"][action].get<std::string>();
            if (std::find(old_values.begin(), old_values.end(), current) != old_values.end()) {
                data["hotkeys"][action] = next;
                changed = true;
            }
        }
        auto& hotkeys = data["hotkeys"];
        if (!hotkeys.contains("toggle_window") || !hotkeys["toggle_window"].is_string()) {
            hotkeys["toggle_window"] = "<home>";
            changed = true;
        }
        if (hotkeys.value("fire_key", std::string("end")) == hotkeys.value("toggle_window", std::string("<home>"))) {
            hotkeys["fire_key"] = "end";
            hotkeys["toggle_window"] = "<home>";
            changed = true;
        }
    });
    if (changed) {
        config_.save();
    }
}

void App::syncMarkerColorsFromConfig() {
    const auto colors = config_.markerColors();
    const auto hex = config_.markerHex();
    minimap_->setMarkerColors(colors);
    elevation_->setMarkerColors(colors);
    large_map_->setMarkerColors(colors);
    special_->setMarkerHex(hex);
    throwables_->setMarkerHex(hex);
    c4_->setMarkerHex(hex);
    if (status_hud_) {
        status_hud_->reloadMarkerHex();
    }
}

void App::setWeaponDetectionEnabled(bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection_enabled_ = enabled;
    }
    if (main_window_) {
        QMetaObject::invokeMethod(main_window_, [this, enabled] { main_window_->setWeaponDetectionState(enabled); }, Qt::QueuedConnection);
    }
    weapon_detector_->setEnabled(enabled);
    equipment_detector_->setEnabled(enabled);
    gesture_identifier_->setEnabled(enabled);
    if (!enabled) {
        updateWeaponFromDetectors("", 0.0);
    }
    updateStatusHud();
    printStatus();
}

void App::setDisplayEnabled(bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        display_enabled_ = enabled;
    }
    if (main_window_) {
        QMetaObject::invokeMethod(main_window_, [this, enabled] { main_window_->setDisplayState(enabled); }, Qt::QueuedConnection);
    }
    minimap_->setEnabled(enabled);
    minimap_->setDisplay(enabled);
    elevation_->setEnabled(enabled);
    elevation_->setDisplay(enabled);
    large_map_->setDisplay(enabled);
    special_->setDisplayEnabled(enabled);
    if (!enabled) {
        map_points_->setEnabled(false);
        {
            std::lock_guard lock(state_mutex_);
            for (auto& [_, state] : manual_assistants_) {
                state = false;
            }
        }
        throwables_->setEnabled(false);
        c4_->setEnabled(false);
    }
    updateAssistantRouting();
    updateStatusHud();
    printStatus();
}

void App::setRecoilEnabled(bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    std::string current_weapon;
    {
        std::lock_guard lock(state_mutex_);
        recoil_enabled_ = enabled;
        current_weapon = current_weapon_;
    }
    if (main_window_) {
        QMetaObject::invokeMethod(main_window_, [this, enabled] { main_window_->setRecoilState(enabled); }, Qt::QueuedConnection);
    }
    recoil_->setEnabled(enabled);
    if (!enabled) {
        recoil_->updateCurrentWeapon("");
    } else if (isRecoilWeapon(current_weapon)) {
        recoil_->updateCurrentWeapon(current_weapon);
        syncRecoilAttachmentsForCurrentWeapon();
    }
    updateStatusHud();
    printStatus();
}

void App::toggleWeaponDetection() {
    const bool next = [this] {
        std::lock_guard lock(state_mutex_);
        return !weapon_detection_enabled_;
    }();
    setWeaponDetectionEnabled(next);
}

void App::toggleDisplay() {
    const bool next = [this] {
        std::lock_guard lock(state_mutex_);
        return !display_enabled_;
    }();
    setDisplayEnabled(next);
}

void App::toggleRecoil() {
    const bool next = [this] {
        std::lock_guard lock(state_mutex_);
        return !recoil_enabled_;
    }();
    setRecoilEnabled(next);
}

void App::cycleMarkerColor(int direction) {
    if (!shouldShowMarkerIndicator()) {
        return;
    }
    std::string color;
    {
        std::lock_guard lock(state_mutex_);
        auto it = std::find(marker_color_order_.begin(), marker_color_order_.end(), current_marker_color_);
        int index = it == marker_color_order_.end() ? 0 : static_cast<int>(std::distance(marker_color_order_.begin(), it));
        index = (index + direction + static_cast<int>(marker_color_order_.size())) % static_cast<int>(marker_color_order_.size());
        current_marker_color_ = marker_color_order_[index];
        color = current_marker_color_;
    }
    throwables_->setSelectedColor(color);
    c4_->setSelectedColor(color);
    if (status_hud_) {
        status_hud_->setMarkerColor(color);
    }
}

void App::updateWeaponFromDetectors(const std::string& weapon, double score) {
    if (recoil_->isFiring()) {
        return;
    }
    bool recoil_enabled = false;
    {
        std::lock_guard lock(state_mutex_);
        current_weapon_ = weapon;
        recoil_enabled = recoil_enabled_;
    }
    if (recoil_enabled) {
        if (isRecoilWeapon(weapon)) {
            recoil_->updateCurrentWeapon(weapon);
            syncRecoilAttachmentsForCurrentWeapon();
        } else {
            recoil_->updateCurrentWeapon("");
        }
    }
    special_->setCurrentWeapon(weapon);
    if (!weapon.empty()) {
        map_points_->setEnabled(false);
    }
    updateAssistantRouting();
    if (status_hud_) {
        status_hud_->setCurrentWeapon(weapon);
    }
    std::cout << "[weapon] " << (weapon.empty() ? "(none)" : weapon) << " score=" << score << "\n";
}

void App::updateEquipment(const std::map<int, WeaponSlotInfo>& equipment_slots) {
    {
        std::lock_guard lock(state_mutex_);
        equipment_ = equipment_slots;
    }
    if (status_hud_) {
        status_hud_->setEquipment(equipment_slots);
    }
    std::optional<std::string> w1;
    std::optional<std::string> w2;
    if (auto it = equipment_slots.find(1); it != equipment_slots.end() && !it->second.name.empty()) {
        w1 = it->second.name;
    }
    if (auto it = equipment_slots.find(2); it != equipment_slots.end() && !it->second.name.empty()) {
        w2 = it->second.name;
    }
    weapon_detector_->updatePrimaryWeapons(w1, w2);
    for (const auto& [slot, info] : equipment_slots) {
        std::cout << "[equipment] slot " << slot << " weapon=" << info.name
                  << " scope=" << info.scope << " grip=" << info.grip
                  << " muzzle=" << info.muzzle << " stock=" << info.stock << "\n";
    }
    const bool has_current_weapon = [this] {
        std::lock_guard lock(state_mutex_);
        return !current_weapon_.empty();
    }();
    if (has_current_weapon) {
        syncRecoilAttachmentsForCurrentWeapon();
    }
}

bool App::isRecoilWeapon(const std::string& weapon) const {
    return !weapon.empty() && weapon != "Rocket" && weapon != "Grenade" &&
           weapon != "VSS" && weapon != "Crossbow" && weapon != "C4";
}

void App::syncRecoilAttachmentsForCurrentWeapon() {
    std::string current_weapon;
    std::map<int, WeaponSlotInfo> equipment;
    {
        std::lock_guard lock(state_mutex_);
        current_weapon = current_weapon_;
        equipment = equipment_;
    }
    if (!isRecoilWeapon(current_weapon)) {
        return;
    }
    for (const auto& [_, info] : equipment) {
        if (info.name == current_weapon) {
            recoil_->updateAttachments(info);
            return;
        }
    }
}

void App::setAssistantManual(const std::string& key, bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        if (!display_enabled_) {
            manual_assistants_[key] = false;
        } else {
            manual_assistants_[key] = enabled;
        }
    }
    updateAssistantRouting();
}

bool App::shouldShowMarkerIndicator() const {
    std::lock_guard lock(state_mutex_);
    return display_enabled_;
}

void App::updateAssistantRouting() {
    bool display_enabled = false;
    std::string current_weapon;
    std::unordered_map<std::string, bool> manual;
    {
        std::lock_guard lock(state_mutex_);
        display_enabled = display_enabled_;
        current_weapon = current_weapon_;
        manual = manual_assistants_;
    }

    if (!display_enabled) {
        special_->setManualEnabled("mortar", false);
        special_->setManualEnabled("rocket", false);
        special_->setManualEnabled("vss", false);
        special_->setManualEnabled("crossbow", false);
        throwables_->setEnabled(false);
        c4_->setEnabled(false);
        if (status_hud_) status_hud_->setMarkerIndicatorVisible(false);
        return;
    }

    special_->setManualEnabled("mortar", true);
    special_->setManualEnabled("rocket", manual["rocket"]);
    special_->setManualEnabled("vss", manual["vss"]);
    special_->setManualEnabled("crossbow", manual["crossbow"]);

    const bool throwables_on = manual["throwables"] || current_weapon == "Grenade";
    const bool c4_on = manual["c4"] || current_weapon == "C4";
    throwables_->setEnabled(throwables_on);
    c4_->setEnabled(c4_on);
    if (status_hud_) {
        status_hud_->setMarkerIndicatorVisible(display_enabled);
    }
}

void App::updateStatusHud() {
    if (!status_hud_) {
        return;
    }
    bool weapon_detection = false;
    bool display = false;
    bool recoil = false;
    std::string current_weapon;
    std::string current_stance;
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection = weapon_detection_enabled_;
        display = display_enabled_;
        recoil = recoil_enabled_;
        current_weapon = current_weapon_;
        current_stance = current_stance_;
    }
    status_hud_->setSwitches(weapon_detection, display, recoil);
    status_hud_->setCurrentWeapon(current_weapon);
    status_hud_->setStance(current_stance);
    status_hud_->setMarkerIndicatorVisible(display);
}

void App::printStatus() const {
    bool weapon_detection = false;
    bool display = false;
    bool recoil = false;
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection = weapon_detection_enabled_;
        display = display_enabled_;
        recoil = recoil_enabled_;
    }
    std::cout << "[status] weapon_detection=" << weapon_detection
              << " display=" << display
              << " recoil=" << recoil << "\n";
}

int App::run() {
    std::cout << "PUBGAssistant C++ port started.\n";
    std::cout << "F1 weapon detection, F2 display, F3 recoil, Tab equipment scan.\n";
    syncMarkerColorsFromConfig();
    setWeaponDetectionEnabled(true);
    pubg::ui::MainWindow::ControlCallbacks callbacks;
    callbacks.set_weapon_detection = [this](bool enabled) { setWeaponDetectionEnabled(enabled); };
    callbacks.set_display = [this](bool enabled) { setDisplayEnabled(enabled); };
    callbacks.set_recoil = [this](bool enabled) { setRecoilEnabled(enabled); };
    callbacks.set_assistant = [this](const std::string& key, bool enabled) { setAssistantManual(key, enabled); };
    callbacks.sync_marker_colors = [this] { syncMarkerColorsFromConfig(); };
    callbacks.reload_hotkeys = [this] { reloadHotkeys(); };
    callbacks.shutdown = [this] { shutdown(); };
    pubg::ui::MainWindow window(config_, *regions_, *minimap_, *elevation_, *weapon_detector_,
                                *equipment_detector_, *gesture_identifier_, *recoil_, *special_,
                                *map_points_, *large_map_, *throwables_, *c4_, std::move(callbacks));
    main_window_ = &window;
    window.show();
    hotkeys_.start();
    const int code = QApplication::exec();
    main_window_ = nullptr;
    shutdown();
    return code;
}

} // namespace pubg
