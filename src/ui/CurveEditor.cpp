#include "ui/CurveEditor.hpp"

#include <QFont>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pubg::ui {

namespace {

bool axisLockKeyDown(char key) {
#ifdef _WIN32
    return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
#else
    (void)key;
    return false;
#endif
}

int integerTickStep(double min_value, double max_value, int target_ticks) {
    const double span = std::max(1.0, max_value - min_value);
    return std::max(1, static_cast<int>(std::ceil(span / std::max(1, target_ticks))));
}

double fractionalTickStep(double min_value, double max_value, int target_ticks) {
    const double span = std::max(1e-6, max_value - min_value);
    const double raw = span / std::max(1, target_ticks);
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalized = raw / magnitude;
    const double nice = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
    return nice * magnitude;
}

} // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 260);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void CurveEditor::setCurves(std::vector<Curve> curves) {
    curves_ = std::move(curves);
    normalizeSelection();
    update();
}

void CurveEditor::setThemeColors(const QColor& background, const QColor& grid, const QColor& text,
                                 const QColor& axis, const QColor& point_outline) {
    background_color_ = background;
    grid_color_ = grid;
    text_color_ = text;
    axis_color_ = axis;
    point_outline_color_ = point_outline;
    update();
}

void CurveEditor::setFixedXRange(double min_x, double max_x) {
    fixed_min_x_ = min_x;
    fixed_max_x_ = max_x;
    update();
}

void CurveEditor::setFixedYRange(double min_y, double max_y) {
    fixed_min_y_ = min_y;
    fixed_max_y_ = max_y;
    update();
}

void CurveEditor::setFixedRightYRange(double min_y, double max_y) {
    fixed_right_min_y_ = min_y;
    fixed_right_max_y_ = max_y;
    update();
}

void CurveEditor::setAxisLabels(QString left_label, QString right_label) {
    left_axis_label_ = std::move(left_label);
    right_axis_label_ = std::move(right_label);
    update();
}

void CurveEditor::clearFixedRanges() {
    fixed_min_x_.reset();
    fixed_max_x_.reset();
    fixed_min_y_.reset();
    fixed_max_y_.reset();
    fixed_right_min_y_.reset();
    fixed_right_max_y_.reset();
    left_axis_label_.clear();
    right_axis_label_.clear();
    update();
}

void CurveEditor::clearSelection() {
    selected_points_.clear();
    drag_curve_ = -1;
    drag_index_ = -1;
    update();
}

void CurveEditor::clearUndoHistory() {
    undo_stack_.clear();
}

void CurveEditor::nudgeSelectedY(double delta) {
    if (curves_.empty() || std::abs(delta) < 1e-12) return;
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    pushUndoSnapshot();

    std::vector<std::pair<int, int>> targets;
    if (!selected_points_.empty()) {
        targets.assign(selected_points_.begin(), selected_points_.end());
    } else {
        for (int ci = 0; ci < static_cast<int>(curves_.size()); ++ci) {
            const auto& c = curves_[ci];
            if (!c.xs || !c.ys) continue;
            const int n = static_cast<int>(std::min(c.xs->size(), c.ys->size()));
            for (int pi = 0; pi < n; ++pi) targets.push_back({ci, pi});
        }
    }

    for (const auto& [ci, pi] : targets) {
        if (ci < 0 || ci >= static_cast<int>(curves_.size())) continue;
        auto& c = curves_[ci];
        if (!c.ys || pi < 0 || pi >= static_cast<int>(c.ys->size())) continue;
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(ci, min_y, max_y);
        (*c.ys)[pi] = std::clamp((*c.ys)[pi] + delta, curve_min_y, curve_max_y);
    }
    normalizeSelection();
    emit curveChanged();
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
    const bool has_right_axis = std::any_of(curves_.begin(), curves_.end(), [](const Curve& c) {
        return c.axis == AxisSide::Right;
    });
    const int left_margin = left_axis_label_.isEmpty() ? 56 : 84;
    const int right_margin = has_right_axis ? 84 : 24;
    return rect().adjusted(left_margin, 66, -right_margin, -40);
}

