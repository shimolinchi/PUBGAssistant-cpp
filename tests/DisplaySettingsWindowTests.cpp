#include "ui/DisplaySettingsWindow.hpp"
#include "ui/Theme.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QWheelEvent>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using pubg::Json;
using pubg::ui::DisplaySettingsWindow;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool nearlyEqual(double actual, double expected) {
    return std::abs(actual - expected) < 0.000001;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "cannot create temporary display test configuration");
    output << text;
    output.close();
    require(output.good(), "cannot write temporary display test configuration");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read temporary display test configuration");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TemporaryConfigRoot final {
public:
    TemporaryConfigRoot() : parent_(std::filesystem::canonical(std::filesystem::current_path())) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = parent_ /
                ("display_settings_ui_test_" + std::to_string(nonce) + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                root_ = candidate;
                break;
            }
        }
        require(!root_.empty(), "cannot reserve a unique display test directory");
        std::filesystem::create_directory(root_ / "config");
    }

    ~TemporaryConfigRoot() {
        if (root_.empty() || root_.parent_path() != parent_ ||
            !root_.filename().string().starts_with("display_settings_ui_test_")) return;
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path parent_;
    std::filesystem::path root_;
};

Json initialDocument() {
    return Json{
        {"test_unrelated", "preserve"},
        {"ui_state", {
            {"theme", {{"name", "navy"}}},
            {"display", {
                {"status_font_size", 13}, {"status_line_gap", 25},
                {"status_text_alpha", 1.0}, {"window_opacity", 0.60},
                {"distance_panel_scale", 1.0}, {"assist_text_size", 16},
                {"assist_text_alpha", 1.0},
            }},
        }},
    };
}

struct Fixture final {
    TemporaryConfigRoot root;
    pubg::Config config;

    explicit Fixture(const Json& document = initialDocument()) : config(preparePaths(root, document)) {
        require(config.paths().configFile() == mainPath(), "display fixture resolved outside its owned directory");
        require(config.load(), "temporary display test configuration did not load");
    }

    static pubg::ResourcePaths preparePaths(const TemporaryConfigRoot& root, const Json& document) {
        // ResourcePaths uses CWD as a fallback if the requested main file is absent.
        writeText(root.path() / "config" / "config.json", document.dump(2));
        return pubg::ResourcePaths(root.path());
    }

    [[nodiscard]] std::filesystem::path mainPath() const {
        return root.path() / "config" / "config.json";
    }

    [[nodiscard]] Json snapshot() const {
        return config.read([](const Json& data) { return data; });
    }

    [[nodiscard]] double value(const char* key) const {
        return snapshot().at("ui_state").at("display").at(key).get<double>();
    }
};

template <typename T>
T* control(DisplaySettingsWindow& window, const char* name) {
    auto* result = window.findChild<T*>(QString::fromUtf8(name));
    require(result != nullptr, std::string("missing display control: ") + name);
    return result;
}

QSlider* slider(DisplaySettingsWindow& window, const char* key) {
    return control<QSlider>(window, key);
}

void processPreviewFrame() {
    QEventLoop loop;
    QTimer::singleShot(40, &loop, &QEventLoop::quit);
    loop.exec();
}

void sendWheel(QWidget& widget, const QPoint& angle_delta, const QPoint& pixel_delta = {}) {
    const QPoint center = widget.rect().center();
    QWheelEvent wheel(QPointF(center), QPointF(widget.mapToGlobal(center)),
                      pixel_delta, angle_delta, Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheel);
}

void testWheelDoesNotChangeDisplaySliders() {
    Fixture fixture;
    DisplaySettingsWindow window(fixture.config);
    const auto original_memory = fixture.snapshot();
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    for (auto* input : window.findChildren<QSlider*>()) {
        const int original = input->value();
        sendWheel(*input, QPoint(0, 120));
        require(input->value() == original, "wheel changed a display slider");
        sendWheel(*input, QPoint(0, -120));
        require(input->value() == original, "reverse wheel changed a display slider");
        sendWheel(*input, QPoint(120, 0), QPoint(12, 0));
        require(input->value() == original, "horizontal wheel changed a display slider");
    }
    processPreviewFrame();
    require(redraws == 0 && fixture.snapshot() == original_memory,
            "wheel scheduled or applied a settings change");

    auto* font = slider(window, "status_font_size");
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(font, &right);
    require(font->value() == 131, "wheel guard also blocked keyboard slider adjustment");
}

