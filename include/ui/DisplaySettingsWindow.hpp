#pragma once

#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include <vector>

#include "Config.hpp"

class QCloseEvent;
class QHideEvent;
class QLabel;
class QTimer;

namespace pubg::ui {

class DisplaySettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit DisplaySettingsWindow(Config& config, QWidget* parent = nullptr);
    ~DisplaySettingsWindow() override;

signals:
    void themeChanged();
    void hudChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct SliderBinding {
        const char* key;
        QSlider* slider;
        QLabel* value_label;
        double divisor;
        double default_value;
        int decimals;
        QString suffix;
    };

    void buildUi();
    void loadValues();
    void applyTheme();
    void previewDisplayValue(const char* key, double value);
    void updateValueLabel(const SliderBinding& binding);
    void flushPreview(bool notify = true);
    void saveAll();
    void previewTheme(const QString& key);

    Config& config_;
    bool loading_ = false;
    std::vector<SliderBinding> sliders_;
    QTimer* preview_timer_ = nullptr;
    QLabel* message_ = nullptr;
    Json pending_display_ = Json::object();
};

} // namespace pubg::ui