void CurveEditor::ranges(double& min_x, double& max_x, double& min_y, double& max_y) const {
    min_x = std::numeric_limits<double>::max();
    max_x = std::numeric_limits<double>::lowest();
    min_y = std::numeric_limits<double>::max();
    max_y = std::numeric_limits<double>::lowest();
    for (const auto& c : curves_) {
        if (c.axis == AxisSide::Right) continue;
        if (!c.xs || !c.ys) continue;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        for (size_t i = 0; i < n; ++i) {
            min_x = std::min(min_x, (*c.xs)[i]);
            max_x = std::max(max_x, (*c.xs)[i]);
            min_y = std::min(min_y, (*c.ys)[i]);
            max_y = std::max(max_y, (*c.ys)[i]);
        }
    }
    if (fixed_min_x_ && fixed_max_x_ && *fixed_max_x_ > *fixed_min_x_) {
        min_x = *fixed_min_x_;
        max_x = *fixed_max_x_;
    } else {
        if (min_x > max_x) { min_x = 0; max_x = 100; }
        const double px = (max_x - min_x) * 0.08;
        min_x -= px;
        max_x += px;
    }
    if (fixed_min_y_ && fixed_max_y_ && *fixed_max_y_ > *fixed_min_y_) {
        min_y = *fixed_min_y_;
        max_y = *fixed_max_y_;
    } else {
        if (min_y > max_y) { min_y = 0; max_y = 1; }
        min_y = std::min(0.0, min_y);
        const double py = (max_y - min_y) * 0.12;
        min_y -= py;
        max_y += py;
    }
    if (std::abs(max_x - min_x) < 1e-6) max_x += 1.0;
    if (std::abs(max_y - min_y) < 1e-6) max_y += 1.0;
}

void CurveEditor::rightYRange(double& min_y, double& max_y) const {
    if (fixed_right_min_y_ && fixed_right_max_y_ && *fixed_right_max_y_ > *fixed_right_min_y_) {
        min_y = *fixed_right_min_y_;
        max_y = *fixed_right_max_y_;
        return;
    }
    min_y = std::numeric_limits<double>::max();
    max_y = std::numeric_limits<double>::lowest();
    for (const auto& c : curves_) {
        if (c.axis != AxisSide::Right || !c.xs || !c.ys) continue;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        for (size_t i = 0; i < n; ++i) {
            min_y = std::min(min_y, (*c.ys)[i]);
            max_y = std::max(max_y, (*c.ys)[i]);
        }
    }
    if (min_y > max_y) { min_y = 0.0; max_y = 1.0; }
    const double py = std::max(1e-6, max_y - min_y) * 0.12;
    min_y -= py;
    max_y += py;
    if (std::abs(max_y - min_y) < 1e-6) max_y += 1.0;
}

std::pair<double, double> CurveEditor::yRangeForCurve(int curve_index, double left_min_y, double left_max_y) const {
    if (curve_index >= 0 && curve_index < static_cast<int>(curves_.size()) && curves_[curve_index].axis == AxisSide::Right) {
        double min_y = 0.0;
        double max_y = 1.0;
        rightYRange(min_y, max_y);
        return {min_y, max_y};
    }
    return {left_min_y, left_max_y};
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
    return {std::clamp(x, min_x, max_x), std::clamp(y, min_y, max_y)};
}

double CurveEditor::xFromScreen(const QPointF& p, double min_x, double max_x) const {
    QRectF r = plotRect();
    const double x = min_x + (p.x() - r.left()) / r.width() * (max_x - min_x);
    return std::clamp(x, min_x, max_x);
}

