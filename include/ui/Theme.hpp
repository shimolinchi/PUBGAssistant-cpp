#pragma once

#include <QString>
#include <QWidget>

#include "Config.hpp"

namespace pubg::ui {

struct UiTheme {
    QString key;
    QString name;
    QString frame;
    QString page;
    QString border;
    QString label;
    QString button;
    QString button_hover;
    QString button_pressed;
    QString button_active;
    QString button_text;
    QString panel;
    QString field;
    QString accent;
};

UiTheme uiThemeFromName(const std::string& name);
UiTheme currentUiTheme(Config& config);
double themedWindowOpacity(Config& config);
QString themedWidgetStyle(const UiTheme& theme, const QString& root_selector = {});
QString themedSliderStyle(const UiTheme& theme);
QString themedPanelStyle(const UiTheme& theme);
void applyThemedPopupWindow(QWidget* window, Config& config);

} // namespace pubg::ui
