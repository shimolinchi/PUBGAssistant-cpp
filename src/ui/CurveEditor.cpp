#include "ui/CurveEditor.hpp"

#include <QFont>
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

QString CurveEditor::displayLabel(const QString& label) const {
    if (label == "hip") return QStringLiteral("腰射");
    if (label == "red_dot") return QStringLiteral("红点");
    if (label == "holographic") return QStringLiteral("全息");
    if (label == "vertical") return QStringLiteral("垂直握把");
    if (label == "half") return QStringLiteral("半截握把");
    if (label == "light") return QStringLiteral("轻型握把");
    if (label == "thumb") return QStringLiteral("拇指握把");
    if (label == "tilted") return QStringLiteral("直角握把");
    if (label == "laser") return QStringLiteral("激光瞄准器");
    if (label == "ar_dmr_compensator") return QStringLiteral("步枪/连狙补偿器");
    if (label == "ar_dmr_silencer") return QStringLiteral("步枪/连狙消音器");
    if (label == "ar_dmr_suppressor") return QStringLiteral("步枪/连狙消焰器");
    if (label == "ar_dmr_braker") return QStringLiteral("步枪/连狙制退器");
    if (label == "dmr_sr_compensator") return QStringLiteral("连狙/栓狙补偿器");
    if (label == "smg_compensator") return QStringLiteral("冲锋枪补偿器");
    if (label == "smg_suppressor") return QStringLiteral("冲锋枪消焰器");
    if (label == "smg_silencer") return QStringLiteral("冲锋枪消音器");
    if (label == "tactical") return QStringLiteral("战术枪托");
    if (label == "heavy") return QStringLiteral("重型枪托");
    if (label == "uzi") return QStringLiteral("微冲枪托");
    if (label == "cheek_pad") return QStringLiteral("托腮板");
    return label;
}

