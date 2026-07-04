#include "ui/SpecialWeaponDebuggerWindow.hpp"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

#include "ui/Theme.hpp"

namespace pubg::ui {

namespace {

std::pair<double, double> paddedYRange(const std::vector<double>& ys) {
    if (ys.empty()) return {0.0, 1.0};
    const auto [min_it, max_it] = std::minmax_element(ys.begin(), ys.end());
    const double span = std::max(1.0, *max_it - *min_it);
    return {*min_it - span * 0.1, *max_it + span * 0.1};
}

std::pair<double, double> paddedYRange(const std::vector<double>& ys, const std::vector<double>& ys2) {
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    bool has_value = false;
    for (double value : ys) {
        min_y = std::min(min_y, value);
        max_y = std::max(max_y, value);
        has_value = true;
    }
    for (double value : ys2) {
        min_y = std::min(min_y, value);
        max_y = std::max(max_y, value);
        has_value = true;
    }
    if (!has_value) return {0.0, 1.0};
    const double span = std::max(1.0, max_y - min_y);
    return {min_y - span * 0.1, max_y + span * 0.1};
}

std::pair<double, double> throwableXRange(const Config& config, int mode_index) {
    const auto cfg = config.read([](const Json& data) {
        return data.value("throwables_config", Json::object());
    });
    if (mode_index == 2) {
        return {cfg.value("run_jump_min_dist", 65.0), cfg.value("run_jump_max_dist", 90.0)};
    }
    if (mode_index == 1) {
        return {cfg.value("jump_min_dist", 50.0), cfg.value("jump_max_dist", 65.0)};
    }
    return {cfg.value("normal_min_dist", 20.0), cfg.value("normal_max_dist", 50.0)};
}

} // namespace

SpecialWeaponDebuggerWindow::SpecialWeaponDebuggerWindow(Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    setWindowTitle(QStringLiteral("特殊武器参数设置"));
    resize(720, 480);
    applyThemedPopupWindow(this, config_);
    buildUi();
    renderCurrentWeapon();
}

SpecialWeaponDebuggerWindow::~SpecialWeaponDebuggerWindow() {
    cleanupBeforeClose();
}

void SpecialWeaponDebuggerWindow::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}

void SpecialWeaponDebuggerWindow::cleanupBeforeClose() {
    if (curve_editor_) {
        curve_editor_->setCurves({});
        curve_editor_->clearSelection();
        curve_editor_->clearUndoHistory();
    }
    param_edits_.clear();
    active_x_.clear();
    active_y_.clear();
    active_y2_.clear();
}

