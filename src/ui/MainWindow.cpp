#include "ui/MainWindow.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QApplication>
#include <QKeyEvent>
#include <QFrame>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPair>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>
#include <utility>

#include "ui/RecoilDebuggerWindow.hpp"
#include "ui/RegionCalibrationOverlay.hpp"
#include "ui/ScaleCalibrationWindow.hpp"
#include "ui/SpecialWeaponDebuggerWindow.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pubg::ui {

#ifdef _WIN32
MainWindow* MainWindow::s_capture_window_ = nullptr;
#endif

namespace {

QString functionKeyFromNativeScanCode(quint32 scan_code) {
    switch (scan_code) {
        case 0x3B: return QStringLiteral("<f1>");
        case 0x3C: return QStringLiteral("<f2>");
        case 0x3D: return QStringLiteral("<f3>");
        case 0x3E: return QStringLiteral("<f4>");
        case 0x3F: return QStringLiteral("<f5>");
        case 0x40: return QStringLiteral("<f6>");
        case 0x41: return QStringLiteral("<f7>");
        case 0x42: return QStringLiteral("<f8>");
        case 0x43: return QStringLiteral("<f9>");
        case 0x44: return QStringLiteral("<f10>");
        case 0x57: return QStringLiteral("<f11>");
        case 0x58: return QStringLiteral("<f12>");
        default: return {};
    }
}

QString supportedHotkeyFromEvent(QKeyEvent* event) {
    if (const QString physical_function_key = functionKeyFromNativeScanCode(event->nativeScanCode());
        !physical_function_key.isEmpty()) {
        return physical_function_key;
    }
    if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) return QString(QChar('a' + event->key() - Qt::Key_A));
    if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) return QString(QChar('0' + event->key() - Qt::Key_0));
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12) return QString("<f%1>").arg(event->key() - Qt::Key_F1 + 1);
    if (event->key() == Qt::Key_Tab) return QStringLiteral("tab");
    if (event->key() == Qt::Key_Space) return QStringLiteral("<space>");
    if (event->key() == Qt::Key_End) return QStringLiteral("end");
    if (event->key() == Qt::Key_Home) return QStringLiteral("<home>");
    return {};
}

QStringList activeModifierNames(QKeyEvent* event) {
    QStringList modifiers;
#ifdef _WIN32
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers << QStringLiteral("<ctrl>");
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers << QStringLiteral("<shift>");
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers << QStringLiteral("<alt>");
#else
    const auto mods = event->modifiers();
    if (mods.testFlag(Qt::ControlModifier)) modifiers << QStringLiteral("<ctrl>");
    if (mods.testFlag(Qt::ShiftModifier)) modifiers << QStringLiteral("<shift>");
    if (mods.testFlag(Qt::AltModifier)) modifiers << QStringLiteral("<alt>");
#endif
    return modifiers;
}

#ifdef _WIN32
QString supportedHotkeyFromWindowsMessage(const MSG* msg) {
    const auto vk = static_cast<int>(msg->wParam);
    const auto scan_code = static_cast<quint32>((msg->lParam >> 16) & 0xff);
    if (const QString physical_function_key = functionKeyFromNativeScanCode(scan_code);
        !physical_function_key.isEmpty()) {
        return physical_function_key;
    }
    if (vk >= 'A' && vk <= 'Z') return QString(QChar('a' + vk - 'A'));
    if (vk >= '0' && vk <= '9') return QString(QChar('0' + vk - '0'));
    if (vk == VK_TAB) return QStringLiteral("tab");
    if (vk == VK_SPACE) return QStringLiteral("<space>");
    if (vk == VK_END) return QStringLiteral("end");
    if (vk == VK_HOME) return QStringLiteral("<home>");
    return {};
}

QStringList activeModifierNames() {
    QStringList modifiers;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers << QStringLiteral("<ctrl>");
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers << QStringLiteral("<shift>");
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers << QStringLiteral("<alt>");
    return modifiers;
}

QString supportedHotkeyFromLowLevelHook(const KBDLLHOOKSTRUCT* kb) {
    const auto scan_code = static_cast<quint32>(kb->scanCode & 0xff);
    if (const QString physical_function_key = functionKeyFromNativeScanCode(scan_code);
        !physical_function_key.isEmpty()) {
        return physical_function_key;
    }
    const auto vk = static_cast<int>(kb->vkCode);
    if (vk >= 'A' && vk <= 'Z') return QString(QChar('a' + vk - 'A'));
    if (vk >= '0' && vk <= '9') return QString(QChar('0' + vk - '0'));
    if (vk == VK_TAB) return QStringLiteral("tab");
    if (vk == VK_SPACE) return QStringLiteral("<space>");
    if (vk == VK_END) return QStringLiteral("end");
    if (vk == VK_HOME) return QStringLiteral("<home>");
    return {};
}
#endif

} // namespace

