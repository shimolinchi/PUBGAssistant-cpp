#include "ui/MainWindow.hpp"

#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QFrame>
#include <QFont>
#include <QLineEdit>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSizePolicy>
#include <QDesktopServices>
#include <QEvent>
#include <QStringList>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "ui/RecoilDebuggerWindow.hpp"
#include "ui/RegionCalibrationOverlay.hpp"
#include "ui/ScaleCalibrationWindow.hpp"
#include "ui/SpecialWeaponDebuggerWindow.hpp"
#include "ui/DisplaySettingsWindow.hpp"
#include "ui/Theme.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pubg::ui {

#ifdef _WIN32
MainWindow* MainWindow::s_capture_window_ = nullptr;
#endif

namespace {

class ChildGeometryFollower final : public QObject {
public:
    ChildGeometryFollower(QWidget* child, QMargins margins, QObject* parent)
        : QObject(parent), child_(child), margins_(margins) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            if (auto* parent = qobject_cast<QWidget*>(watched); parent && child_) {
                child_->setGeometry(margins_.left(), margins_.top(),
                                    std::max(1, parent->width() - margins_.left() - margins_.right()),
                                    std::max(1, parent->height() - margins_.top() - margins_.bottom()));
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* child_ = nullptr;
    QMargins margins_;
};

class HoverCallbackFilter final : public QObject {
public:
    HoverCallbackFilter(std::function<void()> callback, QObject* parent)
        : QObject(parent), callback_(std::move(callback)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Enter && callback_) {
            callback_();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> callback_;
};

class RoundedPreviewWidget final : public QWidget {
public:
    explicit RoundedPreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 270);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setColors(const QString& background, const QString& border, const QString& text) {
        background_ = QColor(background);
        border_ = QColor(border);
        text_ = QColor(text);
        update();
    }

    void setPreviewImage(const QImage& image) {
        image_ = image;
        update();
    }

    void setPixmap(const QPixmap& pixmap) {
        image_ = pixmap.isNull() ? QImage{} : pixmap.toImage();
        if (!pixmap.isNull()) {
            message_.clear();
        }
        update();
    }

    void setMessage(const QString& message) {
        message_ = message;
        update();
    }

    void setText(const QString& text) {
        message_ = text;
        if (!text.isEmpty()) {
            image_ = {};
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF outer = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath outer_path;
        outer_path.addRoundedRect(outer, 8, 8);
        painter.fillPath(outer_path, background_);
        painter.setPen(QPen(border_, 1));
        painter.drawPath(outer_path);

        const QRectF content = rect().adjusted(1.0, 1.0, -1.0, -1.0);
        QPainterPath clip_path;
        clip_path.addRoundedRect(content, 7, 7);
        painter.setClipPath(clip_path);

        if (!image_.isNull()) {
            const QSize target_size = image_.size().scaled(content.size().toSize(), Qt::KeepAspectRatioByExpanding);
            QRectF target(QPointF(0, 0), QSizeF(target_size));
            target.moveCenter(content.center());
            painter.drawImage(target, image_);
            return;
        }

        painter.setClipping(false);
        painter.setPen(text_);
        QFont font(QStringLiteral("Microsoft YaHei"), 10);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(rect().adjusted(16, 12, -16, -12), Qt::AlignCenter | Qt::TextWordWrap, message_);
    }

private:
    QColor background_ = QColor(255, 255, 255, 210);
    QColor border_ = QColor(255, 255, 255);
    QColor text_ = QColor(17, 24, 39);
    QImage image_;
    QString message_;
};

QIcon appIconFromPath(const std::filesystem::path& icon_path) {
    if (!std::filesystem::exists(icon_path)) {
        return {};
    }
    return QIcon(QString::fromStdWString(icon_path.wstring()));
}

cv::Mat loadPreviewImage(const QString& image_path) {
    const std::filesystem::path path = std::filesystem::path(image_path.toStdWString());
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return {};
    }
    return cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
}

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
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) return QStringLiteral("<enter>");
    if (event->key() == Qt::Key_Space) return QStringLiteral("<space>");
    if (event->key() == Qt::Key_End) return QStringLiteral("end");
    if (event->key() == Qt::Key_Home) return QStringLiteral("<home>");
    if (event->key() == Qt::Key_Left) return QStringLiteral("<left>");
    if (event->key() == Qt::Key_Up) return QStringLiteral("<up>");
    if (event->key() == Qt::Key_Right) return QStringLiteral("<right>");
    if (event->key() == Qt::Key_Down) return QStringLiteral("<down>");
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
    if (vk == VK_RETURN) return QStringLiteral("<enter>");
    if (vk == VK_SPACE) return QStringLiteral("<space>");
    if (vk == VK_END) return QStringLiteral("end");
    if (vk == VK_HOME) return QStringLiteral("<home>");
    if (vk == VK_LEFT) return QStringLiteral("<left>");
    if (vk == VK_UP) return QStringLiteral("<up>");
    if (vk == VK_RIGHT) return QStringLiteral("<right>");
    if (vk == VK_DOWN) return QStringLiteral("<down>");
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
    if (vk == VK_RETURN) return QStringLiteral("<enter>");
    if (vk == VK_SPACE) return QStringLiteral("<space>");
    if (vk == VK_END) return QStringLiteral("end");
    if (vk == VK_HOME) return QStringLiteral("<home>");
    if (vk == VK_LEFT) return QStringLiteral("<left>");
    if (vk == VK_UP) return QStringLiteral("<up>");
    if (vk == VK_RIGHT) return QStringLiteral("<right>");
    if (vk == VK_DOWN) return QStringLiteral("<down>");
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
    const QIcon app_icon = appIconFromPath(config_.paths().iconFile());
    if (!app_icon.isNull()) {
        QApplication::setWindowIcon(app_icon);
        setWindowIcon(app_icon);
    }
    setFixedSize(280, 372);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setWindowOpacity(themedWindowOpacity(config_));
    setAttribute(Qt::WA_TranslucentBackground, true);
    central_ = new QWidget(this);
    central_->setObjectName("windowFrame");
    setCentralWidget(central_);