void SpecialWeaponDebuggerWindow::buildUi() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* left = new QWidget(this);
    left->setFixedWidth(196);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(8);

    const auto theme = currentUiTheme(config_);
    auto* title = new QLabel(QStringLiteral("特殊武器"), this);
    title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:700;color:%1;").arg(theme.button_text));
    ll->addWidget(title);

    weapon_combo_ = new QComboBox(this);
    weapon_combo_->addItems({QStringLiteral("火箭筒"), "VSS", QStringLiteral("十字弩"),
                             QStringLiteral("迫击炮"), QStringLiteral("投掷物"), "C4"});
    ll->addWidget(weapon_combo_);

    throw_mode_combo_ = new QComboBox(this);
    throw_mode_combo_->addItems({QStringLiteral("普通投掷"), QStringLiteral("跳投"),
                                 QStringLiteral("助跑跳投"), QStringLiteral("助跑参数")});
    ll->addWidget(throw_mode_combo_);

    auto* button_grid = new QGridLayout();
    button_grid->setHorizontalSpacing(8);
    button_grid->setVerticalSpacing(8);
    auto* add = new QPushButton(QStringLiteral("添加标点"), this);
    auto* sub01 = new QPushButton(QStringLiteral("-0.1"), this);
    auto* sub001 = new QPushButton(QStringLiteral("-0.01"), this);
    auto* add001 = new QPushButton(QStringLiteral("+0.01"), this);
    auto* add01 = new QPushButton(QStringLiteral("+0.1"), this);
    auto* save = new QPushButton(QStringLiteral("保存并应用"), this);
    button_grid->addWidget(sub01, 0, 0);
    button_grid->addWidget(sub001, 0, 1);
    button_grid->addWidget(add, 0, 2);
    button_grid->addWidget(add001, 1, 0);
    button_grid->addWidget(add01, 1, 1);
    button_grid->addWidget(save, 1, 2);
    ll->addLayout(button_grid);

    status_label_ = new QLabel(QStringLiteral("就绪"), this);
    status_label_->setWordWrap(true);
    status_label_->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:700;").arg(theme.accent));
    ll->addWidget(status_label_);
    ll->addStretch();

    auto* hint = new QLabel(QStringLiteral(
        "操作提示\n"
        "- 拖拽控制点：修改数值\n"
        "- Ctrl + 点击：多选控制点\n"
        "- Ctrl + Z：撤回上一次拖动\n"
        "- 按住 X/Y：锁定对应轴\n"
        "- 左键双击空白：新增控制点\n"
        "- 右键双击控制点：删除"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;%2padding:8px;").arg(theme.button_text, themedPanelStyle(theme)));
    ll->addWidget(hint);

    curve_editor_ = new CurveEditor(this);
    right_panel_ = curve_editor_;
    right_panel_->setStyleSheet(themedPanelStyle(theme));
    curve_editor_->setThemeColors(QColor(theme.panel), QColor(theme.border), QColor(theme.label),
                                  QColor(theme.button_text), QColor(theme.field));
    root->addWidget(left);
    root->addWidget(right_panel_, 1);

    connect(weapon_combo_, &QComboBox::currentTextChanged, this, [this] { renderCurrentWeapon(); });
    connect(throw_mode_combo_, &QComboBox::currentTextChanged, this, [this] { renderCurrentWeapon(); });
    connect(add, &QPushButton::clicked, this, &SpecialWeaponDebuggerWindow::addPoint);
    connect(sub01, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(-0.1); });
    connect(sub001, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(-0.01); });
    connect(add001, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(0.01); });
    connect(add01, &QPushButton::clicked, this, [this] { curve_editor_->nudgeSelectedY(0.1); });
    connect(save, &QPushButton::clicked, this, &SpecialWeaponDebuggerWindow::saveAndApply);
    connect(curve_editor_, &CurveEditor::curveChanged, this, [this] {
        status_label_->setText(QStringLiteral("曲线已修改，点击保存并应用。"));
    });
}

void SpecialWeaponDebuggerWindow::renderCurrentWeapon() {
    const auto w = weapon_combo_->currentText();
    param_edits_.clear();
    active_y2_.clear();
    ys2_.clear();
    if (right_panel_ != curve_editor_) {
        QWidget* old_panel = right_panel_;
        layout()->replaceWidget(right_panel_, curve_editor_);
        right_panel_ = curve_editor_;
        if (old_panel) {
            old_panel->hide();
            old_panel->setParent(nullptr);
            delete old_panel;
        }
    }
    curve_editor_->show();
    throw_mode_combo_->setVisible(w == QStringLiteral("投掷物"));

    if (w == QStringLiteral("火箭筒")) {
        bindCurve("rocket_config", "calib_dists", "calib_ratios", QColor("#2563EB"), QStringLiteral("下坠比例"));
    } else if (w == "VSS") {
        bindCurve("vss_config", "calib_dists", "calib_drops_ratio", QColor("#16A34A"), QStringLiteral("下坠比例"));
    } else if (w == QStringLiteral("十字弩")) {
        bindCurve("crossbow_config", "calib_dists", "calib_drops_ratio", QColor("#EA580C"), QStringLiteral("下坠比例"));
    } else if (w == QStringLiteral("投掷物")) {
        bindThrowables(throw_mode_combo_->currentIndex());
    } else if (w == QStringLiteral("迫击炮")) {
        bindParams("mortar_config", {
            {"a_param", QStringLiteral("上坡修正 a"), 0.2},
            {"b_param", QStringLiteral("下坡修正 b"), 0.2},
            {"direction_auto_aim_kp", QStringLiteral("方向 PID - Kp"), 0.045},
            {"direction_auto_aim_ki", QStringLiteral("方向 PID - Ki"), 0.0},
            {"direction_auto_aim_kd", QStringLiteral("方向 PID - Kd"), 0.012},
            {"direction_auto_aim_step_delay_ms", QStringLiteral("方向 PID - 间隔 ms"), 2.0},
            {"direction_auto_aim_max_step_px", QStringLiteral("方向 PID - 单步上限 px"), 80.0},
        });
    } else if (w == "C4") {
        bindParams("c4_config", {
            {"target_speed", QStringLiteral("推荐起步速度 km/h"), 60.0},
            {"jump_distance_threshold", QStringLiteral("推荐跳车距离 m"), 20.0},
        });
    }
    status_label_->setText(QStringLiteral("当前：") + w);
}