class WheelReceivingArea final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;
    int wheel_events = 0;

protected:
    void wheelEvent(QWheelEvent* event) override {
        ++wheel_events;
        QScrollArea::wheelEvent(event);
    }
};

void testSharedPopupRuleCoversLateControlsAndPreservesScrolling() {
    Fixture fixture;
    QWidget popup;
    pubg::ui::applyThemedPopupWindow(&popup, fixture.config);
    // Match settings windows that build controls after installing the controller.
    WheelReceivingArea area(&popup);
    area.resize(240, 200);
    auto* page = new QWidget;
    page->setMinimumSize(200, 1200);
    area.setWidget(page);
    area.ensurePolished();
    page->ensurePolished();
    QSpinBox integer(page);
    QDoubleSpinBox decimal(page);
    QComboBox choice(page);
    integer.setValue(40);
    decimal.setValue(40.5);
    choice.addItems({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    choice.setCurrentIndex(1);
    integer.ensurePolished();
    decimal.ensurePolished();
    choice.ensurePolished();
    area.verticalScrollBar()->setRange(0, 1000);
    area.verticalScrollBar()->setValue(500);

    sendWheel(integer, QPoint(0, 120));
    require(integer.value() == 40, "wheel changed a late-created integer input");
    require(area.verticalScrollBar()->value() < 500, "wheel over an input did not scroll its page");
    sendWheel(decimal, QPoint(0, -120));
    require(nearlyEqual(decimal.value(), 40.5), "wheel changed a late-created decimal input");
    sendWheel(choice, QPoint(0, 120));
    require(choice.currentIndex() == 1, "wheel changed a collapsed settings dropdown");
    require(area.wheel_events == 3, "wheel did not reach its page exactly once per event");

    QScrollBar scroll(Qt::Vertical, page);
    scroll.setRange(0, 100);
    scroll.setValue(50);
    scroll.ensurePolished();
    sendWheel(scroll, QPoint(0, -120));
    require(scroll.value() > 50, "wheel guard blocked the page scrollbar");

    integer.stepUp();
    decimal.stepUp();
    require(integer.value() == 41 && nearlyEqual(decimal.value(), 41.5),
            "wheel guard disabled explicit numeric adjustment");
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&choice, &down);
    require(choice.currentIndex() == 2, "wheel guard disabled explicit dropdown selection");
}

void testInitialLoadDoesNotMutateOrSchedulePreview() {
    auto document = initialDocument();
    document["ui_state"]["display"]["status_font_size"] = 13.4;
    document["ui_state"]["display"]["distance_panel_scale"] = 1.23;
    Fixture fixture(document);
    const auto original_memory = fixture.snapshot();
    const auto original_file = readText(fixture.mainPath());
    DisplaySettingsWindow window(fixture.config);
    int notifications = 0;
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++notifications; });
    QObject::connect(&window, &DisplaySettingsWindow::themeChanged, [&] { ++notifications; });
    require(slider(window, "status_font_size")->value() == 134, "fractional legacy field did not load in tenths");
    require(slider(window, "status_font_size")->minimum() == 100 &&
            slider(window, "status_font_size")->maximum() == 220, "font bounds changed");
    require(slider(window, "status_font_size")->singleStep() == 1, "font is not adjustable in 0.1-pixel steps");
    require(slider(window, "distance_panel_scale")->value() == 123, "existing scale mapping changed");
    require(control<QLabel>(window, "status_font_size_value")->text() == QStringLiteral("13.4 px"),
            "fractional font value is not displayed");
    require(!control<QTimer>(window, "displayPreviewTimer")->isActive(), "loading scheduled a preview");
    processPreviewFrame();
    require(notifications == 0, "loading emitted a preview notification");
    require(fixture.snapshot() == original_memory, "loading changed the in-memory configuration");
    require(readText(fixture.mainPath()) == original_file, "loading changed the configuration file");
}

