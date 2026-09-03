#pragma once

#include "Config.hpp"
#include "ui/AiAimParameterCatalog.hpp"

#include <QWidget>
#include <functional>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace pubg::ui {

// Draft edits are isolated; a successful save applies one validated snapshot.
class AiAimSettingsWindow : public QWidget {
public:
    explicit AiAimSettingsWindow(Config& config, QWidget* parent = nullptr);
    using ApplySettingsCallback = std::function<bool(const AiAimSettings&, std::string&)>;
    void setApplySettingsCallback(ApplySettingsCallback callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Field {
        const AiAimParameterSpec* spec = nullptr;
        QDoubleSpinBox* number = nullptr;
        QCheckBox* check = nullptr;
        bool edited = false;
    };

    QFormLayout* addPage(const QString& title, const QString& description);
    void addParameter(QFormLayout* form, const AiAimParameterSpec& spec);
    void showParameterHelp(std::size_t index);
    bool loadFromDisk();
    void setValues(const Json& document);
    void resetDefaults();
    void saveSettings();
    void setMessage(const QString& message, bool error = false);

    Config& config_;
    ApplySettingsCallback apply_settings_;
    AiAimSettingsDocumentSnapshot baseline_;
    Json canonical_;
    Json defaults_;
    std::vector<Field> fields_;
    QTabWidget* tabs_ = nullptr;
    QTextBrowser* help_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* reset_ = nullptr;
    bool loading_ = false;
};

} // namespace pubg::ui
