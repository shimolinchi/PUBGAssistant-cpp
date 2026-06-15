#pragma once

#include <QWidget>

#include "RegionManager.hpp"

namespace pubg::ui {

// 全屏区域/比例尺校准覆盖层。
// 对应 Python RegionManager._start_calibration_overlay：鼠标拖拽选择矩形或线段，松开后写入 config。
class RegionCalibrationOverlay : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Region, Scale };

    RegionCalibrationOverlay(RegionManager& regions, QString target_name, Mode mode, bool force_square, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    RegionManager& regions_;
    QString target_name_;
    Mode mode_;
    bool force_square_ = false;
    bool dragging_ = false;
    QPoint start_;
    QPoint current_;
};

} // namespace pubg::ui