void testRapidChangesCoalesceButContinueAcrossFrames() {
    Fixture fixture;
    const auto original_file = readText(fixture.mainPath());
    DisplaySettingsWindow window(fixture.config);
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    for (int ticks = 131; ticks <= 149; ++ticks) slider(window, "status_font_size")->setValue(ticks);
    slider(window, "status_line_gap")->setValue(31);
    slider(window, "status_text_alpha")->setValue(72);
    require(redraws == 0 && nearlyEqual(fixture.value("status_font_size"), 13),
            "each value change synchronously refreshed the HUD");
    processPreviewFrame();
    require(redraws == 1, "rapid display edits were not coalesced into one notification");
    require(nearlyEqual(fixture.value("status_font_size"), 14.9), "preview did not use the latest font value");
    require(nearlyEqual(fixture.value("status_line_gap"), 31), "preview dropped a second edited field");
    require(nearlyEqual(fixture.value("status_text_alpha"), 0.72), "alpha percentage mapping changed");
    slider(window, "status_font_size")->setValue(153);
    processPreviewFrame();
    require(redraws == 2 && nearlyEqual(fixture.value("status_font_size"), 15.3),
            "preview stopped updating after its first frame");
    require(readText(fixture.mainPath()) == original_file, "live preview wrote to disk");
}

void testSliderReleaseImmediatelyFlushesLatestValue() {
    Fixture fixture;
    DisplaySettingsWindow window(fixture.config);
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    auto* font = slider(window, "status_font_size");
    font->setSliderDown(true);
    font->setValue(166);
    font->setValue(167);
    require(redraws == 0, "slider moves were not deferred");
    font->setSliderDown(false);
    require(redraws == 1 && nearlyEqual(fixture.value("status_font_size"), 16.7),
            "slider release did not synchronously flush the final value");
    require(!control<QTimer>(window, "displayPreviewTimer")->isActive(), "release left a redundant timer pending");
    processPreviewFrame();
    require(redraws == 1, "release caused a second stale timer redraw");
}

void testSaveFlushesLatestValueBeforeWriting() {
    Fixture fixture;
    DisplaySettingsWindow window(fixture.config);
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    slider(window, "status_font_size")->setValue(182);
    slider(window, "distance_panel_scale")->setValue(124);
    control<QPushButton>(window, "displaySettingsSave")->click();
    const auto saved = Json::parse(readText(fixture.mainPath()));
    require(nearlyEqual(saved["ui_state"]["display"]["status_font_size"].get<double>(), 18.2),
            "save persisted the previous frame instead of the latest slider value");
    require(nearlyEqual(saved["ui_state"]["display"]["distance_panel_scale"].get<double>(), 1.24),
            "save changed the existing panel-scale mapping");
    require(saved["test_unrelated"] == "preserve", "display save dropped unrelated config");
    require(redraws == 1, "save triggered more than one pending HUD redraw");
    require(control<QLabel>(window, "displaySettingsMessage")->text().contains(QStringLiteral("已保存")),
            "save outcome was not reported");
}

void testCloseHideAndDestructionFlushWithoutDiskWrites() {
    Fixture fixture;
    const auto original_file = readText(fixture.mainPath());
    {
        DisplaySettingsWindow window(fixture.config);
        int redraws = 0;
        QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
        slider(window, "status_font_size")->setValue(173);
        QCloseEvent close;
        QApplication::sendEvent(&window, &close);
        require(redraws == 1 && nearlyEqual(fixture.value("status_font_size"), 17.3),
                "close did not flush its pending preview");
    }
    {
        DisplaySettingsWindow window(fixture.config);
        slider(window, "status_font_size")->setValue(174);
        QHideEvent hide;
        QApplication::sendEvent(&window, &hide);
        require(nearlyEqual(fixture.value("status_font_size"), 17.4), "titlebar hide dropped a pending preview");
    }
    int destructor_redraws = 0;
    {
        DisplaySettingsWindow window(fixture.config);
        QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++destructor_redraws; });
        slider(window, "status_font_size")->setValue(175);
    }
    require(nearlyEqual(fixture.value("status_font_size"), 17.5), "direct auxiliary destruction dropped the final value");
    require(destructor_redraws == 0, "destruction called back into the closing owner");
    require(readText(fixture.mainPath()) == original_file, "closing an unsaved display window wrote to disk");
}

