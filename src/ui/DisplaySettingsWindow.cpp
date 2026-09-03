#include "ui/DisplaySettingsWindow.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/Theme.hpp"

namespace pubg::ui {

namespace {
struct ThemePreset {
    const char* key;
    const char* name;
    const char* color;
    const char* border;
};

const ThemePreset kThemes[] = {
    {"red", "赤红", "#EF4444", "#7F1D1D"},
    {"rose", "玫瑰", "#E11D48", "#881337"},
    {"orange", "橙霞", "#F97316", "#7C2D12"},
    {"amber", "琥珀", "#F59E0B", "#78350F"},
    {"yellow", "暖黄", "#CA8A04", "#713F12"},
    {"lime", "青柠", "#65A30D", "#365314"},
    {"green", "森绿", "#22C55E", "#064E3B"},
    {"emerald", "翡翠", "#10B981", "#064E3B"},
    {"teal", "青色", "#14B8A6", "#134E4A"},
    {"cyan", "湖青", "#06B6D4", "#164E63"},
    {"sky", "晴空", "#0EA5E9", "#0C4A6E"},
    {"blue", "天蓝", "#3B82F6", "#1E3A8A"},
    {"indigo", "靛蓝", "#6366F1", "#312E81"},
    {"purple", "紫罗兰", "#8B5CF6", "#4C1D95"},
    {"fuchsia", "洋红", "#D946EF", "#701A75"},
    {"navy", "深蓝", "#203655", "#7DD3FC"},
    {"gray", "灰色", "#94A3B8", "#334155"},
    {"dark", "黑色", "#252B37", "#F9FAFB"},
    {"light", "白色", "#FFFFFF", "#64748B"},
};

} // namespace

DisplaySettingsWindow::DisplaySettingsWindow(Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    preview_timer_ = new QTimer(this);
    preview_timer_->setObjectName(QStringLiteral("displayPreviewTimer"));
    preview_timer_->setInterval(16);
    preview_timer_->setSingleShot(true);
    preview_timer_->setTimerType(Qt::PreciseTimer);
    connect(preview_timer_, &QTimer::timeout, this, [this] { flushPreview(); });
    buildUi();
    loadValues();
}

DisplaySettingsWindow::~DisplaySettingsWindow() {
    // MainWindow also destroys auxiliary widgets directly during shutdown.
    // Preserve the last in-memory preview without calling back into its owner.
    flushPreview(false);
}

void DisplaySettingsWindow::buildUi() {
    setWindowTitle(QStringLiteral("自定义显示界面"));
    resize(520, 700);
    setMinimumSize(480, 640);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 40, 24, 22);
    root->setSpacing(10);

    auto addSlider = [&](const QString& title, const char* key, int minimum, int maximum,
                         double divisor, double default_value, int decimals,
                         const QString& suffix) {
        auto* caption = new QHBoxLayout;
        auto* label = new QLabel(title, this);
        auto* value_label = new QLabel(this);
        value_label->setObjectName(QString::fromUtf8(key) + QStringLiteral("_value"));
        value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value_label->setFixedWidth(84);
        caption->addWidget(label, 1);
        caption->addWidget(value_label);
        root->addLayout(caption);
        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setObjectName(QString::fromUtf8(key));
        slider->setAccessibleName(title);
        slider->setRange(minimum, maximum);
        slider->setSingleStep(1);
        slider->setPageStep(std::max(1, (maximum - minimum) / 10));
        slider->setTracking(true);
        slider->setMinimumHeight(18);
        root->addWidget(slider);
        const auto index = sliders_.size();
        sliders_.push_back({key, slider, value_label, divisor, default_value, decimals, suffix});
        connect(slider, &QSlider::valueChanged, this, [this, index](int value) {
            const auto& binding = sliders_[index];
            updateValueLabel(binding);
            previewDisplayValue(binding.key, static_cast<double>(value) / binding.divisor);
        });
        connect(slider, &QSlider::sliderReleased, this, [this] { flushPreview(); });
    };

    // The old status_font_size key remains in pixels; only the UI ticks use tenths.
    addSlider(QStringLiteral("左下角状态栏文字大小"), "status_font_size", 100, 220, 10, 13, 1, QStringLiteral(" px"));
    addSlider(QStringLiteral("左下角状态栏上下行间距"), "status_line_gap", 18, 40, 1, 25, 0, QStringLiteral(" px"));
    addSlider(QStringLiteral("左下角状态栏文字透明度"), "status_text_alpha", 40, 100, 100, 1, 0, QStringLiteral("%"));
    addSlider(QStringLiteral("所有窗口透明度"), "window_opacity", 30, 100, 100, 0.60, 0, QStringLiteral("%"));
    addSlider(QStringLiteral("右侧圆角矩形整体大小"), "distance_panel_scale", 80, 140, 100, 1, 0, QStringLiteral("%"));
    addSlider(QStringLiteral("测距显示层文字大小"), "assist_text_size", 12, 28, 1, 16, 0, QStringLiteral(" px"));
    addSlider(QStringLiteral("测距显示层文字透明度"), "assist_text_alpha", 40, 100, 100, 1, 0, QStringLiteral("%"));

