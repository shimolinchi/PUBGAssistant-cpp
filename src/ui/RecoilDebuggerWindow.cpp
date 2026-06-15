#include "ui/RecoilDebuggerWindow.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace pubg::ui {

RecoilDebuggerWindow::RecoilDebuggerWindow(Config& config, RecoilControl& recoil, QWidget* parent)
    : QWidget(parent), config_(config), recoil_(recoil) {
    setWindowTitle(QStringLiteral("压枪参数调试"));
    resize(800, 630);
    buildUi();
    reloadOptions();
}

void RecoilDebuggerWindow::buildUi() {
    auto* root = new QHBoxLayout(this);
    auto* left = new QWidget(this);
    left->setFixedWidth(230);
    auto* ll = new QVBoxLayout(left);
    auto* title = new QLabel(QStringLiteral("压枪参数调试"), this);
    title->setStyleSheet("font-size:18px;font-weight:700;");
    ll->addWidget(title);
    auto* form = new QFormLayout();
    weapon_combo_ = new QComboBox(this);
    scope_combo_ = new QComboBox(this);
    grip_combo_ = new QComboBox(this);
    muzzle_combo_ = new QComboBox(this);
    stock_combo_ = new QComboBox(this);
    curve_type_combo_ = new QComboBox(this);
    curve_type_combo_->addItems({QStringLiteral("武器曲线"), QStringLiteral("倍镜倍率"), QStringLiteral("握把倍率"), QStringLiteral("枪口倍率"), QStringLiteral("枪托倍率")});
    form->addRow(QStringLiteral("武器"), weapon_combo_);
    form->addRow(QStringLiteral("编辑"), curve_type_combo_);
    form->addRow(QStringLiteral("倍镜"), scope_combo_);
    form->addRow(QStringLiteral("握把"), grip_combo_);
    form->addRow(QStringLiteral("枪口"), muzzle_combo_);
    form->addRow(QStringLiteral("枪托"), stock_combo_);
    ll->addLayout(form);
    auto* add_point = new QPushButton(QStringLiteral("添加标点"), this);
    ll->addWidget(add_point);
    auto* save = new QPushButton(QStringLiteral("保存并应用"), this);
    ll->addWidget(save);
    status_label_ = new QLabel(QStringLiteral("就绪"), this);
    ll->addWidget(status_label_);
    root->addWidget(left);
    curve_editor_ = new CurveEditor(this);
    curve_editor_->setStyleSheet("background:#F8FAFC;border:1px solid #E5E7EB;");
    root->addWidget(curve_editor_, 1);
    connect(add_point, &QPushButton::clicked, this, &RecoilDebuggerWindow::addCurrentPoint);
    connect(save, &QPushButton::clicked, this, &RecoilDebuggerWindow::saveAndApply);
    connect(weapon_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(curve_type_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(scope_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(grip_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(muzzle_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(stock_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(curve_editor_, &CurveEditor::curveChanged, this, [this] {
        status_label_->setText(QStringLiteral("曲线已修改，点击保存并应用。"));
    });
}

void RecoilDebuggerWindow::reloadOptions() {
    weapon_combo_->clear();
    const auto weapons = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object()).value("weapons", Json::object());
    });
    for (auto it = weapons.begin(); it != weapons.end(); ++it) {
        weapon_combo_->addItem(QString::fromStdString(it.key()));
    }
    scope_combo_->addItems({"hip", "red_dot", "holographic", "2", "3", "4", "6", "8"});
    grip_combo_->addItems({"vertical", "half", "light", "thumb", "tilted", "laser"});
    muzzle_combo_->addItems({"ar_dmr_compensator", "ar_dmr_silencer", "ar_dmr_suppressor", "ar_dmr_braker", "dmr_sr_compensator", "smg_compensator", "smg_suppressor", "smg_silencer"});
    stock_combo_->addItems({"tactical", "heavy", "uzi", "cheek_pad"});
    reloadCurves();
}

std::vector<double> RecoilDebuggerWindow::xAxisFor(size_t count) const {
    std::vector<double> xs;
    const double step = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object()).value("recoil_curve_step", 0.4);
    });
    for (size_t i = 0; i < count; ++i) xs.push_back(static_cast<double>(i) * step);
    return xs;
}

void RecoilDebuggerWindow::reloadCurves() {
    curve_x_.clear();
    curve_y_.clear();
    active_json_key_.clear();
    active_curve_name_.clear();
    const auto rc = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object());
    });
    const QString type = curve_type_combo_->currentText();
    Json curve;
    bool has_curve = false;
    if (type == QStringLiteral("武器曲线")) {
        auto weapon = weapon_combo_->currentText().toStdString();
        active_json_key_ = "weapons";
        active_curve_name_ = weapon;
        const auto weapons = rc.value("weapons", Json::object());
        if (weapons.contains(weapon) && weapons[weapon].contains("recoil_curve")) {
            curve = weapons[weapon]["recoil_curve"];
            has_curve = true;
        }
    } else {
        std::string group;
        std::string key;
        if (type == QStringLiteral("倍镜倍率")) { group = "scope_multipliers"; key = scope_combo_->currentText().toStdString(); }
        else if (type == QStringLiteral("握把倍率")) { group = "grip_multipliers"; key = grip_combo_->currentText().toStdString(); }
        else if (type == QStringLiteral("枪口倍率")) { group = "muzzle_multipliers"; key = muzzle_combo_->currentText().toStdString(); }
        else { group = "stock_multipliers"; key = stock_combo_->currentText().toStdString(); }
        active_json_key_ = group;
        active_curve_name_ = key;
        const auto group_obj = rc.value(group, Json::object());
        if (group_obj.contains(key)) {
            curve = group_obj[key];
            has_curve = true;
        }
    }
    if (has_curve) {
        if (curve.is_array()) {
            for (const auto& v : curve) curve_y_.push_back(v.get<double>());
        } else if (curve.is_number()) {
            curve_y_.push_back(curve.get<double>());
        }
    }
    if (curve_y_.empty()) curve_y_.push_back(1.0);
    curve_x_ = xAxisFor(curve_y_.size());
    curve_editor_->setCurves({CurveEditor::Curve{type, QColor("#2563EB"), &curve_x_, &curve_y_}});
}

void RecoilDebuggerWindow::addCurrentPoint() {
    if (curve_y_.empty()) {
        curve_x_.push_back(0.0);
        curve_y_.push_back(1.0);
    } else {
        const double step = config_.read([](const Json& data) {
            return data.value("recoil_settings", Json::object()).value("recoil_curve_step", 0.4);
        });
        curve_x_.push_back(curve_x_.back() + step);
        curve_y_.push_back(curve_y_.back());
    }
    curve_editor_->setCurves({CurveEditor::Curve{curve_type_combo_->currentText(), QColor("#2563EB"), &curve_x_, &curve_y_}});
    status_label_->setText(QStringLiteral("已添加标点，点击保存并应用。"));
}

void RecoilDebuggerWindow::saveAndApply() {
    config_.write([&](Json& data) {
        auto& rc = data["recoil_settings"];
        if (active_json_key_ == "weapons") {
            rc["weapons"][active_curve_name_]["recoil_curve"] = curve_y_;
        } else if (!active_json_key_.empty()) {
            rc[active_json_key_][active_curve_name_] = curve_y_;
        }
    });
    config_.save();
    recoil_.reloadConfig();
    status_label_->setText(QStringLiteral("参数已保存、重载并应用。"));
}

} // namespace pubg::ui