void SpecialWeaponDebuggerWindow::bindCurve(const std::string& config_key, const std::string& x_key,
                                            const std::string& y_key, const QColor& color, const QString& label) {
    active_config_ = config_key;
    active_x_ = x_key;
    active_y_ = y_key;
    active_color_ = color;
    active_label_ = label;
    xs_.clear();
    ys_.clear();
    const auto cfg = config_.read([&](const Json& data) {
        return data.value(config_key, Json::object());
    });
    if (cfg.contains(x_key)) for (const auto& v : cfg[x_key]) xs_.push_back(v.get<double>());
    if (cfg.contains(y_key)) for (const auto& v : cfg[y_key]) ys_.push_back(v.get<double>());
    const double max_x = xs_.empty() ? 100.0 : std::max(100.0, *std::max_element(xs_.begin(), xs_.end()));
    const auto [min_y, max_y] = paddedYRange(ys_);
    curve_editor_->setAxisLabels(QStringLiteral("下坠比例"));
    curve_editor_->setFixedXRange(0.0, max_x);
    curve_editor_->setFixedYRange(min_y, max_y);
    curve_editor_->clearSelection();
    curve_editor_->clearUndoHistory();
    curve_editor_->setCurves({CurveEditor::Curve{label, color, &xs_, &ys_}});
}

void SpecialWeaponDebuggerWindow::bindThrowables(int mode_index) {
    active_config_ = "throwables_config";
    if (mode_index == 3) {
        bindParams("throwables_config", {
            {"normal_min_dist", QStringLiteral("普通投掷最小距离 m"), 20.0},
            {"normal_max_dist", QStringLiteral("普通投掷最大距离 m"), 50.0},
            {"jump_min_dist", QStringLiteral("跳投最小距离 m"), 50.0},
            {"jump_max_dist", QStringLiteral("跳投最大距离 m"), 65.0},
            {"run_jump_min_dist", QStringLiteral("助跑跳投最小距离 m"), 65.0},
            {"run_jump_max_dist", QStringLiteral("助跑跳投最大距离 m"), 90.0},
            {"run_jump_run_seconds", QStringLiteral("助跑时间 s"), 2.0},
            {"jump_delay_after_release", QStringLiteral("松左键后跳跃延迟 s"), 0.3},
        });
        return;
    }
    active_x_ = mode_index == 2 ? "run_jump_calib_dists" : mode_index == 1 ? "jump_calib_dists" : "calib_dists";
    active_y_ = mode_index == 2 ? "run_jump_calib_elevations_ratio" : mode_index == 1 ? "jump_calib_elevations_ratio" : "calib_elevations_ratio";
    active_y2_ = mode_index == 2 ? "run_jump_calib_times" : mode_index == 1 ? "jump_calib_times" : "calib_times";
    xs_.clear();
    ys_.clear();
    ys2_.clear();

    const auto cfg = config_.read([&](const Json& data) {
        return data.value(active_config_, Json::object());
    });
    if (cfg.contains(active_x_)) for (const auto& v : cfg[active_x_]) xs_.push_back(v.get<double>());
    if (cfg.contains(active_y_)) for (const auto& v : cfg[active_y_]) ys_.push_back(v.get<double>());
    if (cfg.contains(active_y2_)) for (const auto& v : cfg[active_y2_]) ys2_.push_back(v.get<double>());
    const auto [min_x, max_x] = throwableXRange(config_, mode_index);
    const auto [min_y, max_y] = paddedYRange(ys_);
    const auto [right_min_y, right_max_y] = paddedYRange(ys2_);
    curve_editor_->setAxisLabels(QStringLiteral("准星抬高高度"), QStringLiteral("瞬爆时间"));
    curve_editor_->setFixedXRange(min_x, max_x);
    curve_editor_->setFixedYRange(min_y, max_y);
    curve_editor_->setFixedRightYRange(right_min_y, right_max_y);
    curve_editor_->clearSelection();
    curve_editor_->clearUndoHistory();
    curve_editor_->setCurves({
        CurveEditor::Curve{QStringLiteral("抬高高度"), QColor("#7C3AED"), &xs_, &ys_},
        CurveEditor::Curve{QStringLiteral("瞬爆时间"), QColor("#EA580C"), &xs_, &ys2_, CurveEditor::AxisSide::Right},
    });
}

