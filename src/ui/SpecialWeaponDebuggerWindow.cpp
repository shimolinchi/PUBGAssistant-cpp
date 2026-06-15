#include "ui/SpecialWeaponDebuggerWindow.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace pubg::ui {

SpecialWeaponDebuggerWindow::SpecialWeaponDebuggerWindow(Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    setWindowTitle(QStringLiteral("特殊武器参数调试"));
    resize(840, 500);
    buildUi();
    renderCurrentWeapon();
}

void SpecialWeaponDebuggerWindow::buildUi() {
    auto* root = new QHBoxLayout(this);
    auto* left = new QWidget(this);
    left->setFixedWidth(190);
    auto* ll = new QVBoxLayout(left);
    auto* title = new QLabel(QStringLiteral("特殊武器"), this);
    title->setStyleSheet("font-size:18px;font-weight:700;");
    ll->addWidget(title);
    weapon_combo_ = new QComboBox(this);
    weapon_combo_->addItems({QStringLiteral("火箭筒"), "VSS", QStringLiteral("十字弩"), QStringLiteral("迫击炮"), QStringLiteral("投掷物"), "C4"});
    ll->addWidget(weapon_combo_);
    throw_mode_combo_ = new QComboBox(this);
    throw_mode_combo_->addItems({QStringLiteral("普通投掷"), QStringLiteral("跳投")});
    ll->addWidget(throw_mode_combo_);
    auto* add = new QPushButton(QStringLiteral("添加标点"), this);
    ll->addWidget(add);
    auto* save = new QPushButton(QStringLiteral("保存并应用"), this);
    ll->addWidget(save);
    status_label_ = new QLabel(QStringLiteral("就绪"), this);
    ll->addWidget(status_label_);
    ll->addStretch();
    curve_editor_ = new CurveEditor(this);
    right_panel_ = curve_editor_;
    right_panel_->setStyleSheet("background:#FFFFFF;border:1px solid #E5E7EB;color:#6B7280;");
    root->addWidget(left);
    root->addWidget(right_panel_, 1);
    connect(weapon_combo_, &QComboBox::currentTextChanged, this, [this] { renderCurrentWeapon(); });
    connect(throw_mode_combo_, &QComboBox::currentTextChanged, this, [this] { renderCurrentWeapon(); });
    connect(add, &QPushButton::clicked, this, &SpecialWeaponDebuggerWindow::addPoint);
    connect(save, &QPushButton::clicked, this, &SpecialWeaponDebuggerWindow::saveAndApply);
    connect(curve_editor_, &CurveEditor::curveChanged, this, [this] { status_label_->setText(QStringLiteral("曲线已修改，点击保存并应用。")); });
}

void SpecialWeaponDebuggerWindow::renderCurrentWeapon() {
    const auto w = weapon_combo_->currentText();
    param_edits_.clear();
    active_y2_.clear();
    ys2_.clear();
    if (right_panel_ != curve_editor_) {
        layout()->replaceWidget(right_panel_, curve_editor_);
        right_panel_->deleteLater();
        right_panel_ = curve_editor_;
    }
    curve_editor_->show();
    throw_mode_combo_->setVisible(w == QStringLiteral("投掷物"));
    if (w == QStringLiteral("火箭筒")) bindCurve("rocket_config", "calib_dists", "calib_ratios", QColor("#2563EB"));
    else if (w == "VSS") bindCurve("vss_config", "calib_dists", "calib_drops_ratio", QColor("#16A34A"));
    else if (w == QStringLiteral("十字弩")) bindCurve("crossbow_config", "calib_dists", "calib_drops_ratio", QColor("#EA580C"));
    else if (w == QStringLiteral("投掷物")) bindThrowables(throw_mode_combo_->currentText() == QStringLiteral("跳投"));
    else if (w == QStringLiteral("迫击炮")) bindParams("mortar_config", {{"a_param", QStringLiteral("上坡修正 a")}, {"b_param", QStringLiteral("下坡修正 b")}});
    else if (w == "C4") bindParams("c4_config", {{"target_speed", QStringLiteral("推荐起步速度 km/h")}, {"jump_distance_threshold", QStringLiteral("推荐跳车距离 m")}});
    status_label_->setText(QStringLiteral("当前：") + w);
}