MainWindow::MainWindow(Config& config,
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
                       ControlCallbacks callbacks,
                       QWidget* parent)
    : QMainWindow(parent),
      config_(config), regions_(regions), minimap_(minimap), elevation_(elevation),
      weapon_detector_(weapon_detector), equipment_detector_(equipment_detector),
      gesture_identifier_(gesture_identifier), recoil_(recoil), special_(special),
      map_points_(map_points), large_map_(large_map), throwables_(throwables), c4_(c4),
      callbacks_(std::move(callbacks)) {
    buildUi();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("PUBG 战术助手"));
    const auto icon_path = config_.paths().iconFile();
    if (std::filesystem::exists(icon_path)) {
        setWindowIcon(QIcon(QString::fromStdString(icon_path.string())));
    }
    setFixedSize(280, 372);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setWindowOpacity(0.60);
    setAttribute(Qt::WA_TranslucentBackground, true);
    central_ = new QWidget(this);
    central_->setObjectName("windowFrame");
    central_->setStyleSheet(
        "#windowFrame{background:#DDE6F0;border:1px solid #FFFFFF;border-radius:18px;}"
        "QWidget{background:transparent;border:0;}"
        "QStackedWidget{background:transparent;border:0;}"
        "QLabel{background:transparent;border:0;}"
    );
    setCentralWidget(central_);

    buildTitleBar(central_);

    tab_bar_ = new QWidget(central_);
    tab_bar_->setGeometry(9, 32, 262, 28);

    pages_ = new QStackedWidget(central_);
    pages_->setFrameShape(QFrame::NoFrame);
    pages_->setGeometry(9, 64, 262, 299);

    buildMapTab(addTab(QStringLiteral("地图点位")));
    buildLaunchTab(addTab(QStringLiteral("启动助手")));
    buildCalibrationTab(addTab(QStringLiteral("校准区域")));
    buildKeyTab(addTab(QStringLiteral("按键设置")));
    selectTab(0);
    QTimer::singleShot(0, this, [this] {
        applyNativeRoundedRegion();
        applyCaptureExclusion();
    });
}

void MainWindow::applyNativeRoundedRegion() {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    const double scale = devicePixelRatioF();
    const int physical_w = static_cast<int>(std::ceil(width() * scale));
    const int physical_h = static_cast<int>(std::ceil(height() * scale));
    const int physical_radius = static_cast<int>(std::round(36.0 * scale));
    HRGN region = CreateRoundRectRgn(0, 0, physical_w + 1, physical_h + 1, physical_radius, physical_radius);
    if (region && !SetWindowRgn(hwnd, region, TRUE)) {
        DeleteObject(region);
    }
#endif
}

void MainWindow::applyCaptureExclusion() {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    constexpr DWORD kWdaExcludeFromCapture = 0x00000011;
    if (!SetWindowDisplayAffinity(hwnd, kWdaExcludeFromCapture)) {
        SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
    }
#endif
}

void MainWindow::buildTitleBar(QWidget* root) {
    auto* bar = new QWidget(root);
    bar->setObjectName("titlebar");
    bar->setGeometry(1, 1, 278, 30);
    auto* title = new QLabel(QStringLiteral("PUBG 战术助手"), bar);
    title->setGeometry(10, 2, 128, 25);
    title->setStyleSheet("color:#111827;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;");

    auto* close = new QPushButton(bar);
    close->setCursor(Qt::PointingHandCursor);
    close->setFocusPolicy(Qt::NoFocus);
    close->setGeometry(256, 11, 14, 14);
    close->setStyleSheet(
        "QPushButton{border-radius:7px;background:#FF5F57;border:1px solid #E0473F;}"
        "QPushButton:hover{background:#FF746D;}"
        "QPushButton:pressed{background:#E0473F;}"
    );
    connect(close, &QPushButton::clicked, this, &QWidget::close);
}

QWidget* MainWindow::addTab(const QString& title) {
    int idx = pages_->count();
    auto* btn = new RoundedButton(title, tab_bar_);
    btn->configure(61, 28, 12, 13);
    btn->setToggleMode(true);
    btn->setGeometry(2 + idx * 65, 0, 61, 28);
    tab_buttons_.push_back(btn);
    connect(btn, &QPushButton::clicked, this, [this, idx] { selectTab(idx); });
    auto* page = new QWidget(this);
    page->setStyleSheet("background:#DDE6F0;");
    pages_->addWidget(page);
    return page;
}

void MainWindow::selectTab(int index) {
    pages_->setCurrentIndex(index);
    for (int i = 0; i < tab_buttons_.size(); ++i) tab_buttons_[i]->setActive(i == index);
}

void MainWindow::toggleWindowVisible() {
    if (isVisible()) {
        hide();
    } else {
        show();
        raise();
        activateWindow();
    }
}

void MainWindow::switchTab(int direction) {
    if (!pages_ || pages_->count() <= 0) return;
    selectTab((pages_->currentIndex() + pages_->count() + direction) % pages_->count());
}

void MainWindow::setWeaponDetectionState(bool enabled) {
    weapon_detection_enabled_ = enabled;
    if (btn_weapon_detect_) {
        btn_weapon_detect_->setActive(enabled);
        btn_weapon_detect_->setText(enabled ? QStringLiteral("关闭武器检测") : QStringLiteral("开启武器检测"));
    }
}

