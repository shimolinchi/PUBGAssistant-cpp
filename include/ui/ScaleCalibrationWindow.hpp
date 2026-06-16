#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSlider>
#include <QTimer>
#include <QWidget>

#include <filesystem>
#include <optional>
#include <vector>

#include "Config.hpp"
#include "RegionManager.hpp"

namespace pubg::ui {

// 截图缩放比例校准窗口，对应 Python modules/region_calibrator_auto.py。
// 用于调整 region_scaling_settings 中武器图标、武器名、姿势区域的目标缩放尺寸。
class ScaleCalibrationWindow : public QWidget {
    Q_OBJECT
public:
    ScaleCalibrationWindow(Config& config, RegionManager& regions, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void loadRegion();
    void applySize();
    void saveConfig();
    void runAutoSearch();
    void updatePreview();
    void reloadTemplates();
    void updateSizeFromSliders();
    void loadTemplatePreview();
    void stopRefresh();
    void startRefresh();
    [[nodiscard]] std::string currentRegionKey() const;
    [[nodiscard]] QPixmap scaledCapturePixmap(const std::string& key, int width, int height) const;
    [[nodiscard]] std::optional<double> scaledMatchScore(const std::string& key, int width, int height) const;

    Config& config_;
    RegionManager& regions_;
    QComboBox* region_combo_ = nullptr;
    QLineEdit* width_edit_ = nullptr;
    QLineEdit* height_edit_ = nullptr;
    QSlider* width_slider_ = nullptr;
    QSlider* height_slider_ = nullptr;
    QLabel* width_scale_label_ = nullptr;
    QLabel* height_scale_label_ = nullptr;
    QLabel* result_label_ = nullptr;
    QComboBox* template_combo_ = nullptr;
    QLabel* template_preview_ = nullptr;
    QLabel* capture_preview_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    std::vector<std::filesystem::path> template_paths_;
    std::filesystem::path current_template_path_;
    QPixmap template_outline_;
    int base_width_ = 100;
    int base_height_ = 50;
    bool updating_preview_ = false;
    bool closing_ = false;  // 窗口正在关闭，定时器回调据此早退，避免半析构状态下访问控件。
};

} // namespace pubg::ui
