#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include "Config.hpp"
#include "RegionManager.hpp"

namespace pubg::ui {

// 截图缩放比例校准窗口，对应 Python modules/region_calibrator_auto.py。
// 用于调整 region_scaling_settings 中武器图标、武器名、姿势区域的目标缩放尺寸。
class ScaleCalibrationWindow : public QWidget {
    Q_OBJECT
public:
    ScaleCalibrationWindow(Config& config, RegionManager& regions, QWidget* parent = nullptr);

private:
    void buildUi();
    void loadRegion();
    void applySize();
    void saveConfig();
    void resetDefault();
    void runAutoSearch();

    Config& config_;
    RegionManager& regions_;
    QComboBox* region_combo_ = nullptr;
    QLineEdit* width_edit_ = nullptr;
    QLineEdit* height_edit_ = nullptr;
    QLabel* result_label_ = nullptr;
};

} // namespace pubg::ui