void MainWindow::setDisplayState(bool enabled) {
    display_enabled_ = enabled;
    if (btn_display_) {
        btn_display_->setActive(enabled);
        btn_display_->setText(enabled ? QStringLiteral("关闭瞄准辅助") : QStringLiteral("开启瞄准辅助"));
    }
    if (!enabled) {
        for (auto* button : assistant_buttons_) {
            button->setActive(false);
        }
    }
}

void MainWindow::setRecoilState(bool enabled) {
    recoil_enabled_ = enabled;
    if (btn_recoil_) {
        btn_recoil_->setActive(enabled);
        btn_recoil_->setText(enabled ? QStringLiteral("关闭辅助压枪") : QStringLiteral("开启辅助压枪"));
    }
}

void MainWindow::buildMapTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    QStringList maps{QStringLiteral("艾伦格"), QStringLiteral("米拉玛"), QStringLiteral("泰戈"), QStringLiteral("荣都"), QStringLiteral("帝斯顿"), QStringLiteral("维寒迪")};
    QStringList full{QStringLiteral("艾伦格 (Erangel)"), QStringLiteral("米拉玛 (Miramar)"), QStringLiteral("泰戈 (Taego)"), QStringLiteral("荣都 (Rondo)"), QStringLiteral("帝斯顿 (Deston)"), QStringLiteral("维寒迪 (Vikendi)")};
    for (int i = 0; i < maps.size(); ++i) {
        auto* b = new RoundedButton(maps[i], tab);
        b->configure((i % 2) ? 128 : 126, 30, 12, 16);
        b->setToggleMode(true);
        b->setGeometry(2 + (i % 2) * 130, 3 + (i / 2) * 36, (i % 2) ? 128 : 126, 30);
        map_buttons_.push_back(b);
        connect(b, &QPushButton::clicked, this, [this, i] { selectMap(i); });
    }
    auto label = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet("color:#6B7280;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;");
        return l;
    };
    auto* marker_size_label = label(QStringLiteral("--标点尺寸--"));
    marker_size_label->setParent(tab);
    marker_size_label->setGeometry(104, 112, 54, 25);
    auto* size_frame = new QWidget(tab);
    size_frame->setGeometry(0, 140, 261, 30);
    QStringList sizes{QStringLiteral("小"), QStringLiteral("中"), QStringLiteral("大")};
    for (int i = 0; i < 3; ++i) {
        auto* b = new RoundedButton(sizes[i], size_frame);
        b->configure(81, 30, 12, 14);
        b->setToggleMode(true);
        b->setGeometry(3 + i * 87, 0, 81, 30);
        size_buttons_.push_back(b);
        connect(b, &QPushButton::clicked, this, [this, i] { selectMarkerSize(i); });
    }
    auto* color_mode_label = label(QStringLiteral("--色盲选择--"));
    color_mode_label->setParent(tab);
    color_mode_label->setGeometry(104, 176, 54, 25);
    auto* mode_frame = new QWidget(tab);
    mode_frame->setGeometry(1, 204, 260, 30);
    QVector<QPair<QString, QString>> modes{
        {QStringLiteral("无色盲"), "normal"},
        {QStringLiteral("绿色盲"), "deuteranopia"},
        {QStringLiteral("红色盲"), "protanopia"},
        {QStringLiteral("蓝色盲"), "tritanopia"},
    };
    const QString current_mode = QString::fromStdString(config_.read([](const Json& data) {
        return data.value("pnt_color_mode", std::string("normal"));
    }));
    for (const auto& mode : modes) {
        auto* b = new RoundedButton(mode.first, mode_frame);
        b->configure(61, 30, 12, 13);
        b->setToggleMode(true);
        b->setActive(mode.second == current_mode);
        b->setGeometry(2 + pnt_mode_buttons_.size() * 65, 0, 61, 30);
        pnt_mode_buttons_[mode.second] = b;
        connect(b, &QPushButton::clicked, this, [this, key = mode.second] { selectPntColorMode(key); });
    }
    auto* group_label = label(QStringLiteral("--点位类型--"));
    group_label->setParent(tab);
    group_label->setGeometry(104, 238, 54, 25);
    auto* group_frame = new QWidget(tab);
    group_frame->setGeometry(1, 266, 260, 30);
    QStringList groups{QStringLiteral("载具"), QStringLiteral("飞机"), QStringLiteral("密室"), QStringLiteral("其他")};
    QStringList keys{"vehicles", "planes", "rooms", "other"};
    for (int i = 0; i < 4; ++i) {
        auto* b = new RoundedButton(groups[i], group_frame);
        b->configure(61, 30, 12, 13);
        b->setToggleMode(true);
        b->setActive(true);
        b->setGeometry(2 + i * 65, 0, 61, 30);
        group_buttons_[keys[i]] = b;
        const std::string key = keys[i].toStdString();
        connect(b, &QPushButton::clicked, this, [this, key, b] { map_points_.setCategoryEnabled(key, b->active()); });
    }
    selectMap(0);
    selectMarkerSize(1);
}

