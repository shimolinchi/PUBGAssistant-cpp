#include "ui/AiAimSettingsWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QWheelEvent>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <set>

namespace {

using pubg::Json;
using pubg::ui::AiAimSettingsWindow;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool nearlyEqual(double actual, double expected, double tolerance = 0.000001) {
    return std::abs(actual - expected) <= tolerance;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "cannot create temporary UI test configuration");
    output << text;
    output.close();
    require(output.good(), "cannot write temporary UI test configuration");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read temporary UI test configuration");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TemporaryConfigRoot final {
public:
    TemporaryConfigRoot() : parent_(std::filesystem::canonical(std::filesystem::current_path())) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = parent_ /
                ("ai_aim_settings_ui_test_" + std::to_string(nonce) + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                root_ = candidate;
                break;
            }
        }
        require(!root_.empty(), "cannot reserve a unique temporary UI test directory");
        std::filesystem::create_directory(root_ / "config");
    }

    ~TemporaryConfigRoot() {
        // Only the unique child directory owned by this fixture may be removed.
        if (root_.empty() || root_.parent_path() != parent_ ||
            !root_.filename().string().starts_with("ai_aim_settings_ui_test_")) return;
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path parent_;
    std::filesystem::path root_;
};

Json settingsDocument() {
    return pubg::serializeAiAimSettings(pubg::AiAimSettings{});
}

struct Fixture final {
    TemporaryConfigRoot root;
    pubg::Config config;

    explicit Fixture(const Json& document) : Fixture(document.dump(2)) {}

    explicit Fixture(const std::string& text) : config(preparePaths(root, text)) {
        require(config.paths().configFile() == mainPath(),
                "temporary fixture resolved outside its owned config directory");
        require(config.load(), "temporary main config did not load");
    }

    static pubg::ResourcePaths preparePaths(const TemporaryConfigRoot& root, const std::string& text) {
        // ResourcePaths falls back to the process working directory when the
        // requested main file is absent. Seed both files before resolving it.
        writeText(root.path() / "config" / "config.json", "{}");
        writeText(root.path() / "config" / "ai_aim_settings.json", text);
        return pubg::ResourcePaths(root.path());
    }

    [[nodiscard]] std::filesystem::path mainPath() const {
        return root.path() / "config" / "config.json";
    }

    [[nodiscard]] std::filesystem::path aiPath() const {
        return root.path() / "config" / "ai_aim_settings.json";
    }

    [[nodiscard]] Json savedDocument() const { return Json::parse(readText(aiPath())); }

    [[nodiscard]] pubg::AiAimSettings freshSettings() const {
        pubg::Config reloaded{pubg::ResourcePaths(root.path())};
        require(reloaded.load(), "saved UI configuration did not reload");
        const auto settings = reloaded.aiAimSettings();
        require(settings.valid(), "saved UI configuration is not valid");
        return settings;
    }
};

template <typename T>
T* control(AiAimSettingsWindow& window, const char* name) {
    auto* result = window.findChild<T*>(QString::fromUtf8(name));
    require(result != nullptr, std::string("missing settings control: ") + name);
    return result;
}

QDoubleSpinBox* number(AiAimSettingsWindow& window, const char* name) {
    return control<QDoubleSpinBox>(window, name);
}

QCheckBox* check(AiAimSettingsWindow& window, const char* name) {
    return control<QCheckBox>(window, name);
}

void click(AiAimSettingsWindow& window, const char* name) {
    auto* button = control<QPushButton>(window, name);
    require(button->isEnabled(), std::string("settings action unexpectedly disabled: ") + name);
    button->click();
    QApplication::processEvents();
}