QRectF CurveEditor::plotRect() const {
    // 顶部多留出图例行的高度，左侧给 y 轴刻度数字留足宽度。
    return rect().adjusted(56, 44, -24, -40);
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
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.fillRect(rect(), QColor("#F8FAFC"));
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    QRectF pr = plotRect();

    // 刻度数字字体：略大、加粗、深色，避免和浅灰网格线混在一起看不清。
    QFont tick_font = p.font();
    tick_font.setPointSizeF(9.0);
    tick_font.setBold(true);

    // 先画浅灰网格线，再单独用深色字体画刻度数字。
    for (int i = 0; i <= 5; ++i) {
        double y = pr.bottom() - pr.height() * i / 5.0;
        p.setPen(QColor("#E5E7EB"));
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.right(), y));
        p.setPen(QColor("#374151"));
        p.setFont(tick_font);
        const QString label = QString::number(min_y + (max_y - min_y) * i / 5.0, 'f', 2);
        // y 轴刻度数字右对齐到绘图区左边界，读起来更整齐清晰。
        p.drawText(QRectF(0, y - 9, pr.left() - 6, 18),
                   Qt::AlignRight | Qt::AlignVCenter, label);
    }
    for (int i = 0; i <= 5; ++i) {
        double x = pr.left() + pr.width() * i / 5.0;
        p.setPen(QColor("#E5E7EB"));
        p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.bottom()));
        p.setPen(QColor("#374151"));
        p.setFont(tick_font);
        const QString label = QString::number(min_x + (max_x - min_x) * i / 5.0, 'f', 0);
        p.drawText(QRectF(x - 28, pr.bottom() + 4, 56, 18),
                   Qt::AlignHCenter | Qt::AlignTop, label);
    }
    p.setPen(QPen(QColor("#111827"), 2));
    p.drawRect(pr);

    // 顶部图例：利用绘图区上方的空白标出每条曲线的颜色和名称。
    QFont legend_font = p.font();
    legend_font.setPointSizeF(9.5);
    legend_font.setBold(true);
    p.setFont(legend_font);
    double legend_x = pr.left();
    const double legend_y = (pr.top() - 44.0) / 2.0 + 8.0;
    for (const auto& c : curves_) {
        if (c.label.isEmpty()) continue;
        const QString label = displayLabel(c.label);
        p.setBrush(c.color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(legend_x + 6, legend_y - 4), 5, 5);
        p.setPen(QColor("#374151"));
        const double text_w = p.fontMetrics().horizontalAdvance(label);
        p.drawText(QPointF(legend_x + 16, legend_y), label);
        legend_x += 16 + text_w + 22;
    }

    for (const auto& c : curves_) {
        if (!c.xs || !c.ys) continue;
        QPolygonF poly;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        for (size_t i = 0; i < n; ++i) poly << toScreen((*c.xs)[i], (*c.ys)[i], min_x, max_x, min_y, max_y);
        p.setPen(QPen(c.color, 2));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
        p.setPen(QPen(QColor("#FFFFFF"), 3));
        p.setBrush(c.color);
        for (const auto& pt : poly) {
            p.drawEllipse(pt, 5, 5);
            p.setPen(QPen(QColor("#111827"), 1));
            p.drawEllipse(pt, 5, 5);
            p.setPen(QPen(QColor("#FFFFFF"), 3));
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

double CurveEditor::interpolatedY(const std::vector<double>& xs, const std::vector<double>& ys, double x) const {
    const size_t n = std::min(xs.size(), ys.size());
    if (n == 0) return 0.0;
    if (n == 1 || x <= xs.front()) return ys.front();
    if (x >= xs[n - 1]) return ys[n - 1];
    for (size_t i = 1; i < n; ++i) {
        if (x <= xs[i]) {
            const double left_x = xs[i - 1];
            const double right_x = xs[i];
            if (std::abs(right_x - left_x) < 1e-9) return ys[i];
            const double t = (x - left_x) / (right_x - left_x);
            return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
        }
    }
    return ys[n - 1];
}

void CurveEditor::deletePoint(int curve_index, int point_index) {
    if (curve_index < 0 || curve_index >= static_cast<int>(curves_.size())) return;
    auto& c = curves_[curve_index];
    if (!c.xs || !c.ys || c.xs->size() <= 2 || point_index < 0 || point_index >= static_cast<int>(c.xs->size())) {
        return;
    }
    auto* shared_xs = c.xs;
    shared_xs->erase(shared_xs->begin() + point_index);
    for (auto& related : curves_) {
        if (related.xs == shared_xs && related.ys && point_index < static_cast<int>(related.ys->size())) {
            related.ys->erase(related.ys->begin() + point_index);
        }
    }
    emit curveChanged();
    update();
}

void CurveEditor::sortSharedAxis(std::vector<double>* shared_xs) {
    if (!shared_xs || shared_xs->size() < 2) return;
    std::vector<size_t> order(shared_xs->size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return (*shared_xs)[a] < (*shared_xs)[b];
    });
    std::vector<double> sorted_xs;
    sorted_xs.reserve(shared_xs->size());
    for (size_t index : order) sorted_xs.push_back((*shared_xs)[index]);
    *shared_xs = std::move(sorted_xs);
    for (auto& curve : curves_) {
        if (curve.xs != shared_xs || !curve.ys || curve.ys->size() != order.size()) continue;
        std::vector<double> sorted_ys;
        sorted_ys.reserve(curve.ys->size());
        for (size_t index : order) sorted_ys.push_back((*curve.ys)[index]);
        *curve.ys = std::move(sorted_ys);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    auto [ci, pi, d] = nearestPoint(event->position());
    if (event->button() == Qt::RightButton && ci >= 0 && d <= 14.0) {
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
        sortSharedAxis(c.xs);
        auto [new_curve, new_index, new_distance] = nearestPoint(event->position());
        if (new_curve >= 0 && new_distance <= 18.0) {
            drag_curve_ = new_curve;
            drag_index_ = new_index;
        }
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
    if (event->button() == Qt::RightButton) {
        auto [ci, pi, d] = nearestPoint(event->position());
        if (ci >= 0 && d <= 14.0) deletePoint(ci, pi);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    auto [x, y] = fromScreen(event->position(), min_x, max_x, min_y, max_y);
    auto* shared_xs = curves_[0].xs;
    const auto insert_it = std::lower_bound(shared_xs->begin(), shared_xs->end(), x);
    const int insert_index = static_cast<int>(std::distance(shared_xs->begin(), insert_it));
    std::vector<double> inserted_ys(curves_.size(), y);
    for (size_t i = 1; i < curves_.size(); ++i) {
        auto& curve = curves_[i];
        if (curve.xs == shared_xs && curve.ys) {
            inserted_ys[i] = interpolatedY(*shared_xs, *curve.ys, x);
        }
    }
    shared_xs->insert(insert_it, x);
    for (size_t i = 0; i < curves_.size(); ++i) {
        auto& curve = curves_[i];
        if (curve.xs != shared_xs || !curve.ys) continue;
        curve.ys->insert(curve.ys->begin() + insert_index, inserted_ys[i]);
    }
    emit curveChanged();
    update();
}

} // namespace pubg::ui