double CurveEditor::yFromScreen(const QPointF& p, double min_y, double max_y) const {
    QRectF r = plotRect();
    const double y = max_y - (p.y() - r.top()) / r.height() * (max_y - min_y);
    return std::clamp(y, min_y, max_y);
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const QRectF outer = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath rounded_background;
    rounded_background.addRoundedRect(outer, 8.0, 8.0);
    p.fillPath(rounded_background, background_color_);
    p.setClipPath(rounded_background);
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    double right_min_y = 0.0;
    double right_max_y = 1.0;
    const bool has_right_axis = std::any_of(curves_.begin(), curves_.end(), [](const Curve& c) {
        return c.axis == AxisSide::Right;
    });
    if (has_right_axis) rightYRange(right_min_y, right_max_y);
    QRectF pr = plotRect();

    // 刻度数字字体：略大、加粗、深色，避免和浅灰网格线混在一起看不清。
    QFont tick_font = p.font();
    tick_font.setPointSizeF(9.0);
    tick_font.setBold(true);

    // Draw denser helper grid first, then label only real integer ticks.
    for (int i = 0; i <= 10; ++i) {
        const double y = pr.bottom() - pr.height() * i / 10.0;
        p.setPen(grid_color_);
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.right(), y));
    }
    const double y_step = fractionalTickStep(min_y, max_y, 12);
    const double first_y_tick = std::ceil(min_y / y_step) * y_step;
    for (double value = first_y_tick; value <= max_y + y_step * 0.25; value += y_step) {
        const double y = pr.bottom() - (value - min_y) / (max_y - min_y) * pr.height();
        if (y < pr.top() - 0.5 || y > pr.bottom() + 0.5) continue;
        p.setPen(text_color_);
        p.setFont(tick_font);
        // y 轴刻度数字右对齐到绘图区左边界，读起来更整齐清晰。
        p.drawText(QRectF(0, y - 9, pr.left() - 6, 18),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(value, 'f', y_step < 1.0 ? 2 : 0));
    }
    if (!left_axis_label_.isEmpty()) {
        p.setPen(text_color_);
        p.setFont(tick_font);
        p.drawText(QRectF(0, pr.top() - 28, pr.left() - 4, 18),
                   Qt::AlignRight | Qt::AlignVCenter, left_axis_label_);
    }
    if (has_right_axis) {
        const double right_step = fractionalTickStep(right_min_y, right_max_y, 12);
        const double first_right_tick = std::ceil(right_min_y / right_step) * right_step;
        for (double value = first_right_tick; value <= right_max_y + right_step * 0.25; value += right_step) {
            const double y = pr.bottom() - (value - right_min_y) / (right_max_y - right_min_y) * pr.height();
            if (y < pr.top() - 0.5 || y > pr.bottom() + 0.5) continue;
            p.setPen(QColor("#EA580C"));
            p.setFont(tick_font);
            p.drawText(QRectF(pr.right() + 6, y - 9, width() - pr.right() - 8, 18),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(value, 'f', right_step < 1.0 ? 2 : 0));
        }
        if (!right_axis_label_.isEmpty()) {
            p.setPen(QColor("#EA580C"));
            p.setFont(tick_font);
            p.drawText(QRectF(pr.right() + 6, pr.top() - 28, width() - pr.right() - 8, 18),
                       Qt::AlignLeft | Qt::AlignVCenter, right_axis_label_);
        }
    }
    for (int i = 0; i <= 10; ++i) {
        const double x = pr.left() + pr.width() * i / 10.0;
        p.setPen(grid_color_);
        p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.bottom()));
    }
    const int x_step = integerTickStep(min_x, max_x, 10);
    const int first_x_tick = static_cast<int>(std::ceil(min_x / x_step)) * x_step;
    for (int value = first_x_tick; value <= static_cast<int>(std::floor(max_x)); value += x_step) {
        const double x = pr.left() + (static_cast<double>(value) - min_x) / (max_x - min_x) * pr.width();
        p.setPen(text_color_);
        p.setFont(tick_font);
        p.drawText(QRectF(x - 28, pr.bottom() + 4, 56, 18),
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(value));
    }
    p.setPen(QPen(axis_color_, 2));
    p.drawRect(pr);
    if (has_right_axis) {
        p.setPen(QPen(QColor("#EA580C"), 2));
        p.drawLine(QPointF(pr.right(), pr.top()), QPointF(pr.right(), pr.bottom()));
    }

    // 顶部图例：利用绘图区上方的空白标出每条曲线的颜色和名称。
    QFont legend_font = p.font();
    legend_font.setPointSizeF(9.5);
    legend_font.setBold(true);
    p.setFont(legend_font);
    double legend_x = pr.left();
    const double legend_y = pr.top() - 20.0;
    for (const auto& c : curves_) {
        if (c.label.isEmpty()) continue;
        const QString label = displayLabel(c.label);
        p.setBrush(c.color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(legend_x + 6, legend_y - 4), 5, 5);
        p.setPen(text_color_);
        const double text_w = p.fontMetrics().horizontalAdvance(label);
        p.drawText(QPointF(legend_x + 16, legend_y), label);
        legend_x += 16 + text_w + 22;
    }

    for (const auto& c : curves_) {
        if (!c.xs || !c.ys) continue;
        QPolygonF poly;
        const size_t n = std::min(c.xs->size(), c.ys->size());
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(static_cast<int>(&c - &curves_[0]), min_y, max_y);
        for (size_t i = 0; i < n; ++i) poly << toScreen((*c.xs)[i], (*c.ys)[i], min_x, max_x, curve_min_y, curve_max_y);
        p.setPen(QPen(c.color, 2));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
        p.setPen(QPen(point_outline_color_, 3));
        p.setBrush(c.color);
        for (int i = 0; i < poly.size(); ++i) {
            const auto& pt = poly[i];
            p.drawEllipse(pt, 5, 5);
            p.setPen(QPen(isSelected(static_cast<int>(&c - &curves_[0]), i) ? QColor("#FACC15") : axis_color_,
                          isSelected(static_cast<int>(&c - &curves_[0]), i) ? 3 : 1));
            p.drawEllipse(pt, isSelected(static_cast<int>(&c - &curves_[0]), i) ? 7 : 5,
                          isSelected(static_cast<int>(&c - &curves_[0]), i) ? 7 : 5);
            p.setPen(QPen(point_outline_color_, 3));
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
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(ci, min_y, max_y);
        const int n = static_cast<int>(std::min(c.xs->size(), c.ys->size()));
        for (int i = 0; i < n; ++i) {
            QPointF s = toScreen((*c.xs)[i], (*c.ys)[i], min_x, max_x, curve_min_y, curve_max_y);
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
    pushUndoSnapshot();
    auto* shared_xs = c.xs;
    shared_xs->erase(shared_xs->begin() + point_index);
    for (auto& related : curves_) {
        if (related.xs == shared_xs && related.ys && point_index < static_cast<int>(related.ys->size())) {
            related.ys->erase(related.ys->begin() + point_index);
        }
    }
    normalizeSelection();
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
    setFocus(Qt::MouseFocusReason);
    auto [ci, pi, d] = nearestPoint(event->position());
    if (event->button() == Qt::RightButton && ci >= 0 && d <= 14.0) {
        return;
    }
    if (ci >= 0 && d <= 14.0) {
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const auto key = std::make_pair(ci, pi);
            if (selected_points_.contains(key)) selected_points_.erase(key);
            else selected_points_.insert(key);
        } else if (!selected_points_.contains({ci, pi})) {
            selected_points_.clear();
            selected_points_.insert({ci, pi});
        }
        drag_curve_ = ci;
        drag_index_ = pi;
        drag_start_pos_ = event->position();
        double min_x, max_x, min_y, max_y;
        ranges(min_x, max_x, min_y, max_y);
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(ci, min_y, max_y);
        drag_anchor_x_ = xFromScreen(event->position(), min_x, max_x);
        drag_anchor_y_ = yFromScreen(event->position(), curve_min_y, curve_max_y);
        drag_started_ = false;
        update();
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (drag_curve_ < 0 || drag_index_ < 0) return;
    if (!drag_started_ && (event->position() - drag_start_pos_).manhattanLength() >= 2.0) {
        pushUndoSnapshot();
        drag_started_ = true;
    }
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    const double x = xFromScreen(event->position(), min_x, max_x);
    const auto [anchor_min_y, anchor_max_y] = yRangeForCurve(drag_curve_, min_y, max_y);
    double y = yFromScreen(event->position(), anchor_min_y, anchor_max_y);

    const bool lock_x = axisLockKeyDown('X');
    const bool lock_y = axisLockKeyDown('Y');
    const double target_x = lock_x ? drag_anchor_x_ : x;
    if (lock_y) y = drag_anchor_y_;
    const double dx = target_x - drag_anchor_x_;
    const double dy = y - drag_anchor_y_;
    const double screen_dy = event->position().y() - drag_start_pos_.y();

    std::vector<std::pair<int, int>> targets;
    if (!selected_points_.empty()) {
        targets.assign(selected_points_.begin(), selected_points_.end());
    } else {
        targets.push_back({drag_curve_, drag_index_});
    }

    for (const auto& [ci, pi] : targets) {
        if (ci < 0 || ci >= static_cast<int>(curves_.size())) continue;
        auto& c = curves_[ci];
        if (!c.xs || !c.ys || pi < 0 || pi >= static_cast<int>(std::min(c.xs->size(), c.ys->size()))) continue;
        (*c.xs)[pi] = std::clamp((*c.xs)[pi] + dx, min_x, max_x);
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(ci, min_y, max_y);
        const double curve_dy = lock_y ? 0.0 : -screen_dy / plotRect().height() * (curve_max_y - curve_min_y);
        (*c.ys)[pi] = std::clamp((*c.ys)[pi] + curve_dy, curve_min_y, curve_max_y);
    }
    for (auto& c : curves_) {
        sortSharedAxis(c.xs);
    }
    drag_start_pos_ = event->position();
    drag_anchor_x_ = target_x;
    drag_anchor_y_ = y;
    normalizeSelection();
    auto [new_curve, new_index, new_distance] = nearestPoint(event->position());
    if (new_curve >= 0 && new_distance <= 18.0) {
        drag_curve_ = new_curve;
        drag_index_ = new_index;
        if (selected_points_.empty()) {
            selected_points_.insert({new_curve, new_index});
        }
    }
    emit curveChanged();
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    drag_curve_ = -1;
    drag_index_ = -1;
    drag_started_ = false;
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (curves_.empty() || !curves_[0].xs || !curves_[0].ys) return;
    if (event->button() == Qt::RightButton) {
        auto [ci, pi, d] = nearestPoint(event->position());
        if (ci >= 0 && d <= 14.0) deletePoint(ci, pi);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    pushUndoSnapshot();
    double min_x, max_x, min_y, max_y;
    ranges(min_x, max_x, min_y, max_y);
    const double x = xFromScreen(event->position(), min_x, max_x);
    const double y = yFromScreen(event->position(), min_y, max_y);
    auto* shared_xs = curves_[0].xs;
    const auto insert_it = std::lower_bound(shared_xs->begin(), shared_xs->end(), x);
    const int insert_index = static_cast<int>(std::distance(shared_xs->begin(), insert_it));
    std::vector<double> inserted_ys(curves_.size(), y);
    for (size_t i = 0; i < curves_.size(); ++i) {
        const auto [curve_min_y, curve_max_y] = yRangeForCurve(static_cast<int>(i), min_y, max_y);
        inserted_ys[i] = yFromScreen(event->position(), curve_min_y, curve_max_y);
    }
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

void CurveEditor::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Undo)) {
        undo();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

CurveEditor::Snapshot CurveEditor::makeSnapshot() const {
    Snapshot snapshot;
    snapshot.xs.reserve(curves_.size());
    snapshot.ys.reserve(curves_.size());
    for (const auto& curve : curves_) {
        snapshot.xs.push_back(curve.xs ? *curve.xs : std::vector<double>{});
        snapshot.ys.push_back(curve.ys ? *curve.ys : std::vector<double>{});
    }
    return snapshot;
}

void CurveEditor::restoreSnapshot(const Snapshot& snapshot) {
    for (size_t i = 0; i < curves_.size(); ++i) {
        if (i < snapshot.xs.size() && curves_[i].xs) *curves_[i].xs = snapshot.xs[i];
        if (i < snapshot.ys.size() && curves_[i].ys) *curves_[i].ys = snapshot.ys[i];
    }
    normalizeSelection();
    emit curveChanged();
    update();
}

void CurveEditor::pushUndoSnapshot() {
    undo_stack_.push_back(makeSnapshot());
    if (undo_stack_.size() > 80) {
        undo_stack_.erase(undo_stack_.begin());
    }
}

void CurveEditor::undo() {
    if (undo_stack_.empty()) return;
    const Snapshot snapshot = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    restoreSnapshot(snapshot);
}

bool CurveEditor::isSelected(int curve_index, int point_index) const {
    return selected_points_.contains({curve_index, point_index});
}

void CurveEditor::normalizeSelection() {
    std::set<std::pair<int, int>> normalized;
    for (const auto& [ci, pi] : selected_points_) {
        if (ci < 0 || ci >= static_cast<int>(curves_.size())) continue;
        const auto& c = curves_[ci];
        if (!c.xs || !c.ys || pi < 0 || pi >= static_cast<int>(std::min(c.xs->size(), c.ys->size()))) continue;
        normalized.insert({ci, pi});
    }
    selected_points_ = std::move(normalized);
}

} // namespace pubg::ui
