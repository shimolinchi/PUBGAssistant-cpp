#include "ui/RecoilDebuggerWindow.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

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

} // namespace

RecoilDebuggerWindow::RecoilDebuggerWindow(Config& config, RecoilControl& recoil, QWidget* parent)
    : QWidget(parent), config_(config), recoil_(recoil) {
    setWindowTitle(QStringLiteral("压枪参数调试"));
    setObjectName("recoilDbgRoot");
    setStyleSheet(
        "#recoilDbgRoot{background:#FFFFFF;}"
        "QLabel{color:#030712;font-family:'Microsoft YaHei';}"
        "QComboBox{background:#FFFFFF;color:#030712;border:1px solid #9CA3AF;border-radius:4px;padding:2px 6px;selection-background-color:#DBEAFE;selection-color:#030712;}"
        "QComboBox QAbstractItemView{background:#FFFFFF;color:#030712;border:1px solid #9CA3AF;selection-background-color:#DBEAFE;selection-color:#030712;}"
        "QScrollBar:vertical{background:#FFFFFF;width:10px;margin:0;border:0;}"
        "QScrollBar::handle:vertical{background:#CBD5E1;border-radius:5px;min-height:20px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;background:#FFFFFF;}"
        "QLineEdit{background:#FFFFFF;color:#030712;border:1px solid #9CA3AF;border-radius:4px;padding:3px 6px;}"
        "QPushButton{background:#EEF2F7;color:#030712;border:1px solid #9CA3AF;border-radius:5px;padding:6px 0;}"
        "QPushButton:hover{background:#E2E8F0;}"
        "QPushButton:pressed{background:#CBD5E1;}"
    );
    setWindowOpacity(0.85);
    resize(760, 520);
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
    title->setStyleSheet("font-size:17px;font-weight:700;");
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

    auto* add_point = new QPushButton(QStringLiteral("添加标点"), this);
    ll->addWidget(add_point);
    auto* save = new QPushButton(QStringLiteral("保存并应用"), this);
    ll->addWidget(save);
    status_label_ = new QLabel(QStringLiteral("就绪"), this);
    status_label_->setWordWrap(true);
    status_label_->setStyleSheet("color:#1D4ED8;font-size:12px;font-weight:700;");
    ll->addWidget(status_label_);
    ll->addStretch();

    auto* hint = new QLabel(QStringLiteral("操作提示\n- 拖拽控制点：修改数值\n- 双击空白：新增控制点\n- 右键控制点：删除\n- 横轴为开火时序，纵轴为后坐补偿"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#111827;font-size:12px;background:#F8FAFC;border:1px solid #D1D5DB;border-radius:6px;padding:8px;");
    ll->addWidget(hint);
    root->addWidget(left);

    curve_editor_ = new CurveEditor(this);
    curve_editor_->setStyleSheet("background:#F8FAFC;border:1px solid #E5E7EB;");
    root->addWidget(curve_editor_, 1);

    connect(add_point, &QPushButton::clicked, this, &RecoilDebuggerWindow::addCurrentPoint);
    connect(save, &QPushButton::clicked, this, &RecoilDebuggerWindow::saveAndApply);
    connect(weapon_combo_, &QComboBox::currentTextChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(curve_type_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(scope_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(grip_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(muzzle_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
    connect(stock_combo_, &QComboBox::currentIndexChanged, this, &RecoilDebuggerWindow::reloadCurves);
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
    const QString type = currentCurveTypeKey(curve_type_combo_);
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
    curve_editor_->setCurves({CurveEditor::Curve{curve_type_combo_->currentText(), QColor("#2563EB"), &curve_x_, &curve_y_}});
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
