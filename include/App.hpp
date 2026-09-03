#pragma once

#include "AiAimSettings.hpp"
#include "AiAimHostContext.hpp"
#include "AiAimRuntimeErrorTracker.hpp"
#include "EquipmentDetector.hpp"
#include "AiAimCoordinator.hpp"
#include "GestureIdentifier.hpp"
#include "GameMenuStateTracker.hpp"
#include "HotkeyManager.hpp"
#include "C4Assistant.hpp"
#include "LargeMapRadar.hpp"
#include "MapPointAssistant.hpp"
#include "MinimapRadar.hpp"
#include "MortarAutoAim.hpp"
#include "MortarMountDetector.hpp"
#include "RecoilControl.hpp"
#include "ScopeExit.hpp"
#include "ShutdownSequence.hpp"
#include "SpecialAssistants.hpp"
#include "StatusHud.hpp"
#include "ThrowablesAssistant.hpp"
#include "WeaponDetector.hpp"
#include "ui/MainWindow.hpp"
#include <memory>
#include <mutex>
#include <thread>

namespace pubg {

// C++ 版主控入口，承担 Python main.py 中 TacticalHub 的“总调度”职责。
// 负责加载配置、创建识别/辅助模块、连接回调、启动 Qt 主窗口和全局热键轮询。
class App {
public:
    // 构造时加载配置，并按正确顺序创建 RegionManager、识别模块、压枪模块和特殊武器 HUD。
    App();
    // 异常离开 run() 时也先停止并 join 所有 producer，再按成员顺序析构依赖状态。
    ~App() noexcept;

    // 启动热键轮询和主循环。程序会一直运行，直到进程被关闭。
    int run();

    // 由主窗口或热键调用的统一开关入口，保持 UI 按钮和全局热键行为一致。
    void setWeaponDetectionEnabled(bool enabled);
    void setDisplayEnabled(bool enabled);
    void setRecoilEnabled(bool enabled);
    void setAiAimEnabled(bool enabled);
    void toggleWeaponDetection();
    void toggleDisplay();
    void toggleRecoil();
    void toggleAiAim();
    void setAssistantEnabled(const std::string& key, bool enabled);

    // 切换标点颜色，供标点切换热键和主窗口状态同步使用。
    void cycleMarkerColor(int direction);
    [[nodiscard]] std::string currentMarkerColor() const;
    void reloadHotkeys();
    void prepareForUiClose();
    void shutdown() noexcept;

private:
    // 连接各模块之间的回调关系：
    // 武器识别 -> 压枪/特殊武器，装备栏识别 -> 当前两把主武器，姿势识别 -> 压枪姿势。
    void wireCallbacks();
    void registerHotkeys();

    // WeaponDetector 识别到手持武器变化时调用。
    // weapon 为空表示当前没有识别到有效武器；score 是模板匹配置信度。
    void updateWeaponFromDetectors(const std::string& weapon, double score);
    void applyWeaponUpdate(const std::string& weapon, double score);

    // EquipmentDetector 扫描装备栏后调用。
    // slots 包含 1/2 号武器槽的武器名和配件信息，用于限定手持武器候选和更新压枪倍率。
    void updateEquipment(const std::map<int, WeaponSlotInfo>& equipment_slots);

    // 打印当前四个核心开关状态，便于 Windows 首轮调试。
    void printStatus() const;
    void loadUiState();
    void saveSwitchState(const std::string& key, bool enabled);
    void syncMarkerColorsFromConfig();
    int hotkeyVk(const std::string& name, const std::string& fallback) const;
    HotkeyManager::HotkeyCombo hotkeyCombo(const std::string& name, const std::string& fallback) const;
    void migrateLegacyDefaultHotkeys();
    void updateAssistantRouting();
    void updateStatusHud();
    void updateSpecialWeaponMinimapState(const std::string& previous_weapon, const std::string& current_weapon);
    void setSpecialDisplayActive(bool active);
    bool shouldAutoUseMinimapForWeapon(const std::string& weapon) const;
    bool shouldRunSpecialAssistantsForWeapon(const std::string& weapon) const;
    void toggleGameMinimap();
    void openGameMinimapForAssist();
    void closeGameMinimapForAssist();
    void handleManualGameMinimapToggle(bool pressed);
    void handleEscapeKey(bool pressed);
    // state_mutex_ must be held while taking this cross-module UI snapshot.
    [[nodiscard]] GameMenuStateTracker::KnownUiState knownGameUiStateLocked() const noexcept;
    void holdMortarStateForBlockingUi();
    bool gameMenuInputSuppressed() const;
    bool shouldHoldMortarStateAfterEsc() const;
    void handleMortarInteractKey(bool pressed);
    void handleMeasureMapHotkey();
    void joinMortarAutoAimHoldWorker();
    void startMortarMountConfirmation();
    void setMortarMounted(bool mounted);
    void applyMortarMounted(bool mounted);
    void joinMortarMountWorker();
    void requireOwnerThread() const noexcept;
    void stopRuntimeForShutdown() noexcept;
    bool shouldShowMarkerIndicator() const;
    bool canShowMapPointsNow() const;
    bool isRecoilWeapon(const std::string& weapon) const;
    void syncRecoilAttachmentsForCurrentWeapon();
    void publishAiCombatContext();
    void publishAiActivation(bool game_foreground);
    void startAiStateMonitor();
    void stopAiStateMonitor();
    void handleAiAimStatus(const AiAimRuntimeStatus& status);
    [[nodiscard]] bool applyAiAimSettings(const AiAimSettings& settings, std::string& error);
    [[nodiscard]] bool isAiAimWeapon(const std::string& weapon) const;
    RegionManager::DisplayInfo resolveGameDisplay();

