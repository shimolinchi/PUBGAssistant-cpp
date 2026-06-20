#include "App.hpp"

#include "BuildConfig.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <tuple>
#include <QApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
namespace {
bool heapCheckEnabled() {
    static const bool on = [] {
        size_t len = 0;
        char buf[8]{};
        return getenv_s(&len, buf, sizeof(buf), "PUBG_HEAPCHECK") == 0 && len > 0 && buf[0] == '1';
    }();
    return on;
}
void heapProbe(const char* where) {
    if (!heapCheckEnabled()) return;
    if (_CrtCheckMemory()) {
        std::cerr << "[heapcheck] OK at " << where << "\n";
    } else {
        std::cerr << "[heapcheck] *** CORRUPTED detected at " << where << " ***\n";
    }
    std::cerr.flush();
}
} // namespace
#define PUBG_HEAP_PROBE(where) heapProbe(where)
#else
#define PUBG_HEAP_PROBE(where) ((void)0)
#endif

namespace pubg {

App::App()
    : paths_(),
      config_(paths_) {
    PUBG_HEAP_PROBE("ctor.start");
    config_.load();
    PUBG_HEAP_PROBE("ctor.afterConfigLoad");
    migrateLegacyDefaultHotkeys();
    PUBG_HEAP_PROBE("ctor.afterHotkeyMigrate");
    regions_ = std::make_unique<RegionManager>(config_);
    PUBG_HEAP_PROBE("ctor.afterRegions");
    minimap_ = std::make_unique<MinimapRadar>(config_, *regions_, 60);
    PUBG_HEAP_PROBE("ctor.afterMinimap");
    elevation_ = std::make_unique<ElevationRadar>(config_, *regions_, 30);
    PUBG_HEAP_PROBE("ctor.afterElevation");
    weapon_detector_ = std::make_unique<WeaponDetector>(config_, *regions_, 30, 0.65);
    PUBG_HEAP_PROBE("ctor.afterWeaponDetector");
    equipment_detector_ = std::make_unique<EquipmentDetector>(config_, *regions_, 60, 10.0);
    PUBG_HEAP_PROBE("ctor.afterEquipmentDetector");
    gesture_identifier_ = std::make_unique<GestureIdentifier>(config_, *regions_, 30, 0.65);
    PUBG_HEAP_PROBE("ctor.afterGestureIdentifier");
    recoil_ = std::make_unique<RecoilControl>(config_, regions_.get());
    PUBG_HEAP_PROBE("ctor.afterRecoil");
    large_map_ = std::make_unique<LargeMapRadar>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterLargeMap");
    special_ = std::make_unique<SpecialAssistants>(config_, *regions_, *minimap_, *elevation_, *large_map_);
    PUBG_HEAP_PROBE("ctor.afterSpecial");
    map_points_ = std::make_unique<MapPointAssistant>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterMapPoints");
    mortar_auto_aim_ = std::make_unique<MortarAutoAim>(config_, *regions_, *minimap_, *large_map_, *elevation_,
        [this](const std::string& message, const std::string& marker_color) {
        if (status_hud_) {
            const auto hex = config_.markerHex();
            const auto it = hex.find(marker_color);
            status_hud_->showTemporaryMessage(message, 1500, it != hex.end() ? it->second : "#FFFFFF");
        }
    });
    PUBG_HEAP_PROBE("ctor.afterMortarAutoAim");
    mortar_mount_detector_ = std::make_unique<MortarMountDetector>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterMortarMountDetector");
    throwables_ = std::make_unique<ThrowablesAssistant>(config_, *regions_, *minimap_);
    PUBG_HEAP_PROBE("ctor.afterThrowables");
    c4_ = std::make_unique<C4Assistant>(config_, *regions_, *minimap_);
    PUBG_HEAP_PROBE("ctor.afterC4");
    status_hud_ = std::make_unique<StatusHud>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterStatusHud");
    assistant_enabled_ = {
        {"mortar", true}, {"rocket", true}, {"throwables", true},
        {"vss", true}, {"crossbow", true}, {"c4", true},
        {"auto_recoil", true}, {"dmr_tap", true},
        {"sr_breath", true}, {"sg_peek", true},
    };
    loadUiState();
    wireCallbacks();
    PUBG_HEAP_PROBE("ctor.afterWireCallbacks");
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
    hotkeys_.setPassthroughProcess(config_.read([](const Json& data) {
        return data.value("game_process", std::string("TslGame.exe"));
    }));
    hotkeys_.addHotkey("toggle_weapon_detection", hotkeyCombo("toggle_weapon_detection", "<f1>"), [this] { toggleWeaponDetection(); });
    hotkeys_.addHotkey("toggle_display", hotkeyCombo("toggle_display", "<f2>"), [this] { toggleDisplay(); });
#if PUBG_ENABLE_INPUT_CONTROL
    hotkeys_.addHotkey("toggle_recoil", hotkeyCombo("toggle_recoil", "<f3>"), [this] { toggleRecoil(); });
#endif
    hotkeys_.addHotkey("measure_map", hotkeyCombo("measure_map", "<f4>"), [this] {
        large_map_->toggleMode();
    });
    hotkeys_.addStateWatcher("mortar_auto_aim", hotkeyVk("mortar_auto_aim", "mouse_right"), [this](bool pressed) {
        static std::atomic_uint64_t hold_session{0};
        const auto session = ++hold_session;
        if (!pressed) {
            return;
        }
        const int hold_ms = config_.read([](const Json& data) {
            return data.value("mortar_config", Json::object()).value("direction_auto_aim_hold_ms", 180);
        });
        std::thread([this, session, hold_ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, hold_ms)));
            if (session != hold_session.load()) {
                return;
            }
            bool can_trigger = false;
            {
                std::lock_guard lock(state_mutex_);
                can_trigger = display_enabled_ && mortar_mounted_;
            }
            if (can_trigger && mortar_auto_aim_) {
                mortar_auto_aim_->trigger(currentMarkerColor());
            }
        }).detach();
    });
    hotkeys_.addHotkey("toggle_window", hotkeyCombo("toggle_window", "<home>"), [this] {
        if (main_window_) {
            QMetaObject::invokeMethod(main_window_, [this] { main_window_->toggleWindowVisible(); }, Qt::QueuedConnection);
        }
    });
    hotkeys_.addHotkey("toggle_equipment", hotkeyCombo("toggle_equipment", ""), [this] {
        const bool enabled = [this] {
            std::lock_guard lock(state_mutex_);
            return weapon_detection_enabled_;
        }();
        if (enabled) {
            equipment_detector_->requestEquipmentConfirmation();
        }
    });
    hotkeys_.addHotkey("marker_prev", hotkeyCombo("marker_prev", "<up>"), [this] { cycleMarkerColor(-1); });
    hotkeys_.addHotkey("marker_next", hotkeyCombo("marker_next", "<down>"), [this] { cycleMarkerColor(1); });
    hotkeys_.addStateWatcher("sg_peek_left", InputController::parseVirtualKey("q"), [this](bool pressed) {
        if (pressed && recoil_) {
            recoil_->setSgPeekDirection(1);
            updateStatusHud();
        }
    });
    hotkeys_.addStateWatcher("sg_peek_right", InputController::parseVirtualKey("e"), [this](bool pressed) {
        if (pressed && recoil_) {
            recoil_->setSgPeekDirection(2);
            updateStatusHud();
        }
    });
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
        bool weapon_detection = false;
        std::optional<std::pair<std::string, double>> pending_weapon;
        {
            std::lock_guard lock(state_mutex_);
            left_pressed_ = pressed;
            show_map_points = left_pressed_ && middle_pressed_ && current_weapon_.empty();
            weapon_detection = weapon_detection_enabled_;
            if (!pressed) {
                pending_weapon = pending_weapon_update_;
                pending_weapon_update_.reset();
            }
        }
        if (pressed) {
            const auto [x, y] = InputController::cursorPosition();
            large_map_->onMouseClick(x, y, true);
            c4_->onMouseLeftPress();
        } else if (weapon_detection) {
            if (pending_weapon) {
                applyWeaponUpdate(pending_weapon->first, pending_weapon->second);
            }
            equipment_detector_->requestScan();
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
        bool weapon_detection = false;
        {
            std::lock_guard lock(state_mutex_);
            right_pressed_ = pressed;
            hide_map_points = pressed && !alt_pressed_;
            weapon_detection = weapon_detection_enabled_;
        }
        c4_->onMouseRightClick(pressed);
        if (pressed && large_map_) {
            large_map_->cancel();
        }
        if (!pressed && weapon_detection) {
            equipment_detector_->requestScan();
        }
        if (hide_map_points && map_points_) {
            map_points_->setEnabled(false);
        }
    });
    hotkeys_.addStateWatcher("cancel_map_helpers", hotkeyVk("open_large_map", "m"), [this](bool pressed) {
        if (!pressed) {
            return;
        }
        if (large_map_) {
            large_map_->cancel();
        }
        if (map_points_) {
            map_points_->setEnabled(false);
        }
    });
    hotkeys_.addStateWatcher("mortar_interact", InputController::parseVirtualKey("f"), [this](bool pressed) {
        handleMortarInteractKey(pressed);
    });
    hotkeys_.addStateWatcher("manual_game_minimap", hotkeyVk("game_minimap", "n"), [this](bool pressed) {
        handleManualGameMinimapToggle(pressed);
    });
    hotkeys_.addStateWatcher("game_menu_escape", VK_ESCAPE, [this](bool pressed) {
        handleEscapeKey(pressed);
    });
    hotkeys_.addStateWatcher("alt", VK_MENU, [this](bool pressed) {
        std::lock_guard lock(state_mutex_);
        alt_pressed_ = pressed;
    });
}