void SpecialWeaponDebuggerWindow::bindCurve(const std::string& config_key, const std::string& x_key, const std::string& y_key, const QColor& color) {
    active_config_ = config_key;
    active_x_ = x_key;
    active_y_ = y_key;
    xs_.clear();
    ys_.clear();
    const auto cfg = config_.read([&](const Json& data) {
        return data.value(config_key, Json::object());
    });
    if (cfg.contains(x_key)) for (const auto& v : cfg[x_key]) xs_.push_back(v.get<double>());
    if (cfg.contains(y_key)) for (const auto& v : cfg[y_key]) ys_.push_back(v.get<double>());
    curve_editor_->setCurves({CurveEditor::Curve{QString::fromStdString(y_key), color, &xs_, &ys_}});
}

void SpecialWeaponDebuggerWindow::bindThrowables(bool jump) {
    active_config_ = "throwables_config";
    active_x_ = jump ? "jump_calib_dists" : "calib_dists";
    active_y_ = jump ? "jump_calib_elevations_ratio" : "calib_elevations_ratio";
    active_y2_ = jump ? "jump_calib_times" : "calib_times";
    xs_.clear();
    ys_.clear();
    ys2_.clear();
    const auto cfg = config_.read([&](const Json& data) {
        return data.value(active_config_, Json::object());
    });
    if (cfg.contains(active_x_)) for (const auto& v : cfg[active_x_]) xs_.push_back(v.get<double>());
    if (cfg.contains(active_y_)) for (const auto& v : cfg[active_y_]) ys_.push_back(v.get<double>());
    if (cfg.contains(active_y2_)) for (const auto& v : cfg[active_y2_]) ys2_.push_back(v.get<double>());
    curve_editor_->setCurves({
        CurveEditor::Curve{QStringLiteral("抬高高度"), QColor("#7C3AED"), &xs_, &ys_},
        CurveEditor::Curve{QStringLiteral("瞬爆时间"), QColor("#EA580C"), &xs_, &ys2_},
    });
}

void SpecialWeaponDebuggerWindow::bindParams(const std::string& config_key, const std::vector<std::pair<std::string, QString>>& fields) {
    active_config_ = config_key;
    active_x_.clear();
    active_y_.clear();
    auto* panel = new QWidget(this);
    panel->setStyleSheet("background:#FFFFFF;color:#111827;");
    auto* form = new QFormLayout(panel);
    const auto cfg = config_.read([&](const Json& data) {
        return data.value(config_key, Json::object());
    });
    for (const auto& [key, label] : fields) {
        auto* edit = new QLineEdit(panel);
        edit->setText(QString::number(cfg.value(key, 0.0)));
        param_edits_[key] = edit;
        form->addRow(label, edit);
    }
    layout()->replaceWidget(right_panel_, panel);
    right_panel_->hide();
    right_panel_ = panel;
}

void SpecialWeaponDebuggerWindow::addPoint() {
    if (right_panel_ != curve_editor_) return;
    if (xs_.empty()) {
        xs_.push_back(50.0);
        ys_.push_back(0.5);
        if (!active_y2_.empty()) ys2_.push_back(1.0);
    } else {
        xs_.push_back(xs_.back() + 5.0);
        ys_.push_back(ys_.empty() ? 0.5 : ys_.back());
        if (!active_y2_.empty()) ys2_.push_back(ys2_.empty() ? 1.0 : ys2_.back());
    }
    if (!active_y2_.empty()) {
        curve_editor_->setCurves({
            CurveEditor::Curve{QStringLiteral("抬高高度"), QColor("#7C3AED"), &xs_, &ys_},
            CurveEditor::Curve{QStringLiteral("瞬爆时间"), QColor("#EA580C"), &xs_, &ys2_},
        });
    } else {
        curve_editor_->setCurves({CurveEditor::Curve{QString::fromStdString(active_y_), QColor("#2563EB"), &xs_, &ys_}});
    }
}

void SpecialWeaponDebuggerWindow::saveAndApply() {
    config_.write([&](Json& data) {
        if (!param_edits_.empty()) {
            for (const auto& [key, edit] : param_edits_) {
                data[active_config_][key] = edit->text().toDouble();
            }
        } else {
            data[active_config_][active_x_] = xs_;
            data[active_config_][active_y_] = ys_;
            if (!active_y2_.empty()) data[active_config_][active_y2_] = ys2_;
        }
    });
    config_.save();
    status_label_->setText(QStringLiteral("参数已保存、重载并应用。"));
}

} // namespace pubg::ui
