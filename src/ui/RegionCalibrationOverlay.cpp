#include "ui/RegionCalibrationOverlay.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <cmath>

namespace pubg::ui {

RegionCalibrationOverlay::RegionCalibrationOverlay(RegionManager& regions, QString target_name, Mode mode, bool force_square, QWidget* parent)
    : QWidget(parent), regions_(regions), target_name_(std::move(target_name)), mode_(mode), force_square_(force_square) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowOpacity(0.99);
    setCursor(Qt::CrossCursor);
    const QRect target(regions_.qtScreenLeft(), regions_.qtScreenTop(), regions_.qtScreenWidth(), regions_.qtScreenHeight());
    setGeometry(target);
    if (QScreen* screen = QGuiApplication::screenAt(target.center())) {
        setScreen(screen);
    } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
        setScreen(screen);
    }
}

void RegionCalibrationOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 3));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(mode_ == Mode::Scale ? QColor("#10B981") : QColor("#2563EB"), 1));
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
    if (event->button() == Qt::RightButton) {
        emit calibrationClosed(false);
        close();
        deleteLater();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    start_ = current_ = event->pos();
    update();
}

void RegionCalibrationOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    current_ = event->pos();
    update();
}

void RegionCalibrationOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) return;
    current_ = event->pos();
    dragging_ = false;
    if ((current_ - start_).manhattanLength() < 3) {
        emit calibrationClosed(false);
        close();
        deleteLater();
        return;
    }
    if (mode_ == Mode::Scale) {
        const QPoint physical_start = toPhysical(start_);
        const QPoint physical_current = toPhysical(current_);
        const double len = std::hypot(physical_current.x() - physical_start.x(),
                                      physical_current.y() - physical_start.y());
        regions_.setRealScale(target_name_.toStdString(), len);
    } else {
        QRect r(start_, current_);
        if (force_square_) {
            int side = std::max(std::abs(r.width()), std::abs(r.height()));
            r = QRect(start_, QPoint(start_.x() + (r.width() >= 0 ? side : -side), start_.y() + (r.height() >= 0 ? side : -side)));
        }
        r = r.normalized();
        const QPoint physical_top_left = toPhysical(r.topLeft());
        const QPoint physical_bottom_right = toPhysical(r.bottomRight());
        QRect physical_rect(physical_top_left, physical_bottom_right);
        physical_rect = physical_rect.normalized();
        regions_.setRealRegion(target_name_.toStdString(), Rect{
            physical_rect.left(),
            physical_rect.top(),
            physical_rect.width() + 1,
            physical_rect.height() + 1
        });
    }
    emit calibrationClosed(true);
    close();
    deleteLater();
}

void RegionCalibrationOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit calibrationClosed(false);
        close();
        deleteLater();
    }
}

QPoint RegionCalibrationOverlay::toPhysical(const QPoint& point) const {
    const QPoint top_left = geometry().topLeft();
    const double scale = regions_.devicePixelRatio();
    return {
        static_cast<int>(std::round((top_left.x() + point.x()) * scale)),
        static_cast<int>(std::round((top_left.y() + point.y()) * scale))
    };
}

} // namespace pubg::ui