void App::reloadHotkeys() {
    const bool was_running = hotkeys_.isRunning();
    hotkeys_.clear();
    std::lock_guard control_lock(control_mutex_);
    registerHotkeys();
    recoil_->reloadConfig();
    if (was_running) {
        hotkeys_.start();
    }
}

void App::shutdown() {
    {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
    }
    PUBG_HEAP_PROBE("shutdown.enter");
    hotkeys_.stop();
    ++mortar_mount_session_;
    joinMortarMountWorker();
    std::lock_guard control_lock(control_mutex_);
    equipment_detector_->setEnabled(false);
    weapon_detector_->setEnabled(false);
    gesture_identifier_->setEnabled(false);
    recoil_->setEnabled(false);
    minimap_->setEnabled(false);
    elevation_->setEnabled(false);
    large_map_->setDisplay(false);
    if (mortar_auto_aim_) mortar_auto_aim_->shutdown();
    throwables_->setEnabled(false);
    c4_->setEnabled(false);
    special_->shutdown();
    c4_->shutdown();

    status_hud_.reset();
    c4_.reset();
    throwables_.reset();
    mortar_auto_aim_.reset();
    mortar_mount_detector_.reset();
    special_.reset();
    large_map_.reset();
    map_points_.reset();
    recoil_.reset();
    gesture_identifier_.reset();
    equipment_detector_.reset();
    weapon_detector_.reset();
    elevation_.reset();
    minimap_.reset();
    regions_.reset();
    PUBG_HEAP_PROBE("shutdown.afterReset");
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
        {"mortar_auto_aim", {"<f6>"}, "mouse_right"},
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
        if (!hotkeys.contains("mortar_auto_aim") || !hotkeys["mortar_auto_aim"].is_string()) {
            hotkeys["mortar_auto_aim"] = "mouse_right";
            changed = true;
        }
        if (!hotkeys.contains("game_minimap") || !hotkeys["game_minimap"].is_string()) {
            hotkeys["game_minimap"] = "n";
            changed = true;
        }
        if (!hotkeys.contains("hip_aim_key") || !hotkeys["hip_aim_key"].is_string()) {
            hotkeys["hip_aim_key"] = "mouse_right";
            changed = true;
        }
        if (!hotkeys.contains("open_large_map") || !hotkeys["open_large_map"].is_string()) {
            hotkeys["open_large_map"] = "m";
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

void App::loadUiState() {
    const Json ui = config_.read([](const Json& data) {
        return data.value("ui_state", Json::object());
    });
    const Json switches = ui.value("switches", Json::object());
    std::string marker_color;
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection_enabled_ = switches.value("weapon_detection", true);
        display_enabled_ = switches.value("display", false);
        recoil_enabled_ = switches.value("recoil", false);
        if (ui.contains("current_marker_color") && ui["current_marker_color"].is_string()) {
            current_marker_color_ = ui["current_marker_color"].get<std::string>();
        }
        marker_color = current_marker_color_;
        const Json assistants = ui.value("assistants", Json::object());
        for (auto& [key, value] : assistant_enabled_) {
            value = assistants.value(key, value);
        }
    }
    if (throwables_) {
        throwables_->setSelectedColor(marker_color);
    }
    if (c4_) {
        c4_->setSelectedColor(marker_color);
    }
    if (status_hud_) {
        status_hud_->setMarkerColor(marker_color);
    }
    if (recoil_) {
        for (const auto& key : {"auto_recoil", "dmr_tap", "sr_breath", "sg_peek"}) {
            auto it = assistant_enabled_.find(key);
            recoil_->setFeatureEnabled(key, it == assistant_enabled_.end() ? true : it->second);
        }
    }
}

void App::saveSwitchState(const std::string& key, bool enabled) {
    config_.write([&](Json& data) {
        data["ui_state"]["switches"][key] = enabled;
    });
    config_.save();
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
    saveSwitchState("weapon_detection", enabled);
    weapon_detector_->setEnabled(enabled);
    equipment_detector_->setEnabled(enabled);
    gesture_identifier_->setEnabled(enabled);
    if (!enabled) {
        applyWeaponUpdate("", 0.0);
    }
    updateStatusHud();
    printStatus();
}

void App::setDisplayEnabled(bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    std::string current_weapon;
    {
        std::lock_guard lock(state_mutex_);
        display_enabled_ = enabled;
        current_weapon = current_weapon_;
        if (!enabled) {
            special_display_active_ = false;
        }
    }
    if (main_window_) {
        QMetaObject::invokeMethod(main_window_, [this, enabled] { main_window_->setDisplayState(enabled); }, Qt::QueuedConnection);
    }
    saveSwitchState("display", enabled);
    if (!enabled) {
        closeGameMinimapForAssist();
        setSpecialDisplayActive(false);
        map_points_->setEnabled(false);
        throwables_->setEnabled(false);
        c4_->setEnabled(false);
        setMortarMounted(false);
    } else {
        updateSpecialWeaponMinimapState("", current_weapon);
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
    saveSwitchState("recoil", enabled);
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
    config_.write([&](Json& data) {
        data["ui_state"]["current_marker_color"] = color;
    });
    config_.save();
    if (status_hud_) {
        status_hud_->setMarkerColor(color);
    }
}

void App::updateWeaponFromDetectors(const std::string& weapon, double score) {
    if (InputController::isLeftMouseDown()) {
        std::lock_guard lock(state_mutex_);
        pending_weapon_update_ = std::make_pair(weapon, score);
        return;
    }
    applyWeaponUpdate(weapon, score);
}

void App::applyWeaponUpdate(const std::string& weapon, double score) {
    bool recoil_enabled = false;
    std::string previous_weapon;
    bool ignore_empty_special_jitter = false;
    bool leaving_mortar_hold = false;
    {
        std::lock_guard lock(state_mutex_);
        previous_weapon = current_weapon_;
        ignore_empty_special_jitter = weapon.empty() && special_display_active_ &&
                                      shouldAutoUseMinimapForWeapon(previous_weapon);
        if (!ignore_empty_special_jitter) {
            if (hold_mortar_until_reacquired_ && weapon.empty()) {
                pending_weapon_update_.reset();
                return;
            }
            if (hold_mortar_until_reacquired_ && weapon != "Mortar") {
                hold_mortar_until_reacquired_ = false;
                mortar_mounted_ = false;
                leaving_mortar_hold = true;
            }
            current_weapon_ = weapon;
            recoil_enabled = recoil_enabled_;
        }
        pending_weapon_update_.reset();
    }
    if (ignore_empty_special_jitter) {
        std::cout << "[weapon] ignore empty special jitter after " << previous_weapon
                  << " score=" << score << "\n";
        return;
    }
    if (leaving_mortar_hold) {
        ++mortar_mount_session_;
        joinMortarMountWorker();
    }
    if (isRecoilWeapon(weapon)) {
        recoil_->updateCurrentWeapon(weapon);
        if (recoil_enabled) {
            syncRecoilAttachmentsForCurrentWeapon();
        }
    } else if (weapon.empty()) {
        recoil_->updateCurrentWeapon("");
    } else {
        recoil_->updateCurrentWeapon("");
        recoil_->hideSgPeekDirection();
    }
    special_->setCurrentWeapon(weapon);
    if (!weapon.empty()) {
        map_points_->setEnabled(false);
    }
    updateSpecialWeaponMinimapState(previous_weapon, weapon);
    updateAssistantRouting();
    if (status_hud_) {
        status_hud_->setCurrentWeapon(weapon);
        status_hud_->setPeekDirection(recoil_->sgPeekDirection(), recoil_->shouldShowSgPeekDirection());
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
           weapon != "VSS" && weapon != "Crossbow" && weapon != "C4" &&
           weapon != "Mortar";
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
    const auto slot1 = equipment.find(1);
    const auto slot2 = equipment.find(2);
    if (slot1 != equipment.end() && slot2 != equipment.end() &&
        slot1->second.name == current_weapon && slot2->second.name == current_weapon) {
        recoil_->updateAttachments(slot2->second);
        return;
    }
    for (const auto& [_, info] : equipment) {
        if (info.name == current_weapon) {
            recoil_->updateAttachments(info);
            return;
        }
    }
}

void App::setAssistantEnabled(const std::string& key, bool enabled) {
    std::lock_guard control_lock(control_mutex_);
    const bool recoil_feature =
        key == "auto_recoil" || key == "dmr_tap" || key == "sr_breath" || key == "sg_peek";
    std::string current_weapon;
    bool disabling_mortar = false;
    {
        std::lock_guard lock(state_mutex_);
        assistant_enabled_[key] = enabled;
        current_weapon = current_weapon_;
        if (key == "mortar" && !enabled) {
            mortar_mounted_ = false;
            ++mortar_mount_session_;
            disabling_mortar = true;
        }
    }
    config_.write([&](Json& data) {
        data["ui_state"]["assistants"][key] = enabled;
    });
    config_.save();
    if (recoil_feature && recoil_) {
        recoil_->setFeatureEnabled(key, enabled);
    }
    if (disabling_mortar) {
        closeGameMinimapForAssist();
        if (!shouldAutoUseMinimapForWeapon(current_weapon)) {
            setSpecialDisplayActive(false);
        }
    }
    if (recoil_feature) {
        updateStatusHud();
        printStatus();
        return;
    }
    updateSpecialWeaponMinimapState(current_weapon, current_weapon);
    updateAssistantRouting();
}

bool App::shouldAutoUseMinimapForWeapon(const std::string& weapon) const {
    return weapon == "Rocket" || weapon == "Grenade" || weapon == "VSS" ||
           weapon == "Crossbow" || weapon == "C4" || weapon == "Mortar";
}

bool App::shouldRunSpecialAssistantsForWeapon(const std::string& weapon) const {
    return shouldAutoUseMinimapForWeapon(weapon);
}

void App::toggleGameMinimap() {
    const int vk = hotkeyVk("game_minimap", "n");
    if (!vk) {
        return;
    }
    InputController::keyDown(vk);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    InputController::keyUp(vk);
}

bool App::gameMenuInputSuppressed() const {
    std::lock_guard lock(state_mutex_);
    return game_menu_active_ || nowSeconds() < game_menu_input_suppress_until_;
}

bool App::shouldHoldMortarStateAfterEsc() const {
    std::lock_guard lock(state_mutex_);
    return hold_mortar_until_reacquired_;
}

void App::openGameMinimapForAssist() {
    bool should_toggle = false;
    {
        std::lock_guard lock(state_mutex_);
        should_toggle = !game_minimap_opened_by_app_;
        game_minimap_opened_by_app_ = true;
    }
    if (should_toggle) {
        toggleGameMinimap();
    }
}

void App::closeGameMinimapForAssist() {
    bool should_toggle = false;
    {
        std::lock_guard lock(state_mutex_);
        if (game_menu_active_ || nowSeconds() < game_menu_input_suppress_until_) {
            return;
        }
        should_toggle = game_minimap_opened_by_app_;
        game_minimap_opened_by_app_ = false;
    }
    if (should_toggle) {
        toggleGameMinimap();
    }
}

void App::handleManualGameMinimapToggle(bool pressed) {
    if (!pressed) {
        return;
    }
    bool should_close_assist_display = false;
    bool should_open_assist_display = false;
    bool weapon_detection_enabled = false;
    std::string current_weapon;
    {
        std::lock_guard lock(state_mutex_);
        if (nowSeconds() < manual_minimap_toggle_ignore_until_) {
            return;
        }
        should_close_assist_display = special_display_active_;
        weapon_detection_enabled = weapon_detection_enabled_;
        current_weapon = current_weapon_;
        if (should_close_assist_display) {
            game_minimap_opened_by_app_ = false;
            manual_special_display_suppressed_ = true;
            manual_minimap_toggle_ignore_until_ = nowSeconds() + 0.35;
        } else {
            auto enabledFor = [&](const std::string& key) {
                auto it = assistant_enabled_.find(key);
                return it != assistant_enabled_.end() && it->second;
            };
            bool current_allowed = false;
            if (current_weapon_ == "Rocket") current_allowed = enabledFor("rocket");
            else if (current_weapon_ == "Grenade") current_allowed = enabledFor("throwables");
            else if (current_weapon_ == "VSS") current_allowed = enabledFor("vss");
            else if (current_weapon_ == "Crossbow") current_allowed = enabledFor("crossbow");
            else if (current_weapon_ == "C4") current_allowed = enabledFor("c4");
            else if (current_weapon_ == "Mortar") current_allowed = enabledFor("mortar") && mortar_mounted_;
            should_open_assist_display = shouldAutoUseMinimapForWeapon(current_weapon_) && current_allowed;
            if (should_open_assist_display) {
                game_minimap_opened_by_app_ = true;
                manual_special_display_suppressed_ = false;
                manual_minimap_toggle_ignore_until_ = nowSeconds() + 0.35;
            } else {
                game_minimap_opened_by_app_ = false;
            }
        }
    }
    if (should_close_assist_display) {
        setSpecialDisplayActive(false);
        if (weapon_detection_enabled && current_weapon != "Mortar") {
            weapon_detector_->setEnabled(true);
        }
        updateAssistantRouting();
        updateStatusHud();
    } else if (should_open_assist_display) {
        special_->setCurrentWeapon(current_weapon);
        setSpecialDisplayActive(true);
        updateAssistantRouting();
        updateStatusHud();
    }
}

void App::handleEscapeKey(bool pressed) {
    std::lock_guard lock(state_mutex_);
    if (pressed) {
        game_menu_active_ = true;
        game_menu_input_suppress_until_ = nowSeconds() + 2.0;
        if (mortar_mounted_) {
            hold_mortar_until_reacquired_ = true;
        }
    } else {
        game_menu_active_ = false;
        game_menu_input_suppress_until_ = nowSeconds() + 0.45;
    }
}

void App::setSpecialDisplayActive(bool active) {
    bool changed = false;
    {
        std::lock_guard lock(state_mutex_);
        changed = special_display_active_ != active;
        special_display_active_ = active;
    }
    minimap_->setEnabled(active);
    minimap_->setDisplay(active);
    elevation_->setEnabled(active);
    elevation_->setDisplay(active);
    large_map_->setDisplay(active);
    special_->setDisplayEnabled(active);
    if (!active) {
        throwables_->setEnabled(false);
        c4_->setEnabled(false);
    }
    if (changed) {
        updateAssistantRouting();
    }
}

void App::updateSpecialWeaponMinimapState(const std::string& previous_weapon, const std::string& current_weapon) {
    bool display_enabled = false;
    bool current_allowed = false;
    bool previous_special = shouldAutoUseMinimapForWeapon(previous_weapon);
    bool current_special = shouldAutoUseMinimapForWeapon(current_weapon);
    bool current_unknown = current_weapon.empty();
    bool mortar_mounted = false;
    bool special_display_active = false;
    bool manual_suppressed = false;
    {
        std::lock_guard lock(state_mutex_);
        display_enabled = display_enabled_;
        mortar_mounted = mortar_mounted_;
        special_display_active = special_display_active_;
        manual_suppressed = manual_special_display_suppressed_;
        if (!current_special && !current_unknown) {
            manual_special_display_suppressed_ = false;
            manual_minimap_toggle_ignore_until_ = 0.0;
            manual_suppressed = false;
        }
        auto enabledFor = [&](const std::string& key) {
            auto it = assistant_enabled_.find(key);
            return it != assistant_enabled_.end() && it->second;
        };
        if (current_weapon == "Rocket") current_allowed = enabledFor("rocket");
        else if (current_weapon == "Grenade") current_allowed = enabledFor("throwables");
        else if (current_weapon == "VSS") current_allowed = enabledFor("vss");
        else if (current_weapon == "Crossbow") current_allowed = enabledFor("crossbow");
        else if (current_weapon == "C4") current_allowed = enabledFor("c4");
        else if (current_weapon == "Mortar") current_allowed = enabledFor("mortar") && mortar_mounted_;
    }

    if (!display_enabled) {
        closeGameMinimapForAssist();
        setSpecialDisplayActive(false);
        return;
    }

    if (current_special && current_allowed) {
        if (manual_suppressed) {
            return;
        }
        if (!special_display_active) {
            openGameMinimapForAssist();
        }
        setSpecialDisplayActive(true);
        return;
    }

    if (current_unknown && special_display_active) {
        return;
    }

    if (((previous_special && !current_special) || (current_special && !current_allowed)) && !mortar_mounted) {
        closeGameMinimapForAssist();
    }
    if (!mortar_mounted) {
        setSpecialDisplayActive(false);
    }
}

void App::handleMortarInteractKey(bool pressed) {
    if (!pressed) {
        return;
    }
    bool display_enabled = false;
    bool mortar_allowed = false;
    bool mounted = false;
    bool special_display_active = false;
    {
        std::lock_guard lock(state_mutex_);
        display_enabled = display_enabled_;
        mortar_allowed = assistant_enabled_["mortar"];
        mounted = mortar_mounted_;
        special_display_active = special_display_active_;
    }
    if (!display_enabled || !mortar_allowed) {
        return;
    }
    if (mounted) {
        {
            std::lock_guard lock(state_mutex_);
            hold_mortar_until_reacquired_ = false;
        }
        setMortarMounted(false);
        return;
    }
    startMortarMountConfirmation();
}

void App::startMortarMountConfirmation() {
    const std::uint64_t session = ++mortar_mount_session_;
    joinMortarMountWorker();
    {
        std::lock_guard worker_lock(mortar_mount_mutex_);
        mortar_mount_worker_ = std::thread([this, session] {
            const double deadline = nowSeconds() + 0.8;
            bool mounted = false;
            while (nowSeconds() <= deadline && session == mortar_mount_session_.load()) {
                bool display_enabled = false;
                bool mortar_allowed = false;
                {
                    std::lock_guard lock(state_mutex_);
                    display_enabled = display_enabled_;
                    mortar_allowed = assistant_enabled_["mortar"];
                }
                if (!display_enabled || !mortar_allowed || !mortar_mount_detector_) {
                    break;
                }
                if (mortar_mount_detector_->detectMounted()) {
                    mounted = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            if (session != mortar_mount_session_.load()) {
                return;
            }
            applyMortarMounted(mounted);
            if (!mounted) {
                return;
            }

            int missed_frames = 0;
            while (session == mortar_mount_session_.load()) {
                bool display_enabled = false;
                bool mortar_allowed = false;
                bool menu_suppressed = false;
                bool hold_after_esc = false;
                {
                    std::lock_guard lock(state_mutex_);
                    display_enabled = display_enabled_;
                    mortar_allowed = assistant_enabled_["mortar"];
                    menu_suppressed = game_menu_active_ || nowSeconds() < game_menu_input_suppress_until_;
                    hold_after_esc = hold_mortar_until_reacquired_;
                }
                if (!display_enabled || !mortar_allowed || !mortar_mount_detector_) {
                    break;
                }
                if (menu_suppressed) {
                    missed_frames = 0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    continue;
                }
                if (mortar_mount_detector_->detectMounted()) {
                    missed_frames = 0;
                    std::lock_guard lock(state_mutex_);
                    hold_mortar_until_reacquired_ = false;
                } else {
                    if (hold_after_esc) {
                        missed_frames = 0;
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        continue;
                    }
                    ++missed_frames;
                    if (missed_frames >= 2) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            if (session == mortar_mount_session_.load()) {
                if (shouldHoldMortarStateAfterEsc()) {
                    return;
                }
                applyMortarMounted(false);
            }
        });
    }
}

void App::setMortarMounted(bool mounted) {
    ++mortar_mount_session_;
    joinMortarMountWorker();
    applyMortarMounted(mounted);
}

void App::applyMortarMounted(bool mounted) {
    bool display_enabled = false;
    bool mortar_allowed = false;
    std::string current_weapon;
    bool weapon_detection_enabled = false;
    bool was_mortar_weapon = false;
    {
        std::lock_guard lock(state_mutex_);
        was_mortar_weapon = current_weapon_ == "Mortar";
        mortar_mounted_ = mounted;
        display_enabled = display_enabled_;
        current_weapon = current_weapon_;
        weapon_detection_enabled = weapon_detection_enabled_;
        auto it = assistant_enabled_.find("mortar");
        mortar_allowed = it != assistant_enabled_.end() && it->second;
        if (mounted) {
            current_weapon_ = "Mortar";
            hold_mortar_until_reacquired_ = false;
            manual_special_display_suppressed_ = false;
            manual_minimap_toggle_ignore_until_ = 0.0;
            pending_weapon_update_.reset();
        }
    }
    if (mounted && display_enabled && mortar_allowed) {
        weapon_detector_->setEnabled(false);
        c4_->onWeaponDetected("", 0.0);
        recoil_->updateCurrentWeapon("");
        special_->setCurrentWeapon("Mortar");
        if (status_hud_) {
            status_hud_->setCurrentWeapon("Mortar");
        }
        openGameMinimapForAssist();
        setSpecialDisplayActive(true);
    } else if (!mounted && was_mortar_weapon) {
        {
            std::lock_guard lock(state_mutex_);
            current_weapon_.clear();
            pending_weapon_update_.reset();
            hold_mortar_until_reacquired_ = false;
            manual_special_display_suppressed_ = false;
            manual_minimap_toggle_ignore_until_ = 0.0;
        }
        c4_->onWeaponDetected("", 0.0);
        recoil_->updateCurrentWeapon("");
        special_->setCurrentWeapon("");
        if (status_hud_) {
            status_hud_->setCurrentWeapon("");
        }
        closeGameMinimapForAssist();
        setSpecialDisplayActive(false);
        if (weapon_detection_enabled) {
            weapon_detector_->setEnabled(true);
        }
    } else if (!mounted && !shouldAutoUseMinimapForWeapon(current_weapon)) {
        closeGameMinimapForAssist();
        setSpecialDisplayActive(false);
        if (weapon_detection_enabled) {
            weapon_detector_->setEnabled(true);
        }
    }
    updateAssistantRouting();
}

void App::joinMortarMountWorker() {
    std::thread worker;
    {
        std::lock_guard worker_lock(mortar_mount_mutex_);
        if (!mortar_mount_worker_.joinable()) {
            return;
        }
        if (mortar_mount_worker_.get_id() == std::this_thread::get_id()) {
            return;
        }
        worker = std::move(mortar_mount_worker_);
    }
    worker.join();
}

bool App::shouldShowMarkerIndicator() const {
    std::lock_guard lock(state_mutex_);
    return display_enabled_;
}

void App::updateAssistantRouting() {
    bool display_enabled = false;
    std::string current_weapon;
    std::unordered_map<std::string, bool> assistant_enabled;
    bool mortar_mounted = false;
    bool special_display_active = false;
    {
        std::lock_guard lock(state_mutex_);
        display_enabled = display_enabled_;
        current_weapon = current_weapon_;
        assistant_enabled = assistant_enabled_;
        mortar_mounted = mortar_mounted_;
        special_display_active = special_display_active_;
    }

    if (!display_enabled || !special_display_active) {
        special_->setManualEnabled("mortar", false);
        special_->setManualEnabled("rocket", false);
        special_->setManualEnabled("vss", false);
        special_->setManualEnabled("crossbow", false);
        throwables_->setEnabled(false);
        c4_->setEnabled(false);
        if (status_hud_) status_hud_->setMarkerIndicatorVisible(false);
        return;
    }

    special_->setManualEnabled("mortar", assistant_enabled["mortar"] && mortar_mounted);
    special_->setManualEnabled("rocket", assistant_enabled["rocket"] && current_weapon == "Rocket");
    special_->setManualEnabled("vss", assistant_enabled["vss"] && current_weapon == "VSS");
    special_->setManualEnabled("crossbow", assistant_enabled["crossbow"] && current_weapon == "Crossbow");

    const bool throwables_on = assistant_enabled["throwables"] && current_weapon == "Grenade";
    const bool c4_on = assistant_enabled["c4"] && current_weapon == "C4";
    throwables_->setEnabled(throwables_on);
    c4_->setEnabled(c4_on);
    if (status_hud_) {
        status_hud_->setMarkerIndicatorVisible(display_enabled && special_display_active);
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
    status_hud_->setPeekDirection(recoil_ ? recoil_->sgPeekDirection() : 0,
                                  recoil_ && recoil_->shouldShowSgPeekDirection());
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
    std::cout << "F1 weapon detection, F2 display, F3 recoil, configured equipment scan key.\n";
    PUBG_HEAP_PROBE("run.start");
    syncMarkerColorsFromConfig();
    bool initial_weapon_detection = false;
    bool initial_display = false;
    bool initial_recoil = false;
    {
        std::lock_guard lock(state_mutex_);
        initial_weapon_detection = weapon_detection_enabled_;
        initial_display = display_enabled_;
        initial_recoil = recoil_enabled_;
    }
    setWeaponDetectionEnabled(initial_weapon_detection);
    setDisplayEnabled(initial_display);
    setRecoilEnabled(initial_recoil);
    PUBG_HEAP_PROBE("run.afterEnableDetection");
    pubg::ui::MainWindow::ControlCallbacks callbacks;
    callbacks.set_weapon_detection = [this](bool enabled) { setWeaponDetectionEnabled(enabled); };
    callbacks.set_display = [this](bool enabled) { setDisplayEnabled(enabled); };
    callbacks.set_recoil = [this](bool enabled) { setRecoilEnabled(enabled); };
    callbacks.set_assistant = [this](const std::string& key, bool enabled) { setAssistantEnabled(key, enabled); };
    callbacks.sync_marker_colors = [this] { syncMarkerColorsFromConfig(); };
    callbacks.refresh_status_hud = [this] { updateStatusHud(); };
    callbacks.reload_hotkeys = [this] { reloadHotkeys(); };
    callbacks.shutdown = [this] { shutdown(); };
    pubg::ui::MainWindow window(config_, *regions_, *minimap_, *elevation_, *weapon_detector_,
                                *equipment_detector_, *gesture_identifier_, *recoil_, *special_,
                                *map_points_, *large_map_, *throwables_, *c4_, std::move(callbacks));
    main_window_ = &window;
    if (auto* screen = QGuiApplication::primaryScreen()) {
        window.move(screen->availableGeometry().topLeft());
    } else {
        window.move(0, 0);
    }
    window.show();
    hotkeys_.start();
    const int code = QApplication::exec();
    main_window_ = nullptr;
    shutdown();
    return code;
}

} // namespace pubg