void testWheelDoesNotEditAiParameters() {
    const auto document = settingsDocument();
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    for (auto* input : window.findChildren<QDoubleSpinBox*>()) {
        const double original = input->value();
        for (const int delta : {120, -120}) {
            const QPoint center = input->rect().center();
            QWheelEvent wheel(QPointF(center), QPointF(input->mapToGlobal(center)),
                              QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(input, &wheel);
            require(nearlyEqual(input->value(), original), "wheel edited an AI parameter");
        }
    }
    click(window, "aiSettingsSave");
    require(fixture.savedDocument() == document, "wheel changed the saved AI settings draft");
}

void testLoadsCurrentValuesAndConstrainsFields() {
    auto document = settingsDocument();
    document["detector"]["confidence"] = 0.617;
    document["detector"]["iou"] = 0.37;
    document["runtime"]["target_fps"] = 90;
    document["controller"]["global_scale"] = 1.25;
    document["controller"]["scope_sensitivity"] = Json::array({21, 22, 23, 24, 26, 28});
    document["targeting"]["enabled_classes"] = Json::array({true, false, false, false});
    Fixture fixture(document);
    const auto original = readText(fixture.aiPath());
    AiAimSettingsWindow window(fixture.config);

    const double displayed_confidence = number(window, "/detector/confidence")->value();
    require(nearlyEqual(displayed_confidence, 0.617),
            "confidence was not loaded: expected 0.617, displayed " + std::to_string(displayed_confidence) +
            ", Config snapshot " + std::to_string(fixture.config.aiAimSettings().algorithm.detector.confidence));
    require(nearlyEqual(number(window, "/detector/iou")->value(), 0.37), "IoU was not loaded");
    require(nearlyEqual(number(window, "/runtime/target_fps")->value(), 90), "FPS was not loaded");
    require(nearlyEqual(number(window, "/controller/global_scale")->value(), 1.25), "scale was not loaded");
    require(check(window, "/targeting/enabled_classes/0")->isChecked(), "head class was not loaded");
    require(!check(window, "/targeting/enabled_classes/1")->isChecked(), "body class was not loaded");
    require(control<QTabWidget>(window, "aiSettingsTabs")->count() == 4, "expected four settings pages");
    require(control<QPushButton>(window, "aiSettingsSave")->isEnabled(), "valid settings cannot be saved");

    struct Range { const char* path; double minimum; double maximum; int decimals; };
    const Range ranges[]{
        {"/detector/confidence", 0, 1, 3},
        {"/detector/iou", 0, 1, 3},
        {"/targeting/effective_range", 1, 8192, 1},
        {"/targeting/target_stickiness", 0.5, 10, 2},
        {"/runtime/target_fps", 1, 1000, 0},
        {"/tracking/max_frame_age_ms", 1, 10000, 0},
        {"/controller/horizontal_fov", 1, 179, 1},
        {"/controller/vertical_fov", 1, 179, 1},
        {"/controller/max_pixels", 1, 8192, 1},
        {"/controller/global_scale", 0.1, 5, 3},
        {"/controller/ads_sensitivity_base", 0.01, 1000, 2},
        {"/controller/ads_sensitivity_current", 0.01, 1000, 2},
        {"/prediction/automatic_scale", 0, 2.5, 3},
        {"/prediction/dmr_scale", 0, 3, 3},
        {"/prediction/max_lead_pixels", 1, 4096, 1},
    };
    for (const auto& range : ranges) {
        auto* input = number(window, range.path);
        require(nearlyEqual(input->minimum(), range.minimum) && nearlyEqual(input->maximum(), range.maximum),
                std::string("unexpected bounds: ") + range.path);
        require(input->decimals() == range.decimals, std::string("unexpected decimals: ") + range.path);
        input->setValue(range.minimum - 100);
        require(nearlyEqual(input->value(), range.minimum), std::string("minimum not enforced: ") + range.path);
        input->setValue(range.maximum + 100);
        require(nearlyEqual(input->value(), range.maximum), std::string("maximum not enforced: ") + range.path);
    }
    const double expected_scopes[]{21, 22, 23, 24, 26, 28};
    for (int index = 0; index < 6; ++index) {
        const auto path = "/controller/scope_sensitivity/" + std::to_string(index);
        auto* input = number(window, path.c_str());
        require(nearlyEqual(input->value(), expected_scopes[index]), "scope sensitivity order changed");
        require(nearlyEqual(input->minimum(), 0.01) && nearlyEqual(input->maximum(), 1000), "invalid scope sensitivity bounds");
    }
    require(readText(fixture.aiPath()) == original, "loading or editing numeric controls wrote to disk");
}

void testEditsAreSavedAndLoadedByFreshConfig() {
    Fixture fixture(settingsDocument());
    const auto main_original = readText(fixture.mainPath());
    AiAimSettingsWindow window(fixture.config);
    number(window, "/detector/confidence")->setValue(0.68);
    number(window, "/runtime/target_fps")->setValue(77);
    number(window, "/tracking/max_frame_age_ms")->setValue(65);
    number(window, "/controller/max_pixels")->setValue(32);
    number(window, "/controller/scope_sensitivity/4")->setValue(61.25);
    check(window, "/prediction/enabled")->setChecked(false);
    check(window, "/targeting/enabled_classes/0")->setChecked(true);
    click(window, "aiSettingsSave");

    const auto saved = fixture.freshSettings();
    require(nearlyEqual(saved.algorithm.detector.confidence, 0.68), "edited confidence was not persisted");
    require(saved.algorithm.target_fps == 77, "edited FPS was not persisted");
    require(saved.algorithm.tracking.max_frame_age_ms == 65, "edited frame age was not persisted");
    require(nearlyEqual(saved.algorithm.controller.max_pixels, 32), "edited output limit was not persisted");
    require(nearlyEqual(saved.algorithm.controller.scope_sensitivity[4], 61.25), "edited 6x sensitivity was not persisted");
    require(!saved.algorithm.prediction.enabled, "edited checkbox was not persisted");
    require(saved.algorithm.targeting.enabled_classes[0], "edited target class was not persisted");
    require(fixture.savedDocument()["runtime"]["target_fps"].is_number_integer(), "FPS was stored as a float");
    require(readText(fixture.mainPath()) == main_original, "AI save changed the unrelated main config");
    require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("已保存")),
            "successful save was not reported");
}

