#include "ui/RecoilDebuggerWindow.hpp"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ui/Theme.hpp"

namespace pubg::ui {

namespace {

void addKeyItem(QComboBox* combo, const QString& text, const QString& key) {
    combo->addItem(text, key);
}

std::string currentKey(QComboBox* combo) {
    const auto data = combo->currentData();
    return data.isValid() ? data.toString().toStdString() : combo->currentText().toStdString();
}

QString currentCurveTypeKey(QComboBox* combo) {
    const auto data = combo->currentData();
    return data.isValid() ? data.toString() : combo->currentText();
}

double paddedYMaxFromZero(const std::vector<double>& values) {
    if (values.empty()) return 1.0;
    const auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
    const double span = std::max(1.0, *max_it - std::min(0.0, *min_it));
    return std::max(1.0, *max_it + span * 0.1);
}

double sampleSortedCurve(const std::vector<std::pair<double, double>>& points, double x) {
    if (points.empty()) return 0.0;
    if (points.size() == 1 || x <= points.front().first) return points.front().second;
    if (x >= points.back().first) return points.back().second;
    for (size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].first) {
            const auto [x0, y0] = points[i - 1];
            const auto [x1, y1] = points[i];
            if (std::abs(x1 - x0) < 1e-9) return y1;
            const double t = (x - x0) / (x1 - x0);
            return y0 + (y1 - y0) * t;
        }
    }
    return points.back().second;
}

} // namespace

RecoilDebuggerWindow::RecoilDebuggerWindow(Config& config, RecoilControl& recoil, QWidget* parent)
    : QWidget(parent), config_(config), recoil_(recoil) {
    setWindowTitle(QStringLiteral("压枪参数调试"));
    resize(760, 520);
    applyThemedPopupWindow(this, config_);
    buildUi();
    reloadOptions();
}

