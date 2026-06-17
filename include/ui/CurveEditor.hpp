#pragma once

#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWidget>
#include <optional>
#include <set>

namespace pubg::ui {

// 可拖拽曲线编辑控件。
// 支持多条曲线共享 x 轴，左键拖点修改 x/y，右键删除点，双击添加中点。
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    enum class AxisSide {
        Left,
        Right,
    };

    struct Curve {
        QString label;
        QColor color;
        std::vector<double>* xs = nullptr;
        std::vector<double>* ys = nullptr;
        AxisSide axis = AxisSide::Left;
    };

    explicit CurveEditor(QWidget* parent = nullptr);
    void setCurves(std::vector<Curve> curves);
    void setFixedXRange(double min_x, double max_x);
    void setFixedYRange(double min_y, double max_y);
    void setFixedRightYRange(double min_y, double max_y);
    void setAxisLabels(QString left_label, QString right_label = {});
    void clearFixedRanges();
    void clearSelection();
    void clearUndoHistory();
    void nudgeSelectedY(double delta);

signals:
    void curveChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Snapshot {
        std::vector<std::vector<double>> xs;
        std::vector<std::vector<double>> ys;
    };

    QRectF plotRect() const;
    void ranges(double& min_x, double& max_x, double& min_y, double& max_y) const;
    void rightYRange(double& min_y, double& max_y) const;
    std::pair<double, double> yRangeForCurve(int curve_index, double left_min_y, double left_max_y) const;
    QPointF toScreen(double x, double y, double min_x, double max_x, double min_y, double max_y) const;
    std::pair<double, double> fromScreen(const QPointF& p, double min_x, double max_x, double min_y, double max_y) const;
    double xFromScreen(const QPointF& p, double min_x, double max_x) const;
    double yFromScreen(const QPointF& p, double min_y, double max_y) const;
    std::tuple<int, int, double> nearestPoint(const QPointF& p) const;
    QString displayLabel(const QString& label) const;
    double interpolatedY(const std::vector<double>& xs, const std::vector<double>& ys, double x) const;
    void deletePoint(int curve_index, int point_index);
    void sortSharedAxis(std::vector<double>* shared_xs);
    void pushUndoSnapshot();
    void undo();
    Snapshot makeSnapshot() const;
    void restoreSnapshot(const Snapshot& snapshot);
    bool isSelected(int curve_index, int point_index) const;
    void normalizeSelection();

    std::vector<Curve> curves_;
    int drag_curve_ = -1;
    int drag_index_ = -1;
    QPointF drag_start_pos_;
    double drag_anchor_x_ = 0.0;
    double drag_anchor_y_ = 0.0;
    bool drag_started_ = false;
    std::set<std::pair<int, int>> selected_points_;
    std::vector<Snapshot> undo_stack_;
    std::optional<double> fixed_min_x_;
    std::optional<double> fixed_max_x_;
    std::optional<double> fixed_min_y_;
    std::optional<double> fixed_max_y_;
    std::optional<double> fixed_right_min_y_;
    std::optional<double> fixed_right_max_y_;
    QString left_axis_label_;
    QString right_axis_label_;
};

} // namespace pubg::ui
