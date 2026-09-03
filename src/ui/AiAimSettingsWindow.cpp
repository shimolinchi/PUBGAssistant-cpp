#include "ui/AiAimSettingsWindow.hpp"

#include "ui/Theme.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <array>

namespace pubg::ui {

AiAimSettingsWindow::AiAimSettingsWindow(Config& config, QWidget* parent)
    : QWidget(parent), config_(config), defaults_(serializeAiAimSettings(AiAimSettings{})) {
    setWindowTitle(QStringLiteral("AI 瞄准参数设置"));
    resize(1020, 760);
    setMinimumSize(900, 620);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 40, 22, 18);
    root->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("AI 瞄准参数"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;"));
    root->addWidget(title);
    auto* notice = new QLabel(QStringLiteral(
        "保存成功后直接应用，无需重启，不会自动开启自瞄。每次只改少量参数，便于对比。\n"
        "点击参数或「说明」查看右侧详细解释；滚轮只滚动页面，不改变参数值。"), this);
    notice->setWordWrap(true);
    root->addWidget(notice);

    auto* body = new QHBoxLayout;
    body->setSpacing(14);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("aiSettingsTabs"));
    body->addWidget(tabs_, 3);
    auto* help_group = new QGroupBox(QStringLiteral("参数详细说明"), this);
    help_group->setMinimumWidth(280);
    auto* help_layout = new QVBoxLayout(help_group);
    help_ = new QTextBrowser(help_group);
    help_->setObjectName(QStringLiteral("aiSettingsHelp"));
    help_->setOpenExternalLinks(false);
    help_->setOpenLinks(false);
    help_layout->addWidget(help_);
    body->addWidget(help_group, 2);
    root->addLayout(body, 1);

    const std::array<QFormLayout*, 4> pages{
        addPage(QStringLiteral("目标识别"), QStringLiteral("调整画面目标，不是手持武器识别。瞄点高度和开火偏移是两种不同设置。")),
        addPage(QStringLiteral("跟随 / 灵敏度"), QStringLiteral("跟随慢优先检查水平、垂直 P；单次移动上限不是速度。先校准实际镜型和灵敏度。")),
        addPage(QStringLiteral("开火控制"), QStringLiteral("用于分离 AI 纵向跟随与原有压枪。这里不会自动开压枪，也不修改压枪曲线。")),
        addPage(QStringLiteral("移动预测"), QStringLiteral("预测只负责提前量，不是压枪或跟随力度。静止目标可先关闭预测做基线对比。")),
    };
    for (const auto& spec : aiAimParameterCatalog()) {
        addParameter(pages.at(static_cast<std::size_t>(spec.page)), spec);
    }

    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("aiSettingsMessage"));
    message_->setWordWrap(true);
    message_->setMinimumHeight(38);
    root->addWidget(message_);
    auto* actions = new QHBoxLayout;
    reset_ = new QPushButton(QStringLiteral("恢复可调参数默认值"), this);
    reset_->setObjectName(QStringLiteral("aiSettingsReset"));
    auto* reload = new QPushButton(QStringLiteral("重新载入"), this);
    reload->setObjectName(QStringLiteral("aiSettingsReload"));
    save_ = new QPushButton(QStringLiteral("保存并应用"), this);
    save_->setObjectName(QStringLiteral("aiSettingsSave"));
    actions->addWidget(reset_);
    actions->addWidget(reload);
    actions->addStretch();
    actions->addWidget(save_);
    root->addLayout(actions);
    connect(reset_, &QPushButton::clicked, this, [this] { resetDefaults(); });
    connect(reload, &QPushButton::clicked, this, [this] { loadFromDisk(); });
    connect(save_, &QPushButton::clicked, this, [this] { saveSettings(); });

    applyThemedPopupWindow(this, config_);
    const auto theme = currentUiTheme(config_);
    setStyleSheet(styleSheet() + QStringLiteral(
        "QDoubleSpinBox{background:%1;color:%2;border:1px solid %3;border-radius:4px;padding:4px 8px;}"
        "QCheckBox{color:%2;spacing:8px;}"
        "QGroupBox{color:%2;border:1px solid %3;border-radius:6px;margin-top:10px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;}"
        "QTextBrowser{background:%1;color:%2;border:0;padding:6px;}"
        "QTabWidget::pane{border:1px solid %3;border-radius:6px;}"
        "QTabBar::tab{background:%1;color:%2;padding:9px 10px;}"
        "QTabBar::tab:selected{background:%4;font-weight:700;}")
        .arg(theme.field, theme.label, theme.border, theme.button_active));
    loadFromDisk();
    showParameterHelp(0);
}

