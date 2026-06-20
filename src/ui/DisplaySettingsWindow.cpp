#include "ui/DisplaySettingsWindow.hpp"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

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

QString sliderStyle(const UiTheme& theme) {
    return themedSliderStyle(theme);
}
} // namespace

DisplaySettingsWindow::DisplaySettingsWindow(Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    buildUi();
    loadValues();
}

void DisplaySettingsWindow::buildUi() {
    setWindowTitle(QStringLiteral("自定义显示界面"));
    resize(500, 660);
    setMinimumSize(460, 620);
    const auto theme = currentUiTheme(config_);
    applyThemedPopupWindow(this, config_);
    setStyleSheet(styleSheet() + themedSliderStyle(theme));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 22);
    root->setSpacing(14);

    auto addSlider = [&](const QString& title, int min, int max, QSlider*& out) {
        auto* label = new QLabel(title, this);
        root->addWidget(label);
        out = new QSlider(Qt::Horizontal, this);
        out->setRange(min, max);
        out->setStyleSheet(sliderStyle(theme));
        root->addWidget(out);
    };

    addSlider(QStringLiteral("左下角状态栏文字大小"), 10, 22, status_font_slider_);
    addSlider(QStringLiteral("左下角状态栏上下行间距"), 18, 40, status_line_gap_slider_);
    addSlider(QStringLiteral("左下角状态栏文字透明度"), 40, 100, status_text_alpha_slider_);
    addSlider(QStringLiteral("所有窗口透明度"), 30, 100, window_opacity_slider_);
    addSlider(QStringLiteral("右侧圆角矩形整体大小"), 80, 140, distance_panel_slider_);
    addSlider(QStringLiteral("测距显示层文字大小"), 12, 28, assist_text_size_slider_);
    addSlider(QStringLiteral("测距显示层文字透明度"), 40, 100, assist_text_alpha_slider_);

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
    auto* save = new QPushButton(QStringLiteral("保存设置"), this);
    save->setMinimumHeight(34);
    root->addWidget(save);
    root->addStretch();

    connect(status_font_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("status_font_size", static_cast<double>(value));
        emit hudChanged();
    });
    connect(status_line_gap_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("status_line_gap", static_cast<double>(value));
        emit hudChanged();
    });
    connect(status_text_alpha_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("status_text_alpha", static_cast<double>(value) / 100.0);
        emit hudChanged();
    });
    connect(window_opacity_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("window_opacity", static_cast<double>(value) / 100.0);
        applyThemedPopupWindow(this, config_);
        emit themeChanged();
    });
    connect(distance_panel_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("distance_panel_scale", static_cast<double>(value) / 100.0);
    });
    connect(assist_text_size_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("assist_text_size", static_cast<double>(value));
    });
    connect(assist_text_alpha_slider_, &QSlider::valueChanged, this, [this](int value) {
        previewDisplayValue("assist_text_alpha", static_cast<double>(value) / 100.0);
    });
    connect(save, &QPushButton::clicked, this, &DisplaySettingsWindow::saveAll);
}

void DisplaySettingsWindow::loadValues() {
    loading_ = true;
    const auto ui = config_.read([](const Json& data) {
        return data.value("ui_state", Json::object());
    });
    const Json display = ui.value("display", Json::object());
    status_font_slider_->setValue(display.value("status_font_size", 13));
    status_line_gap_slider_->setValue(display.value("status_line_gap", 25));
    status_text_alpha_slider_->setValue(static_cast<int>(std::round(display.value("status_text_alpha", 1.0) * 100.0)));
    window_opacity_slider_->setValue(static_cast<int>(std::round(display.value("window_opacity", 0.60) * 100.0)));
    distance_panel_slider_->setValue(static_cast<int>(std::round(display.value("distance_panel_scale", 1.0) * 100.0)));
    assist_text_size_slider_->setValue(display.value("assist_text_size", 16));
    assist_text_alpha_slider_->setValue(static_cast<int>(std::round(display.value("assist_text_alpha", 1.0) * 100.0)));

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
    config_.write([&](Json& data) {
        data["ui_state"]["display"][key] = value;
    });
}

void DisplaySettingsWindow::saveAll() {
    previewDisplayValue("status_font_size", static_cast<double>(status_font_slider_->value()));
    previewDisplayValue("status_line_gap", static_cast<double>(status_line_gap_slider_->value()));
    previewDisplayValue("status_text_alpha", static_cast<double>(status_text_alpha_slider_->value()) / 100.0);
    previewDisplayValue("window_opacity", static_cast<double>(window_opacity_slider_->value()) / 100.0);
    previewDisplayValue("distance_panel_scale", static_cast<double>(distance_panel_slider_->value()) / 100.0);
    previewDisplayValue("assist_text_size", static_cast<double>(assist_text_size_slider_->value()));
    previewDisplayValue("assist_text_alpha", static_cast<double>(assist_text_alpha_slider_->value()) / 100.0);
    config_.save();
    emit hudChanged();
    emit themeChanged();
}

void DisplaySettingsWindow::previewTheme(const QString& key) {
    config_.write([&](Json& data) {
        data["ui_state"]["theme"]["name"] = key.toStdString();
    });
    applyTheme();
    emit themeChanged();
}

} // namespace pubg::ui