void testDraftAndResetDoNotPersistWithoutSave() {
    auto document = settingsDocument();
    document["detector"]["confidence"] = 0.81;
    Fixture fixture(document);
    const auto original = readText(fixture.aiPath());
    {
        AiAimSettingsWindow window(fixture.config);
        number(window, "/detector/confidence")->setValue(0.73);
        require(readText(fixture.aiPath()) == original, "draft edit was implicitly saved");
        click(window, "aiSettingsReset");
        require(nearlyEqual(number(window, "/detector/confidence")->value(),
                     pubg::AiAimSettings{}.algorithm.detector.confidence), "reset did not update draft controls");
        require(readText(fixture.aiPath()) == original, "reset implicitly wrote to disk");
        require(nearlyEqual(fixture.config.aiAimSettings().algorithm.detector.confidence, 0.81),
                "draft reset changed the Config snapshot");
        click(window, "aiSettingsReload");
        require(nearlyEqual(number(window, "/detector/confidence")->value(), 0.81), "reload did not discard draft changes");
        number(window, "/detector/confidence")->setValue(0.52);
    }
    require(readText(fixture.aiPath()) == original, "closing an unsaved editor persisted its draft");
}

void testResetAndSavePreserveUnexposedSettings() {
    auto document = settingsDocument();
    document["compatibility"] = "original";
    document["runtime"] = {
        {"prefer_directml", false}, {"allow_cpu_fallback", true}, {"dml_device_index", 3},
        {"capture_adapter_index", 2}, {"capture_output_index", 1}, {"target_fps", 76},
    };
    document["activation"]["disable_variable_high_zoom"] = false;
    document["targeting"]["model_classes"] = false;
    document["targeting"]["location"] = 0.25;
    document["prediction"]["predict_factor"] = 9;
    document["prediction"]["velocity_deadzone"] = 7;
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    number(window, "/detector/confidence")->setValue(0.55);
    click(window, "aiSettingsSave");
    require(fixture.savedDocument()["activation"] == document["activation"],
            "saving an unrelated field changed the untouched high-zoom policy");
    click(window, "aiSettingsReset");
    click(window, "aiSettingsSave");

    const auto saved = fixture.savedDocument();
    const char* hidden_paths[]{
        "/compatibility", "/runtime/prefer_directml", "/runtime/allow_cpu_fallback",
        "/runtime/dml_device_index", "/runtime/capture_adapter_index", "/runtime/capture_output_index",
        "/targeting/model_classes", "/targeting/location",
        "/prediction/predict_factor", "/prediction/velocity_deadzone",
    };
    for (const auto* path : hidden_paths) {
        const Json::json_pointer pointer(path);
        require(saved.at(pointer) == document.at(pointer), std::string("reset overwrote hidden setting: ") + path);
    }
    require(saved["runtime"]["target_fps"] == pubg::AiAimSettings{}.algorithm.target_fps,
            "explicit reset did not persist the visible FPS default");
    require(saved["activation"]["disable_variable_high_zoom"] ==
                pubg::AiAimSettings{}.activation.disable_variable_high_zoom,
            "explicit reset did not restore the visible high-zoom policy");
    require(fixture.freshSettings().valid(), "reset generated an invalid configuration");
}

