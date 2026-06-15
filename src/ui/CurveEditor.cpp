#include "ui/CurveEditor.hpp"

#include <QMouseEvent>
#include <QPainter>

namespace pubg::ui {

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 260);
    setMouseTracking(true);
}

void CurveEditor::setCurves(std::vector<Curve> curves) {
    curves_ = std::move(curves);
    update();
}

QRectF CurveEditor::plotRect() const {
    return rect().adjusted(52, 26, -28, -38);
}

void CurveEditor::ranges(double& min_x, double& max_x, double& min_y, double& max_y) const {
    min_x = 1e9; max_x = -1e9; min_y = 1e9; max_y = -1e9;
    for (const auto& c : curves_) {
        if (!c.xs || !c.ys) continue;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        for (size_t i = 0; i < n; ++i) {
            min_x = std::min(min_x, (*c.xs)[i]);
            max_x = std::max(max_x, (*c.xs)[i]);
            min_y = std::min(min_y, (*c.ys)[i]);
            max_y = std::max(max_y, (*c.ys)[i]);
        }
    }
    if (min_x > max_x) { min_x = 0; max_x = 100; }
    if (min_y > max_y) { min_y = 0; max_y = 1; }
    min_y = std::min(0.0, min_y);
    if (std::abs(max_x - min_x) < 1e-6) max_x += 1.0;
    if (std::abs(max_y - min_y) < 1e-6) max_y += 1.0;
    const double px = (max_x - min_x) * 0.08;
    const double py = (max_y - min_y) * 0.12;
    min_x -= px; max_x += px; min_y -= py; max_y += py;
}

QPointF CurveEditor::toScreen(double x, double y, double min_x, double max_x, double min_y, double max_y) const {
    QRectF r = plotRect();
    return {r.left() + (x - min_x) / (max_x - min_x) * r.width(),
            r.bottom() - (y - min_y) / (max_y - min_y) * r.height()};
}

std::pair<double, double> CurveEditor::fromScreen(const QPointF& p, double min_x, double max_x, double min_y, double max_y) const {
    QRectF r = plotRect();
    double x = min_x + (p.x() - r.left()) / r.width() * (max_x - min_x);
    double y = max_y - (p.y() - r.top()) / r.height() * (max_y - min_y);
    return {std::max(0.0, x), std::max(0.0, y)};
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#F8FAFC"));
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    QRectF pr = plotRect();
    p.setPen(QColor("#E5E7EB"));
    for (int i = 0; i <= 5; ++i) {
        double y = pr.bottom() - pr.height() * i / 5.0;
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.right(), y));
        p.drawText(QPointF(4, y + 4), QString::number(min_y + (max_y - min_y) * i / 5.0, 'f', 2));
    }
    for (int i = 0; i <= 5; ++i) {
        double x = pr.left() + pr.width() * i / 5.0;
        p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.bottom()));
        p.drawText(QPointF(x - 12, pr.bottom() + 18), QString::number(min_x + (max_x - min_x) * i / 5.0, 'f', 0));
    }
    p.setPen(QPen(QColor("#111827"), 2));
    p.drawRect(pr);
    for (const auto& c : curves_) {
        if (!c.xs || !c.ys) continue;
        QPolygonF poly;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        for (size_t i = 0; i < n; ++i) poly << toScreen((*c.xs)[i], (*c.ys)[i], min_x, max_x, min_y, max_y);
        p.setPen(QPen(c.color, 2));
        p.drawPolyline(poly);
        p.setBrush(c.color);
        for (const auto& pt : poly) {
            p.drawEllipse(pt, 5, 5);
        }
    }
}

std::tuple<int, int, double> CurveEditor::nearestPoint(const QPointF& p) const {
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    int best_c = -1, best_i = -1;
    double best_d = 1e9;
    for (int ci = 0; ci < static_cast<int>(curves_.size()); ++ci) {
        const auto& c = curves_[ci];
        if (!c.xs || !c.ys) continue;
        const int n = static_cast<int>(std::min(c.xs->size(), c.ys->size()));
        for (int i = 0; i < n; ++i) {
            QPointF s = toScreen((*c.xs)[i], (*c.ys)[i], min_x, max_x, min_y, max_y);
            double d = std::hypot(s.x() - p.x(), s.y() - p.y());
            if (d < best_d) { best_d = d; best_c = ci; best_i = i; }
        }
    }
    return {best_c, best_i, best_d};
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    auto [ci, pi, d] = nearestPoint(event->position());
    if (event->button() == Qt::RightButton && ci >= 0 && d <= 14.0) {
        auto& c = curves_[ci];
        if (c.xs && c.ys && c.xs->size() > 2 && pi < static_cast<int>(c.xs->size())) {
            auto* shared_xs = c.xs;
            shared_xs->erase(shared_xs->begin() + pi);
            for (auto& related : curves_) {
                if (related.xs == shared_xs && related.ys && pi < static_cast<int>(related.ys->size())) {
                    related.ys->erase(related.ys->begin() + pi);
                }
            }
            emit curveChanged();
            update();
        }
        return;
    }
    if (ci >= 0 && d <= 14.0) {
        drag_curve_ = ci;
        drag_index_ = pi;
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (drag_curve_ < 0 || drag_index_ < 0) return;
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    auto [x, y] = fromScreen(event->position(), min_x, max_x, min_y, max_y);
    auto& c = curves_[drag_curve_];
    if (c.xs && c.ys && drag_index_ < static_cast<int>(c.xs->size())) {
        (*c.xs)[drag_index_] = x;
        (*c.ys)[drag_index_] = y;
        emit curveChanged();
        update();
    }
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    drag_curve_ = -1;
    drag_index_ = -1;
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (curves_.empty() || !curves_[0].xs || !curves_[0].ys) return;
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    auto [x, y] = fromScreen(event->position(), min_x, max_x, min_y, max_y);
    auto* shared_xs = curves_[0].xs;
    shared_xs->push_back(x);
    for (size_t i = 0; i < curves_.size(); ++i) {
        auto& curve = curves_[i];
        if (curve.xs != shared_xs || !curve.ys) continue;
        if (i == 0) {
            curve.ys->push_back(y);
        } else {
            curve.ys->push_back(curve.ys->empty() ? y : curve.ys->back());
        }
    }
    emit curveChanged();
    update();
}

} // namespace pubg::ui
