#include "App.hpp"

#include "AiAimCoordinatorProduction.hpp"
#include "BuildConfig.hpp"
#include "EquipmentSelection.hpp"
#include "PrimaryHotkeys.hpp"
#include "ShutdownSequence.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <utility>
#include <vector>
#include <QApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include "ui/Theme.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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

namespace {

Json screenSnapshot() {
    Json screens = Json::array();
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect g = screen->geometry();
        const double dpr = screen->devicePixelRatio();
        screens.push_back(Json{
            {"name", screen->name().toStdString()},
            {"left", static_cast<int>(std::lround(g.left() * dpr))},
            {"top", static_cast<int>(std::lround(g.top() * dpr))},
            {"width", std::max(1, static_cast<int>(std::lround(g.width() * dpr)))},
            {"height", std::max(1, static_cast<int>(std::lround(g.height() * dpr)))},
        });
    }
    return screens;
}

bool sameScreenSnapshot(const Json& a, const Json& b) {
    return a.dump() == b.dump();
}

std::vector<std::pair<int, int>> existingRegionProfiles(const ResourcePaths& paths) {
    std::vector<std::pair<int, int>> profiles;
    std::error_code ec;
    const auto dir = paths.regionConfigDir();
    if (!std::filesystem::exists(dir, ec)) {
        return profiles;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        const auto underscore = filename.find('_');
        const auto dot = filename.rfind(".json");
        if (underscore == std::string::npos || dot == std::string::npos || underscore >= dot) {
            continue;
        }
        try {
            const int profile_w = std::stoi(filename.substr(0, underscore));
            const int profile_h = std::stoi(filename.substr(underscore + 1, dot - underscore - 1));
            if (profile_w > 0 && profile_h > 0) {
                profiles.emplace_back(profile_w, profile_h);
            }
        } catch (...) {
        }
    }
    return profiles;
}

std::pair<int, int> snapToExistingRegionProfile(int width, int height, const ResourcePaths& paths) {
    const auto profiles = existingRegionProfiles(paths);
    if (profiles.empty()) {
        return {width, height};
    }

    auto hasProfile = [&](int w, int h) {
        return std::find(profiles.begin(), profiles.end(), std::make_pair(w, h)) != profiles.end();
    };

    // Qt/DPI occasionally reports 1921x1080 for a physical 1920x1080 monitor.
    // Prefer the adjacent even profile so a stale 1921_1080.json cannot hijack loading.
    if ((width % 2) != 0 && hasProfile(width - 1, height)) {
        return {width - 1, height};
    }
    if ((height % 2) != 0 && hasProfile(width, height - 1)) {
        return {width, height - 1};
    }
    if (hasProfile(width, height)) {
        return {width, height};
    }

    std::pair<int, int> best{width, height};
    int best_distance = 3;
    for (const auto& profile : profiles) {
        const int distance = std::abs(profile.first - width) + std::abs(profile.second - height);
        if (std::abs(profile.first - width) <= 1 && std::abs(profile.second - height) <= 1 &&
            distance < best_distance) {
            best = profile;
            best_distance = distance;
        }
    }
    if (best_distance <= 2) {
        return best;
    }
    return {width, height};
}

RegionManager::DisplayInfo displayInfoFromScreen(QScreen* screen) {
    if (!screen) {
        return {};
    }
    const QRect g = screen->geometry();
    const double dpr = screen->devicePixelRatio();
    RegionManager::DisplayInfo display{
        static_cast<int>(std::lround(g.left() * dpr)),
        static_cast<int>(std::lround(g.top() * dpr)),
        std::max(1, static_cast<int>(std::lround(g.width() * dpr))),
        std::max(1, static_cast<int>(std::lround(g.height() * dpr))),
        g.left(),
        g.top(),
        g.width(),
        g.height(),
        dpr,
        screen->name().toStdString(),
    };

#ifdef _WIN32
    const std::wstring device_name = screen->name().toStdWString();
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!device_name.empty() && EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        display.left = mode.dmPosition.x;
        display.top = mode.dmPosition.y;
        display.width = std::max(1, static_cast<int>(mode.dmPelsWidth));
        display.height = std::max(1, static_cast<int>(mode.dmPelsHeight));
        if (g.width() > 0) {
            display.device_pixel_ratio = static_cast<double>(display.width) / static_cast<double>(g.width());
        }
    }
#endif

    return display;
}

} // namespace