void testExternalChangeRejectsStaleSaveAndReloadRecovers() {
    auto document = settingsDocument();
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    number(window, "/runtime/target_fps")->setValue(88);
    document["runtime"]["target_fps"] = 234;
    const auto external = document.dump(2);
    writeText(fixture.aiPath(), external);
    click(window, "aiSettingsSave");
    require(readText(fixture.aiPath()) == external, "stale UI save overwrote an external edit");
    require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("保存失败")),
            "stale edit conflict was not reported");
    require(fixture.config.aiAimSettings().algorithm.target_fps == 120, "rejected save changed the Config snapshot");

    click(window, "aiSettingsReload");
    require(nearlyEqual(number(window, "/runtime/target_fps")->value(), 234), "reload ignored the external edit");
    number(window, "/runtime/target_fps")->setValue(235);
    click(window, "aiSettingsSave");
    require(fixture.freshSettings().algorithm.target_fps == 235, "reload did not establish a new save baseline");
}

void testInvalidConfigurationDisablesSaveAndReset() {
    auto invalid_schema = settingsDocument();
    invalid_schema["schema_version"] = 999;
    auto invalid_type = settingsDocument();
    invalid_type["detector"]["confidence"] = "invalid";
    const std::string invalid_documents[]{"{ malformed AI settings", invalid_schema.dump(), invalid_type.dump()};
    for (const auto& invalid : invalid_documents) {
        Fixture fixture(invalid);
        AiAimSettingsWindow window(fixture.config);
        require(!control<QPushButton>(window, "aiSettingsSave")->isEnabled(), "invalid config enabled Save");
        require(!control<QPushButton>(window, "aiSettingsReset")->isEnabled(), "invalid config enabled Reset");
        require(!control<QTabWidget>(window, "aiSettingsTabs")->isEnabled(), "invalid config enabled editable tabs");
        control<QPushButton>(window, "aiSettingsSave")->click();
        control<QPushButton>(window, "aiSettingsReset")->click();
        require(readText(fixture.aiPath()) == invalid, "invalid configuration was overwritten");
        require(!control<QLabel>(window, "aiSettingsMessage")->text().isEmpty(), "invalid config has no diagnostic");

        writeText(fixture.aiPath(), settingsDocument().dump(2));
        click(window, "aiSettingsReload");
        require(control<QPushButton>(window, "aiSettingsSave")->isEnabled(), "fixed config did not re-enable Save");
        require(control<QTabWidget>(window, "aiSettingsTabs")->isEnabled(), "fixed config did not re-enable editing");
    }
}