void MainWindow::buildLaunchTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    int button_y = 3;
    auto addBtn = [&](const QString& text, auto slot) {
        auto* b = new RoundedButton(text, tab);
        b->configure(256, 34, 12, 16);
        b->setToggleMode(true);
        b->setGeometry(3, button_y, 256, 34);
        button_y += 40;
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    btn_weapon_detect_ = addBtn(QStringLiteral("关闭武器检测"), [this] { toggleWeaponDetection(); });
    btn_weapon_detect_->setActive(true);
    btn_display_ = addBtn(QStringLiteral("开启瞄准辅助"), [this] { toggleDisplay(); });
    btn_recoil_ = addBtn(QStringLiteral("开启辅助压枪"), [this] { toggleRecoil(); });
    auto* sw = new RoundedButton(QStringLiteral("调试特殊武器"), tab);
    sw->configure(126, 34, 12, 14);
    sw->setGeometry(3, 123, 126, 34);
    auto* rw = new RoundedButton(QStringLiteral("调试压枪参数"), tab);
    rw->configure(126, 34, 12, 14);
    rw->setGeometry(133, 123, 126, 34);
    connect(sw, &QPushButton::clicked, this, &MainWindow::openSpecialWeaponDebugger);
    connect(rw, &QPushButton::clicked, this, &MainWindow::openRecoilDebugger);
    auto* lab = new QLabel(QStringLiteral("--启用特殊武器助手--"), tab);
    lab->setAlignment(Qt::AlignCenter);
    lab->setGeometry(92, 164, 78, 25);
    lab->setStyleSheet("color:#6B7280;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;");
    QStringList names{QStringLiteral("迫击炮"), QStringLiteral("火箭筒"), QStringLiteral("投掷物"), "VSS", QStringLiteral("十字弩"), "C4"};
    QStringList keys{"mortar", "rocket", "throwables", "vss", "crossbow", "c4"};
    for (int i = 0; i < names.size(); ++i) {
        auto* b = new RoundedButton(names[i], tab);
        b->configure(126, 29, 12, 14);
        b->setToggleMode(true);
        b->setGeometry(3 + (i % 2) * 130, 198 + (i / 2) * 35, 126, 29);
        assistant_buttons_[keys[i]] = b;
        connect(b, &QPushButton::clicked, this, [this, key = keys[i]] { toggleAssistant(key); });
    }
}

void MainWindow::buildCalibrationTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    auto square_keys = QSet<QString>{
        "minimap_region", "largemap_region",
        "weapon1_number_region", "weapon2_number_region",
        "weapon1_scope_region", "weapon1_grip_region", "weapon1_muzzle_region", "weapon1_stock_region",
        "weapon2_scope_region", "weapon2_grip_region", "weapon2_muzzle_region", "weapon2_stock_region",
    };
    auto add = [&](const QString& name, const QString& key, bool scale, int row, int col) {
        auto* b = new RoundedButton(name, tab);
        b->configure(82, 33, 12, 13);
        b->setGeometry(2 + col * 87 + (col == 2 ? 1 : 0), 41 + (row - 1) * 37, 82, 33);
        connect(b, &QPushButton::clicked, this, [this, key, scale, square_keys] {
            auto* ov = new RegionCalibrationOverlay(regions_, key, scale ? RegionCalibrationOverlay::Mode::Scale : RegionCalibrationOverlay::Mode::Region, square_keys.contains(key));
            if (debug_overlay_.created()) {
                debug_overlay_.clear();
                debug_overlay_.show(false);
            }
            connect(ov, &RegionCalibrationOverlay::calibrationClosed, this, [this](bool changed) {
                if (debug_overlay_enabled_) {
                    if (!debug_overlay_.created()) {
                        debug_overlay_.create(L"PUBGAssistant DebugRegions", regions_.screenWidth(), regions_.screenHeight(), true);
                    }
                    debug_overlay_.show(true);
                    if (changed) drawDebugOverlay();
                }
            });
            ov->show();
        });
    };
    btn_debug_ = new RoundedButton(QStringLiteral("显示所有区域框"), tab);
    btn_debug_->configure(125, 36, 12, 16);
    btn_debug_->setToggleMode(true);
    btn_debug_->setGeometry(3, 0, 125, 36);
    auto* top = new RoundedButton(QStringLiteral("调试缩放比例"), tab);
    top->configure(125, 36, 12, 16);
    top->setGeometry(134, 0, 125, 36);
    connect(btn_debug_, &QPushButton::clicked, this, &MainWindow::toggleDebugOverlay);
    connect(top, &QPushButton::clicked, this, &MainWindow::openScaleCalibrator);
    QVector<QVector<std::tuple<QString, bool, QString>>> rows{
        {{QStringLiteral("大地图"), false, "largemap_region"}, {QStringLiteral("小地图"), false, "minimap_region"}, {QStringLiteral("1km比例尺"), true, "largemap_1km_px"}},
        {{QStringLiteral("武器1编号"), false, "weapon1_number_region"}, {QStringLiteral("武器2编号"), false, "weapon2_number_region"}, {QStringLiteral("垂直测高"), false, "elevation_region"}},
        {{QStringLiteral("武器1名称"), false, "weapon1_name_region"}, {QStringLiteral("武器2名称"), false, "weapon2_name_region"}, {QStringLiteral("武器图标"), false, "weapon_region"}},
        {{QStringLiteral("武器1倍镜"), false, "weapon1_scope_region"}, {QStringLiteral("武器2倍镜"), false, "weapon2_scope_region"}, {QStringLiteral("姿势区域"), false, "stance_region"}},
        {{QStringLiteral("武器1枪口"), false, "weapon1_muzzle_region"}, {QStringLiteral("武器2枪口"), false, "weapon2_muzzle_region"}, {QStringLiteral("四倍镜内边"), false, "scope_top_edge_4x_region"}},
        {{QStringLiteral("武器1握把"), false, "weapon1_grip_region"}, {QStringLiteral("武器2握把"), false, "weapon2_grip_region"}, {QStringLiteral("六倍镜内边"), false, "scope_top_edge_6x_region"}},
        {{QStringLiteral("武器1枪托"), false, "weapon1_stock_region"}, {QStringLiteral("武器2枪托"), false, "weapon2_stock_region"}, {QStringLiteral("八倍镜内边"), false, "scope_top_edge_8x_region"}},
    };
    for (int r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < rows[r].size(); ++c) {
            add(std::get<0>(rows[r][c]), std::get<2>(rows[r][c]), std::get<1>(rows[r][c]), r + 1, c);
        }
    }
}

