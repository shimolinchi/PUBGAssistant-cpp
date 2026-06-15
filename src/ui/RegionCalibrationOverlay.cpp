#include "ui/RegionCalibrationOverlay.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace pubg::ui {

RegionCalibrationOverlay::RegionCalibrationOverlay(RegionManager& regions, QString target_name, Mode mode, bool force_square, QWidget* parent)
    : QWidget(parent), regions_(regions), target_name_(std::move(target_name)), mode_(mode), force_square_(force_square) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowOpacity(0.99);
    setCursor(Qt::CrossCursor);
}

void RegionCalibrationOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 3));
    p.setPen(QPen(mode_ == Mode::Scale ? QColor("#2ECC71") : QColor("#E74C3C"), 3));
    if (!dragging_ && start_.isNull() && current_.isNull()) return;
    if (mode_ == Mode::Scale) {
        p.drawLine(start_, current_);
    } else {
        QRect r(start_, current_);
        if (force_square_) {
            int side = std::max(std::abs(r.width()), std::abs(r.height()));
            r = QRect(start_, QPoint(start_.x() + (r.width() >= 0 ? side : -side), start_.y() + (r.height() >= 0 ? side : -side)));
        }
        p.drawRect(r.normalized());
    }
}

void RegionCalibrationOverlay::mousePressEvent(QMouseEvent* event) {
    dragging_ = true;
    start_ = current_ = event->pos();
    update();
}

void RegionCalibrationOverlay::mouseMoveEvent(QMouseEvent* event) {
    current_ = event->pos();
    update();
}

void RegionCalibrationOverlay::mouseReleaseEvent(QMouseEvent* event) {
    current_ = event->pos();
    dragging_ = false;
    if (mode_ == Mode::Scale) {
        const double len = std::hypot(current_.x() - start_.x(), current_.y() - start_.y());
        regions_.setRealScale(target_name_.toStdString(), len);
    } else {
        QRect r(start_, current_);
        if (force_square_) {
            int side = std::max(std::abs(r.width()), std::abs(r.height()));
            r = QRect(start_, QPoint(start_.x() + (r.width() >= 0 ? side : -side), start_.y() + (r.height() >= 0 ? side : -side)));
        }
        r = r.normalized();
        regions_.setRealRegion(target_name_.toStdString(), Rect{r.left(), r.top(), r.width(), r.height()});
    }
    close();
    deleteLater();
}

void RegionCalibrationOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        deleteLater();
    }
}

} // namespace pubg::ui