void RecoilDebuggerWindow::buildUi() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* left = new QWidget(this);
    left->setFixedWidth(220);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("压枪参数调试"), this);
    const auto theme = currentUiTheme(config_);
    title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:700;color:%1;").arg(theme.button_text));
    ll->addWidget(title);

    auto* form = new QFormLayout();
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    weapon_combo_ = new QComboBox(this);
    scope_combo_ = new QComboBox(this);
    grip_combo_ = new QComboBox(this);
    muzzle_combo_ = new QComboBox(this);
    stock_combo_ = new QComboBox(this);
    curve_type_combo_ = new QComboBox(this);
    addKeyItem(curve_type_combo_, QStringLiteral("武器曲线"), QStringLiteral("weapon"));
    addKeyItem(curve_type_combo_, QStringLiteral("倍镜倍率"), QStringLiteral("scope"));
    addKeyItem(curve_type_combo_, QStringLiteral("握把倍率"), QStringLiteral("grip"));
    addKeyItem(curve_type_combo_, QStringLiteral("枪口倍率"), QStringLiteral("muzzle"));
    addKeyItem(curve_type_combo_, QStringLiteral("枪托倍率"), QStringLiteral("stock"));

    form->addRow(QStringLiteral("武器"), weapon_combo_);
    form->addRow(QStringLiteral("编辑"), curve_type_combo_);
    form->addRow(QStringLiteral("倍镜"), scope_combo_);
    form->addRow(QStringLiteral("握把"), grip_combo_);
    form->addRow(QStringLiteral("枪口"), muzzle_combo_);
    form->addRow(QStringLiteral("枪托"), stock_combo_);
    ll->addLayout(form);

    auto* button_grid = new QGridLayout();
    button_grid->setHorizontalSpacing(8);
    button_grid->setVerticalSpacing(8);
    auto* add_point = new QPushButton(QStringLiteral("添加标点"), this);
    auto* sub01 = new QPushButton(QStringLiteral("-0.1"), this);
    auto* sub001 = new QPushButton(QStringLiteral("-0.01"), this);
    auto* add001 = new QPushButton(QStringLiteral("+0.01"), this);
    auto* add01 = new QPushButton(QStringLiteral("+0.1"), this);
    auto* save = new QPushButton(QStringLiteral("保存并应用"), this);
    button_grid->addWidget(sub01, 0, 0);
    button_grid->addWidget(sub001, 0, 1);
    button_grid->addWidget(add_point, 0, 2);
    button_grid->addWidget(add001, 1, 0);
    button_grid->addWidget(add01, 1, 1);
    button_grid->addWidget(save, 1, 2);
    ll->addLayout(button_grid);
    status_label_ = new QLabel(QStringLiteral("就绪"), this);
    status_label_->setWordWrap(true);
    status_label_->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:700;").arg(theme.accent));
    ll->addWidget(status_label_);
    ll->addStretch();

    auto* hint = new QLabel(QStringLiteral("操作提示\n- 拖拽控制点：修改数值\n- Ctrl + 点击：多选控制点\n- Ctrl + Z：撤回上一次拖动\n- 按住 X/Y：锁定对应轴\n- 双击空白：新增控制点\n- 右键双击控制点：删除"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;%2padding:8px;").arg(theme.button_text, themedPanelStyle(theme)));
    ll->addWidget(hint);
    root->addWidget(left);

    curve_editor_ = new CurveEditor(this);
    curve_editor_->setStyleSheet(themedPanelStyle(theme));
    curve_editor_->setThemeColors(QColor(theme.panel), QColor(theme.border), QColor(theme.label),
                                  QColor(theme.button_text), QColor(theme.field));
    right_panel_ = curve_editor_;
    root->addWidget(right_panel_, 1);

    dmr_panel_ = new QWidget(this);
    dmr_panel_->setStyleSheet(themedPanelStyle(theme));
    auto* dmr_layout = new QVBoxLayout(dmr_panel_);
    dmr_layout->setContentsMargins(28, 28, 28, 28);
    dmr_layout->setSpacing(12);
    auto* dmr_title = new QLabel(QStringLiteral("射手步枪单点压枪力度"), dmr_panel_);
    dmr_title->setStyleSheet(QStringLiteral("font-size:18px;font-weight:700;color:%1;").arg(theme.button_text));
    dmr_layout->addWidget(dmr_title);
    auto* dmr_desc = new QLabel(QStringLiteral("射手步枪只在每次左键按下时补偿一次，因此这里使用单个滑块，不绘制时间曲线。"), dmr_panel_);
    dmr_desc->setWordWrap(true);
    dmr_desc->setStyleSheet(QStringLiteral("font-size:13px;color:%1;").arg(theme.label));
    dmr_layout->addWidget(dmr_desc);
    dmr_value_label_ = new QLabel(dmr_panel_);
    dmr_value_label_->setStyleSheet(QStringLiteral("font-size:28px;font-weight:800;color:%1;").arg(theme.button_text));
    dmr_layout->addWidget(dmr_value_label_);
    dmr_slider_ = new QSlider(Qt::Horizontal, dmr_panel_);
    dmr_slider_->setRange(0, 400);
    dmr_slider_->setStyleSheet(themedSliderStyle(currentUiTheme(config_)));
    dmr_layout->addWidget(dmr_slider_);
    dmr_layout->addStretch();
    dmr_panel_->hide();
    root->addWidget(dmr_panel_, 1);

    connect(add_point, &QPushButton::clicked, this, &RecoilDebuggerWindow::addCurrentPoint);
    connect(sub01, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(-0.1); });
    connect(sub001, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(-0.01); });
    connect(add001, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(0.01); });
    connect(add01, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(0.1); });
    connect(save, &QPushButton::clicked, this, &RecoilDebuggerWindow::saveAndApply);
    connect(weapon_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(curve_type_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(scope_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(grip_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(muzzle_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(stock_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(dmr_slider_, &QSlider::valueChanged, this, [this](int value) {
        const double strength = static_cast<double>(value) / 10.0;
        curve_y_ = {strength};
        dmr_value_label_->setText(QStringLiteral("%1 px").arg(strength, 0, 'f', 1));
        status_label_->setText(QStringLiteral("单点力度已修改，点击保存并应用。"));
    });
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
        if (it.value().value("type", "ar") == "sr") continue;
        weapon_combo_->addItem(QString::fromStdString(it.key()));
    }

    scope_combo_->clear();
    addKeyItem(scope_combo_, QStringLiteral("腰射"), QStringLiteral("hip"));
    addKeyItem(scope_combo_, QStringLiteral("红点"), QStringLiteral("red_dot"));
    addKeyItem(scope_combo_, QStringLiteral("全息"), QStringLiteral("holographic"));
    addKeyItem(scope_combo_, QStringLiteral("2倍镜"), QStringLiteral("2"));
    addKeyItem(scope_combo_, QStringLiteral("3倍镜"), QStringLiteral("3"));
    addKeyItem(scope_combo_, QStringLiteral("4倍镜"), QStringLiteral("4"));
    addKeyItem(scope_combo_, QStringLiteral("6倍镜"), QStringLiteral("6"));
    addKeyItem(scope_combo_, QStringLiteral("8倍镜"), QStringLiteral("8"));

    grip_combo_->clear();
    addKeyItem(grip_combo_, QStringLiteral("垂直握把"), QStringLiteral("vertical"));
    addKeyItem(grip_combo_, QStringLiteral("半截握把"), QStringLiteral("half"));
    addKeyItem(grip_combo_, QStringLiteral("轻型握把"), QStringLiteral("light"));
    addKeyItem(grip_combo_, QStringLiteral("拇指握把"), QStringLiteral("thumb"));
    addKeyItem(grip_combo_, QStringLiteral("直角握把"), QStringLiteral("tilted"));
    addKeyItem(grip_combo_, QStringLiteral("激光瞄准器"), QStringLiteral("laser"));

    muzzle_combo_->clear();
    addKeyItem(muzzle_combo_, QStringLiteral("步枪/连狙补偿器"), QStringLiteral("ar_dmr_compensator"));
    addKeyItem(muzzle_combo_, QStringLiteral("步枪/连狙消音器"), QStringLiteral("ar_dmr_silencer"));
    addKeyItem(muzzle_combo_, QStringLiteral("步枪/连狙消焰器"), QStringLiteral("ar_dmr_suppressor"));
    addKeyItem(muzzle_combo_, QStringLiteral("步枪/连狙制退器"), QStringLiteral("ar_dmr_braker"));
    addKeyItem(muzzle_combo_, QStringLiteral("连狙/栓狙补偿器"), QStringLiteral("dmr_sr_compensator"));
    addKeyItem(muzzle_combo_, QStringLiteral("冲锋枪补偿器"), QStringLiteral("smg_compensator"));
    addKeyItem(muzzle_combo_, QStringLiteral("冲锋枪消焰器"), QStringLiteral("smg_suppressor"));
    addKeyItem(muzzle_combo_, QStringLiteral("冲锋枪消音器"), QStringLiteral("smg_silencer"));

    stock_combo_->clear();
    addKeyItem(stock_combo_, QStringLiteral("战术枪托"), QStringLiteral("tactical"));
    addKeyItem(stock_combo_, QStringLiteral("重型枪托"), QStringLiteral("heavy"));
    addKeyItem(stock_combo_, QStringLiteral("微冲枪托"), QStringLiteral("uzi"));
    addKeyItem(stock_combo_, QStringLiteral("托腮板"), QStringLiteral("cheek_pad"));
    reloadCurves();
}

std::vector<double> RecoilDebuggerWindow::xAxisFor(size_t count) const {
    std::vector<double> xs;
    const double step = recoilCurveStep();
    for (size_t i = 0; i < count; ++i) xs.push_back(static_cast<double>(i) * step);
    return xs;
}

double RecoilDebuggerWindow::recoilCurveStep() const {
    return config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object()).value("recoil_curve_step", 0.4);
    });
}

std::string RecoilDebuggerWindow::currentWeaponType(const Json& rc) const {
    const auto weapon = weapon_combo_->currentText().toStdString();
    const auto weapons = rc.value("weapons", Json::object());
    if (weapons.contains(weapon)) {
        return weapons[weapon].value("type", "ar");
    }
    return "ar";
}

double RecoilDebuggerWindow::currentXAxisMax(const Json& rc) const {
    return currentWeaponType(rc) == "lmg" ? 8.0 : 4.0;
}

double RecoilDebuggerWindow::activeXAxisMax(const Json& rc) const {
    const QString type = currentCurveTypeKey(curve_type_combo_);
    return type == QStringLiteral("weapon") ? currentXAxisMax(rc) : 4.0;
}

std::vector<double> RecoilDebuggerWindow::fixedGridCurveForSave(const Json& rc) const {
    if (curve_y_.empty()) {
        return {1.0};
    }
    if (right_panel_ == dmr_panel_) {
        return {curve_y_.front()};
    }
    if (curve_y_.size() == 1 || curve_x_.size() != curve_y_.size()) {
        return curve_y_;
    }

    const double step = recoilCurveStep();
    const double max_x = activeXAxisMax(rc);
    if (step <= 0.0 || max_x <= 0.0) {
        return curve_y_;
    }

    std::vector<std::pair<double, double>> points;
    points.reserve(curve_y_.size());
    for (size_t i = 0; i < curve_y_.size(); ++i) {
        const double x = std::clamp(curve_x_[i], 0.0, max_x);
        if (!std::isfinite(x) || !std::isfinite(curve_y_[i])) continue;
        points.push_back({x, curve_y_[i]});
    }
    if (points.empty()) {
        return {1.0};
    }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::vector<std::pair<double, double>> unique_points;
    unique_points.reserve(points.size());
    for (const auto& point : points) {
        if (!unique_points.empty() && std::abs(unique_points.back().first - point.first) < 1e-6) {
            unique_points.back() = point;
        } else {
            unique_points.push_back(point);
        }
    }

    const int count = std::max(1, static_cast<int>(std::floor(max_x / step + 1e-6)) + 1);
    std::vector<double> out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back(sampleSortedCurve(unique_points, static_cast<double>(i) * step));
    }
    return out;
}