void MainWindow::buildKeyTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    QVector<std::tuple<QString, QString, bool>> rows{
        {QStringLiteral("武器检测开关"), "toggle_weapon_detection", true},
        {QStringLiteral("测距瞄准显示开关"), "toggle_display", true},
        {QStringLiteral("辅助压枪开关"), "toggle_recoil", true},
        {QStringLiteral("大地图测距"), "measure_map", true},
        {QStringLiteral("窗口显示开关"), "toggle_window", true},
        {QStringLiteral("打开装备栏"), "toggle_equipment", true},
        {QStringLiteral("开火按键"), "fire_key", true},
        {QStringLiteral("地图点位显示"), "mouse_map_assist", false},
        {QStringLiteral("标点前后切换"), "marker_pair", true},
    };
    const auto hotkeys = config_.hotkeys();
    auto hotkeyValue = [&](const QString& action, const std::string& fallback) {
        auto it = hotkeys.find(action.toStdString());
        return it == hotkeys.end() ? fallback : it->second;
    };
    // 10 行紧凑排布：行距压到 26px，最后一行约到 y=263，正好压在底部保存/恢复按钮(y=267)之上，不溢出窗口。
    const int row_step = 26;
    const int value_x = 126;
    const int record_x = 209;
    const int record_h = 22;
    for (int row_index = 0; row_index < rows.size(); ++row_index) {
        const auto& row = rows[row_index];
        const QString label_text = std::get<0>(row);
        const QString action = std::get<1>(row);
        const bool editable = std::get<2>(row);
        const int y = 3 + row_index * row_step;
        auto* line = new QWidget(tab);
        line->setGeometry(0, y, 262, 24);

        auto* desc = new QLabel(label_text, tab);
        desc->setGeometry(0, y, 120, 24);
        desc->setStyleSheet("color:#333333;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;");
        if (action == "marker_pair") {
            auto* prev = new QLabel(formatHotkey(hotkeyValue("marker_prev", "q")), tab);
            auto* next = new QLabel(formatHotkey(hotkeyValue("marker_next", "e")), tab);
            prev->setGeometry(value_x, y + 2, 28, 20);
            next->setGeometry(value_x + 68, y + 2, 28, 20);
            prev->setStyleSheet("color:#2563EB;font-family:Consolas;font-size:13px;font-weight:700;");
            next->setStyleSheet("color:#2563EB;font-family:Consolas;font-size:13px;font-weight:700;");
            hotkey_labels_["marker_prev"] = prev;
            hotkey_labels_["marker_next"] = next;
            auto* prev_btn = new RoundedButton(QStringLiteral("录制"), tab);
            prev_btn->configure(44, record_h, 11, 13);
            prev_btn->setGeometry(value_x + 24, y + 1, 44, record_h);
            auto* next_btn = new RoundedButton(QStringLiteral("录制"), tab);
            next_btn->configure(50, record_h, 11, 13);
            next_btn->setGeometry(record_x, y + 1, 50, record_h);
            connect(prev_btn, &QPushButton::clicked, this, [this] { beginCaptureHotkey("marker_prev"); });
            connect(next_btn, &QPushButton::clicked, this, [this] { beginCaptureHotkey("marker_next"); });
        } else {
            const std::string fallback = action == "fire_key" ? "end"
                : action == "toggle_window" ? "<home>"
                : "";
            const QString display = editable
                ? formatHotkey(hotkeyValue(action, fallback))
                : QStringLiteral("鼠标左键 + 中键");
            auto* value = new QLabel(display, tab);
            value->setGeometry(value_x, y + 2, editable ? 80 : 132, 20);
            value->setStyleSheet("color:#2563EB;font-family:Consolas;font-size:13px;font-weight:700;");
            if (editable) {
                hotkey_labels_[action] = value;
                auto* rec = new RoundedButton(QStringLiteral("录制"), tab);
                rec->configure(50, record_h, 11, 13);
                rec->setGeometry(record_x, y + 1, 50, record_h);
                connect(rec, &QPushButton::clicked, this, [this, action] { beginCaptureHotkey(action); });
            }
        }
    }
    auto* btns = new QWidget(tab);
    btns->setGeometry(1, 267, 260, 28);
    auto* save = new RoundedButton(QStringLiteral("保存快捷键"), tab);
    auto* reset = new RoundedButton(QStringLiteral("恢复默认"), tab);
    save->configure(126, 28, 12, 14);
    reset->configure(126, 28, 12, 14);
    save->setGeometry(3, 267, 126, 28);
    reset->setGeometry(133, 267, 126, 28);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveHotkeys);
    connect(reset, &QPushButton::clicked, this, &MainWindow::resetDefaultHotkeys);
}