    buildTitleBar(central_);

    tab_bar_ = new QWidget(central_);
    tab_bar_->setGeometry(9, 32, 262, 28);

    pages_ = new QStackedWidget(central_);
    pages_->setFrameShape(QFrame::NoFrame);
    pages_->setGeometry(9, 64, 262, 299);

    buildMapTab(addTab(QStringLiteral("地图点位")));
    buildLaunchTab(addTab(QStringLiteral("启动助手")));
    buildCalibrationTab(addTab(QStringLiteral("系统设置")));
    buildHelpTab(addTab(QStringLiteral("使用说明")));
    applyTheme();
    selectTab(0);
    QTimer::singleShot(0, this, [this] {
        applyNativeWindowIcon();
        applyNativeRoundedRegion();
        applyCaptureExclusion();
    });
}

std::string regionDisplayName(const std::string& key) {
    static const std::vector<std::pair<const char*, const char*>> names{
        {"largemap_region", "大地图"},
        {"minimap_region", "小地图"},
        {"largemap_1km_px", "1km比例"},
        {"weapon1_number_region", "武器1编号"},
        {"weapon2_number_region", "武器2编号"},
        {"elevation_region", "垂直测高"},
        {"weapon1_name_region", "武器1名称"},
        {"weapon2_name_region", "武器2名称"},
        {"weapon_region", "武器图标"},
        {"weapon1_scope_region", "武器1倍镜"},
        {"weapon2_scope_region", "武器2倍镜"},
        {"stance_region", "姿势区域"},
        {"weapon1_muzzle_region", "武器1枪口"},
        {"weapon2_muzzle_region", "武器2枪口"},
        {"scope_top_edge_4x_region", "四倍镜内边"},
        {"weapon1_grip_region", "武器1握把"},
        {"weapon2_grip_region", "武器2握把"},
        {"scope_top_edge_6x_region", "六倍镜内边"},
        {"weapon1_stock_region", "武器1枪托"},
        {"weapon2_stock_region", "武器2枪托"},
        {"scope_top_edge_8x_region", "八倍镜内边"},
        {"mortar_mount_region", "迫击炮上炮"},
        {"compass_region", "顶部方向"},
        {"crosshair_region", "准星区域"},
    };
    for (const auto& [name_key, display] : names) {
        if (key == name_key) {
            return display;
        }
    }
    return key;
}

void showWindowInsideScreen(QWidget* window) {
    if (!window) return;
    window->show();

    QScreen* screen = window->screen();
    if (!screen) {
        const QPoint center = window->frameGeometry().center();
        screen = QGuiApplication::screenAt(center);
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen) {
        const QRect available = screen->availableGeometry().adjusted(8, 8, -8, -8);
        QSize size = window->size();
        if (size.width() > available.width() || size.height() > available.height()) {
            size = size.boundedTo(available.size());
            window->resize(size);
        }
        QPoint pos = window->pos();
        pos.setX(std::clamp(pos.x(), available.left(), std::max(available.left(), available.right() - window->width() + 1)));
        pos.setY(std::clamp(pos.y(), available.top(), std::max(available.top(), available.bottom() - window->height() + 1)));
        window->move(pos);
    }

    window->raise();
    window->activateWindow();
}

void MainWindow::applyTheme() {
    const auto theme = currentUiTheme(config_);
    setWindowOpacity(themedWindowOpacity(config_));
    RoundedButton::setThemeColors(theme.button, theme.button_hover, theme.button_pressed,
                                  theme.button_active, theme.border, theme.button_text);
    if (central_) {
        central_->setStyleSheet(QStringLiteral(
            "#windowFrame{background:%1;border:1px solid %2;border-radius:18px;}"
            "QWidget{background:transparent;border:0;}"
            "QStackedWidget{background:transparent;border:0;}"
            "QLabel{background:transparent;border:0;color:%3;}"
        ).arg(theme.frame, theme.border, theme.label));
    }
    if (pages_) {
        for (int i = 0; i < pages_->count(); ++i) {
            if (auto* page = pages_->widget(i)) {
                page->setStyleSheet(QStringLiteral("background:%1;").arg(theme.page));
            }
        }
    }
    const auto label_style = QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;").arg(theme.label);
    const auto value_style = QStringLiteral("color:%1;font-family:Consolas;font-size:13px;font-weight:700;").arg(theme.accent);
    const auto labels = findChildren<QLabel*>();
    for (auto* label : labels) {
        if (!label->styleSheet().contains("Consolas")) {
            label->setStyleSheet(label_style);
        } else {
            label->setStyleSheet(value_style);
        }
    }
    const auto buttons = findChildren<RoundedButton*>();
    for (auto* button : buttons) {
        button->update();
    }
}

