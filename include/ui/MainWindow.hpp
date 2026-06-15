#pragma once

#include <QHash>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPoint>
#include <QStackedWidget>
#include <QTimer>
#include <QVector>
#include <functional>

#include "C4Assistant.hpp"
#include "Config.hpp"
#include "ElevationRadar.hpp"
#include "EquipmentDetector.hpp"
#include "GestureIdentifier.hpp"
#include "LargeMapRadar.hpp"
#include "MapPointAssistant.hpp"
#include "MinimapRadar.hpp"
#include "OverlayWindow.hpp"
#include "RecoilControl.hpp"
#include "RegionManager.hpp"
#include "SpecialAssistants.hpp"
#include "ThrowablesAssistant.hpp"
#include "WeaponDetector.hpp"
#include "ui/RoundedButton.hpp"

namespace pubg::ui {

// Qt 主窗口，对应 Python main.py 中 TacticalHub 的可见控制台。
// 目标是还原 280x372 半透明、置顶、无边框、四个选项卡的交互。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct ControlCallbacks {
        std::function<void(bool)> set_weapon_detection;
        std::function<void(bool)> set_display;
        std::function<void(bool)> set_recoil;
        std::function<void(const std::string&, bool)> set_assistant;
        std::function<void()> sync_marker_colors;
        std::function<void()> reload_hotkeys;
        std::function<void()> shutdown;
    };

    MainWindow(Config& config,
               RegionManager& regions,
               MinimapRadar& minimap,
               ElevationRadar& elevation,
               WeaponDetector& weapon_detector,
               EquipmentDetector& equipment_detector,
               GestureIdentifier& gesture_identifier,
               RecoilControl& recoil,
               SpecialAssistants& special,
               MapPointAssistant& map_points,
               LargeMapRadar& large_map,
               ThrowablesAssistant& throwables,
               C4Assistant& c4,
               ControlCallbacks callbacks = {},
               QWidget* parent = nullptr);

    // 全局热键线程通过 Qt 主线程调用这两个函数，保持和 Python 的 Home/方向键体验一致。
    void toggleWindowVisible();
    void switchTab(int direction);
    void setWeaponDetectionState(bool enabled);
    void setDisplayState(bool enabled);
    void setRecoilState(bool enabled);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // 构建窗口标题栏、tab bar 和四个页面。
    void buildUi();
    void buildTitleBar(QWidget* root);
    void applyNativeRoundedRegion();
    QWidget* addTab(const QString& title);
    void selectTab(int index);
    void buildMapTab(QWidget* tab);
    void buildLaunchTab(QWidget* tab);
    void buildCalibrationTab(QWidget* tab);
    void buildKeyTab(QWidget* tab);

    // 主窗口按钮回调。
    void toggleWeaponDetection();
    void toggleDisplay();
    void toggleRecoil();
    void toggleAssistant(const QString& key);
    void selectMap(int index);
    void selectMarkerSize(int index);
    void selectPntColorMode(const QString& mode);
    void toggleDebugOverlay();
    void drawDebugOverlay();
    void openScaleCalibrator();
    void openRecoilDebugger();
    void openSpecialWeaponDebugger();
    void applyCaptureExclusion();
    void beginCaptureHotkey(const QString& action);
    void saveHotkeys();
    void resetDefaultHotkeys();
    QString formatHotkey(const std::string& value) const;

    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    ElevationRadar& elevation_;
    WeaponDetector& weapon_detector_;
    EquipmentDetector& equipment_detector_;
    GestureIdentifier& gesture_identifier_;
    RecoilControl& recoil_;
    SpecialAssistants& special_;
    MapPointAssistant& map_points_;
    LargeMapRadar& large_map_;
    ThrowablesAssistant& throwables_;
    C4Assistant& c4_;
    ControlCallbacks callbacks_;

    QWidget* central_ = nullptr;
    QWidget* tab_bar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QVector<RoundedButton*> tab_buttons_;
    QLabel* status_label_ = nullptr;
    QPoint drag_offset_;

    bool weapon_detection_enabled_ = true;
    bool display_enabled_ = false;
    bool recoil_enabled_ = false;
    bool debug_overlay_enabled_ = false;
    bool closing_ = false;
    QString capturing_action_;
    RoundedButton* btn_weapon_detect_ = nullptr;
    RoundedButton* btn_display_ = nullptr;
    RoundedButton* btn_recoil_ = nullptr;
    RoundedButton* btn_debug_ = nullptr;
    QVector<RoundedButton*> map_buttons_;
    QVector<RoundedButton*> size_buttons_;
    QHash<QString, RoundedButton*> pnt_mode_buttons_;
    QHash<QString, RoundedButton*> group_buttons_;
    QHash<QString, QLabel*> hotkey_labels_;
    QHash<QString, RoundedButton*> assistant_buttons_;
    OverlayWindow debug_overlay_;
};

} // namespace pubg::ui
