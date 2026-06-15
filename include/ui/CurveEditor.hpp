#pragma once

#include <QColor>
#include <QMouseEvent>
#include <QWidget>

namespace pubg::ui {

// 可拖拽曲线编辑控件。
// 支持多条曲线共享 x 轴，左键拖点修改 x/y，右键删除点，双击添加中点。
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    struct Curve {
        QString label;
        QColor color;
        std::vector<double>* xs = nullptr;
        std::vector<double>* ys = nullptr;
    };

    explicit CurveEditor(QWidget* parent = nullptr);
    void setCurves(std::vector<Curve> curves);

signals:
    void curveChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF plotRect() const;
    void ranges(double& min_x, double& max_x, double& min_y, double& max_y) const;
    QPointF toScreen(double x, double y, double min_x, double max_x, double min_y, double max_y) const;
    std::pair<double, double> fromScreen(const QPointF& p, double min_x, double max_x, double min_y, double max_y) const;
    std::tuple<int, int, double> nearestPoint(const QPointF& p) const;
    QString displayLabel(const QString& label) const;
    double interpolatedY(const std::vector<double>& xs, const std::vector<double>& ys, double x) const;
    void deletePoint(int curve_index, int point_index);
    void sortSharedAxis(std::vector<double>* shared_xs);

    std::vector<Curve> curves_;
    int drag_curve_ = -1;
    int drag_index_ = -1;
};

} // namespace pubg::ui
