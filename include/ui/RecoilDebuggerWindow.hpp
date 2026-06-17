#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include "RecoilControl.hpp"
#include "ui/CurveEditor.hpp"

namespace pubg::ui {

// 压枪参数调试窗口，对应 Python modules/recoil_debugger.py。
// 左侧选择武器/配件和曲线类型，右侧曲线图支持拖点修改、添加、删除并保存热重载。
class RecoilDebuggerWindow : public QWidget {
    Q_OBJECT
public:
    RecoilDebuggerWindow(Config& config, RecoilControl& recoil, QWidget* parent = nullptr);

private:
    void buildUi();
    void reloadOptions();
    void reloadCurves();
    void saveAndApply();
    void addCurrentPoint();
    std::vector<double> xAxisFor(size_t count) const;
    std::string currentWeaponType(const Json& rc) const;
    double currentXAxisMax(const Json& rc) const;
    void showCurveEditor();
    void showDmrSlider();

    Config& config_;
    RecoilControl& recoil_;
    QComboBox* weapon_combo_ = nullptr;
    QComboBox* scope_combo_ = nullptr;
    QComboBox* grip_combo_ = nullptr;
    QComboBox* muzzle_combo_ = nullptr;
    QComboBox* stock_combo_ = nullptr;
    QComboBox* curve_type_combo_ = nullptr;
    QLabel* status_label_ = nullptr;
    QWidget* right_panel_ = nullptr;
    CurveEditor* curve_editor_ = nullptr;
    QWidget* dmr_panel_ = nullptr;
    QSlider* dmr_slider_ = nullptr;
    QLabel* dmr_value_label_ = nullptr;
    std::vector<double> curve_x_;
    std::vector<double> curve_y_;
    std::string active_json_key_;
    std::string active_curve_name_;
};

} // namespace pubg::ui
