#include "ui/RoundedButton.hpp"

#include <QPainter>

namespace pubg::ui {

namespace {
QColor g_normal("#FFFFFF");
QColor g_hover("#F4F7FB");
QColor g_pressed("#D7DEE8");
QColor g_active("#E8EEF6");
QColor g_border("#FFFFFF");
QColor g_text("#111827");
} // namespace

RoundedButton::RoundedButton(const QString& text, QWidget* parent) : QPushButton(text, parent) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setFlat(true);
    refresh();
}

void RoundedButton::setThemeColors(const QString& normal, const QString& hover, const QString& pressed,
                                   const QString& active, const QString& border, const QString& text) {
    g_normal = QColor(normal);
    g_hover = QColor(hover);
    g_pressed = QColor(pressed);
    g_active = QColor(active);
    g_border = QColor(border);
    g_text = QColor(text);
}

void RoundedButton::configure(int width, int height, int radius, int pixel_font_size) {
    resize(width, height);
    radius_ = radius;
    pixel_font_size_ = pixel_font_size;
    refresh();
}

void RoundedButton::setToggleMode(bool toggle) {
    toggle_mode_ = toggle;
    refresh();
}

void RoundedButton::setActive(bool active) {
    active_ = active;
    refresh();
}

void RoundedButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        pressed_ = true;
        update();
    }
    QPushButton::mousePressEvent(event);
}

void RoundedButton::mouseReleaseEvent(QMouseEvent* event) {
    const bool inside = rect().contains(event->pos());
    if (toggle_mode_ && inside && event->button() == Qt::LeftButton) {
        active_ = !active_;
    }
    pressed_ = false;
    update();
    QPushButton::mouseReleaseEvent(event);
}

void RoundedButton::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QPushButton::enterEvent(event);
}

void RoundedButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    pressed_ = false;
    update();
    QPushButton::leaveEvent(event);
}

void RoundedButton::paintEvent(QPaintEvent*) {
    QColor bg = g_normal;
    if (pressed_) {
        bg = g_pressed;
    } else if (active_) {
        bg = g_active;
    } else if (hovered_) {
        bg = g_hover;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(g_border, 1));
    painter.setBrush(bg);
    const QRectF box = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(box, radius_, radius_);

    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(pixel_font_size_);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(g_text);
    painter.drawText(rect(), Qt::AlignCenter, text());
}

void RoundedButton::refresh() {
    setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:0;}"));
    update();
}

} // namespace pubg::ui