void MainWindow::applyNativeWindowIcon() {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    const auto icon_path = config_.paths().iconFile();
    if (!std::filesystem::exists(icon_path)) return;
    const auto wide_path = icon_path.wstring();
    HICON big_icon = static_cast<HICON>(LoadImageW(nullptr, wide_path.c_str(), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
    HICON small_icon = static_cast<HICON>(LoadImageW(nullptr, wide_path.c_str(), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
    if (big_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
    if (small_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }
#endif
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
    const QIcon app_icon = windowIcon();
    const int title_x = app_icon.isNull() ? 10 : 30;
    if (!app_icon.isNull()) {
        auto* icon = new QLabel(bar);
        icon->setGeometry(10, 7, 16, 16);
        icon->setPixmap(app_icon.pixmap(16, 16));
        icon->setScaledContents(true);
    }
    auto* title = new QLabel(QStringLiteral("PUBG 战术助手"), bar);
    title->setGeometry(title_x, 2, 150, 25);
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
    const auto ui_state = config_.read([](const Json& data) {
        return data.value("ui_state", Json::object());
    });
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
        const auto ui = data.value("ui_state", Json::object());
        return ui.value("pnt_color_mode", data.value("pnt_color_mode", std::string("normal")));
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
    const Json category_state = ui_state.value("map_point_categories", Json::object());
    for (int i = 0; i < 4; ++i) {
        auto* b = new RoundedButton(groups[i], group_frame);
        b->configure(61, 30, 12, 13);
        b->setToggleMode(true);
        const bool active = category_state.value(keys[i].toStdString(), true);
        b->setActive(active);
        b->setGeometry(2 + i * 65, 0, 61, 30);
        group_buttons_[keys[i]] = b;
        const std::string key = keys[i].toStdString();
        map_points_.setCategoryEnabled(key, active);
        connect(b, &QPushButton::clicked, this, [this, key, b] {
            const bool active = b->active();
            map_points_.setCategoryEnabled(key, active);
            config_.write([&](Json& data) {
                data["ui_state"]["map_point_categories"][key] = active;
            });
            config_.save();
        });
    }
    selectMap(std::clamp(ui_state.value("map_index", 0), 0, static_cast<int>(maps.size()) - 1));
    selectMarkerSize(std::clamp(ui_state.value("marker_size_index", 1), 0, 2));
}
void MainWindow::buildLaunchTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    int button_y = 3;
    auto addBtn = [&](const QString& text, auto slot) {
        auto* b = new RoundedButton(text, tab);
        b->configure(256, 28, 12, 14);
        b->setToggleMode(true);
        b->setGeometry(3, button_y, 256, 28);
        button_y += 33;
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    btn_weapon_detect_ = addBtn(QStringLiteral("关闭武器检测"), [this] { toggleWeaponDetection(); });
    btn_weapon_detect_->setActive(true);
    btn_display_ = addBtn(QStringLiteral("开启瞄准辅助"), [this] { toggleDisplay(); });
    btn_recoil_ = addBtn(QStringLiteral("开启辅助压枪"), [this] { toggleRecoil(); });
    const auto switch_state = config_.read([](const Json& data) {
        return data.value("ui_state", Json::object()).value("switches", Json::object());
    });
    setWeaponDetectionState(switch_state.value("weapon_detection", true));
    setDisplayState(switch_state.value("display", false));
    setRecoilState(switch_state.value("recoil", false));

    auto* lab = new QLabel(QStringLiteral("功能开关"), tab);
    lab->setAlignment(Qt::AlignCenter);
    lab->setGeometry(3, 104, 256, 22);
    lab->setStyleSheet("color:#6B7280;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;");

    QStringList names{
        QStringLiteral("迫击炮"), QStringLiteral("火箭筒"),
        QStringLiteral("投掷物"), QStringLiteral("VSS"),
        QStringLiteral("十字弩"), QStringLiteral("C4"),
        QStringLiteral("全自动压枪"), QStringLiteral("连狙点压"),
        QStringLiteral("狙呼吸晃动稳定"), QStringLiteral("自动闪身喷"),
    };
    QStringList keys{
        "mortar", "rocket", "throwables", "vss", "crossbow", "c4",
        "auto_recoil", "dmr_tap", "sr_breath", "sg_peek",
    };
    const Json assistant_state = config_.read([](const Json& data) {
        return data.value("ui_state", Json::object()).value("assistants", Json::object());
    });
    for (int i = 0; i < names.size(); ++i) {
        auto* b = new RoundedButton(names[i], tab);
        b->configure(126, 28, 12, 13);
        b->setToggleMode(true);
        b->setActive(assistant_state.value(keys[i].toStdString(), true));
        b->setGeometry(3 + (i % 2) * 130, 132 + (i / 2) * 34, 126, 28);
        assistant_buttons_[keys[i]] = b;
        connect(b, &QPushButton::clicked, this, [this, key = keys[i]] { toggleAssistant(key); });
    }
}
void MainWindow::buildCalibrationTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    QVector<std::pair<QString, std::function<void()>>> actions{
        {QStringLiteral("显示所有区域框"), [this] { toggleDebugOverlay(); }},
        {QStringLiteral("识别区域设置"), [this] { openRegionCalibrationWindow(); }},
        {QStringLiteral("识别区域缩放设置"), [this] { openScaleCalibrator(); }},
        {QStringLiteral("按键设置"), [this] { openHotkeySettingsWindow(); }},
        {QStringLiteral("压枪参数设置"), [this] { openRecoilDebugger(); }},
        {QStringLiteral("特殊武器参数设置"), [this] { openSpecialWeaponDebugger(); }},
        {QStringLiteral("界面设置"), [this] { openDisplaySettingsWindow(); }},
    };
    for (int i = 0; i < actions.size(); ++i) {
        auto* b = new RoundedButton(actions[i].first, tab);
        b->configure(256, 36, 12, 15);
        b->setGeometry(3, 8 + i * 42, 256, 36);
        if (i == 0) {
            b->setToggleMode(true);
            btn_debug_ = b;
        }
        if (i == 1) {
            btn_region_calibration_ = b;
        } else if (i == 2) {
            btn_scale_calibration_ = b;
        }
        connect(b, &QPushButton::clicked, this, [action = actions[i].second] { action(); });
    }
    refreshCalibrationWarnings();
}

bool MainWindow::hasMissingRegionCalibration() const {
    static const std::vector<std::string> region_keys{
        "largemap_region", "minimap_region",
        "weapon1_number_region", "weapon2_number_region",
        "elevation_region", "weapon1_name_region", "weapon2_name_region",
        "weapon_region", "weapon1_scope_region", "weapon2_scope_region",
        "stance_region", "weapon1_muzzle_region", "weapon2_muzzle_region",
        "scope_top_edge_4x_region", "weapon1_grip_region", "weapon2_grip_region",
        "scope_top_edge_6x_region", "weapon1_stock_region", "weapon2_stock_region",
        "scope_top_edge_8x_region", "mortar_mount_region", "compass_region",
    };
    for (const auto& key : region_keys) {
        if (!regions_.getRealRegion(key)) {
            return true;
        }
    }
    return regions_.getRealScale("largemap_1km_px", 0.0) <= 0.0;
}

bool MainWindow::hasMissingScaleCalibration() const {
    static const std::vector<std::string> scale_keys{
        "weapon_region", "weapon1_name_region", "weapon2_name_region", "stance_region",
    };
    return config_.read([&](const Json& data) {
        const auto settings = data.value("region_scaling_settings", Json::object());
        for (const auto& key : scale_keys) {
            if (!settings.contains(key) || !settings[key].is_object()) {
                return true;
            }
            const auto& item = settings[key];
            if (item.value("width", 0) <= 0 || item.value("height", 0) <= 0) {
                return true;
            }
        }
        return false;
    });
}

void MainWindow::refreshCalibrationWarnings() {
    if (btn_region_calibration_) {
        btn_region_calibration_->setWarning(hasMissingRegionCalibration());
    }
    if (btn_scale_calibration_) {
        btn_scale_calibration_->setWarning(hasMissingScaleCalibration());
    }
    refreshHelpText();
}

void MainWindow::refreshHelpText() {
    if (!help_summary_label_) {
        return;
    }
    QStringList steps{
        QStringLiteral("选择与游戏一致的色盲模式"),
    };
    if (hasMissingRegionCalibration()) {
        steps.push_back(QStringLiteral("点击系统设置-识别区域设置，按照图片指引校准区域框。"));
    }
    if (hasMissingScaleCalibration()) {
        steps.push_back(QStringLiteral("点击识别区域缩放设置，将四个区域分别校准模板缩放比例。"));
    }
    steps.push_back(QStringLiteral("点击按键设置，将游戏内同步按键修改为和游戏中一致（开火按键游戏中必须修改，修改为键盘不常用按键，实际使用时按鼠标左键自动触发），并自定义程序快捷键。"));
    steps.push_back(QStringLiteral("修改游戏中灵敏度为默认（均为50，垂直灵敏度调为1）"));

    QString text = QStringLiteral("首次使用：\n");
    for (int i = 0; i < steps.size(); ++i) {
        text += QStringLiteral("%1. %2").arg(i + 1).arg(steps[i]);
        if (i + 1 < steps.size()) {
            text += QLatin1Char('\n');
        }
    }
    help_summary_label_->setText(text);
}

void MainWindow::openRegionCalibrationWindow() {
    if (region_calibration_window_) {
        showWindowInsideScreen(region_calibration_window_);
        return;
    }
    auto* window = new QWidget(this);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    window->setWindowFlag(Qt::Window, true);
    window->setWindowTitle(QStringLiteral("校准区域框"));
    window->resize(1080, 555);
    window->setMinimumSize(980, 520);
    const auto theme = currentUiTheme(config_);
    applyThemedPopupWindow(window, config_);
    region_calibration_window_ = window;
    connect(window, &QObject::destroyed, this, [this] { region_calibration_window_ = nullptr; });

    auto square_keys = QSet<QString>{
        "minimap_region", "largemap_region",
        "weapon1_number_region", "weapon2_number_region", "mortar_mount_region",
        "weapon1_scope_region", "weapon1_grip_region", "weapon1_muzzle_region", "weapon1_stock_region",
        "weapon2_scope_region", "weapon2_grip_region", "weapon2_muzzle_region", "weapon2_stock_region",
    };
    struct CalibrationItem {
        QString name;
        QString key;
        bool scale = false;
        QString note;
    };
    const QVector<CalibrationItem> items{
        {QStringLiteral("大地图"), "largemap_region", false, QStringLiteral("选择完整大地图区域，尽量贴合地图内边缘。")},
        {QStringLiteral("小地图"), "minimap_region", false, QStringLiteral("选择小地图内边框部分，注意不要框到灰色轮廓条。")},
        {QStringLiteral("1km比例"), "largemap_1km_px", true, QStringLiteral("在大地图上框选 1km 标尺长度，用于大地图测距换算。")},
        {QStringLiteral("武器1编号"), "weapon1_number_region", false, QStringLiteral("选择武器编号白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("武器2编号"), "weapon2_number_region", false, QStringLiteral("选择武器编号白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("垂直测高"), "elevation_region", false, QStringLiteral("选择屏幕中用于识别标点高度的区域，尽量包含标点附近背景。")},
        {QStringLiteral("武器1名称"), "weapon1_name_region", false, QStringLiteral("比武器名外缘略大即可，右侧需适当延申。")},
        {QStringLiteral("武器2名称"), "weapon2_name_region", false, QStringLiteral("比武器名外缘略大即可，右侧需适当延申。")},
        {QStringLiteral("武器图标"), "weapon_region", false, QStringLiteral("选择当前手持武器图标区域，尽量只包含图标本体。")},
        {QStringLiteral("武器1倍镜"), "weapon1_scope_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("武器2倍镜"), "weapon2_scope_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("姿势区域"), "stance_region", false, QStringLiteral("选择站立/蹲下/趴下姿势图标区域，边缘留一点空隙。")},
        {QStringLiteral("武器1枪口"), "weapon1_muzzle_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("武器2枪口"), "weapon2_muzzle_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("四倍镜内边"), "scope_top_edge_4x_region", false, QStringLiteral("框选四倍镜内框上边缘附近，用于呼吸晃动稳定。")},
        {QStringLiteral("武器1握把"), "weapon1_grip_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("武器2握把"), "weapon2_grip_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("六倍镜内边"), "scope_top_edge_6x_region", false, QStringLiteral("框选六倍镜内框上边缘附近，用于呼吸晃动稳定。")},
        {QStringLiteral("武器1枪托"), "weapon1_stock_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("武器2枪托"), "weapon2_stock_region", false, QStringLiteral("选择配件白边外侧，注意不要过大或框到白边内侧。")},
        {QStringLiteral("八倍镜内边"), "scope_top_edge_8x_region", false, QStringLiteral("框选八倍镜内框上边缘附近，用于呼吸晃动稳定。")},
        {QStringLiteral("迫击炮上炮"), "mortar_mount_region", false, QStringLiteral("选择上炮后出现 F 提示的正方形区域。")},
        {QStringLiteral("顶部方向"), "compass_region", false, QStringLiteral("选择屏幕顶部方向条区域，用于迫击炮方向自动瞄准。")},
    };

    auto* root = new QHBoxLayout(window);
    root->setContentsMargins(24, 42, 24, 22);
    root->setSpacing(18);

    auto* left_panel = new QWidget(window);
    left_panel->setFixedWidth(292);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("选择要校准的区域"), left_panel);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:15px;font-weight:800;").arg(theme.button_text));
    left_layout->addWidget(title);
    auto* button_grid = new QGridLayout();
    button_grid->setContentsMargins(0, 0, 0, 0);
    button_grid->setHorizontalSpacing(8);
    button_grid->setVerticalSpacing(7);
    left_layout->addLayout(button_grid);
    root->addWidget(left_panel);

    auto* right_panel = new QWidget(window);
    auto* right_layout = new QVBoxLayout(right_panel);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(10);
    auto* hint = new QLabel(QStringLiteral("将鼠标移动到校准按钮上预览校准效果"), right_panel);
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:14px;font-weight:800;").arg(theme.button_text));
    right_layout->addWidget(hint);

    auto* preview = new RoundedPreviewWidget(right_panel);
    preview->setColors(theme.panel, theme.border, theme.label);
    preview->setFixedSize(720, 405);
    right_layout->addWidget(preview, 0, Qt::AlignHCenter);

    left_layout->addSpacing(24);
    left_layout->addStretch(1);

    auto* note = new QLabel(QStringLiteral("默认预览：移动到左侧按钮查看该区域校准建议。"), right_panel);
    note->setWordWrap(true);
    note->setMinimumHeight(46);
    note->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;").arg(theme.label));
    right_layout->addWidget(note);
    root->addWidget(right_panel, 1);

    auto guideImagePath = [this](const QString& key) -> QString {
        std::vector<std::filesystem::path> roots;
        const auto config_root = config_.paths().root();
        roots.push_back(config_root);
        roots.push_back(std::filesystem::current_path());
        roots.push_back(std::filesystem::absolute("."));
        for (auto probe = config_root; !probe.empty(); probe = probe.parent_path()) {
            roots.push_back(probe);
            if (probe == probe.parent_path()) break;
        }
        std::vector<std::string> guide_keys{key.toStdString()};
        if (key == QStringLiteral("largemap_1km_px")) {
            guide_keys.push_back("map_1km_pixels");
        }
        for (const auto& root_path : roots) {
            for (const auto& guide_key : guide_keys) {
                const std::vector<std::filesystem::path> dirs{
                    root_path / "assets" / "calibrate_guide" / guide_key,
                    root_path / "assets" / "calibration_guide" / guide_key,
                };
                for (const auto& dir : dirs) {
                    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) continue;
                    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                            return static_cast<char>(std::tolower(ch));
                        });
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                            return QString::fromStdWString(entry.path().wstring());
                        }
                    }
                }
            }
        }
        return {};
    };
    auto showPreview = [this, preview, note, guideImagePath](const CalibrationItem& item) {
        note->setText(item.note);
        const QString image_path = guideImagePath(item.key);
        if (image_path.isEmpty()) {
            preview->setPixmap({});
            preview->setText(QStringLiteral("暂无预览图\n%1").arg(item.name));
            return;
        }
        cv::Mat image_mat = loadPreviewImage(image_path);
        if (!image_mat.empty()) {
            if (image_mat.channels() == 4) {
                cv::cvtColor(image_mat, image_mat, cv::COLOR_BGRA2RGBA);
            } else if (image_mat.channels() == 3) {
                cv::cvtColor(image_mat, image_mat, cv::COLOR_BGR2RGB);
            } else if (image_mat.channels() == 1) {
                cv::cvtColor(image_mat, image_mat, cv::COLOR_GRAY2RGB);
            }
        }
        QImage image;
        if (!image_mat.empty()) {
            const QImage::Format format = image_mat.channels() == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
            image = QImage(image_mat.data, image_mat.cols, image_mat.rows,
                           static_cast<int>(image_mat.step), format).copy();
        }
        if (image.isNull()) {
            preview->setPixmap({});
            preview->setText(QStringLiteral("预览图读取失败\n%1\n%2").arg(item.name, image_path));
            return;
        }
        const QPixmap pixmap = QPixmap::fromImage(image);
        preview->setText({});
        const QPixmap scaled = pixmap.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        preview->setPixmap(scaled);
    };
    const CalibrationItem default_item{QStringLiteral("默认预览"), "default", false, QStringLiteral("默认预览：移动到左侧按钮查看该区域校准建议。")};
    showPreview(default_item);

    for (int i = 0; i < items.size(); ++i) {
        const auto item = items[i];
        auto* b = new RoundedButton(item.name, left_panel);
        b->configure(138, 28, 12, 12);
        b->setFixedSize(138, 28);
        b->setWarning(item.scale ? regions_.getRealScale(item.key.toStdString(), 0.0) <= 0.0
                                 : !regions_.getRealRegion(item.key.toStdString()).has_value());
        button_grid->addWidget(b, i / 2, i % 2);
        b->installEventFilter(new HoverCallbackFilter([showPreview, item] { showPreview(item); }, b));
        connect(b, &QPushButton::clicked, this, [this, item, square_keys] {
            if (region_calibration_overlay_) {
                region_calibration_overlay_->raise();
                region_calibration_overlay_->activateWindow();
                return;
            }
            auto* ov = new RegionCalibrationOverlay(regions_, item.key,
                item.scale ? RegionCalibrationOverlay::Mode::Scale : RegionCalibrationOverlay::Mode::Region,
                square_keys.contains(item.key));
            region_calibration_overlay_ = ov;
            if (debug_overlay_.created()) {
                debug_overlay_.clear();
                debug_overlay_.show(false);
            }
            connect(ov, &RegionCalibrationOverlay::calibrationClosed, this, [this](bool changed) {
                refreshCalibrationWarnings();
                if (debug_overlay_enabled_) {
                    if (!debug_overlay_.created()) {
                        regions_.createOverlay(debug_overlay_, L"PUBGAssistant DebugRegions", true, false);
                    }
                    debug_overlay_.show(true);
                    if (changed) drawDebugOverlay();
                }
            });
            connect(ov, &QObject::destroyed, this, [this] { region_calibration_overlay_ = nullptr; });
            ov->show();
        });
        connect(b, &QPushButton::pressed, this, [showPreview, item] { showPreview(item); });
        b->setAttribute(Qt::WA_Hover, true);
        b->setToolTip(item.note);
    }
    showWindowInsideScreen(window);
}
void MainWindow::buildKeyTab(QWidget* tab) {
    tab->setMinimumSize(542, 658);
    hotkey_labels_.clear();
    struct RowDef {
        QString label;
        QString action;
        bool editable = true;
    };
    const QVector<RowDef> program_rows{
        {QStringLiteral("武器检测开关"), "toggle_weapon_detection", true},
        {QStringLiteral("测距瞄准显示开关"), "toggle_display", true},
        {QStringLiteral("辅助压枪开关"), "toggle_recoil", true},
        {QStringLiteral("大地图测距"), "measure_map", true},
        {QStringLiteral("迫击炮自动瞄准（长按）"), "mortar_auto_aim", true},
        {QStringLiteral("窗口显示开关"), "toggle_window", true},
        {QStringLiteral("标点向前切换"), "marker_prev", true},
        {QStringLiteral("标点向后切换"), "marker_next", true},
        {QStringLiteral("地图点位显示"), "mouse_map_assist", false},
        {QStringLiteral("投掷物自动瞬爆"), "throw", true},
    };
    const QVector<RowDef> game_rows{
        {QStringLiteral("打开装备栏"), "toggle_equipment", true},
        {QStringLiteral("打开小地图"), "game_minimap", true},
        {QStringLiteral("打开大地图"), "open_large_map", true},
        {QStringLiteral("开火按键"), "fire_key", true},
        {QStringLiteral("腰射瞄准（长按）"), "hip_aim_key", true},
    };
    const auto hotkeys = config_.hotkeys();
    auto hotkeyValue = [&](const QString& action, const std::string& fallback) {
        auto it = hotkeys.find(action.toStdString());
        return it == hotkeys.end() ? fallback : it->second;
    };
    const int record_h = 24;
    const auto theme = currentUiTheme(config_);
    auto* root = new QVBoxLayout(tab);
    root->setContentsMargins(14, 8, 14, 14);
    root->setSpacing(4);
    auto addSection = [&](const QString& title) {
        auto* label = new QLabel(title, tab);
        label->setFixedHeight(24);
        label->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:14px;font-weight:800;").arg(theme.button_text));
        root->addWidget(label);
    };
    auto addRow = [&](const RowDef& row) {
        const QString label_text = row.label;
        const QString action = row.action;
        const bool editable = row.editable;
        auto* row_widget = new QWidget(tab);
        row_widget->setFixedHeight(27);
        auto* layout = new QHBoxLayout(row_widget);
        layout->setContentsMargins(4, 0, 4, 0);
        layout->setSpacing(10);

        auto* desc = new QLabel(label_text, row_widget);
        desc->setFixedWidth(170);
        desc->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;").arg(theme.label));
        layout->addWidget(desc);

        const std::string fallback = action == "fire_key" ? "end"
            : action == "mortar_auto_aim" ? "mouse_right"
            : action == "toggle_window" ? "<home>"
            : action == "game_minimap" ? "n"
            : action == "hip_aim_key" ? "mouse_right"
            : action == "open_large_map" ? "m"
            : action == "marker_prev" ? "<up>"
            : action == "marker_next" ? "<down>"
            : action == "throw" ? "b"
            : "";
        const QString display = editable
            ? formatHotkey(hotkeyValue(action, fallback))
            : QStringLiteral("鼠标左键 + 中键");
        auto* value = new QLabel(display, row_widget);
        value->setStyleSheet(QStringLiteral("color:%1;font-family:Consolas;font-size:13px;font-weight:700;").arg(theme.accent));
        layout->addWidget(value, 1);

        if (editable) {
            hotkey_labels_[action] = value;
            auto* rec = new RoundedButton(QStringLiteral("录制"), row_widget);
            rec->configure(64, record_h, 11, 13);
            rec->setFixedSize(64, record_h);
            layout->addWidget(rec);
            connect(rec, &QPushButton::clicked, this, [this, action] { beginCaptureHotkey(action); });
        }
        root->addWidget(row_widget);
    };
    addSection(QStringLiteral("程序快捷键"));
    for (const auto& row : program_rows) {
        addRow(row);
    }
    root->addSpacing(8);
    addSection(QStringLiteral("游戏按键同步"));
    for (const auto& row : game_rows) {
        addRow(row);
    }
    root->addStretch(1);

    auto* bottom = new QWidget(tab);
    auto* bottom_layout = new QHBoxLayout(bottom);
    bottom_layout->setContentsMargins(4, 0, 4, 0);
    bottom_layout->setSpacing(10);
    auto* save = new RoundedButton(QStringLiteral("保存快捷键"), tab);
    auto* reset = new RoundedButton(QStringLiteral("恢复默认"), tab);
    save->configure(220, 32, 12, 14);
    reset->configure(220, 32, 12, 14);
    save->setFixedHeight(32);
    reset->setFixedHeight(32);
    bottom_layout->addWidget(save);
    bottom_layout->addWidget(reset);
    root->addWidget(bottom);
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
    if (callbacks_.set_assistant) {
        callbacks_.set_assistant(key.toStdString(), active);
        return;
    }
    config_.write([&](Json& data) {
        data["ui_state"]["assistants"][key.toStdString()] = active;
    });
    config_.save();
    special_.setManualEnabled(key.toStdString(), active);
    if (key == "throwables") throwables_.setEnabled(active);
    if (key == "c4") c4_.setEnabled(active);
}