void RecoilDebuggerWindow::showCurveEditor() {
    if (right_panel_ == curve_editor_) return;
    right_panel_->hide();
    right_panel_ = curve_editor_;
    curve_editor_->show();
}

void RecoilDebuggerWindow::showDmrSlider() {
    if (right_panel_ == dmr_panel_) return;
    right_panel_->hide();
    right_panel_ = dmr_panel_;
    dmr_panel_->show();
}

void RecoilDebuggerWindow::reloadCurves() {
    curve_x_.clear();
    curve_y_.clear();
    active_json_key_.clear();
    active_curve_name_.clear();
    const auto rc = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object());
    });
    const QString type = currentCurveTypeKey(curve_type_combo_);
    const std::string weapon_type = currentWeaponType(rc);
    Json curve;
    bool has_curve = false;
    if (type == QStringLiteral("weapon")) {
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
        if (type == QStringLiteral("scope")) { group = "scope_multipliers"; key = currentKey(scope_combo_); }
        else if (type == QStringLiteral("grip")) { group = "grip_multipliers"; key = currentKey(grip_combo_); }
        else if (type == QStringLiteral("muzzle")) { group = "muzzle_multipliers"; key = currentKey(muzzle_combo_); }
        else { group = "stock_multipliers"; key = currentKey(stock_combo_); }
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
    if (type == QStringLiteral("weapon") && weapon_type == "dmr") {
        if (curve_y_.size() > 1) curve_y_ = {curve_y_.front()};
        showDmrSlider();
        dmr_slider_->blockSignals(true);
        dmr_slider_->setValue(std::clamp(static_cast<int>(std::lround(curve_y_.front() * 10.0)),
                                        dmr_slider_->minimum(), dmr_slider_->maximum()));
        dmr_slider_->blockSignals(false);
        dmr_value_label_->setText(QStringLiteral("%1 px").arg(curve_y_.front(), 0, 'f', 1));
    } else {
        showCurveEditor();
        curve_editor_->setFixedXRange(0.0, type == QStringLiteral("weapon") ? currentXAxisMax(rc) : 4.0);
        curve_editor_->setFixedYRange(0.0, paddedYMaxFromZero(curve_y_));
        curve_editor_->clearSelection();
        curve_editor_->clearUndoHistory();
        curve_editor_->setCurves({CurveEditor::Curve{curve_type_combo_->currentText(), QColor("#2563EB"), &curve_x_, &curve_y_}});
    }
}