App::App()
    : paths_(),
      config_(paths_) {
    PUBG_HEAP_PROBE("ctor.start");
    config_.load();
    PUBG_HEAP_PROBE("ctor.afterConfigLoad");
    const auto display = resolveGameDisplay();
    config_.loadRegionProfile(display.width, display.height);
    migrateLegacyDefaultHotkeys();
    PUBG_HEAP_PROBE("ctor.afterHotkeyMigrate");
    config_.save();
    ai_aim_settings_ = config_.aiAimSettings();
    regions_ = std::make_unique<RegionManager>(config_, display);
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
    mouse_motion_dispatcher_ = makeMouseMotionDispatcher();
    recoil_ = std::make_unique<RecoilControl>(config_, regions_.get(), mouse_motion_dispatcher_);
    PUBG_HEAP_PROBE("ctor.afterRecoil");
    large_map_ = std::make_unique<LargeMapRadar>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterLargeMap");
    special_ = std::make_unique<SpecialAssistants>(config_, *regions_, *minimap_, *elevation_, *large_map_);
    PUBG_HEAP_PROBE("ctor.afterSpecial");
    map_points_ = std::make_unique<MapPointAssistant>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterMapPoints");
    mortar_auto_aim_ = std::make_unique<MortarAutoAim>(config_, *regions_, *minimap_, *large_map_, *elevation_,
        mouse_motion_dispatcher_,
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
    throwables_->setPhysicalKeyQuery([this](int vk) {
        return hotkeys_.physicalKeyDown(vk);
    });
    PUBG_HEAP_PROBE("ctor.afterThrowables");
    c4_ = std::make_unique<C4Assistant>(config_, *regions_, *minimap_);
    PUBG_HEAP_PROBE("ctor.afterC4");
    status_hud_ = std::make_unique<StatusHud>(config_, *regions_);
    PUBG_HEAP_PROBE("ctor.afterStatusHud");
    ai_aim_ = makeProductionAiAimCoordinator(
        paths_, ai_aim_settings_, *regions_, mouse_motion_dispatcher_);
    ai_aim_->setStatusCallback([this](const AiAimRuntimeStatus& status) {
        handleAiAimStatus(status);
    });
    PUBG_HEAP_PROBE("ctor.afterAiAim");
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

App::~App() noexcept {
    shutdown();
}

void App::wireCallbacks() {
    weapon_detector_->setEnabled(false, [this](const std::string& weapon, double score) {
        if (shutting_down_.load(std::memory_order_acquire)) return;
        c4_->onWeaponDetected(weapon, score);
        updateWeaponFromDetectors(weapon, score);
    });
    equipment_detector_->setEnabled(false, [this](const std::map<int, WeaponSlotInfo>& equipment_slots) {
        if (shutting_down_.load(std::memory_order_acquire)) return;
        updateEquipment(equipment_slots);
    });
    equipment_detector_->setStatusCallback([this](const std::string& status) {
        if (shutting_down_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(state_mutex_);
            equipment_ui_open_ = status != "closed";
        }
        if (status_hud_) {
            status_hud_->setEquipmentStatus(status);
        }
        publishAiActivation(hotkeys_.isGameForeground());
    });
    gesture_identifier_->setEnabled(false, [this](const std::string& stance, double) {
        if (shutting_down_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(state_mutex_);
            current_stance_ = stance;
        }
        recoil_->updateStance(stance);
        if (status_hud_) {
            status_hud_->setStance(stance);
        }
        publishAiCombatContext();
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
    const auto configured_hotkeys = config_.hotkeys();
    auto addPrimaryHotkey = [&](const char* action, auto callback) {
        if (configured_hotkeys.contains(action)) {
            hotkeys_.addHotkey(action, hotkeyCombo(action, ""), std::move(callback));
        }
    };
    addPrimaryHotkey("toggle_weapon_detection", [this] { toggleWeaponDetection(); });
    addPrimaryHotkey("toggle_display", [this] { toggleDisplay(); });
#if PUBG_ENABLE_INPUT_CONTROL
    addPrimaryHotkey("toggle_recoil", [this] { toggleRecoil(); });
    addPrimaryHotkey("toggle_ai_aim", [this] { toggleAiAim(); });
#endif
    addPrimaryHotkey("measure_map", [this] {
        handleMeasureMapHotkey();
    });
    hotkeys_.addStateWatcher("mortar_auto_aim", hotkeyVk("mortar_auto_aim", "mouse_right"), [this](bool pressed) {
        if (!pressed) {
            ++mortar_auto_aim_hold_session_;
            return;
        }
        const auto session = ++mortar_auto_aim_hold_session_;
        joinMortarAutoAimHoldWorker();
        const int hold_ms = config_.read([](const Json& data) {
            return data.value("mortar_config", Json::object()).value("direction_auto_aim_hold_ms", 180);
        });
        {
            std::lock_guard worker_lock(mortar_auto_aim_hold_mutex_);
            mortar_auto_aim_hold_worker_ = std::thread([this, session, hold_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, hold_ms)));
                if (session != mortar_auto_aim_hold_session_.load()) {
                    return;
                }
                bool can_trigger = false;
                {
                    std::lock_guard lock(state_mutex_);
                    can_trigger = !shutting_down_.load(std::memory_order_acquire) &&
                                  display_enabled_ && mortar_mounted_;
                }
                if (can_trigger && mortar_auto_aim_) {
                    mortar_auto_aim_->trigger(currentMarkerColor());
                }
            });
        }
    });
    hotkeys_.addHotkey("toggle_window", hotkeyCombo("toggle_window", "<home>"), [this] {
        if (auto* window = main_window_.load(std::memory_order_acquire)) {
            QMetaObject::invokeMethod(window, [this] {
                if (auto* current = main_window_.load(std::memory_order_acquire)) {
                    current->toggleWindowVisible();
                }
            }, Qt::QueuedConnection);
        }
    });
    hotkeys_.addHotkey("toggle_equipment", hotkeyCombo("toggle_equipment", ""), [this] {
        const bool enabled = [this] {
            std::lock_guard lock(state_mutex_);
            return weapon_detection_enabled_;
        }();
        holdMortarStateForBlockingUi();
        if (enabled) {
            equipment_detector_->requestEquipmentConfirmation();
        }
    });
    hotkeys_.addHotkey("marker_prev", hotkeyCombo("marker_prev", "<up>"), [this] { cycleMarkerColor(-1); });
    hotkeys_.addHotkey("marker_next", hotkeyCombo("marker_next", "<down>"), [this] { cycleMarkerColor(1); });
    hotkeys_.addStateWatcher("sg_peek_left", InputController::parseVirtualKey("q"), [this](bool pressed) {
        {
            std::lock_guard lock(state_mutex_);
            lean_left_pressed_ = pressed;
        }
        if (pressed && recoil_) {
            recoil_->setSgPeekDirection(1);
            updateStatusHud();
        }
        publishAiCombatContext();
    });
    hotkeys_.addStateWatcher("sg_peek_right", InputController::parseVirtualKey("e"), [this](bool pressed) {
        {
            std::lock_guard lock(state_mutex_);
            lean_right_pressed_ = pressed;
        }
        if (pressed && recoil_) {
            recoil_->setSgPeekDirection(2);
            updateStatusHud();
        }
        publishAiCombatContext();
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
            show_map_points = left_pressed_ && middle_pressed_;
            weapon_detection = weapon_detection_enabled_;
            if (!pressed) {
                pending_weapon = pending_weapon_update_;
                pending_weapon_update_.reset();
            }
        }
        if (show_map_points) {
            show_map_points = canShowMapPointsNow();
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
        publishAiCombatContext();
    });
    hotkeys_.addStateWatcher("middle_mouse", VK_MBUTTON, [this](bool pressed) {
        bool show_map_points = false;
        {
            std::lock_guard lock(state_mutex_);
            middle_pressed_ = pressed;
            show_map_points = left_pressed_ && middle_pressed_;
        }
        if (show_map_points) {
            show_map_points = canShowMapPointsNow();
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
        publishAiCombatContext();
        publishAiActivation(hotkeys_.isGameForeground());
    });
    hotkeys_.addStateWatcher("ai_aim_hold", hotkeyVk("ai_aim_hold", "mouse_right"), [this](bool pressed) {
        {
            std::lock_guard lock(state_mutex_);
            ai_aim_hold_pressed_ = pressed;
        }
        publishAiActivation(hotkeys_.isGameForeground());
    });
    hotkeys_.addStateWatcher("cancel_map_helpers", hotkeyVk("open_large_map", "m"), [this](bool pressed) {
        if (!pressed) {
            return;
        }
        holdMortarStateForBlockingUi();
        if (large_map_) {
            large_map_->cancel();
        }
        if (map_points_) {
            map_points_->setEnabled(false);
        }
        {
            std::lock_guard lock(state_mutex_);
            large_map_open_ = !large_map_open_;
        }
        publishAiActivation(hotkeys_.isGameForeground());
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

void App::requireOwnerThread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_id_) {
        // Qt UI ownership and App-owned worker joins require a single owner.
        // Continuing teardown from a worker would permit self-join/UAF paths.
        std::terminate();
    }
}

void App::stopRuntimeForShutdown() noexcept {
    requireOwnerThread();
    {
        std::lock_guard lock(state_mutex_);
        if (!runtime_shutdown_.begin()) {
            return;
        }
        shutting_down_.store(true, std::memory_order_release);
    }
    PUBG_HEAP_PROBE("shutdown.enter");
    ShutdownSequence shutdown;
    shutdown.runMandatory([this] { hotkeys_.stop(); });
    shutdown.runMandatory([this] { stopAiStateMonitor(); });
    if (ai_aim_) {
        shutdown.runMandatory([this] { ai_aim_->setEnabled(false); });
        shutdown.runMandatory([this] { ai_aim_->shutdown(); });
    }
    ++mortar_auto_aim_hold_session_;
    shutdown.runMandatory([this] { joinMortarAutoAimHoldWorker(); });
    ++mortar_mount_session_;
    shutdown.runMandatory([this] { joinMortarMountWorker(); });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (equipment_detector_) equipment_detector_->setEnabled(false);
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (weapon_detector_) weapon_detector_->setEnabled(false);
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (gesture_identifier_) gesture_identifier_->setEnabled(false);
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (recoil_) recoil_->shutdown();
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (minimap_) {
            minimap_->setEnabled(false);
            minimap_->setDisplay(false);
        }
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (elevation_) {
            elevation_->setEnabled(false);
            elevation_->setDisplay(false);
        }
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (large_map_) large_map_->setDisplay(false);
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (mortar_auto_aim_) mortar_auto_aim_->shutdown();
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (throwables_) throwables_->shutdown();
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (c4_) c4_->setEnabled(false);
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (special_) special_->shutdown();
    });
    shutdown.runMandatory([this] {
        std::lock_guard control_lock(control_mutex_);
        if (c4_) c4_->shutdown();
    });
    if (mouse_motion_dispatcher_) {
        shutdown.runMandatory([this] { mouse_motion_dispatcher_->shutdown(); });
    }
    if (!shutdown.succeeded()) {
        // Releasing App state after an unproven join would permit callbacks to
        // touch destroyed dependencies. This invariant failure is not safely
        // recoverable at the orchestration layer.
        std::terminate();
    }
    {
        std::lock_guard lock(state_mutex_);
        runtime_shutdown_.markStopped();
    }
    PUBG_HEAP_PROBE("shutdown.afterStop");
}

void App::prepareForUiClose() {
    stopRuntimeForShutdown();
}

void App::shutdown() noexcept {
    requireOwnerThread();
    stopRuntimeForShutdown();
    {
        std::lock_guard lock(state_mutex_);
        if (!runtime_shutdown_.stopped()) {
            // A same-thread re-entrant call during mandatory quiescence must
            // not release dependencies owned by the outer stop sequence.
            return;
        }
        if (resources_released_) {
            return;
        }
        resources_released_ = true;
    }

    ai_aim_.reset();
    status_hud_.reset();
    c4_.reset();
    throwables_.reset();
    mortar_auto_aim_.reset();
    mortar_mount_detector_.reset();
    special_.reset();
    large_map_.reset();
    map_points_.reset();
    recoil_.reset();
    mouse_motion_dispatcher_.reset();
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
    bool changed = false;
    config_.write([&](Json& data) {
        if (!data.contains("hotkeys") || !data["hotkeys"].is_object()) {
            data["hotkeys"] = Json::object();
            changed = true;
        }
        auto& hotkeys = data["hotkeys"];

        HotkeyStringMap current_hotkeys;
        for (auto it = hotkeys.begin(); it != hotkeys.end(); ++it) {
            if (it.value().is_string()) {
                current_hotkeys.emplace(it.key(), it.value().get<std::string>());
            }
        }
        const auto configured = withAvailablePrimaryHotkeyDefaults(current_hotkeys);
        for (const auto& binding : kPrimaryHotkeyDefaults) {
            const std::string action(binding.action);
            const auto planned = configured.find(binding.action);
            if (planned == configured.end()) {
                continue;
            }
            const auto current = hotkeys.find(action);
            if (current == hotkeys.end() || !current->is_string() ||
                current->get<std::string>() != planned->second) {
                hotkeys[action] = planned->second;
                changed = true;
            }
        }

        if (!hotkeys.contains("sprint_key") || !hotkeys["sprint_key"].is_string()) {
            hotkeys["sprint_key"] = "shift";
            changed = true;
        }
        if (!hotkeys.contains("toggle_window") || !hotkeys["toggle_window"].is_string()) {
            hotkeys["toggle_window"] = "<home>";
            changed = true;
        }
        if (!hotkeys.contains("mortar_auto_aim") || !hotkeys["mortar_auto_aim"].is_string()) {
            hotkeys["mortar_auto_aim"] = "mouse_right";
            changed = true;
        } else if (hotkeys["mortar_auto_aim"].get<std::string>() == "<f6>") {
            hotkeys["mortar_auto_aim"] = "mouse_right";
            changed = true;
        }
        if (!hotkeys.contains("ai_aim_hold") || !hotkeys["ai_aim_hold"].is_string()) {
            hotkeys["ai_aim_hold"] = "mouse_right";
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
        auto hotkeyOr = [&](const char* action, const char* fallback) {
            const auto value = hotkeys.find(action);
            return value != hotkeys.end() && value->is_string()
                ? value->get<std::string>()
                : std::string(fallback);
        };
        if (hotkeyOr("fire_key", "end") == hotkeyOr("toggle_window", "<home>")) {
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
        // AI 瞄准每次进程启动均要求用户重新手动开启，不恢复上次运行态。
        ai_aim_enabled_ = false;
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
    if (!enabled) {
        const bool disable_ai = [this] {
            std::lock_guard lock(state_mutex_);
            return ai_aim_enabled_;
        }();
        if (disable_ai) {
            setAiAimEnabled(false);
        }
    }
    std::lock_guard control_lock(control_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection_enabled_ = enabled;
    }
    if (auto* window = main_window_.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(window, [this, enabled] {
            if (auto* current = main_window_.load(std::memory_order_acquire)) {
                current->setWeaponDetectionState(enabled);
            }
        }, Qt::QueuedConnection);
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
    if (auto* window = main_window_.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(window, [this, enabled] {
            if (auto* current = main_window_.load(std::memory_order_acquire)) {
                current->setDisplayState(enabled);
            }
        }, Qt::QueuedConnection);
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
    if (auto* window = main_window_.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(window, [this, enabled] {
            if (auto* current = main_window_.load(std::memory_order_acquire)) {
                current->setRecoilState(enabled);
            }
        }, Qt::QueuedConnection);
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

void App::setAiAimEnabled(bool enabled) {
#if !PUBG_ENABLE_INPUT_CONTROL
    enabled = false;
#endif
    if (enabled) {
        const bool recognition_enabled = [this] {
            std::lock_guard lock(state_mutex_);
            return weapon_detection_enabled_;
        }();
        if (!recognition_enabled) {
            setWeaponDetectionEnabled(true);
        }
    }

    std::lock_guard control_lock(control_mutex_);
    std::lock_guard status_lock(ai_aim_status_mutex_);
    bool session_changed = false;
    std::uint64_t session_revision = 0;
    {
        std::lock_guard lock(state_mutex_);
        session_changed = ai_aim_enabled_ != enabled;
        if (session_changed) {
            ++ai_aim_session_revision_;
        }
        session_revision = ai_aim_session_revision_;
        ai_aim_enabled_ = enabled;
        if (!enabled) {
            ai_aim_hold_pressed_ = false;
            ai_aim_runtime_error_.clear();
        }
    }
    if (session_changed && status_hud_) {
        status_hud_->clearTemporaryMessageWithPrefix("AI瞄准");
    }
    if (auto* window = main_window_.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(window, [this, enabled, session_revision] {
            bool current = false;
            {
                std::lock_guard lock(state_mutex_);
                current = ai_aim_session_revision_ == session_revision &&
                          ai_aim_enabled_ == enabled;
            }
            if (current) {
                if (auto* main_window = main_window_.load(std::memory_order_acquire)) {
                    main_window->setAiAimState(enabled);
                }
            }
        }, Qt::QueuedConnection);
    }

    // 运行态不持久化为 true；下次启动仍要求用户主动确认。
    saveSwitchState("ai_aim", false);
    if (ai_aim_) {
        ai_aim_->setEnabled(enabled);
    }
    publishAiCombatContext();
    publishAiActivation(hotkeys_.isGameForeground());
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

void App::toggleAiAim() {
    const bool next = [this] {
        std::lock_guard lock(state_mutex_);
        return !ai_aim_enabled_;
    }();
    setAiAimEnabled(next);
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
        current_weapon_icon_visible_ = !weapon.empty();
        pending_weapon_update_ = std::make_pair(weapon, score);
        return;
    }
    applyWeaponUpdate(weapon, score);
}

void App::applyWeaponUpdate(const std::string& weapon, double score) {
    bool recoil_enabled = false;
    std::string previous_weapon;
    bool ignore_empty_special_jitter = false;
    {
        std::lock_guard lock(state_mutex_);
        previous_weapon = current_weapon_;
        current_weapon_icon_visible_ = !weapon.empty();
        ignore_empty_special_jitter = weapon.empty() && special_display_active_ &&
                                      shouldAutoUseMinimapForWeapon(previous_weapon);
        if (!ignore_empty_special_jitter) {
            if (hold_mortar_until_reacquired_ && mortar_mounted_ && weapon != "Mortar") {
                pending_weapon_update_.reset();
                return;
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
    publishAiCombatContext();
    publishAiActivation(hotkeys_.isGameForeground());
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
    publishAiCombatContext();
}

bool App::isRecoilWeapon(const std::string& weapon) const {
    return !weapon.empty() && !shouldAutoUseMinimapForWeapon(weapon);
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
    if (const auto matched = recognizedEquipmentForWeapon(
            equipment, current_weapon)) {
        recoil_->updateAttachments(*matched);
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
    return game_menu_state_.active() || nowSeconds() < game_menu_input_suppress_until_;
}

GameMenuStateTracker::KnownUiState App::knownGameUiStateLocked() const noexcept {
    return {
        equipment_ui_open_,
        large_map_open_,
        manual_game_minimap_open_,
        game_minimap_opened_by_app_,
    };
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
        if (game_menu_state_.active() || nowSeconds() < game_menu_input_suppress_until_) {
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
        manual_game_minimap_open_ = !manual_game_minimap_open_;
        const double now = nowSeconds();
        if (manual_minimap_toggle_ignore_until_ > 0.0 && now < manual_minimap_toggle_ignore_until_) {
            // A quick second press of the in-game minimap key should leave the
            // game minimap where it was, while the assistant display changes
            // only once for this pair of key presses.
            manual_minimap_toggle_ignore_until_ = 0.0;
            return;
        }
        should_close_assist_display = special_display_active_;
        weapon_detection_enabled = weapon_detection_enabled_;
        current_weapon = current_weapon_;
        if (should_close_assist_display) {
            game_minimap_opened_by_app_ = false;
            manual_special_display_suppressed_ = true;
            manual_minimap_toggle_ignore_until_ = now + 0.32;
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
                manual_minimap_toggle_ignore_until_ = now + 0.32;
            } else {
                game_minimap_opened_by_app_ = false;
                manual_minimap_toggle_ignore_until_ = 0.0;
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
    publishAiActivation(hotkeys_.isGameForeground());
}

void App::handleEscapeKey(bool pressed) {
    if (pressed) {
        bool should_close_assist_display = false;
        bool should_resume_weapon_detection = false;
        {
            std::lock_guard lock(state_mutex_);
            const auto transition =
                game_menu_state_.onEscapePressed(knownGameUiStateLocked());
            if (transition.close_app_minimap) {
                game_minimap_opened_by_app_ = false;
                manual_special_display_suppressed_ = true;
                manual_minimap_toggle_ignore_until_ = 0.0;
                should_close_assist_display = special_display_active_;
                should_resume_weapon_detection =
                    weapon_detection_enabled_ && current_weapon_ != "Mortar";
            }
            large_map_open_ = false;
            manual_game_minimap_open_ = false;
        }
        if (should_close_assist_display) {
            setSpecialDisplayActive(false);
            if (should_resume_weapon_detection) {
                weapon_detector_->setEnabled(true);
            }
            updateStatusHud();
        }
        holdMortarStateForBlockingUi();
    } else {
        std::lock_guard lock(state_mutex_);
        game_menu_input_suppress_until_ = nowSeconds() + 0.45;
    }
    publishAiActivation(hotkeys_.isGameForeground());
}

void App::holdMortarStateForBlockingUi() {
    std::lock_guard lock(state_mutex_);
    if (!mortar_mounted_ && current_weapon_ != "Mortar") {
        return;
    }
    mortar_mounted_ = true;
    current_weapon_ = "Mortar";
    current_weapon_icon_visible_ = true;
    hold_mortar_until_reacquired_ = true;
    game_menu_input_suppress_until_ = std::max(game_menu_input_suppress_until_, nowSeconds() + 2.0);
}

void App::handleMeasureMapHotkey() {
    const bool display_enabled = [this] {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }
        return display_enabled_;
    }();
    if (!display_enabled) {
        return;
    }
    holdMortarStateForBlockingUi();

    const double now = nowSeconds();
    bool double_click = false;
    {
        std::lock_guard lock(state_mutex_);
        double_click = last_measure_map_hotkey_time_ > 0.0 && now - last_measure_map_hotkey_time_ <= 0.35;
        last_measure_map_hotkey_time_ = double_click ? 0.0 : now;
    }
    large_map_->setDisplay(true);
    if (double_click) {
        large_map_->startManualMeasure();
        return;
    }
    large_map_->toggleMode();
}

void App::setSpecialDisplayActive(bool active) {
    bool changed = false;
    {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_.load(std::memory_order_acquire) && active) {
            return;
        }
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
        if (shutting_down_.load(std::memory_order_acquire)) {
            return;
        }
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
            if (hold_mortar_until_reacquired_) {
                return;
            }
            hold_mortar_until_reacquired_ = false;
        }
        setMortarMounted(false);
        return;
    }
    startMortarMountConfirmation();
}

void App::joinMortarAutoAimHoldWorker() {
    std::thread worker;
    {
        std::lock_guard worker_lock(mortar_auto_aim_hold_mutex_);
        if (!mortar_auto_aim_hold_worker_.joinable()) {
            return;
        }
        if (mortar_auto_aim_hold_worker_.get_id() == std::this_thread::get_id()) {
            return;
        }
        worker = std::move(mortar_auto_aim_hold_worker_);
    }
    worker.join();
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
                    menu_suppressed = game_menu_state_.active() ||
                                      nowSeconds() < game_menu_input_suppress_until_;
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
            current_weapon_icon_visible_ = true;
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
            current_weapon_icon_visible_ = false;
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
    publishAiCombatContext();
    publishAiActivation(hotkeys_.isGameForeground());
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

bool App::canShowMapPointsNow() const {
    {
        std::lock_guard lock(state_mutex_);
        if (mortar_mounted_) {
            return false;
        }
    }
    if (!weapon_detector_) {
        return true;
    }
    const auto [weapon, score] = weapon_detector_->currentWeapon();
    return weapon.empty();
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

bool App::isAiAimWeapon(const std::string& weapon) const {
    if (weapon == "VSS") {
        return true;
    }
    if (!isRecoilWeapon(weapon)) {
        return false;
    }
    return recoil_ && recoil_->supportsWeapon(weapon);
}

bool App::applyAiAimSettings(const AiAimSettings& settings, std::string& error) {
    requireOwnerThread();
    std::lock_guard control_lock(control_mutex_);
    std::lock_guard status_lock(ai_aim_status_mutex_);
    if (!settings.valid() || shutting_down_.load(std::memory_order_acquire) || !ai_aim_) {
        error = settings.valid() ? "程序正在关闭或 AI 模块不可用" : settings.validation_error;
        return false;
    }
    // Context/activation publishers use the same status lock, so no stale host
    // activation can reopen output between algorithm and high-zoom policy updates.
    ai_aim_->updateActivation({});
    try {
        if (!applyProductionAiAimSettings(*ai_aim_, paths_, ai_aim_settings_, settings, *regions_)) {
            error = "AI 模块正在关闭";
            return false;
        }
        {
            std::lock_guard lock(state_mutex_);
            ai_aim_settings_ = settings;
            ++ai_aim_session_revision_;
            ai_aim_runtime_error_.clear();
            if (status_hud_) {
                status_hud_->setAiSnapshot({}, settings.display.target_boxes,
                    settings.algorithm.tracking.max_frame_age_ms);
            }
        }
        publishAiCombatContext();
        publishAiActivation(hotkeys_.isGameForeground());
    } catch (const std::exception& exception) {
        error = exception.what();
        return false; // Output remains fail-closed until the host refreshes state.
    }
    return true;
}

void App::publishAiCombatContext() {
    std::lock_guard status_lock(ai_aim_status_mutex_);
    if (!ai_aim_) {
        return;
    }

    AiAimHostCombatSnapshot host;
    std::uint64_t source_revision = 0;
    {
        std::lock_guard lock(state_mutex_);
        host.weapon = current_weapon_;
        host.stance = current_stance_;
        host.equipment = equipment_;
        host.firing = left_pressed_;
        host.ads = right_pressed_;
        host.lean_left = lean_left_pressed_;
        host.lean_right = lean_right_pressed_;
        ai_context_valid_ = false;
        source_revision = ++ai_context_source_revision_;
    }

    host.weapon_supported = isAiAimWeapon(host.weapon);
    host.disable_variable_high_zoom = !ai_aim_settings_.valid() ||
        ai_aim_settings_.activation.disable_variable_high_zoom;
    auto context = buildAiAimCombatContext(host);
    const bool context_valid = !context.weapon.empty();
    ai_aim_->updateCombatContext(std::move(context), source_revision);
    {
        std::lock_guard lock(state_mutex_);
        if (source_revision == ai_context_source_revision_) {
            ai_context_valid_ = context_valid;
        }
    }
}

void App::publishAiActivation(bool game_foreground) {
    std::lock_guard status_lock(ai_aim_status_mutex_);
    if (!ai_aim_) {
        return;
    }

    bool enabled = false;
    bool aim_hold = false;
    bool blocking_ui = false;
    bool paused = false;
    bool context_valid = false;
    std::string weapon;
    {
        std::lock_guard lock(state_mutex_);
        enabled = ai_aim_enabled_ && !shutting_down_.load(std::memory_order_acquire);
        aim_hold = ai_aim_hold_pressed_;
        weapon = current_weapon_;
        blocking_ui = equipment_ui_open_ || large_map_open_ ||
                      game_menu_state_.aimInputBlocked() ||
                      manual_game_minimap_open_ || game_minimap_opened_by_app_ ||
                      nowSeconds() < game_menu_input_suppress_until_;
        paused = shutting_down_.load(std::memory_order_acquire) || mortar_mounted_;
        context_valid = ai_context_valid_;
    }
    paused = paused || !isAiAimWeapon(weapon) || !context_valid;

    pubg::ai_aim::ActivationState activation;
    activation.enabled = enabled;
    activation.aim_primary = aim_hold;
    activation.paused = paused;
    activation.game_foreground = game_foreground;
    activation.blocking_ui_open = blocking_ui;
    ai_aim_->updateActivation(activation);
}

void App::startAiStateMonitor() {
    if (ai_state_worker_.joinable()) {
        return;
    }
    ai_state_worker_ = std::jthread([this](std::stop_token stop) {
        AiAimRuntimeErrorTracker error_tracker;
        while (!stop.stop_requested()) {
            const bool game_foreground = hotkeys_.isGameForeground();
            const auto cursor_visibility = InputController::cursorVisibility();
            {
                std::lock_guard lock(state_mutex_);
                (void)game_menu_state_.observeCursorVisibility(
                    cursor_visibility,
                    knownGameUiStateLocked(),
                    game_foreground,
                    nowSeconds() >= game_menu_input_suppress_until_);
            }
            publishAiActivation(game_foreground);

            bool enabled = false;
            std::uint64_t observed_session = 0;
            {
                std::lock_guard lock(state_mutex_);
                enabled = ai_aim_enabled_ && !shutting_down_.load(std::memory_order_acquire);
                observed_session = ai_aim_session_revision_;
            }
            if (enabled && ai_aim_) {
                const auto snapshot = ai_aim_->snapshot();
                AiAimRuntimeErrorTransition transition;
                bool sample_accepted = false;
                {
                    std::lock_guard lock(state_mutex_);
                    sample_accepted = shouldAcceptAiAimRuntimeSample(
                        ai_aim_enabled_ && !shutting_down_.load(std::memory_order_acquire),
                        observed_session,
                        ai_aim_session_revision_);
                    if (sample_accepted) {
                        if (status_hud_) {
                            status_hud_->setAiSnapshot(snapshot,
                                ai_aim_settings_.display.target_boxes,
                                ai_aim_settings_.algorithm.tracking.max_frame_age_ms);
                        }
                        transition = error_tracker.observe(
                            snapshot.last_error,
                            observed_session);
                        if (transition.changed) {
                            ai_aim_runtime_error_ = transition.active_error;
                        }
                    }
                }
                if (!sample_accepted) {
                    error_tracker.reset();
                } else if (transition.changed) {
                    updateStatusHud();
                    if (!transition.active_error.empty() && status_hud_) {
                        // Hold the App state lock across the final session check
                        // and message write. A concurrent disable then clears the
                        // module-owned message after invalidating this session.
                        std::lock_guard lock(state_mutex_);
                        if (shouldAcceptAiAimRuntimeSample(
                                ai_aim_enabled_ && !shutting_down_.load(std::memory_order_acquire),
                                observed_session,
                                ai_aim_session_revision_)) {
                            status_hud_->showTemporaryMessage(
                                "AI瞄准错误: " + transition.active_error,
                                2500,
                                "#EF4444");
                        }
                    }
                }
            } else {
                error_tracker.reset();
                bool cleared = false;
                {
                    std::lock_guard lock(state_mutex_);
                    cleared = !ai_aim_runtime_error_.empty();
                    ai_aim_runtime_error_.clear();
                    if (status_hud_) {
                        status_hud_->setAiSnapshot({}, ai_aim_settings_.display.target_boxes,
                            ai_aim_settings_.algorithm.tracking.max_frame_age_ms);
                    }
                }
                if (cleared) {
                    updateStatusHud();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void App::stopAiStateMonitor() {
    if (!ai_state_worker_.joinable()) {
        return;
    }
    ai_state_worker_.request_stop();
    ai_state_worker_.join();
}

void App::handleAiAimStatus(const AiAimRuntimeStatus& status) {
    std::lock_guard status_lock(ai_aim_status_mutex_);
    if (!ai_aim_) {
        return;
    }
    const auto current_status = ai_aim_->status();
    if (current_status.lifecycle_generation != status.lifecycle_generation ||
        current_status.revision != status.revision) {
        return;
    }

    const bool rollback = status.state == AiAimRuntimeState::Error;
    bool show_warning = false;
    std::uint64_t message_session = 0;
    {
        std::lock_guard lock(state_mutex_);
        if (status.state != AiAimRuntimeState::Off && !ai_aim_enabled_) {
            return;
        }
        show_warning = !status.warning.empty() &&
                       status.warning != ai_aim_status_.warning;
        ai_aim_status_ = status;
        if (rollback) {
            if (ai_aim_enabled_) {
                ++ai_aim_session_revision_;
            }
            ai_aim_enabled_ = false;
            ai_aim_hold_pressed_ = false;
            ai_aim_runtime_error_.clear();
        } else if (status.state == AiAimRuntimeState::Off) {
            ai_aim_runtime_error_.clear();
        }
        message_session = ai_aim_session_revision_;
    }
    if (rollback) {
        auto* window = main_window_.load(std::memory_order_acquire);
        if (window) {
            QMetaObject::invokeMethod(window, [this, message_session] {
                bool current = false;
                {
                    std::lock_guard lock(state_mutex_);
                    current = ai_aim_session_revision_ == message_session &&
                              !ai_aim_enabled_;
                }
                if (current) {
                    if (auto* main_window = main_window_.load(std::memory_order_acquire)) {
                        main_window->setAiAimState(false);
                    }
                }
            }, Qt::QueuedConnection);
        }
    }
    updateStatusHud();
    if (!status_hud_ || (!rollback && !show_warning)) {
        return;
    }

    // The switch path uses the same recursive lifecycle lock and clears an
    // owned message after changing sessions. Holding state_mutex_ through the
    // final check + write makes the ordering deterministic.
    std::lock_guard lock(state_mutex_);
    const bool expected_enabled = !rollback;
    const auto latest_status = ai_aim_->status();
    if (ai_aim_session_revision_ != message_session ||
        ai_aim_enabled_ != expected_enabled ||
        latest_status.lifecycle_generation != status.lifecycle_generation ||
        latest_status.revision != status.revision) {
        return;
    }
    if (rollback) {
        status_hud_->showTemporaryMessage(
            "AI瞄准加载失败: " + status.detail, 3000, "#EF4444");
    } else {
        status_hud_->showTemporaryMessage(
            "AI瞄准提示: " + status.warning, 3500, "#F59E0B");
    }
}

void App::updateStatusHud() {
    if (!status_hud_) {
        return;
    }
    bool weapon_detection = false;
    bool display = false;
    bool recoil = false;
    bool ai_aim = false;
    std::string current_weapon;
    std::string current_stance;
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection = weapon_detection_enabled_;
        display = display_enabled_;
        recoil = recoil_enabled_;
        ai_aim = ai_aim_enabled_;
        current_weapon = current_weapon_;
        current_stance = current_stance_;
    }
    status_hud_->setSwitches(weapon_detection, display, recoil, ai_aim);
    status_hud_->setCurrentWeapon(current_weapon);
    status_hud_->setStance(current_stance);
    status_hud_->setPeekDirection(recoil_ ? recoil_->sgPeekDirection() : 0,
                                  recoil_ && recoil_->shouldShowSgPeekDirection());
    status_hud_->setMarkerIndicatorVisible(display);
}

RegionManager::DisplayInfo App::resolveGameDisplay() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    const Json current_snapshot = screenSnapshot();
    const Json saved = config_.read([](const Json& data) {
        return data.value("display_config", Json::object());
    });
    const bool has_saved = saved.is_object() && saved.contains("selected_screen");
    const bool changed = has_saved && !sameScreenSnapshot(saved.value("screens", Json::array()), current_snapshot);

    int selected_index = saved.value("selected_screen", Json::object()).value("index", 0);
    const bool need_choose = screens.size() > 1 && (!has_saved || changed);

    if (need_choose) {
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("选择游戏显示器"));
        ui::applyThemedPopupWindow(&dialog, config_);
        dialog.setModal(true);
        dialog.setFixedSize(320, 72 + screens.size() * 42);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(18, 42, 18, 16);
        layout->setSpacing(8);
        auto* label = new QLabel(changed
            ? QStringLiteral("检测到显示器配置变动，请选择你游戏使用的显示器")
            : QStringLiteral("请选择你游戏使用的显示器"), &dialog);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        for (int i = 0; i < screens.size(); ++i) {
            QScreen* screen = screens[i];
            const QRect g = screen->geometry();
            auto* button = new QPushButton(QStringLiteral("%1  %2x%3").arg(screen->name()).arg(g.width()).arg(g.height()), &dialog);
            button->setMinimumHeight(32);
            layout->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, &selected_index, i] {
                selected_index = i;
                dialog.accept();
            });
        }
        if (dialog.exec() != QDialog::Accepted) {
            selected_index = std::clamp(selected_index, 0, std::max(0, static_cast<int>(screens.size()) - 1));
        }
    }

    if (screens.empty()) {
        selected_index = 0;
    } else {
        selected_index = std::clamp(selected_index, 0, static_cast<int>(screens.size()) - 1);
    }
    QScreen* selected_screen = screens.empty() ? QGuiApplication::primaryScreen() : screens[selected_index];
    RegionManager::DisplayInfo display = displayInfoFromScreen(selected_screen);
    const auto [profile_width, profile_height] = snapToExistingRegionProfile(display.width, display.height, paths_);
    if (profile_width != display.width || profile_height != display.height) {
        std::cerr << "[display] snapped detected resolution " << display.width << "x" << display.height
                  << " to region profile " << profile_width << "x" << profile_height << "\n";
        display.width = profile_width;
        display.height = profile_height;
        if (display.device_pixel_ratio > 0.0) {
            display.qt_width = std::max(1, static_cast<int>(std::lround(display.width / display.device_pixel_ratio)));
            display.qt_height = std::max(1, static_cast<int>(std::lround(display.height / display.device_pixel_ratio)));
        }
    }

    config_.write([&](Json& data) {
        data["display_config"]["screens"] = current_snapshot;
        data["display_config"]["selected_screen"] = Json{
            {"index", selected_index},
            {"name", display.name},
            {"left", display.left},
            {"top", display.top},
            {"width", display.width},
            {"height", display.height},
            {"qt_left", display.qt_left},
            {"qt_top", display.qt_top},
            {"qt_width", display.qt_width},
            {"qt_height", display.qt_height},
            {"device_pixel_ratio", display.device_pixel_ratio},
        };
    });
    return display;
}

void App::printStatus() const {
    bool weapon_detection = false;
    bool display = false;
    bool recoil = false;
    bool ai_aim = false;
    {
        std::lock_guard lock(state_mutex_);
        weapon_detection = weapon_detection_enabled_;
        display = display_enabled_;
        recoil = recoil_enabled_;
        ai_aim = ai_aim_enabled_;
    }
    std::cout << "[status] weapon_detection=" << weapon_detection
              << " display=" << display
              << " recoil=" << recoil
              << " ai_aim=" << ai_aim << "\n";
}

int App::run() {
    requireOwnerThread();
    std::cout << "PUBGAssistant C++ port started.\n";
    std::cout << "Default hotkeys: F1 weapon detection, F2 distance display, F3 recoil, F4 AI aim, F5 large-map measurement; all are configurable.\n";
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
    setAiAimEnabled(false);
    {
        std::string current_weapon;
        {
            std::lock_guard lock(state_mutex_);
            current_weapon = current_weapon_;
        }
        updateSpecialWeaponMinimapState("", current_weapon);
        updateAssistantRouting();
    }
    PUBG_HEAP_PROBE("run.afterEnableDetection");
    pubg::ui::MainWindow::ControlCallbacks callbacks;
    callbacks.set_weapon_detection = [this](bool enabled) { setWeaponDetectionEnabled(enabled); };
    callbacks.set_display = [this](bool enabled) { setDisplayEnabled(enabled); };
    callbacks.set_recoil = [this](bool enabled) { setRecoilEnabled(enabled); };
    callbacks.set_ai_aim = [this](bool enabled) { setAiAimEnabled(enabled); };
    callbacks.apply_ai_aim_settings = [this](const AiAimSettings& settings, std::string& error) {
        return applyAiAimSettings(settings, error);
    };
    callbacks.set_assistant = [this](const std::string& key, bool enabled) { setAssistantEnabled(key, enabled); };
    callbacks.sync_marker_colors = [this] { syncMarkerColorsFromConfig(); };
    // Display edits change presentation only. Existing recognition state is already
    // in the HUD; refreshing five state setters would redraw the full overlay five times.
    callbacks.refresh_status_hud = [this] { if (status_hud_) status_hud_->render(); };
    callbacks.reload_hotkeys = [this] { reloadHotkeys(); };
    callbacks.prepare_for_close = [this] { prepareForUiClose(); };
    int code = 0;
    {
        pubg::ui::MainWindow window(config_, *regions_, *minimap_, *elevation_, *weapon_detector_,
                                    *equipment_detector_, *gesture_identifier_, *recoil_, *special_,
                                    *map_points_, *large_map_, *throwables_, *c4_, std::move(callbacks));
        main_window_.store(&window, std::memory_order_release);
        auto window_lifetime = ScopeExit([this]() noexcept {
            // Keep the QObject alive until every producer that can queue work to
            // it has stopped. This also covers partial hotkey startup failures.
            stopRuntimeForShutdown();
            main_window_.store(nullptr, std::memory_order_release);
        });
        window.move(regions_->qtScreenLeft(), regions_->qtScreenTop());
        window.show();
        hotkeys_.start();
        startAiStateMonitor();
        code = QApplication::exec();
        PUBG_HEAP_PROBE("run.afterExec");
    }
    PUBG_HEAP_PROBE("run.afterWindowDestroyed");
    shutdown();
    return code;
}

} // namespace pubg
