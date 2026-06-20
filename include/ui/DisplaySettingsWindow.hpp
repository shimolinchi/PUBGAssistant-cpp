#pragma once

#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include "Config.hpp"

namespace pubg::ui {

class DisplaySettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit DisplaySettingsWindow(Config& config, QWidget* parent = nullptr);

signals:
    void themeChanged();
    void hudChanged();

private:
    void buildUi();
    void loadValues();
    void applyTheme();
    void previewDisplayValue(const char* key, double value);
    void saveAll();
    void previewTheme(const QString& key);

    Config& config_;
    bool loading_ = false;
    QSlider* status_font_slider_ = nullptr;
    QSlider* status_line_gap_slider_ = nullptr;
    QSlider* status_text_alpha_slider_ = nullptr;
    QSlider* window_opacity_slider_ = nullptr;
    QSlider* distance_panel_slider_ = nullptr;
    QSlider* assist_text_size_slider_ = nullptr;
    QSlider* assist_text_alpha_slider_ = nullptr;
};

} // namespace pubg::ui