void testNoTargetClassIsRejectedWithoutWriting() {
    Fixture fixture(settingsDocument());
    const auto original = readText(fixture.aiPath());
    AiAimSettingsWindow window(fixture.config);
    check(window, "/targeting/enabled_classes/0")->setChecked(false);
    check(window, "/targeting/enabled_classes/1")->setChecked(false);
    click(window, "aiSettingsSave");
    require(readText(fixture.aiPath()) == original, "empty target selection was saved");
    require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("至少启用")),
            "empty target selection has no actionable diagnostic");
    check(window, "/targeting/enabled_classes/0")->setChecked(true);
    click(window, "aiSettingsSave");
    const auto settings = fixture.freshSettings();
    require(settings.algorithm.targeting.enabled_classes[0] && !settings.algorithm.targeting.enabled_classes[1],
            "head-only target selection could not be saved after validation failed");
}

void testUntouchedControlsDoNotTruncateNumericPrecision() {
    auto document = settingsDocument();
    document["detector"]["confidence"] = 0.543219;
    document["controller"]["global_scale"] = 1.234567;
    document["controller"]["scope_sensitivity"][2] = 42.123456;
    // Display and algorithm values may use lower precision; saving an unrelated
    // field must retain the original document's exact numeric values.
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    require(nearlyEqual(number(window, "/detector/confidence")->value(), 0.543), "test does not exercise display rounding");
    number(window, "/runtime/target_fps")->setValue(99);
    click(window, "aiSettingsSave");
    const auto saved = fixture.savedDocument();
    const char* precise_paths[]{
        "/detector/confidence", "/controller/global_scale", "/controller/scope_sensitivity/2",
    };
    for (const auto* path : precise_paths) {
        const Json::json_pointer pointer(path);
        require(saved.at(pointer) == document.at(pointer), std::string("untouched control lost precision: ") + path);
    }
    require(fixture.freshSettings().algorithm.target_fps == 99, "precision test did not save the edited field");
}

void testEditingLaterArrayEntryPreservesExistingValuesAndFillsDefaults() {
    auto document = settingsDocument();
    document["controller"]["scope_sensitivity"] = Json::array({31.123456789});
    document["targeting"]["enabled_classes"] = Json::array({true});
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    number(window, "/controller/scope_sensitivity/4")->setValue(61.25);
    check(window, "/targeting/enabled_classes/1")->setChecked(false);
    click(window, "aiSettingsSave");
    const auto saved = fixture.savedDocument();
    require(saved["controller"]["scope_sensitivity"][0] == document["controller"]["scope_sensitivity"][0],
            "extending a short array changed an existing high-precision value");
    const auto settings = fixture.freshSettings();
    require(nearlyEqual(settings.algorithm.controller.scope_sensitivity[1], 40), "short array did not retain the 2x default");
    require(nearlyEqual(settings.algorithm.controller.scope_sensitivity[2], 42), "short array did not retain the 3x default");
    require(nearlyEqual(settings.algorithm.controller.scope_sensitivity[3], 41), "short array did not retain the 4x default");
    require(nearlyEqual(settings.algorithm.controller.scope_sensitivity[4], 61.25), "short array edit was not saved");
    require(settings.algorithm.targeting.enabled_classes[0] && !settings.algorithm.targeting.enabled_classes[1],
            "short target class array was not extended safely");
}

void testMissingFileCanBeCreatedByExplicitSave() {
    Fixture fixture(settingsDocument());
    require(std::filesystem::remove(fixture.aiPath()), "could not seed a missing split file");
    AiAimSettingsWindow window(fixture.config);
    number(window, "/runtime/target_fps")->setValue(91);
    click(window, "aiSettingsSave");
    require(std::filesystem::exists(fixture.aiPath()) && fixture.freshSettings().algorithm.target_fps == 91,
            "editor could not explicitly create a split file that was missing when opened");
}

