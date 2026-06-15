#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include "Ballistics.hpp"
#include "Config.hpp"
#include "ui/CurveEditor.hpp"

namespace pubg::ui {

// 特殊武器参数调试窗口，对应 Python modules/special_weapon_debugger.py。
// 用于编辑火箭/VSS/十字弩/投掷物曲线，以及迫击炮/C4 参数。
class SpecialWeaponDebuggerWindow : public QWidget {
    Q_OBJECT
public:
    SpecialWeaponDebuggerWindow(Config& config, QWidget* parent = nullptr);

private:
    void buildUi();
    void renderCurrentWeapon();
    void saveAndApply();
    void bindCurve(const std::string& config_key, const std::string& x_key, const std::string& y_key, const QColor& color);
    void bindThrowables(bool jump);
    void bindParams(const std::string& config_key, const std::vector<std::pair<std::string, QString>>& fields);
    void addPoint();

    Config& config_;
    QComboBox* weapon_combo_ = nullptr;
    QComboBox* throw_mode_combo_ = nullptr;
    QLabel* status_label_ = nullptr;
    QWidget* right_panel_ = nullptr;
    CurveEditor* curve_editor_ = nullptr;
    std::string active_config_;
    std::string active_x_;
    std::string active_y_;
    std::vector<double> xs_;
    std::vector<double> ys_;
    std::string active_y2_;
    std::vector<double> ys2_;
    std::unordered_map<std::string, QLineEdit*> param_edits_;
};

} // namespace pubg::ui
