#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <QChildEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPushButton>

namespace pubg::ui {

namespace {
#ifdef _WIN32
void applyNativeRoundedRegion(QWidget* window) {
    if (!window) return;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) return;
    const double scale = window->devicePixelRatioF();
    const int physical_w = static_cast<int>(std::ceil(window->width() * scale));
    const int physical_h = static_cast<int>(std::ceil(window->height() * scale));
    const int physical_radius = static_cast<int>(std::round(36.0 * scale));
    HRGN region = CreateRoundRectRgn(0, 0, physical_w + 1, physical_h + 1,
                                     physical_radius, physical_radius);
    if (region && !SetWindowRgn(hwnd, region, TRUE)) {
        DeleteObject(region);
    }
}
#else
void applyNativeRoundedRegion(QWidget*) {}
#endif

QString themedPopupStyle(const UiTheme& theme) {
    return QStringLiteral(
        "#themedPopup{background:%1;border:1px solid %2;border-radius:18px;}"
        "#themedPopup QWidget{background:transparent;color:%3;font-family:'Microsoft YaHei';}"
        "#themedPopup QLabel{background:transparent;color:%4;font-family:'Microsoft YaHei';}"
        "#themedPopup QComboBox,#themedPopup QLineEdit{background:%5;color:%3;border:1px solid %2;border-radius:4px;padding:3px 6px;selection-background-color:%6;selection-color:%3;}"
        "#themedPopup QComboBox QAbstractItemView{background:%5;color:%3;border:1px solid %2;selection-background-color:%6;selection-color:%3;}"
        "#themedPopup QPushButton{background:%7;color:%3;border:1px solid %2;border-radius:5px;padding:6px 8px;font-weight:700;}"
        "#themedPopup QPushButton:hover{background:%8;}"
        "#themedPopup QPushButton:pressed{background:%9;}"
        "#themedPopup QScrollBar:vertical{background:%5;width:10px;margin:0;border:0;}"
        "#themedPopup QScrollBar::handle:vertical{background:%2;border-radius:5px;min-height:20px;}"
        "#themedPopup QScrollBar::add-line:vertical,#themedPopup QScrollBar::sub-line:vertical{height:0;background:%5;}"
    ).arg(theme.frame, theme.border, theme.button_text, theme.label, theme.field,
          theme.button_active, theme.button, theme.button_hover, theme.button_pressed);
}

class PopupWindowController final : public QObject {
public:
    explicit PopupWindowController(QWidget* window)
        : QObject(window), window_(window) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!window_) {
            return QObject::eventFilter(watched, event);
        }
        if (watched == window_ && event->type() == QEvent::ChildAdded) {
            if (auto* child_event = static_cast<QChildEvent*>(event); child_event->child()) {
                if (auto* child_widget = qobject_cast<QWidget*>(child_event->child())) {
                    installRecursive(child_widget);
                }
            }
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::Resize) {
            positionCloseButton();
            applyNativeRoundedRegion(window_);
            return QObject::eventFilter(watched, event);
        }
        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            switch (event->type()) {
            case QEvent::MouseButtonPress:
                return handlePress(widget, static_cast<QMouseEvent*>(event));
            case QEvent::MouseMove:
                return handleMove(widget, static_cast<QMouseEvent*>(event));
            case QEvent::MouseButtonRelease:
                resizing_ = false;
                dragging_ = false;
                resize_edges_ = {};
                updateCursor(widget, static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
                return QObject::eventFilter(watched, event);
            case QEvent::Leave:
                if (!resizing_ && !dragging_) {
                    widget->unsetCursor();
                }
                return QObject::eventFilter(watched, event);
            default:
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

public:
    void installRecursive(QWidget* root) {
        if (!root) return;
        root->installEventFilter(this);
        const auto children = root->findChildren<QWidget*>();
        for (auto* child : children) {
            child->installEventFilter(this);
        }
    }

    void positionCloseButton() {
        if (auto* close = window_->findChild<QPushButton*>(QStringLiteral("popupCloseButton"))) {
            close->move(std::max(8, window_->width() - 25), 11);
            close->raise();
        }
    }

private:
    enum Edge {
        Left = 0x1,
        Top = 0x2,
        Right = 0x4,
        Bottom = 0x8,
    };

    int edgesAt(const QPoint& global_pos) const {
        const QRect frame = window_->frameGeometry();
        constexpr int margin = 7;
        int edges = 0;
        if (std::abs(global_pos.x() - frame.left()) <= margin) edges |= Left;
        if (std::abs(global_pos.x() - frame.right()) <= margin) edges |= Right;
        if (std::abs(global_pos.y() - frame.top()) <= margin) edges |= Top;
        if (std::abs(global_pos.y() - frame.bottom()) <= margin) edges |= Bottom;
        return edges;
    }

    bool isInteractiveChild(QWidget* widget) const {
        return qobject_cast<QPushButton*>(widget) ||
               widget->inherits("QComboBox") ||
               widget->inherits("QLineEdit") ||
               widget->inherits("QSlider") ||
               widget->inherits("QScrollBar") ||
               widget->inherits("QAbstractItemView");
    }

    void updateCursor(QWidget* widget, const QPoint& global_pos) {
        const int edges = edgesAt(global_pos);
        if ((edges & Left && edges & Top) || (edges & Right && edges & Bottom)) {
            widget->setCursor(Qt::SizeFDiagCursor);
        } else if ((edges & Right && edges & Top) || (edges & Left && edges & Bottom)) {
            widget->setCursor(Qt::SizeBDiagCursor);
        } else if (edges & (Left | Right)) {
            widget->setCursor(Qt::SizeHorCursor);
        } else if (edges & (Top | Bottom)) {
            widget->setCursor(Qt::SizeVerCursor);
        } else {
            widget->unsetCursor();
        }
    }

    bool handlePress(QWidget* widget, QMouseEvent* event) {
        if (event->button() != Qt::LeftButton) {
            return QObject::eventFilter(widget, event);
        }
        const QPoint global_pos = event->globalPosition().toPoint();
        resize_edges_ = edgesAt(global_pos);
        if (resize_edges_ != 0) {
            resizing_ = true;
            start_global_ = global_pos;
            start_geometry_ = window_->geometry();
            return true;
        }
        const QPoint local_to_window = window_->mapFromGlobal(global_pos);
        if (local_to_window.y() <= 34 && !isInteractiveChild(widget)) {
            dragging_ = true;
            drag_offset_ = global_pos - window_->frameGeometry().topLeft();
            return true;
        }
        return QObject::eventFilter(widget, event);
    }

    bool handleMove(QWidget* widget, QMouseEvent* event) {
        const QPoint global_pos = event->globalPosition().toPoint();
        if (resizing_) {
            QRect next = start_geometry_;
            const QPoint delta = global_pos - start_global_;
            const QSize min_size = window_->minimumSize().expandedTo(QSize(180, 120));
            if (resize_edges_ & Left) next.setLeft(std::min(next.right() - min_size.width(), next.left() + delta.x()));
            if (resize_edges_ & Right) next.setRight(std::max(next.left() + min_size.width(), next.right() + delta.x()));
            if (resize_edges_ & Top) next.setTop(std::min(next.bottom() - min_size.height(), next.top() + delta.y()));
            if (resize_edges_ & Bottom) next.setBottom(std::max(next.top() + min_size.height(), next.bottom() + delta.y()));
            window_->setGeometry(next);
            return true;
        }
        if (dragging_) {
            window_->move(global_pos - drag_offset_);
            return true;
        }
        updateCursor(widget, global_pos);
        return QObject::eventFilter(widget, event);
    }

    QWidget* window_ = nullptr;
    bool resizing_ = false;
    bool dragging_ = false;
    int resize_edges_ = 0;
    QPoint start_global_;
    QRect start_geometry_;
    QPoint drag_offset_;
};
} // namespace

UiTheme uiThemeFromName(const std::string& name) {
    // Palette names and ramps are inspired by common modern UI systems such as Tailwind's color families.
    if (name == "dark") return {"dark", "黑色", "#151923", "#151923", "#2F3747", "#E5E7EB", "#252B37", "#313A4D", "#475569", "#334155", "#F9FAFB", "#1F2633", "#252B37", "#60A5FA"};
    if (name == "gray") return {"gray", "灰色", "#D6DAE1", "#D6DAE1", "#F8FAFC", "#374151", "#F8FAFC", "#EEF2F7", "#CBD5E1", "#E5E7EB", "#111827", "#EEF2F7", "#FFFFFF", "#4B5563"};
    if (name == "navy") return {"navy", "深蓝", "#152238", "#152238", "#4F7CAC", "#DCEBFF", "#203655", "#294568", "#3B5F89", "#28476D", "#EFF6FF", "#1B2E49", "#203655", "#7DD3FC"};
    if (name == "rose") return {"rose", "玫瑰", "#FFE4E6", "#FFE4E6", "#FDA4AF", "#881337", "#FFF1F2", "#FFE4E6", "#FECDD3", "#FFE4E6", "#881337", "#FFF7F8", "#FFFFFF", "#E11D48"};
    if (name == "red") return {"red", "赤红", "#FEE2E2", "#FEE2E2", "#FCA5A5", "#7F1D1D", "#FFF7F7", "#FEE2E2", "#FECACA", "#FFE4E6", "#7F1D1D", "#FFF1F2", "#FFFFFF", "#EF4444"};
    if (name == "orange") return {"orange", "橙霞", "#FFEDD5", "#FFEDD5", "#FDBA74", "#7C2D12", "#FFF7ED", "#FFEDD5", "#FED7AA", "#FFE7C2", "#7C2D12", "#FFF7ED", "#FFFFFF", "#F97316"};
    if (name == "amber") return {"amber", "琥珀", "#FEF3C7", "#FEF3C7", "#FCD34D", "#78350F", "#FFFBEB", "#FEF3C7", "#FDE68A", "#FFF1B8", "#78350F", "#FFFBEB", "#FFFFFF", "#F59E0B"};
    if (name == "yellow") return {"yellow", "暖黄", "#FEF9C3", "#FEF9C3", "#FDE047", "#713F12", "#FEFCE8", "#FEF9C3", "#FEF08A", "#FFF2A8", "#713F12", "#FFFBEA", "#FFFFFF", "#CA8A04"};
    if (name == "lime") return {"lime", "青柠", "#ECFCCB", "#ECFCCB", "#BEF264", "#365314", "#F7FEE7", "#ECFCCB", "#D9F99D", "#E8FBC0", "#365314", "#F7FEE7", "#FFFFFF", "#65A30D"};
    if (name == "green") return {"green", "森绿", "#DCFCE7", "#DCFCE7", "#86EFAC", "#064E3B", "#F0FDF4", "#DCFCE7", "#BBF7D0", "#D9F99D", "#064E3B", "#ECFDF5", "#FFFFFF", "#22C55E"};
    if (name == "emerald") return {"emerald", "翡翠", "#D1FAE5", "#D1FAE5", "#6EE7B7", "#064E3B", "#ECFDF5", "#D1FAE5", "#A7F3D0", "#CFFAEA", "#064E3B", "#ECFDF5", "#FFFFFF", "#10B981"};
    if (name == "teal") return {"teal", "青色", "#CCFBF1", "#CCFBF1", "#5EEAD4", "#134E4A", "#F0FDFA", "#CCFBF1", "#99F6E4", "#BFF7ED", "#134E4A", "#ECFEFF", "#FFFFFF", "#14B8A6"};
    if (name == "cyan") return {"cyan", "湖青", "#CFFAFE", "#CFFAFE", "#67E8F9", "#164E63", "#ECFEFF", "#CFFAFE", "#A5F3FC", "#C9F7FF", "#164E63", "#ECFEFF", "#FFFFFF", "#06B6D4"};
    if (name == "sky") return {"sky", "晴空", "#E0F2FE", "#E0F2FE", "#7DD3FC", "#0C4A6E", "#F0F9FF", "#E0F2FE", "#BAE6FD", "#DCF4FF", "#0C4A6E", "#F0F9FF", "#FFFFFF", "#0EA5E9"};
    if (name == "blue") return {"blue", "天蓝", "#DBEAFE", "#DBEAFE", "#93C5FD", "#1E3A8A", "#EFF6FF", "#DBEAFE", "#BFDBFE", "#DCEBFF", "#1E3A8A", "#EFF6FF", "#FFFFFF", "#3B82F6"};
    if (name == "indigo") return {"indigo", "靛蓝", "#E0E7FF", "#E0E7FF", "#A5B4FC", "#312E81", "#EEF2FF", "#E0E7FF", "#C7D2FE", "#E4E9FF", "#312E81", "#EEF2FF", "#FFFFFF", "#6366F1"};
    if (name == "purple") return {"purple", "紫罗兰", "#EDE9FE", "#EDE9FE", "#C4B5FD", "#4C1D95", "#F5F3FF", "#EDE9FE", "#DDD6FE", "#EEE6FF", "#4C1D95", "#F5F3FF", "#FFFFFF", "#8B5CF6"};
    if (name == "fuchsia") return {"fuchsia", "洋红", "#FAE8FF", "#FAE8FF", "#F0ABFC", "#701A75", "#FDF4FF", "#FAE8FF", "#F5D0FE", "#FBE8FF", "#701A75", "#FDF4FF", "#FFFFFF", "#D946EF"};
    return {"light", "白色", "#DDE6F0", "#DDE6F0", "#FFFFFF", "#6B7280", "#FFFFFF", "#F4F7FB", "#D7DEE8", "#E8EEF6", "#111827", "#F8FAFC", "#FFFFFF", "#2563EB"};
}

UiTheme currentUiTheme(Config& config) {
    const auto name = config.read([](const Json& data) {
        return data.value("ui_state", Json::object()).value("theme", Json::object()).value("name", std::string("navy"));
    });
    return uiThemeFromName(name);
}

double themedWindowOpacity(Config& config) {
    const double opacity = config.read([](const Json& data) {
        return data.value("ui_state", Json::object())
            .value("display", Json::object())
            .value("window_opacity", 0.60);
    });
    return std::clamp(opacity, 0.30, 1.0);
}

QString themedWidgetStyle(const UiTheme& theme, const QString& root_selector) {
    const QString root = root_selector.isEmpty()
        ? QStringLiteral("QWidget{background:%1;color:%2;font-family:'Microsoft YaHei';}").arg(theme.page, theme.button_text)
        : QStringLiteral("%1{background:%2;}").arg(root_selector, theme.page);
    return root +
        QStringLiteral(
            "QLabel{color:%1;font-family:'Microsoft YaHei';}"
            "QComboBox,QLineEdit{background:%2;color:%3;border:1px solid %4;border-radius:4px;padding:3px 6px;selection-background-color:%5;selection-color:%3;}"
            "QComboBox QAbstractItemView{background:%2;color:%3;border:1px solid %4;selection-background-color:%5;selection-color:%3;}"
            "QPushButton{background:%6;color:%3;border:1px solid %4;border-radius:5px;padding:6px 8px;font-weight:700;}"
            "QPushButton:hover{background:%7;}"
            "QPushButton:pressed{background:%8;}"
            "QScrollBar:vertical{background:%2;width:10px;margin:0;border:0;}"
            "QScrollBar::handle:vertical{background:%4;border-radius:5px;min-height:20px;}"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;background:%2;}"
        ).arg(theme.label, theme.field, theme.button_text, theme.border, theme.button_active,
              theme.button, theme.button_hover, theme.button_pressed);
}

