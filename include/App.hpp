#pragma once

#include "EquipmentDetector.hpp"
#include "GestureIdentifier.hpp"
#include "HotkeyManager.hpp"
#include "C4Assistant.hpp"
#include "LargeMapRadar.hpp"
#include "MapPointAssistant.hpp"
#include "MinimapRadar.hpp"
#include "MortarAutoAim.hpp"
#include "RecoilControl.hpp"
#include "SpecialAssistants.hpp"
#include "StatusHud.hpp"
#include "ThrowablesAssistant.hpp"
#include "WeaponDetector.hpp"
#include "ui/MainWindow.hpp"
#include <memory>
#include <mutex>

namespace pubg {

// C++ 版主控入口，承担 Python main.py 中 TacticalHub 的“总调度”职责。
// 负责加载配置、创建识别/辅助模块、连接回调、启动 Qt 主窗口和全局热键轮询。
class App {
public:
    // 构造时加载配置，并按正确顺序创建 RegionManager、识别模块、压枪模块和特殊武器 HUD。
    App();

    // 启动热键轮询和主循环。程序会一直运行，直到进程被关闭。
    int run();

    // 由主窗口或热键调用的统一开关入口，保持 UI 按钮和全局热键行为一致。
    void setWeaponDetectionEnabled(bool enabled);
    void setDisplayEnabled(bool enabled);
    void setRecoilEnabled(bool enabled);
    void toggleWeaponDetection();
    void toggleDisplay();
    void toggleRecoil();
    void setAssistantManual(const std::string& key, bool enabled);

    // 切换标点颜色，供 Q/E 热键和主窗口状态同步使用。
    void cycleMarkerColor(int direction);
    [[nodiscard]] std::string currentMarkerColor() const;
    void reloadHotkeys();
    void shutdown();

private:
    // 连接各模块之间的回调关系：
    // 武器识别 -> 压枪/特殊武器，装备栏识别 -> 当前两把主武器，姿势识别 -> 压枪姿势。
    void wireCallbacks();
    void registerHotkeys();

    // WeaponDetector 识别到手持武器变化时调用。
    // weapon 为空表示当前没有识别到有效武器；score 是模板匹配置信度。
    void updateWeaponFromDetectors(const std::string& weapon, double score);

    // EquipmentDetector 扫描装备栏后调用。
    // slots 包含 1/2 号武器槽的武器名和配件信息，用于限定手持武器候选和更新压枪倍率。
    void updateEquipment(const std::map<int, WeaponSlotInfo>& equipment_slots);

    // 打印当前 F1/F2/F3 三个核心开关状态，便于 Windows 首轮调试。
    void printStatus() const;
    void syncMarkerColorsFromConfig();
    int hotkeyVk(const std::string& name, const std::string& fallback) const;
    HotkeyManager::HotkeyCombo hotkeyCombo(const std::string& name, const std::string& fallback) const;
    void migrateLegacyDefaultHotkeys();
    void updateAssistantRouting();
    void updateStatusHud();
    bool shouldShowMarkerIndicator() const;
    bool isRecoilWeapon(const std::string& weapon) const;
    void syncRecoilAttachmentsForCurrentWeapon();

    // 资源路径和全局配置。必须先 load config，再构造依赖配置的模块。
    ResourcePaths paths_;
    Config config_;

    // 各业务模块使用 unique_ptr 延迟创建，避免构造顺序早于 config_.load()。
    std::unique_ptr<RegionManager> regions_;
    std::unique_ptr<MinimapRadar> minimap_;
    std::unique_ptr<ElevationRadar> elevation_;
    std::unique_ptr<WeaponDetector> weapon_detector_;
    std::unique_ptr<EquipmentDetector> equipment_detector_;
    std::unique_ptr<GestureIdentifier> gesture_identifier_;
    std::unique_ptr<RecoilControl> recoil_;
    std::unique_ptr<SpecialAssistants> special_;
    std::unique_ptr<MapPointAssistant> map_points_;
    std::unique_ptr<LargeMapRadar> large_map_;
    std::unique_ptr<MortarAutoAim> mortar_auto_aim_;
    std::unique_ptr<ThrowablesAssistant> throwables_;
    std::unique_ptr<C4Assistant> c4_;
    std::unique_ptr<StatusHud> status_hud_;
    HotkeyManager hotkeys_;

    std::atomic_bool running_{true};
    mutable std::mutex control_mutex_;
    mutable std::mutex state_mutex_;
    bool weapon_detection_enabled_ = false;
    bool display_enabled_ = false;
    bool recoil_enabled_ = false;
    std::string current_weapon_;
    bool left_pressed_ = false;
    bool middle_pressed_ = false;
    bool right_pressed_ = false;
    bool alt_pressed_ = false;
    std::vector<std::string> marker_color_order_{"Yellow", "Orange", "Blue", "Green"};
    std::string current_marker_color_ = "Yellow";
    std::map<int, WeaponSlotInfo> equipment_;
    std::string current_stance_;
    std::unordered_map<std::string, bool> manual_assistants_;
    ui::MainWindow* main_window_ = nullptr;
    bool shutting_down_ = false;
};

} // namespace pubg