void MainWindow::selectMap(int index) {
    QStringList full{QStringLiteral("艾伦格 (Erangel)"), QStringLiteral("米拉玛 (Miramar)"), QStringLiteral("泰戈 (Taego)"), QStringLiteral("荣都 (Rondo)"), QStringLiteral("帝斯顿 (Deston)"), QStringLiteral("维寒迪 (Vikendi)")};
    for (int i = 0; i < map_buttons_.size(); ++i) map_buttons_[i]->setActive(i == index);
    if (index >= 0 && index < full.size()) {
        map_points_.setMap(full[index].toStdString());
        config_.write([&](Json& data) {
            data["ui_state"]["map_index"] = index;
            data["ui_state"]["map_name"] = full[index].toStdString();
        });
        config_.save();
    }
}

void MainWindow::selectMarkerSize(int index) {
    QStringList keys{"small", "medium", "large"};
    for (int i = 0; i < size_buttons_.size(); ++i) size_buttons_[i]->setActive(i == index);
    if (index >= 0 && index < keys.size()) {
        map_points_.setMarkerSize(keys[index].toStdString());
        config_.write([&](Json& data) {
            data["ui_state"]["marker_size_index"] = index;
            data["ui_state"]["marker_size"] = keys[index].toStdString();
        });
        config_.save();
    }
}