void testOpacityPreviewDoesNotRebuildThemeOrLoseSliderState() {
    Fixture fixture;
    DisplaySettingsWindow window(fixture.config);
    const auto window_style = window.styleSheet();
    auto* opacity = slider(window, "window_opacity");
    const auto slider_style = opacity->styleSheet();
    int theme_changes = 0;
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::themeChanged, [&] { ++theme_changes; });
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    opacity->setSliderDown(true);
    opacity->setValue(73);
    processPreviewFrame();
    require(nearlyEqual(fixture.value("window_opacity"), 0.73), "opacity was not previewed");
    require(theme_changes == 0 && redraws == 0, "opacity triggered an unrelated theme rebuild or HUD refresh");
    require(window.styleSheet() == window_style && opacity->styleSheet() == slider_style,
            "opacity preview rebuilt widget styles");
    require(opacity->isSliderDown(), "opacity preview lost the active slider drag state");
    opacity->setSliderDown(false);
}

void testThemeButtonsStillPreviewAndSave() {
    Fixture fixture;
    const auto original_file = readText(fixture.mainPath());
    DisplaySettingsWindow window(fixture.config);
    QPushButton* rose = nullptr;
    for (auto* button : window.findChildren<QPushButton*>()) {
        if (button->property("themeKey").toString() == QStringLiteral("rose")) rose = button;
    }
    require(rose != nullptr, "existing theme choices were removed");
    int theme_changes = 0;
    int redraws = 0;
    QObject::connect(&window, &DisplaySettingsWindow::themeChanged, [&] { ++theme_changes; });
    QObject::connect(&window, &DisplaySettingsWindow::hudChanged, [&] { ++redraws; });
    slider(window, "status_font_size")->setValue(141);
    rose->click();
    require(theme_changes == 1 && redraws == 1, "theme change did not flush the existing display draft exactly once");
    require(fixture.snapshot()["ui_state"]["theme"]["name"] == "rose", "theme button no longer previews its color");
    require(rose->isChecked(), "selected theme button was not kept active");
    require(readText(fixture.mainPath()) == original_file, "theme preview saved without the Save button");
    control<QPushButton>(window, "displaySettingsSave")->click();
    require(Json::parse(readText(fixture.mainPath()))["ui_state"]["theme"]["name"] == "rose",
            "Save did not persist the selected theme");
}

void saveOptionalPreview() {
    const auto directory = qEnvironmentVariable("PUBG_TEST_UI_PREVIEW_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "cannot create the requested display preview directory");
    Fixture fixture;
    DisplaySettingsWindow window(fixture.config);
    window.ensurePolished();
    if (window.layout()) window.layout()->activate();
    QApplication::processEvents();
    auto* save = control<QPushButton>(window, "displaySettingsSave");
    require(window.rect().contains(save->geometry()), "display Save button lies outside the window");
    const QPixmap preview = window.grab();
    require(!preview.isNull(), "display settings produced a null preview pixmap");
    require(preview.save(QDir(directory).filePath(QStringLiteral("display-settings.bmp")), "BMP"),
            "cannot save the display settings preview");
}

} // namespace

int main(int argc, char** argv) {
    // CTest selects minimal by default; a hidden native-platform preview may
    // select windows explicitly. Neither path shows widgets or uses input hooks.
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    try {
        testWheelDoesNotChangeDisplaySliders();
        testSharedPopupRuleCoversLateControlsAndPreservesScrolling();
        testInitialLoadDoesNotMutateOrSchedulePreview();
        testRapidChangesCoalesceButContinueAcrossFrames();
        testSliderReleaseImmediatelyFlushesLatestValue();
        testSaveFlushesLatestValueBeforeWriting();
        testCloseHideAndDestructionFlushWithoutDiskWrites();
        testOpacityPreviewDoesNotRebuildThemeOrLoseSliderState();
        testThemeButtonsStillPreviewAndSave();
        saveOptionalPreview();
        std::cout << "Display settings window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Display settings window tests failed: " << error.what() << '\n';
        return 1;
    }
}