void SpecialWeaponDebuggerWindow::bindParams(const std::string& config_key, const std::vector<ParamField>& fields) {
    active_config_ = config_key;
    active_x_.clear();
    active_y_.clear();
    active_y2_.clear();
    ys2_.clear();
    if (curve_editor_) {
        curve_editor_->setCurves({});
        curve_editor_->clearSelection();
        curve_editor_->clearUndoHistory();
    }
    auto* panel = new QWidget(this);
    panel->setStyleSheet(themedWidgetStyle(currentUiTheme(config_)));
    auto* form = new QFormLayout(panel);
    const auto cfg = config_.read([&](const Json& data) {
        return data.value(config_key, Json::object());
    });
    for (const auto& field : fields) {
        auto* edit = new QLineEdit(panel);
        edit->setText(QString::number(cfg.value(field.key, field.fallback)));
        param_edits_[field.key] = edit;
        form->addRow(field.label, edit);
    }
    QWidget* old_panel = right_panel_;
    layout()->replaceWidget(right_panel_, panel);
    right_panel_ = panel;
    if (old_panel && old_panel != curve_editor_) {
        old_panel->hide();
        old_panel->setParent(nullptr);
        delete old_panel;
    } else if (old_panel) {
        old_panel->hide();
    }
}

void SpecialWeaponDebuggerWindow::addPoint() {
    if (right_panel_ != curve_editor_) return;
    const bool is_throwable = !active_y2_.empty();
    const auto [throw_min_x, throw_max_x] = throwableXRange(config_, throw_mode_combo_->currentIndex());
    if (xs_.empty()) {
        xs_.push_back(is_throwable ? throw_min_x : 50.0);
        ys_.push_back(0.5);
        if (is_throwable) ys2_.push_back(1.0);
    } else {
        xs_.push_back(is_throwable ? std::clamp(xs_.back() + 2.0, throw_min_x, throw_max_x) : xs_.back() + 5.0);
        ys_.push_back(ys_.empty() ? 0.5 : ys_.back());
        if (is_throwable) ys2_.push_back(ys2_.empty() ? 1.0 : ys2_.back());
    }
    if (!active_y2_.empty()) {
        curve_editor_->setFixedXRange(throw_min_x, throw_max_x);
        curve_editor_->setCurves({
            CurveEditor::Curve{QStringLiteral("抬高高度"), QColor("#7C3AED"), &xs_, &ys_},
            CurveEditor::Curve{QStringLiteral("瞬爆时间"), QColor("#EA580C"), &xs_, &ys2_, CurveEditor::AxisSide::Right},
        });
    } else {
        const double max_x = xs_.empty() ? 100.0 : std::max(100.0, *std::max_element(xs_.begin(), xs_.end()));
        curve_editor_->setFixedXRange(0.0, max_x);
        curve_editor_->setCurves({CurveEditor::Curve{active_label_, active_color_, &xs_, &ys_}});
    }
}

void SpecialWeaponDebuggerWindow::saveAndApply() {
    const bool editing_params = !param_edits_.empty();
    config_.write([&](Json& data) {
        if (editing_params) {
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
    if (!editing_params) {
        curve_editor_->clearUndoHistory();
        renderCurrentWeapon();
    }
    emit saved();
    status_label_->setText(QStringLiteral("参数已保存、重载并应用。"));
}

} // namespace pubg::ui