void testEveryParameterHasCompleteHelpAndRoundTripsThroughTheUi() {
    Fixture fixture(settingsDocument());
    AiAimSettingsWindow window(fixture.config);
    auto* help = control<QTextBrowser>(window, "aiSettingsHelp");
    std::set<std::string> paths;
    Json expected = Json::object();
    const auto catalog = pubg::ui::aiAimParameterCatalog();
    require(catalog.size() >= 60, "advanced parameter catalog is incomplete");
    for (const auto& spec : catalog) {
        require(paths.insert(spec.path).second, "duplicate parameter path in UI catalog");
        require(std::string(spec.purpose).size() > 10 && std::string(spec.adjustment).size() > 10 &&
                std::string(spec.caution).size() > 10, "parameter lacks detailed explanation");
        const auto help_button = std::string(spec.path) + "/help";
        click(window, help_button.c_str());
        const QString text = help->toPlainText();
        require(help->property("parameterPath").toString() == QString::fromUtf8(spec.path) &&
                text.contains(QString::fromUtf8(spec.title)) &&
                text.contains(QString::fromUtf8(spec.purpose)) &&
                text.contains(QString::fromUtf8(spec.adjustment)) &&
                text.contains(QString::fromUtf8(spec.caution)) &&
                text.contains(QStringLiteral("默认值")) && text.contains(QStringLiteral("界面可调范围")),
                std::string("help pane is incomplete: ") + spec.path);
        if (spec.kind == pubg::ui::AiAimParameterKind::Number) {
            auto* input = number(window, spec.path);
            require(nearlyEqual(input->minimum(), spec.minimum) && nearlyEqual(input->maximum(), spec.maximum),
                    "control bounds differ from its help catalog");
            const double next = input->value() + spec.step <= spec.maximum
                ? input->value() + spec.step : input->value() - spec.step;
            input->setValue(next);
            expected[Json::json_pointer(spec.path)] = input->value();
        } else {
            auto* input = check(window, spec.path);
            input->setChecked(!input->isChecked());
            expected[Json::json_pointer(spec.path)] = input->isChecked();
        }
    }
    click(window, "aiSettingsSave");
    const auto restored = pubg::serializeAiAimSettings(fixture.freshSettings());
    for (const auto& spec : catalog) {
        const Json::json_pointer path(spec.path);
        if (spec.kind == pubg::ui::AiAimParameterKind::Number) {
            require(nearlyEqual(restored.at(path).get<double>(), expected.at(path).get<double>(), 0.0001),
                    std::string("advanced parameter did not reach algorithm settings: ") + spec.path);
        } else {
            require(restored.at(path) == expected.at(path), "advanced checkbox did not reach algorithm settings");
        }
    }
    require(!fixture.freshSettings().algorithm.auto_fire.enabled, "advanced UI enabled automatic firing");
}

void testAdvancedEditsPreserveOldDocumentsAndPartialArrays() {
    auto document = Json{{"schema", std::string(pubg::kAiAimSettingsSchema)},
                         {"schema_version", pubg::kAiAimSettingsSchemaVersion}};
    document["targeting"]["class_location"] = Json::array({0.321987654});
    document["fire_offset"]["class_offset"] = Json::array({15.123456789});
    document["fire_offset"]["scope_scale"] = Json::array({0.623456789});
    Fixture fixture(document);
    AiAimSettingsWindow window(fixture.config);
    click(window, "aiSettingsSave");
    require(fixture.savedDocument() == document, "opening and saving old settings inserted new defaults");
    number(window, "/controller/p_y")->setValue(0.75);
    number(window, "/targeting/class_location/1")->setValue(0.65);
    number(window, "/fire_offset/class_offset/1")->setValue(55.5);
    number(window, "/fire_offset/scope_scale/3")->setValue(1.25);
    click(window, "aiSettingsSave");
    const auto saved = fixture.savedDocument();
    for (const char* path : {"/targeting/class_location/0", "/fire_offset/class_offset/0", "/fire_offset/scope_scale/0"}) {
        require(saved.at(Json::json_pointer(path)) == document.at(Json::json_pointer(path)),
                "extending advanced arrays lost existing precision");
    }
    const auto settings = fixture.freshSettings();
    require(nearlyEqual(settings.algorithm.controller.p_y, 0.75) &&
            nearlyEqual(settings.algorithm.targeting.class_location[1], 0.65) &&
            nearlyEqual(settings.algorithm.fire_offset.class_offset[1], 55.5) &&
            nearlyEqual(settings.algorithm.fire_offset.scope_scale[2], 1.5) &&
            nearlyEqual(settings.algorithm.fire_offset.scope_scale[3], 1.25),
            "advanced partial array save changed or omitted values");
}