void MainWindow::toggleWeaponDetection() {
    weapon_detection_enabled_ = btn_weapon_detect_ ? btn_weapon_detect_->active() : !weapon_detection_enabled_;
    if (callbacks_.set_weapon_detection) callbacks_.set_weapon_detection(weapon_detection_enabled_);
    else {
        weapon_detector_.setEnabled(weapon_detection_enabled_);
        equipment_detector_.setEnabled(weapon_detection_enabled_);
        gesture_identifier_.setEnabled(weapon_detection_enabled_);
    }
    if (btn_weapon_detect_) btn_weapon_detect_->setText(weapon_detection_enabled_ ? QStringLiteral("关闭武器检测") : QStringLiteral("开启武器检测"));
}

void MainWindow::toggleDisplay() {
    display_enabled_ = btn_display_ ? btn_display_->active() : !display_enabled_;
    if (callbacks_.set_display) callbacks_.set_display(display_enabled_);
    else {
        minimap_.setEnabled(display_enabled_);
        minimap_.setDisplay(display_enabled_);
        elevation_.setEnabled(display_enabled_);
        special_.setDisplayEnabled(display_enabled_);
        large_map_.setDisplay(display_enabled_);
    }
    if (btn_display_) btn_display_->setText(display_enabled_ ? QStringLiteral("关闭瞄准辅助") : QStringLiteral("开启瞄准辅助"));
}

void MainWindow::toggleRecoil() {
    recoil_enabled_ = btn_recoil_ ? btn_recoil_->active() : !recoil_enabled_;
    if (callbacks_.set_recoil) callbacks_.set_recoil(recoil_enabled_);
    else recoil_.setEnabled(recoil_enabled_);
    if (btn_recoil_) btn_recoil_->setText(recoil_enabled_ ? QStringLiteral("关闭辅助压枪") : QStringLiteral("开启辅助压枪"));
}

void MainWindow::toggleAssistant(const QString& key) {
    bool active = assistant_buttons_[key]->active();
    if (!display_enabled_) {
        assistant_buttons_[key]->setActive(false);
        active = false;
    }
    if (callbacks_.set_assistant) {
        callbacks_.set_assistant(key.toStdString(), active);
        return;
    }
    special_.setManualEnabled(key.toStdString(), active);
    if (key == "throwables") throwables_.setEnabled(active);
    if (key == "c4") c4_.setEnabled(active);
}

void MainWindow::selectMap(int index) {
    QStringList full{QStringLiteral("艾伦格 (Erangel)"), QStringLiteral("米拉玛 (Miramar)"), QStringLiteral("泰戈 (Taego)"), QStringLiteral("荣都 (Rondo)"), QStringLiteral("帝斯顿 (Deston)"), QStringLiteral("维寒迪 (Vikendi)")};
    for (int i = 0; i < map_buttons_.size(); ++i) map_buttons_[i]->setActive(i == index);
    if (index >= 0 && index < full.size()) map_points_.setMap(full[index].toStdString());
}

void MainWindow::selectMarkerSize(int index) {
    QStringList keys{"small", "medium", "large"};
    for (int i = 0; i < size_buttons_.size(); ++i) size_buttons_[i]->setActive(i == index);
    if (index >= 0 && index < keys.size()) map_points_.setMarkerSize(keys[index].toStdString());
}

void MainWindow::selectPntColorMode(const QString& mode) {
    for (auto it = pnt_mode_buttons_.begin(); it != pnt_mode_buttons_.end(); ++it) {
        it.value()->setActive(it.key() == mode);
    }
    config_.write([&](Json& data) {
        data["pnt_color_mode"] = mode.toStdString();
        if (data.contains("pnt_color_modes") && data["pnt_color_modes"].contains(mode.toStdString())) {
            data["pnt_colors"] = data["pnt_color_modes"][mode.toStdString()];
        }
    });
    config_.save();
    if (callbacks_.sync_marker_colors) callbacks_.sync_marker_colors();
}

void MainWindow::toggleDebugOverlay() {
    debug_overlay_enabled_ = btn_debug_ ? btn_debug_->active() : !debug_overlay_enabled_;
    if (!debug_overlay_.created()) {
        debug_overlay_.create(L"PUBGAssistant DebugRegions", regions_.screenWidth(), regions_.screenHeight(), true);
    }
    debug_overlay_.show(debug_overlay_enabled_);
    if (debug_overlay_enabled_) drawDebugOverlay();
    else debug_overlay_.clear();
}

