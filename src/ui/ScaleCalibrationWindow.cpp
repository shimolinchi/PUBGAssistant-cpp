#include "ui/ScaleCalibrationWindow.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace pubg::ui {

ScaleCalibrationWindow::ScaleCalibrationWindow(Config& config, RegionManager& regions, QWidget* parent)
    : QWidget(parent), config_(config), regions_(regions) {
    setWindowTitle(QStringLiteral("截图区域缩放比例校准"));
    resize(760, 520);
    buildUi();
    loadRegion();
}

void ScaleCalibrationWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("截图区域缩放比例校准"), this);
    title->setStyleSheet("font-size:20px;font-weight:700;color:#111827;");
    root->addWidget(title);
    region_combo_ = new QComboBox(this);
    region_combo_->addItems({"weapon_region", "weapon1_name_region", "weapon2_name_region", "stance_region"});
    root->addWidget(region_combo_);
    auto* form = new QFormLayout();
    width_edit_ = new QLineEdit(this);
    height_edit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("目标宽度"), width_edit_);
    form->addRow(QStringLiteral("目标高度"), height_edit_);
    root->addLayout(form);
    auto* row = new QHBoxLayout();
    auto* apply = new QPushButton(QStringLiteral("应用刷新"), this);
    auto* save = new QPushButton(QStringLiteral("保存配置"), this);
    auto* reset = new QPushButton(QStringLiteral("恢复默认"), this);
    auto* autoBtn = new QPushButton(QStringLiteral("自动校准"), this);
    row->addWidget(apply); row->addWidget(save); row->addWidget(reset); row->addWidget(autoBtn);
    root->addLayout(row);
    result_label_ = new QLabel(QStringLiteral("自动校准结果: --"), this);
    root->addWidget(result_label_);
    connect(region_combo_, &QComboBox::currentTextChanged, this, [this] { loadRegion(); });
    connect(apply, &QPushButton::clicked, this, &ScaleCalibrationWindow::applySize);
    connect(save, &QPushButton::clicked, this, &ScaleCalibrationWindow::saveConfig);
    connect(reset, &QPushButton::clicked, this, &ScaleCalibrationWindow::resetDefault);
    connect(autoBtn, &QPushButton::clicked, this, &ScaleCalibrationWindow::runAutoSearch);
}

void ScaleCalibrationWindow::loadRegion() {
    const std::string key = region_combo_->currentText().toStdString();
    const auto scaling = config_.read([&](const Json& data) {
        return data.value("region_scaling_settings", Json::object()).value(key, Json::object());
    });
    width_edit_->setText(QString::number(scaling.value("width", 100)));
    height_edit_->setText(QString::number(scaling.value("height", 50)));
}

void ScaleCalibrationWindow::applySize() {
    const std::string key = region_combo_->currentText().toStdString();
    const int width = width_edit_->text().toInt();
    const int height = height_edit_->text().toInt();
    config_.write([&](Json& data) {
        auto& r = data["region_scaling_settings"][key];
        r["width"] = width;
        r["height"] = height;
    });
    result_label_->setText(QStringLiteral("已应用到当前配置对象，保存后生效。"));
}

void ScaleCalibrationWindow::saveConfig() {
    applySize();
    config_.save();
    result_label_->setText(QStringLiteral("配置已保存。"));
}

void ScaleCalibrationWindow::resetDefault() {
    width_edit_->setText("100");
    height_edit_->setText("50");
    applySize();
}

void ScaleCalibrationWindow::runAutoSearch() {
    result_label_->setText(QStringLiteral("自动校准入口已还原；具体搜索需在 Windows 截图环境下继续微调。"));
}

} // namespace pubg::ui