    auto* theme_label = new QLabel(QStringLiteral("主窗口主题颜色"), this);
    root->addWidget(theme_label);
    auto* theme_row = new QWidget(this);
    auto* theme_layout = new QGridLayout(theme_row);
    theme_layout->setContentsMargins(0, 0, 0, 0);
    theme_layout->setSpacing(10);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (const auto& theme : kThemes) {
        auto* button = new QPushButton(theme_row);
        button->setCheckable(true);
        button->setFixedSize(24, 24);
        button->setToolTip(QString::fromUtf8(theme.name));
        button->setProperty("themeKey", theme.key);
        button->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;border:2px solid %2;border-radius:12px;padding:0;}"
            "QPushButton:checked{border:3px solid #111827;}"
            "QPushButton:hover{border:3px solid #60A5FA;}"
        ).arg(theme.color, theme.border));
        const int index = group->buttons().size();
        group->addButton(button);
        theme_layout->addWidget(button, index / 10, index % 10, Qt::AlignCenter);
        connect(button, &QPushButton::clicked, this, [this, button] {
            previewTheme(button->property("themeKey").toString());
        });
    }
    root->addWidget(theme_row);
    message_ = new QLabel(QStringLiteral("拖动实时预览，点击保存后写入配置。"), this);
    message_->setObjectName(QStringLiteral("displaySettingsMessage"));
    message_->setWordWrap(true);
    root->addWidget(message_);
    auto* save = new QPushButton(QStringLiteral("保存设置"), this);
    save->setObjectName(QStringLiteral("displaySettingsSave"));
    save->setMinimumHeight(34);
    root->addWidget(save);
    root->addStretch();

    connect(save, &QPushButton::clicked, this, &DisplaySettingsWindow::saveAll);
    applyTheme();
}

void DisplaySettingsWindow::loadValues() {
    loading_ = true;
    const auto ui = config_.read([](const Json& document) {
        return document.value("ui_state", Json::object());
    });
    const Json display = ui.value("display", Json::object());
    for (const auto& binding : sliders_) {
        const QSignalBlocker blocker(binding.slider);
        double value = display.value(binding.key, binding.default_value);
        if (!std::isfinite(value)) value = binding.default_value;
        const double ticks = std::clamp(value * binding.divisor,
            static_cast<double>(binding.slider->minimum()), static_cast<double>(binding.slider->maximum()));
        binding.slider->setValue(static_cast<int>(std::round(ticks)));
        updateValueLabel(binding);
    }

    const QString theme_key = QString::fromStdString(ui.value("theme", Json::object()).value("name", std::string("light")));
    for (auto* button : findChildren<QPushButton*>()) {
        if (button->property("themeKey").toString() == theme_key) {
            button->setChecked(true);
            break;
        }
    }
    loading_ = false;
}

void DisplaySettingsWindow::applyTheme() {
    const auto theme = currentUiTheme(config_);
    applyThemedPopupWindow(this, config_);
    setStyleSheet(styleSheet() + themedSliderStyle(theme));
    for (auto* slider : findChildren<QSlider*>()) {
        slider->setStyleSheet(themedSliderStyle(theme));
    }
}

void DisplaySettingsWindow::previewDisplayValue(const char* key, double value) {
    if (loading_) return;
    pending_display_[key] = value;
    message_->setText(QStringLiteral("正在预览，尚未保存。"));
    // Start only once per frame: restarting on every move would postpone all
    // previews until the user stops dragging instead of updating continuously.
    if (!preview_timer_->isActive()) preview_timer_->start();
}

void DisplaySettingsWindow::updateValueLabel(const SliderBinding& binding) {
    const double value = binding.suffix == QStringLiteral("%")
        ? static_cast<double>(binding.slider->value())
        : static_cast<double>(binding.slider->value()) / binding.divisor;
    binding.value_label->setText(QString::number(value, 'f', binding.decimals) + binding.suffix);
}

void DisplaySettingsWindow::flushPreview(bool notify) {
    preview_timer_->stop();
    if (pending_display_.empty()) return;
    Json pending = std::move(pending_display_);
    pending_display_ = Json::object();
    config_.write([&](Json& document) {
        for (auto item = pending.begin(); item != pending.end(); ++item) {
            document["ui_state"]["display"][item.key()] = item.value();
        }
    });
    if (!notify) return;
    if (pending.contains("window_opacity")) {
        // Opacity does not change colors or widget geometry. Reapplying styles
        // here can repolish/reposition a grabbed slider on every mouse move.
        const double opacity = themedWindowOpacity(config_);
        for (auto* window : QApplication::topLevelWidgets()) {
            window->setWindowOpacity(opacity);
        }
    }
    if (pending.contains("status_font_size") || pending.contains("status_line_gap") ||
        pending.contains("status_text_alpha")) {
        emit hudChanged();
    }
}

void DisplaySettingsWindow::saveAll() {
    flushPreview();
    message_->setText(config_.save()
        ? QStringLiteral("显示设置已保存。")
        : QStringLiteral("保存失败，请检查配置文件是否可写；当前预览仍保留。"));
}

void DisplaySettingsWindow::previewTheme(const QString& key) {
    if (loading_) return;
    flushPreview();
    config_.write([&](Json& document) {
        document["ui_state"]["theme"]["name"] = key.toStdString();
    });
    applyTheme();
    message_->setText(QStringLiteral("正在预览，尚未保存。"));
    emit themeChanged();
}

void DisplaySettingsWindow::closeEvent(QCloseEvent* event) {
    flushPreview();
    QWidget::closeEvent(event);
}

void DisplaySettingsWindow::hideEvent(QHideEvent* event) {
    // The shared popup titlebar's close button hides its window.
    flushPreview();
    QWidget::hideEvent(event);
}

} // namespace pubg::ui