void MainWindow::drawDebugOverlay() {
    std::vector<OverlayCommand> cmds;
    const auto region_names = config_.read([](const Json& data) {
        std::vector<std::string> names;
        if (data.contains("real_regions") && data["real_regions"].is_object()) {
            for (auto it = data["real_regions"].begin(); it != data["real_regions"].end(); ++it) {
                names.push_back(it.key());
            }
        }
        return names;
    });
    for (const auto& region_name : region_names) {
            const auto rect = regions_.getRealRegion(region_name);
            if (!rect) continue;
            cv::Scalar color = cv::Scalar(255, 255, 255);
            if (region_name.find("weapon") != std::string::npos) {
                if (region_name.find("number") != std::string::npos) color = cv::Scalar(255, 107, 53);
                else if (region_name.find("name") != std::string::npos) color = cv::Scalar(255, 159, 28);
                else if (region_name.find("scope") != std::string::npos || region_name.find("grip") != std::string::npos ||
                         region_name.find("muzzle") != std::string::npos || region_name.find("stock") != std::string::npos) {
                    color = cv::Scalar(247, 37, 133);
                } else {
                    color = cv::Scalar(114, 9, 183);
                }
            } else if (region_name.find("mini") != std::string::npos) {
                color = cv::Scalar(0, 180, 216);
            } else if (region_name.find("large") != std::string::npos) {
                color = cv::Scalar(0, 119, 182);
            } else if (region_name.find("stance") != std::string::npos) {
                color = cv::Scalar(46, 196, 182);
            } else if (region_name.find("elevation") != std::string::npos) {
                color = cv::Scalar(131, 56, 236);
            } else if (region_name.find("crosshair") != std::string::npos || region_name.find("scope_top_edge") != std::string::npos) {
                color = cv::Scalar(255, 0, 110);
            }
            cmds.push_back({OverlayCommand::Type::Rect, static_cast<double>(rect->left), static_cast<double>(rect->top),
                            static_cast<double>(rect->left + rect->width), static_cast<double>(rect->top + rect->height),
                            0, "", color, 1});
            cmds.push_back({OverlayCommand::Type::Text, static_cast<double>(rect->left + 5), static_cast<double>(std::max(5, rect->top - 16)),
                            0, 0, 0, region_name, color, 1, 13});
    }
    const double large_scale = regions_.getRealScale("largemap_1km_px", 0.0);
    cmds.push_back({OverlayCommand::Type::Text, regions_.screenWidth() / 2.0 - 110.0, 50.0, 0, 0, 0,
                    "大地图 1km = " + std::to_string(static_cast<int>(std::round(large_scale))) + " px",
                    {178, 209, 0}, 1, 16});
    debug_overlay_.setCommands(std::move(cmds));
    debug_overlay_.pumpMessages();
}

void MainWindow::openScaleCalibrator() {
    if (!scale_calibration_window_) {
        scale_calibration_window_ = new ScaleCalibrationWindow(config_, regions_, this);
        scale_calibration_window_->setWindowFlag(Qt::Window, true);
    }
    scale_calibration_window_->show();
    scale_calibration_window_->raise();
    scale_calibration_window_->activateWindow();
}