void testExternalDeletionAndCreationRejectStaleEditorSnapshots() {
    const auto document = settingsDocument();
    {
        Fixture fixture(document);
        AiAimSettingsWindow window(fixture.config);
        number(window, "/runtime/target_fps")->setValue(93);
        require(std::filesystem::remove(fixture.aiPath()), "could not simulate external deletion");
        click(window, "aiSettingsSave");
        require(!std::filesystem::exists(fixture.aiPath()),
                "stale editor silently recreated an externally deleted split file");
        require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("保存失败")),
                "external deletion conflict has no diagnostic");
        click(window, "aiSettingsReload");
        number(window, "/runtime/target_fps")->setValue(94);
        click(window, "aiSettingsSave");
        require(fixture.freshSettings().algorithm.target_fps == 94,
                "reload did not allow explicit creation after external deletion");
    }
    {
        Fixture fixture(document);
        require(std::filesystem::remove(fixture.aiPath()), "could not seed a missing split file");
        AiAimSettingsWindow window(fixture.config);
        number(window, "/runtime/target_fps")->setValue(95);
        // Identical contents still conflict: absence was part of the editor baseline.
        const std::string external = document.dump(2);
        writeText(fixture.aiPath(), external);
        click(window, "aiSettingsSave");
        require(readText(fixture.aiPath()) == external,
                "stale editor overwrote an externally created split file");
        require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("保存失败")),
                "external creation conflict has no diagnostic");
        click(window, "aiSettingsReload");
        number(window, "/runtime/target_fps")->setValue(96);
        click(window, "aiSettingsSave");
        require(fixture.freshSettings().algorithm.target_fps == 96,
                "reload did not recover from an external creation conflict");
    }
}

void testSuccessfulSaveAppliesExactlyTheCommittedSettings() {
    Fixture fixture(settingsDocument());
    AiAimSettingsWindow window(fixture.config);
    int applied = 0;
    window.setApplySettingsCallback([&](const pubg::AiAimSettings& settings, std::string&) {
        ++applied;
        require(settings.display.target_boxes, "target box toggle not applied");
        require(nearlyEqual(settings.algorithm.controller.p_x, 1.25), "PID edit not applied");
        require(fixture.savedDocument()["controller"]["p_x"] == 1.25,
                "runtime update ran before persistence succeeded");
        return true;
    });
    number(window, "/controller/p_x")->setValue(1.25);
    check(window, "/display/target_boxes")->setChecked(true);
    require(applied == 0, "draft edit applied before save");
    click(window, "aiSettingsSave");
    require(applied == 1, "successful save must apply once");
    require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("已保存并应用")),
            "successful live apply was not reported");
    click(window, "aiSettingsReload");
    click(window, "aiSettingsReset");
    require(applied == 1, "reload or draft reset unexpectedly applied runtime changes");
}

void testRejectedSaveDoesNotApplyAndFailedApplyCanBeRetried() {
    Fixture fixture(settingsDocument());
    AiAimSettingsWindow window(fixture.config);
    int applied = 0;
    bool succeed = false;
    window.setApplySettingsCallback([&](const pubg::AiAimSettings&, std::string& error) {
        ++applied;
        error = "test runtime unavailable";
        return succeed;
    });
    check(window, "/targeting/enabled_classes/0")->setChecked(false);
    check(window, "/targeting/enabled_classes/1")->setChecked(false);
    click(window, "aiSettingsSave");
    require(applied == 0, "invalid target selection reached runtime");
    check(window, "/targeting/enabled_classes/1")->setChecked(true);
    auto external = settingsDocument();
    external["runtime"]["target_fps"] = 98;
    writeText(fixture.aiPath(), external.dump());
    click(window, "aiSettingsSave");
    require(applied == 0, "conflicting disk save reached runtime");
    click(window, "aiSettingsReload");
    number(window, "/controller/p_y")->setValue(1.125);
    click(window, "aiSettingsSave");
    require(applied == 1 && fixture.savedDocument()["controller"]["p_y"] == 1.125,
            "failed application must keep the successful disk save");
    require(control<QLabel>(window, "aiSettingsMessage")->text().contains(QStringLiteral("应用失败")),
            "failed application was falsely reported as active");
    succeed = true;
    click(window, "aiSettingsSave");
    require(applied == 2, "retry after apply failure hit a stale save baseline");
}