void RecoilDebuggerWindow::addCurrentPoint() {
    if (right_panel_ != curve_editor_) return;
    const auto rc = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object());
    });
    const double max_x = activeXAxisMax(rc);
    const double step = recoilCurveStep();
    if (curve_y_.empty()) {
        curve_x_.push_back(0.0);
        curve_y_.push_back(1.0);
    } else {
        const double next_x = curve_x_.empty() ? 0.0 : curve_x_.back() + step;
        if (next_x > max_x + 1e-6) {
            status_label_->setText(QStringLiteral("已到达当前曲线的时间上限，不能继续向右添加标点。"));
            return;
        }
        curve_x_.push_back(std::min(next_x, max_x));
        curve_y_.push_back(curve_y_.back());
    }
    curve_editor_->setCurves({CurveEditor::Curve{curve_type_combo_->currentText(), QColor("#2563EB"), &curve_x_, &curve_y_}});
    status_label_->setText(QStringLiteral("已添加标点，点击保存并应用。"));
}

void RecoilDebuggerWindow::saveAndApply() {
    const auto rc_snapshot = config_.read([](const Json& data) {
        return data.value("recoil_settings", Json::object());
    });
    const std::vector<double> saved_curve = fixedGridCurveForSave(rc_snapshot);
    config_.write([&](Json& data) {
        auto& rc = data["recoil_settings"];
        if (active_json_key_ == "weapons") {
            rc["weapons"][active_curve_name_]["recoil_curve"] = saved_curve;
        } else if (!active_json_key_.empty()) {
            rc[active_json_key_][active_curve_name_] = saved_curve;
        }
    });
    config_.save();
    curve_editor_->clearUndoHistory();
    recoil_.reloadConfig();
    reloadCurves();
    status_label_->setText(QStringLiteral("参数已保存、重载并应用。"));
}

} // namespace pubg::ui