    // 资源路径和全局配置。必须先 load config，再构造依赖配置的模块。
    ResourcePaths paths_;
    Config config_;
    AiAimSettings ai_aim_settings_;

    // 各业务模块使用 unique_ptr 延迟创建，避免构造顺序早于 config_.load()。
    std::unique_ptr<RegionManager> regions_;
    std::unique_ptr<MinimapRadar> minimap_;
    std::unique_ptr<ElevationRadar> elevation_;
    std::unique_ptr<WeaponDetector> weapon_detector_;
    std::unique_ptr<EquipmentDetector> equipment_detector_;
    std::unique_ptr<GestureIdentifier> gesture_identifier_;
    std::shared_ptr<MouseMotionDispatcher> mouse_motion_dispatcher_;
    std::unique_ptr<RecoilControl> recoil_;
    std::unique_ptr<LargeMapRadar> large_map_;
    std::unique_ptr<SpecialAssistants> special_;
    std::unique_ptr<MapPointAssistant> map_points_;
    std::unique_ptr<MortarAutoAim> mortar_auto_aim_;
    std::unique_ptr<MortarMountDetector> mortar_mount_detector_;
    std::unique_ptr<ThrowablesAssistant> throwables_;
    std::unique_ptr<C4Assistant> c4_;
    std::unique_ptr<StatusHud> status_hud_;
    std::unique_ptr<AiAimCoordinator> ai_aim_;
    HotkeyManager hotkeys_;

    std::atomic_bool running_{true};
    mutable std::mutex control_mutex_;
    // Serializes AI switch transitions with status delivery. Recursive because
    // AiAimCoordinator may synchronously publish Loading/Off from setEnabled().
    mutable std::recursive_mutex ai_aim_status_mutex_;
    mutable std::mutex state_mutex_;
    bool weapon_detection_enabled_ = false;
    bool display_enabled_ = false;
    bool special_display_active_ = false;
    bool recoil_enabled_ = false;
    bool ai_aim_enabled_ = false;
    std::uint64_t ai_aim_session_revision_ = 0;
    std::string current_weapon_;
    bool current_weapon_icon_visible_ = false;
    bool left_pressed_ = false;
    bool middle_pressed_ = false;
    bool right_pressed_ = false;
    bool ai_aim_hold_pressed_ = false;
    bool lean_left_pressed_ = false;
    bool lean_right_pressed_ = false;
    bool equipment_ui_open_ = false;
    bool large_map_open_ = false;
    bool manual_game_minimap_open_ = false;
    bool ai_context_valid_ = false;
    std::uint64_t ai_context_source_revision_ = 0;
    bool alt_pressed_ = false;
    bool mortar_mounted_ = false;
    bool game_minimap_opened_by_app_ = false;
    bool manual_special_display_suppressed_ = false;
    double manual_minimap_toggle_ignore_until_ = 0.0;
    GameMenuStateTracker game_menu_state_;
    double game_menu_input_suppress_until_ = 0.0;
    bool hold_mortar_until_reacquired_ = false;
    std::atomic_uint64_t mortar_mount_session_{0};
    std::mutex mortar_mount_mutex_;
    std::thread mortar_mount_worker_;
    std::atomic_uint64_t mortar_auto_aim_hold_session_{0};
    std::mutex mortar_auto_aim_hold_mutex_;
    std::thread mortar_auto_aim_hold_worker_;
    std::jthread ai_state_worker_;
    AiAimRuntimeStatus ai_aim_status_{};
    std::string ai_aim_runtime_error_;
    double last_measure_map_hotkey_time_ = 0.0;
    std::vector<std::string> marker_color_order_{"Yellow", "Orange", "Blue", "Green"};
    std::string current_marker_color_ = "Yellow";
    std::map<int, WeaponSlotInfo> equipment_;
    std::string current_stance_;
    std::unordered_map<std::string, bool> assistant_enabled_;
    std::optional<std::pair<std::string, double>> pending_weapon_update_;
    std::atomic<ui::MainWindow*> main_window_{nullptr};
    const std::thread::id owner_thread_id_{std::this_thread::get_id()};
    ShutdownLifecycle runtime_shutdown_;
    std::atomic_bool shutting_down_{false};
    bool resources_released_ = false;
};

} // namespace pubg