void saveOptionalPreviews() {
    const auto preview_directory = qEnvironmentVariable("PUBG_TEST_UI_PREVIEW_DIR");
    if (preview_directory.isEmpty()) return;
    require(QDir().mkpath(preview_directory), "cannot create the requested UI preview directory");
    std::string supported_formats;
    for (const auto& format : QImageWriter::supportedImageFormats()) {
        if (!supported_formats.empty()) supported_formats += ", ";
        supported_formats += format.toStdString();
    }
    std::cout << "UI preview supported image formats: " << supported_formats << '\n';
    Fixture fixture(settingsDocument());
    AiAimSettingsWindow window(fixture.config);
    window.ensurePolished();
    if (window.layout()) window.layout()->activate();
    QApplication::processEvents();
    auto* tabs = control<QTabWidget>(window, "aiSettingsTabs");
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        for (const auto& spec : pubg::ui::aiAimParameterCatalog()) {
            if (static_cast<int>(spec.page) == index) {
                const auto help_button = std::string(spec.path) + "/help";
                click(window, help_button.c_str());
                break;
            }
        }
        window.ensurePolished();
        if (window.layout()) window.layout()->activate();
        if (tabs->currentWidget() && tabs->currentWidget()->layout()) {
            tabs->currentWidget()->layout()->activate();
        }
        QApplication::processEvents();
        const auto image_path = QDir(preview_directory).filePath(
            QStringLiteral("ai-aim-settings-tab-%1.bmp").arg(index + 1));
        const QPixmap preview = window.grab();
        require(!preview.isNull(), "settings tab " + std::to_string(index + 1) +
                " produced a null pixmap on platform " + QApplication::platformName().toStdString() +
                "; supported formats: " + supported_formats);
        QImageWriter writer(image_path, QByteArrayLiteral("bmp"));
        const bool written = writer.write(preview.toImage());
        require(written, "cannot save settings tab preview " + image_path.toStdString() +
                ": " + writer.errorString().toStdString() + "; supported formats: " + supported_formats);
    }
}

} // namespace

int main(int argc, char** argv) {
    // CTest supplies "-platform minimal"; manual previews may select "windows"
    // for native fonts. The editor stays hidden and never attaches to the game.
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    try {
        testSuccessfulSaveAppliesExactlyTheCommittedSettings();
        testRejectedSaveDoesNotApplyAndFailedApplyCanBeRetried();
        testWheelDoesNotEditAiParameters();
        testLoadsCurrentValuesAndConstrainsFields();
        testEditsAreSavedAndLoadedByFreshConfig();
        testDraftAndResetDoNotPersistWithoutSave();
        testResetAndSavePreserveUnexposedSettings();
        testExternalChangeRejectsStaleSaveAndReloadRecovers();
        testInvalidConfigurationDisablesSaveAndReset();
        testNoTargetClassIsRejectedWithoutWriting();
        testUntouchedControlsDoNotTruncateNumericPrecision();
        testEditingLaterArrayEntryPreservesExistingValuesAndFillsDefaults();
        testMissingFileCanBeCreatedByExplicitSave();
        testExternalDeletionAndCreationRejectStaleEditorSnapshots();
        testEveryParameterHasCompleteHelpAndRoundTripsThroughTheUi();
        testAdvancedEditsPreserveOldDocumentsAndPartialArrays();
        saveOptionalPreviews();
        std::cout << "AI aim settings window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AI aim settings window tests failed: " << error.what() << '\n';
        return 1;
    }
}