void MainWindow::selectPntColorMode(const QString& mode) {
    for (auto it = pnt_mode_buttons_.begin(); it != pnt_mode_buttons_.end(); ++it) {
        it.value()->setActive(it.key() == mode);
    }
    config_.write([&](Json& data) {
        data["pnt_color_mode"] = mode.toStdString();
        data["ui_state"]["pnt_color_mode"] = mode.toStdString();
    });
    config_.save();
    if (callbacks_.sync_marker_colors) callbacks_.sync_marker_colors();
}
void MainWindow::toggleDebugOverlay() {
    debug_overlay_enabled_ = btn_debug_ ? btn_debug_->active() : !debug_overlay_enabled_;
    if (!debug_overlay_.created()) {
        regions_.createOverlay(debug_overlay_, L"PUBGAssistant DebugRegions", true, false);
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
                            0, 0, 0, regionDisplayName(region_name), color, 1, 13});
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
        connect(scale_calibration_window_, &QObject::destroyed, this, [this] {
            scale_calibration_window_ = nullptr;
            refreshCalibrationWarnings();
        });
    }
    showWindowInsideScreen(scale_calibration_window_);
}

void MainWindow::openRecoilDebugger() {
    if (recoil_debugger_window_) {
        showWindowInsideScreen(recoil_debugger_window_);
        return;
    }
    auto* window = new RecoilDebuggerWindow(config_, recoil_);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    window->setWindowFlag(Qt::Window, true);
    recoil_debugger_window_ = window;
    connect(window, &QObject::destroyed, this, [this] { recoil_debugger_window_ = nullptr; });
    showWindowInsideScreen(window);
}