QFormLayout* AiAimSettingsWindow::addPage(const QString& title, const QString& description) {
    auto* scroll = new QScrollArea(tabs_);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 14);
    auto* hint = new QLabel(description, page);
    hint->setWordWrap(true);
    root->addWidget(hint);
    auto* form = new QFormLayout;
    form->setVerticalSpacing(12);
    form->setHorizontalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    root->addLayout(form);
    root->addStretch();
    scroll->setWidget(page);
    tabs_->addTab(scroll, title);
    return form;
}

void AiAimSettingsWindow::addParameter(QFormLayout* form, const AiAimParameterSpec& spec) {
    const auto index = fields_.size();
    const QString title = QString::fromUtf8(spec.title);
    const QString path = QString::fromUtf8(spec.path);
    const QString tooltip = QStringLiteral("作用：%1\n调节影响：%2\n生效条件与注意事项：%3")
        .arg(QString::fromUtf8(spec.purpose), QString::fromUtf8(spec.adjustment), QString::fromUtf8(spec.caution));
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    Field field{&spec};
    QWidget* input = nullptr;
    if (spec.kind == AiAimParameterKind::Number) {
        field.number = new QDoubleSpinBox(row);
        field.number->setDecimals(spec.decimals);
        field.number->setRange(spec.minimum, spec.maximum);
        field.number->setSingleStep(spec.step);
        field.number->setSuffix(QString::fromUtf8(spec.suffix));
        field.number->setMinimumWidth(118);
        field.number->setKeyboardTracking(false);
        input = field.number;
        connect(field.number, &QDoubleSpinBox::valueChanged, this, [this, index] {
            if (!loading_) fields_[index].edited = true;
        });
    } else {
        field.check = new QCheckBox(title, row);
        input = field.check;
        connect(field.check, &QCheckBox::toggled, this, [this, index] {
            if (!loading_) fields_[index].edited = true;
        });
    }
    fields_.push_back(field);
    input->setObjectName(path);
    input->setAccessibleName(title);
    input->setToolTip(tooltip);
    input->setProperty("aiParameterIndex", static_cast<int>(index));
    input->installEventFilter(this);
    for (auto* child : input->findChildren<QWidget*>()) {
        child->setProperty("aiParameterIndex", static_cast<int>(index));
        child->installEventFilter(this);
    }
    layout->addWidget(input, 1);
    auto* explain = new QPushButton(QStringLiteral("说明"), row);
    explain->setObjectName(path + QStringLiteral("/help"));
    explain->setAccessibleName(title + QStringLiteral("：详细说明"));
    explain->setFixedWidth(48);
    connect(explain, &QPushButton::clicked, this, [this, index] { showParameterHelp(index); });
    layout->addWidget(explain);
    if (field.number) form->addRow(title, row);
    else form->addRow(row);
}

bool AiAimSettingsWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress) {
        const auto index = watched->property("aiParameterIndex");
        if (index.isValid()) showParameterHelp(static_cast<std::size_t>(index.toInt()));
    }
    return QWidget::eventFilter(watched, event);
}

void AiAimSettingsWindow::showParameterHelp(std::size_t index) {
    if (!help_ || index >= fields_.size()) return;
    const auto& spec = *fields_[index].spec;
    const auto& default_value = defaults_.at(Json::json_pointer(spec.path));
    const QString default_text = spec.kind == AiAimParameterKind::Check
        ? (default_value.get<bool>() ? QStringLiteral("开启") : QStringLiteral("关闭"))
        : QString::number(default_value.get<double>(), 'f', spec.decimals) + QString::fromUtf8(spec.suffix);
    const QString range = spec.kind == AiAimParameterKind::Check ? QStringLiteral("开启 / 关闭")
        : QStringLiteral("%1 ～ %2%3").arg(spec.minimum).arg(spec.maximum).arg(QString::fromUtf8(spec.suffix));
    help_->setProperty("parameterPath", QString::fromUtf8(spec.path));
    help_->setHtml(QStringLiteral(
        "<h3>%1</h3><p><b>默认值：</b>%2<br><b>界面可调范围：</b>%3</p>"
        "<p><b>作用</b><br>%4</p><p><b>调节影响</b><br>%5</p>"
        "<p><b>生效条件与注意事项</b><br>%6</p>"
        "<p><b>配置字段：</b><br>%7</p><p>点击保存并应用后生效，无需重启。不会自动开启自瞄或压枪。</p>")
        .arg(QString::fromUtf8(spec.title).toHtmlEscaped(), default_text.toHtmlEscaped(), range.toHtmlEscaped(),
             QString::fromUtf8(spec.purpose).toHtmlEscaped(), QString::fromUtf8(spec.adjustment).toHtmlEscaped(),
             QString::fromUtf8(spec.caution).toHtmlEscaped(), QString::fromUtf8(spec.path).toHtmlEscaped()));
}