void MainWindow::openRecoilDebugger() {
    auto* window = new RecoilDebuggerWindow(config_, recoil_);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    window->setWindowFlag(Qt::Window, true);
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::openSpecialWeaponDebugger() {
    auto* window = new SpecialWeaponDebuggerWindow(config_);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    window->setWindowFlag(Qt::Window, true);
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::mousePressEvent(QMouseEvent* event) { drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft(); }
void MainWindow::mouseMoveEvent(QMouseEvent* event) { if (event->buttons() & Qt::LeftButton) move(event->globalPosition().toPoint() - drag_offset_); }
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (!capturing_action_.isEmpty()) {
        const QString key = supportedHotkeyFromEvent(event);
        if (!key.isEmpty()) captureHotkey(key, activeModifierNames(event));
        return;
    }
    if (event->key() == Qt::Key_Left) switchTab(-1);
    if (event->key() == Qt::Key_Right) switchTab(1);
}

#ifdef _WIN32
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (!capturing_action_.isEmpty() && eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg && (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN)) {
            const QString key = supportedHotkeyFromWindowsMessage(msg);
            if (!key.isEmpty()) {
                const bool captured = captureHotkey(key, activeModifierNames());
                if (captured) {
                    if (result) *result = 0;
                    return true;
                }
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::beginCaptureHotkey(const QString& action) {
    stopCaptureHook();
    capturing_action_ = action;
    if (hotkey_labels_.contains(action)) hotkey_labels_[action]->setText(QStringLiteral("请按下..."));
    setFocus(Qt::OtherFocusReason);
    startCaptureHook();
}

bool MainWindow::captureHotkey(const QString& key, const QStringList& modifiers) {
    if (capturing_action_.isEmpty() || key.isEmpty()) return false;

    const bool single_only = capturing_action_ == "throw" || capturing_action_ == "toggle_equipment" || capturing_action_ == "fire_key";
    if (single_only && !modifiers.isEmpty()) {
        const QString action = capturing_action_;
        if (hotkey_labels_.contains(action)) {
            hotkey_labels_[action]->setText(QStringLiteral("仅允许单键"));
            QTimer::singleShot(1000, this, [this, action] {
                if (hotkey_labels_.contains(action)) {
                    const auto stored = config_.read([&](const Json& data) {
                        return data.value("hotkeys", Json::object()).value(action.toStdString(), std::string(""));
                    });
                    hotkey_labels_[action]->setText(formatHotkey(stored));
                }
            });
        }
        capturing_action_.clear();
        stopCaptureHook();
        return true;
    }

    QString combo = key;
    if (!modifiers.isEmpty()) {
        combo = modifiers.join("+") + "+" + key;
    }
    const auto existing_hotkeys = config_.hotkeys();
    const std::string combo_std = combo.toStdString();
    const std::string action_std = capturing_action_.toStdString();
    auto hotkeyOr = [&](const std::string& key_name, const std::string& fallback) {
        const auto it = existing_hotkeys.find(key_name);
        return it == existing_hotkeys.end() ? fallback : it->second;
    };
    const bool conflict = std::any_of(existing_hotkeys.begin(), existing_hotkeys.end(), [&](const auto& item) {
        return item.first != action_std && item.second == combo_std;
    });
    if (conflict || (action_std == "fire_key" && combo_std == hotkeyOr("toggle_window", "<home>")) ||
        (action_std == "toggle_window" && combo_std == hotkeyOr("fire_key", "end"))) {
        const QString action = capturing_action_;
        if (hotkey_labels_.contains(action)) {
            hotkey_labels_[action]->setText(QStringLiteral("按键冲突"));
            QTimer::singleShot(1000, this, [this, action] {
                if (hotkey_labels_.contains(action)) {
                    const auto stored = config_.read([&](const Json& data) {
                        return data.value("hotkeys", Json::object()).value(action.toStdString(), std::string(""));
                    });
                    hotkey_labels_[action]->setText(formatHotkey(stored));
                }
            });
        }
        capturing_action_.clear();
        stopCaptureHook();
        return true;
    }

    const QString action = capturing_action_;
    config_.write([&](Json& data) {
        data["hotkeys"][action.toStdString()] = combo.toStdString();
    });
    if (hotkey_labels_.contains(action)) hotkey_labels_[action]->setText(formatHotkey(combo.toStdString()));
    capturing_action_.clear();
    stopCaptureHook();
    config_.save();
    if (callbacks_.reload_hotkeys) callbacks_.reload_hotkeys();
    return true;
}

#ifdef _WIN32
LRESULT CALLBACK MainWindow::captureKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    auto* self = s_capture_window_;
    if (nCode == HC_ACTION && self && !self->capturing_action_.isEmpty()) {
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        if (down) {
            const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if (kb) {
                const QString key = supportedHotkeyFromLowLevelHook(kb);
                if (!key.isEmpty()) {
                    const QStringList modifiers = activeModifierNames();
                    QMetaObject::invokeMethod(self, [self, key, modifiers] {
                        self->handleCapturedHardwareKey(key, modifiers);
                    }, Qt::QueuedConnection);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void MainWindow::startCaptureHook() {
    if (capture_hook_) return;
    s_capture_window_ = this;
    capture_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &MainWindow::captureKeyboardProc,
                                      GetModuleHandleW(nullptr), 0);
}

void MainWindow::stopCaptureHook() {
    if (capture_hook_) {
        UnhookWindowsHookEx(capture_hook_);
        capture_hook_ = nullptr;
    }
    if (s_capture_window_ == this) {
        s_capture_window_ = nullptr;
    }
}

void MainWindow::handleCapturedHardwareKey(QString key, QStringList modifiers) {
    captureHotkey(key, modifiers);
}
#endif

void MainWindow::saveHotkeys() {
    config_.save();
    if (callbacks_.reload_hotkeys) callbacks_.reload_hotkeys();
}

void MainWindow::closeEvent(QCloseEvent* event) {
#ifdef _WIN32
    stopCaptureHook();
#endif
    if (!closing_) {
        closing_ = true;
        QApplication::quit();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    applyNativeRoundedRegion();
}

void MainWindow::resetDefaultHotkeys() {
    config_.write([](Json& data) {
        data["hotkeys"] = Json{
            {"toggle_window", "<home>"},
            {"throw", "b"},
            {"toggle_weapon_detection", "<f1>"},
            {"toggle_display", "<f2>"},
            {"toggle_recoil", "<f3>"},
            {"measure_map", "<f4>"},
            {"marker_prev", "q"},
            {"marker_next", "e"},
            {"toggle_equipment", "tab"},
            {"fire_key", "end"},
        };
    });
    config_.save();
    if (callbacks_.reload_hotkeys) callbacks_.reload_hotkeys();
    const auto hotkeys = config_.hotkeys();
    for (auto it = hotkey_labels_.begin(); it != hotkey_labels_.end(); ++it) {
        const auto key = it.key().toStdString();
        if (auto hit = hotkeys.find(key); hit != hotkeys.end()) it.value()->setText(formatHotkey(hit->second));
    }
}

QString MainWindow::formatHotkey(const std::string& value) const {
    QString s = QString::fromStdString(value).replace("<", "").replace(">", "");
    return s.toUpper();
}

} // namespace pubg::ui