void MainWindow::openSpecialWeaponDebugger() {
    if (special_weapon_debugger_window_) {
        showWindowInsideScreen(special_weapon_debugger_window_);
        return;
    }
    auto* window = new SpecialWeaponDebuggerWindow(config_);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    window->setWindowFlag(Qt::Window, true);
    special_weapon_debugger_window_ = window;
    connect(window, &QObject::destroyed, this, [this] { special_weapon_debugger_window_ = nullptr; });
    connect(window, &SpecialWeaponDebuggerWindow::saved, this, [this] {
        if (callbacks_.refresh_status_hud) callbacks_.refresh_status_hud();
    });
    showWindowInsideScreen(window);
}

void MainWindow::buildHelpTab(QWidget* tab) {
    tab->setFixedSize(262, 299);
    help_summary_label_ = new QLabel(tab);
    help_summary_label_->setWordWrap(true);
    help_summary_label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    help_summary_label_->setGeometry(7, 8, 248, 238);
    help_summary_label_->setStyleSheet("font-family:'Microsoft YaHei';font-size:10px;font-weight:700;");
    refreshHelpText();

    auto* open = new RoundedButton(QStringLiteral("查看完整使用说明文档"), tab);
    open->configure(256, 36, 12, 14);
    open->setGeometry(3, 260, 256, 36);
    connect(open, &QPushButton::clicked, this, &MainWindow::openFullHelpWindow);
}