void AiAimSettingsWindow::setValues(const Json& document) {
    loading_ = true;
    for (auto& field : fields_) {
        const auto& value = document.at(Json::json_pointer(field.spec->path));
        if (field.number) field.number->setValue(value.get<double>());
        else field.check->setChecked(value.get<bool>());
    }
    loading_ = false;
}

bool AiAimSettingsWindow::loadFromDisk() {
    AiAimSettingsDocumentSnapshot snapshot;
    std::string error;
    const bool read = config_.readAiAimSettingsDocument(snapshot, error);
    const auto settings = read ? parseAiAimSettings(snapshot.document) : AiAimSettings{};
    const bool valid = read && settings.valid();
    tabs_->setEnabled(valid);
    save_->setEnabled(valid);
    reset_->setEnabled(valid);
    if (!valid) {
        setMessage(QStringLiteral("无法编辑：%1。请修复配置后重新载入；原文件没有改动。")
            .arg(QString::fromStdString(read ? settings.validation_error : error)), true);
        return false;
    }
    baseline_ = std::move(snapshot);
    canonical_ = serializeAiAimSettings(settings);
    setValues(canonical_);
    for (auto& field : fields_) field.edited = false;
    setMessage(QStringLiteral("已载入配置。修改只保留在窗口内，点击保存并应用后生效。"));
    return true;
}

void AiAimSettingsWindow::resetDefaults() {
    setValues(defaults_);
    for (auto& field : fields_) field.edited = true;
    setMessage(QStringLiteral("已恢复可调参数的默认值，尚未保存；显卡、截图设备及未展示的参数保持不变。"));
}

void AiAimSettingsWindow::saveSettings() {
    // Preserve exact existing values (including fields this window does not expose).
    // Canonical values are only used to display defaults and complete short arrays.
    Json document = baseline_.document;
    if (document.empty()) {
        document["schema"] = std::string(kAiAimSettingsSchema);
        document["schema_version"] = kAiAimSettingsSchemaVersion;
    }
    for (auto& field : fields_) {
        if (field.number) field.number->interpretText();
        if (!field.edited) continue;
        const Json::json_pointer pointer(field.spec->path);
        const auto parent = pointer.parent_pointer();
        const auto& canonical_parent = canonical_.at(parent);
        if (canonical_parent.is_array()) {
            auto& values = document[parent];
            if (values.is_null()) values = Json::array();
            while (values.size() < canonical_parent.size()) values.push_back(canonical_parent.at(values.size()));
        }
        auto& value = document[Json::json_pointer(field.spec->path)];
        if (field.check) value = field.check->isChecked();
        else if (field.spec->decimals == 0) value = static_cast<int>(field.number->value());
        else value = field.number->value();
    }
    const auto settings = parseAiAimSettings(document);
    if (!settings.algorithm.targeting.enabled_classes[0] &&
        !settings.algorithm.targeting.enabled_classes[1]) {
        setMessage(QStringLiteral("请至少启用头部或身体中的一种目标类别。"), true);
        return;
    }
    std::string error;
    if (!config_.saveAiAimSettings(document, baseline_, error)) {
        setMessage(QStringLiteral("保存失败：%1").arg(QString::fromStdString(error)), true);
        return;
    }
    // Use the exact committed snapshot, not a second disk read that could race
    // an external edit after the atomic save. Failed writes never reach runtime.
    baseline_ = {document, true};
    canonical_ = serializeAiAimSettings(settings);
    setValues(canonical_);
    for (auto& field : fields_) field.edited = false;
    if (!apply_settings_) {
        setMessage(QStringLiteral("已保存，但当前窗口未连接运行模块，参数尚未应用。"), true);
        return;
    }
    try {
        if (!apply_settings_(settings, error)) {
            setMessage(QStringLiteral("已保存，但应用失败：%1。可再次点击保存并应用重试。")
                .arg(QString::fromStdString(error)), true);
            return;
        }
    } catch (const std::exception& exception) {
        setMessage(QStringLiteral("已保存，但应用失败：%1。可再次点击保存并应用重试。")
            .arg(QString::fromUtf8(exception.what())), true);
        return;
    } catch (...) {
        setMessage(QStringLiteral("已保存，但应用时发生未知错误。可再次点击保存并应用重试。"), true);
        return;
    }
    setMessage(QStringLiteral("已保存并应用，无需重启。AI 关闭时，下次开启直接使用新参数；开关状态未改变。"));
}

void AiAimSettingsWindow::setApplySettingsCallback(ApplySettingsCallback callback) {
    apply_settings_ = std::move(callback);
}

void AiAimSettingsWindow::setMessage(const QString& message, bool error) {
    message_->setText(message);
    const auto theme = currentUiTheme(config_);
    message_->setStyleSheet(QStringLiteral("color:%1;").arg(error ? QStringLiteral("#E74C3C") : theme.accent));
}

} // namespace pubg::ui
