#include "ui/RoundedButton.hpp"

#include <QPainter>

namespace pubg::ui {

RoundedButton::RoundedButton(const QString& text, QWidget* parent) : QPushButton(text, parent) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setFlat(true);
    refresh();
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
    QColor bg("#FFFFFF");
    if (pressed_) {
        bg = QColor("#D7DEE8");
    } else if (active_) {
        bg = QColor("#E8EEF6");
    } else if (hovered_) {
        bg = QColor("#F4F7FB");
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor("#FFFFFF"), 1));
    painter.setBrush(bg);
    const QRectF box = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(box, radius_, radius_);

    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(pixel_font_size_);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor("#111827"));
    painter.drawText(rect(), Qt::AlignCenter, text());
}

void RoundedButton::refresh() {
    setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:0;}"));
    update();
}

} // namespace pubg::ui