void MainWindow::openDisplaySettingsWindow() {
    if (!display_settings_window_) {
        display_settings_window_ = new DisplaySettingsWindow(config_, this);
        display_settings_window_->setWindowFlag(Qt::Window, true);
        connect(display_settings_window_, &QObject::destroyed, this, [this] {
            display_settings_window_ = nullptr;
        });
        connect(display_settings_window_, &DisplaySettingsWindow::themeChanged, this, [this] {
            applyTheme();
        });
        connect(display_settings_window_, &DisplaySettingsWindow::hudChanged, this, [this] {
            if (callbacks_.refresh_status_hud) callbacks_.refresh_status_hud();
        });
    }
    showWindowInsideScreen(display_settings_window_);
}

void MainWindow::openHotkeySettingsWindow() {
    if (!hotkey_settings_window_) {
        auto* window = new QWidget(this);
        window->setAttribute(Qt::WA_DeleteOnClose, true);
        window->setWindowFlag(Qt::Window, true);
        window->setWindowTitle(QStringLiteral("按键设置"));
        window->resize(580, 720);
        window->setMinimumSize(560, 680);
        const auto theme = currentUiTheme(config_);
        auto* page = new QWidget(window);
        page->setGeometry(18, 42, 542, 658);
        page->setStyleSheet(QStringLiteral("background:%1;").arg(theme.page));
        buildKeyTab(page);
        auto* follower = new ChildGeometryFollower(page, QMargins(18, 42, 20, 20), window);
        window->installEventFilter(follower);
        applyThemedPopupWindow(window, config_);
        hotkey_settings_window_ = window;
        connect(window, &QObject::destroyed, this, [this] { hotkey_settings_window_ = nullptr; });
    }
    showWindowInsideScreen(hotkey_settings_window_);
}