QString themedSliderStyle(const UiTheme& theme) {
    return QStringLiteral(
        "QSlider::groove:horizontal{height:6px;background:%1;border-radius:3px;}"
        "QSlider::sub-page:horizontal{background:%2;border-radius:3px;}"
        "QSlider::handle:horizontal{width:10px;height:10px;background:%3;border:1px solid %2;border-radius:5px;margin:-3px 0;}"
    ).arg(theme.button_pressed, theme.accent, theme.field);
}

QString themedPanelStyle(const UiTheme& theme) {
    return QStringLiteral("background:%1;border:1px solid %2;border-radius:6px;color:%3;")
        .arg(theme.panel, theme.border, theme.button_text);
}

void applyThemedPopupWindow(QWidget* window, Config& config) {
    if (!window) return;
    const auto theme = currentUiTheme(config);
    window->setObjectName(QStringLiteral("themedPopup"));
    if (!(window->windowFlags() & Qt::FramelessWindowHint)) {
        const bool was_visible = window->isVisible();
        window->setWindowFlags((window->windowFlags() | Qt::Window | Qt::FramelessWindowHint) &
                               ~Qt::WindowMinMaxButtonsHint & ~Qt::WindowCloseButtonHint);
        if (was_visible) {
            window->show();
        }
    }
    // Popup windows use a native rounded region, so the top-level widget itself can
    // paint one continuous backing surface. Keeping WA_TranslucentBackground here
    // makes only child panels visible on some Qt/Win32 combinations.
    window->setAttribute(Qt::WA_TranslucentBackground, false);
    window->setAttribute(Qt::WA_StyledBackground, true);
    window->setAutoFillBackground(true);
    window->setWindowOpacity(themedWindowOpacity(config));
    window->setStyleSheet(themedPopupStyle(theme));
    if (!window->findChild<QPushButton*>(QStringLiteral("popupCloseButton"))) {
        auto* close = new QPushButton(window);
        close->setObjectName(QStringLiteral("popupCloseButton"));
        close->setCursor(Qt::PointingHandCursor);
        close->setFocusPolicy(Qt::NoFocus);
        close->setFixedSize(14, 14);
        close->setStyleSheet(
            "QPushButton#popupCloseButton{border-radius:7px;background:#FF5F57;border:1px solid #E0473F;padding:0;}"
            "QPushButton#popupCloseButton:hover{background:#FF746D;}"
            "QPushButton#popupCloseButton:pressed{background:#E0473F;}"
        );
        QObject::connect(close, &QPushButton::clicked, window, &QWidget::close);
    }
    if (!window->findChild<QObject*>(QStringLiteral("popupWindowController"))) {
        auto* controller = new PopupWindowController(window);
        controller->setObjectName(QStringLiteral("popupWindowController"));
        controller->installRecursive(window);
    }
    auto* controller_object = window->findChild<QObject*>(QStringLiteral("popupWindowController"));
    if (auto* controller = dynamic_cast<PopupWindowController*>(controller_object)) {
        controller->installRecursive(window);
        controller->positionCloseButton();
    }
    applyNativeRoundedRegion(window);
}

} // namespace pubg::ui