void MainWindow::openFullHelpWindow() {
    const auto pdf_path = config_.paths().readmePdfFile();
    if (!std::filesystem::exists(pdf_path)) {
        if (help_window_) {
            showWindowInsideScreen(help_window_);
            return;
        }
        auto* window = new QWidget(this);
        window->setAttribute(Qt::WA_DeleteOnClose, true);
        window->setWindowFlag(Qt::Window, true);
        window->setWindowTitle(QStringLiteral("未找到 README.pdf"));
        window->resize(420, 150);
        window->setMinimumSize(420, 150);
        const auto theme = currentUiTheme(config_);
        applyThemedPopupWindow(window, config_);

        auto* text = new QLabel(QStringLiteral("未找到程序同目录下的 README.pdf。\n请将 README.pdf 放到 exe 所在目录后重新点击。"), window);
        text->setWordWrap(true);
        text->setAlignment(Qt::AlignCenter);
        text->setGeometry(18, 22, 384, 62);
        text->setStyleSheet(QStringLiteral("color:%1;font-family:'Microsoft YaHei';font-size:13px;font-weight:700;").arg(theme.button_text));

        auto* close = new RoundedButton(QStringLiteral("知道了"), window);
        close->configure(128, 34, 12, 14);
        close->setGeometry(146, 98, 128, 34);
        connect(close, &QPushButton::clicked, window, &QWidget::close);
        help_window_ = window;
        connect(window, &QObject::destroyed, this, [this] { help_window_ = nullptr; });
        showWindowInsideScreen(window);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(pdf_path.wstring())));
}

void MainWindow::mousePressEvent(QMouseEvent* event) { drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft(); }
void MainWindow::mouseMoveEvent(QMouseEvent* event) { if (event->buttons() & Qt::LeftButton) move(event->globalPosition().toPoint() - drag_offset_); }
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (!capturing_action_.isEmpty()) {
        const QString key = supportedHotkeyFromEvent(event);
        if (!key.isEmpty()) captureHotkey(key, activeModifierNames(event));
        return;
    }
    QMainWindow::keyPressEvent(event);
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

    const bool single_only = capturing_action_ == "throw" || capturing_action_ == "toggle_equipment" ||
                             capturing_action_ == "game_minimap" || capturing_action_ == "open_large_map" ||
                             capturing_action_ == "hip_aim_key" || capturing_action_ == "fire_key";
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

void MainWindow::closeAuxiliaryWindows() {
#ifdef _WIN32
    stopCaptureHook();
#endif
    auto destroyWidget = []<class Widget>(Widget*& window) {
        if (!window) {
            return;
        }
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        delete window;
        window = nullptr;
    };
    auto destroyPointer = [](QPointer<QWidget>& window) {
        if (!window) {
            return;
        }
        QWidget* raw = window.data();
        raw->setAttribute(Qt::WA_DeleteOnClose, false);
        delete raw;
        window = nullptr;
    };

    if (region_calibration_overlay_) {
        QWidget* raw = region_calibration_overlay_.data();
        raw->setAttribute(Qt::WA_DeleteOnClose, false);
        delete raw;
        region_calibration_overlay_ = nullptr;
    }
    destroyPointer(region_calibration_window_);
    destroyWidget(scale_calibration_window_);
    destroyWidget(display_settings_window_);
    destroyPointer(recoil_debugger_window_);
    destroyPointer(special_weapon_debugger_window_);
    destroyPointer(hotkey_settings_window_);
    destroyPointer(help_window_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
#ifdef _WIN32
    stopCaptureHook();
#endif
    if (!closing_) {
        closing_ = true;
        closeAuxiliaryWindows();
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
            {"mortar_auto_aim", "mouse_right"},
            {"game_minimap", "n"},
            {"hip_aim_key", "mouse_right"},
            {"open_large_map", "m"},
            {"marker_prev", "<up>"},
            {"marker_next", "<down>"},
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
    const QString lower = s.toLower();
    if (lower == QStringLiteral("up")) return QStringLiteral("上");
    if (lower == QStringLiteral("down")) return QStringLiteral("下");
    if (lower == QStringLiteral("left")) return QStringLiteral("左");
    if (lower == QStringLiteral("right")) return QStringLiteral("右");
    if (lower == QStringLiteral("mouse_right") || lower == QStringLiteral("rbutton")) return QStringLiteral("鼠标右键");
    if (lower == QStringLiteral("mouse_left") || lower == QStringLiteral("lbutton")) return QStringLiteral("鼠标左键");
    if (lower == QStringLiteral("mouse_middle") || lower == QStringLiteral("mbutton")) return QStringLiteral("鼠标中键");
    return s.toUpper();
}
} // namespace pubg::ui
