#include "Config.hpp"
#include "ResourcePaths.hpp"
#include "ScopeExit.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryConfigRoot {
public:
    TemporaryConfigRoot() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
            ("pubg_ai_aim_config_test_" + std::to_string(nonce));
        std::filesystem::create_directories(root_ / "config");
    }

    ~TemporaryConfigRoot() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "failed to create temporary config file");
    output << text;
    require(output.good(), "failed to write temporary config file");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "failed to open temporary config file");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

Json validAiSettings(const int target_fps) {
    return Json{
        {"schema", std::string(pubg::kAiAimSettingsSchema)},
        {"schema_version", pubg::kAiAimSettingsSchemaVersion},
        {"runtime", Json{{"target_fps", target_fps}}},
    };
}

void testMalformedAiFileRemainsInvalidAndIsNotOverwritten() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    const std::string malformed = "{ malformed AI settings";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, malformed);

    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    require(!config.aiAimSettings().valid(),
            "malformed AI file silently became valid defaults");
    require(config.save(), "unrelated config save failed");
    require(readText(ai_path) == malformed,
            "unrelated config save overwrote malformed AI settings");
}

void testExternalAiEditIsNotOverwrittenByOrdinarySave() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, validAiSettings(120).dump(2));

    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    require(config.aiAimSettings().algorithm.target_fps == 120,
            "initial AI settings were not loaded");

    const std::string external_edit = validAiSettings(321).dump(2);
    writeText(ai_path, external_edit);
    require(config.save(), "unrelated config save failed");
    require(readText(ai_path) == external_edit,
            "unrelated config save overwrote an external AI edit");
}

void testLegacyEmbeddedAiSettingsRemainWhenSplitFileIsMissing() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    Json main = Json{{"ai_aim", validAiSettings(222)}};
    writeText(config_dir / "config.json", main.dump(2));

    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "legacy main config failed to load");
    require(config.aiAimSettings().algorithm.target_fps == 222,
            "legacy embedded AI settings were not loaded");
    require(config.save(), "legacy main config failed to save");

    Json saved_main;
    std::ifstream input(config_dir / "config.json");
    input >> saved_main;
    require(saved_main.contains("ai_aim"),
            "ordinary save discarded legacy embedded AI settings");
    require(!std::filesystem::exists(ai_path),
            "ordinary save unexpectedly created an AI split file");
}

void testEditorReadsLatestDiskWithoutChangingRuntimeSnapshot() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, validAiSettings(120).dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");

    writeText(ai_path, validAiSettings(321).dump(2));
    pubg::AiAimSettingsDocumentSnapshot snapshot;
    std::string error;
    require(config.readAiAimSettingsDocument(snapshot, error),
            "editor failed to read the latest disk settings");
    require(error.empty() && snapshot.document == validAiSettings(321) && snapshot.split_file_exists,
            "editor returned a stale startup snapshot");
    require(config.aiAimSettings().algorithm.target_fps == 120,
            "editor read unexpectedly changed the startup settings snapshot");
}

void testExplicitSaveUpdatesOnlyAiFileAndMemory() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    const std::string main_text = "{\"custom_main_setting\":42}";
    writeText(config_dir / "config.json", main_text);
    writeText(ai_path, validAiSettings(120).dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    pubg::AiAimSettingsDocumentSnapshot expected;
    std::string error;
    require(config.readAiAimSettingsDocument(expected, error), "editor read failed");

    const Json changed = validAiSettings(144);
    require(config.saveAiAimSettings(changed, expected, error), "explicit AI save failed");
    require(error.empty() && Json::parse(readText(ai_path)) == changed,
            "explicit AI save did not persist the requested document");
    require(config.aiAimSettings().algorithm.target_fps == 144,
            "explicit AI save did not update the in-memory document");
    require(readText(config_dir / "config.json") == main_text,
            "explicit AI save unexpectedly rewrote the main config");
    for (const auto& entry : std::filesystem::directory_iterator(config_dir)) {
        require(entry.path().filename().string().find(".tmp.") == std::string::npos,
                "successful AI save left a temporary file behind");
    }
}

void testExplicitSaveRejectsExternalEditsAndInvalidDocuments() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, validAiSettings(120).dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    pubg::AiAimSettingsDocumentSnapshot expected;
    std::string error;
    require(config.readAiAimSettingsDocument(expected, error), "editor read failed");

    const std::string external = validAiSettings(321).dump(2);
    writeText(ai_path, external);
    require(!config.saveAiAimSettings(validAiSettings(144), expected, error),
            "stale editor overwrote an external AI settings edit");
    require(!error.empty() && readText(ai_path) == external,
            "external settings were modified after a rejected save");
    require(config.aiAimSettings().algorithm.target_fps == 120,
            "rejected save updated the in-memory document");

    require(config.readAiAimSettingsDocument(expected, error), "fresh editor read failed");
    Json invalid = validAiSettings(144);
    invalid["runtime"]["target_fps"] = "fast";
    require(!config.saveAiAimSettings(invalid, expected, error) && !error.empty(),
            "invalid scalar was saved");
    invalid = validAiSettings(144);
    invalid["activation"] = Json{{"auto_fire", true}};
    require(!config.saveAiAimSettings(invalid, expected, error),
            "unsupported safety override was saved");
    invalid = validAiSettings(144);
    invalid["detector"] = Json{{"confidence", std::numeric_limits<double>::infinity()}};
    require(!config.saveAiAimSettings(invalid, expected, error),
            "non-finite input was serialized into invalid null settings");
    require(readText(ai_path) == external && config.aiAimSettings().algorithm.target_fps == 120,
            "invalid document changed disk or memory");
    require(config.saveAiAimSettings(validAiSettings(144), expected, error),
            "reopening the latest external document did not allow an explicit save");
}

void testEditorCreatesSplitFileFromLegacyOrDefaultsOnlyExplicitly() {
    for (const bool legacy : {false, true}) {
        TemporaryConfigRoot root;
        const auto config_dir = root.path() / "config";
        const auto ai_path = config_dir / "ai_aim_settings.json";
        const Json main = legacy ? Json{{"ai_aim", validAiSettings(222)}} : Json::object();
        writeText(config_dir / "config.json", main.dump(2));
        pubg::Config config{pubg::ResourcePaths(root.path())};
        require(config.load(), "legacy/default config failed to load");
        pubg::AiAimSettingsDocumentSnapshot expected;
        std::string error;
        require(config.readAiAimSettingsDocument(expected, error), "legacy/default read failed");
        require(expected.document == (legacy ? validAiSettings(222) : Json::object()) &&
                    !expected.split_file_exists,
                "missing split file did not return legacy/default document");
        require(!std::filesystem::exists(ai_path), "editor read created a split file");
        require(config.saveAiAimSettings(validAiSettings(144), expected, error),
                "explicit save could not create an AI split file");
        require(Json::parse(readText(ai_path)) == validAiSettings(144),
                "new AI split file has the wrong document");
        require(config.save(), "ordinary save after explicit AI save failed");
        require(!Json::parse(readText(config_dir / "config.json")).contains("ai_aim"),
                "ordinary save retained duplicate AI settings after split-file creation");
    }
}

void testExternalDeletionConflictsEvenWhenMemoryMatchesBaseline() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, validAiSettings(120).dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    pubg::AiAimSettingsDocumentSnapshot expected;
    std::string error;
    require(config.readAiAimSettingsDocument(expected, error) && expected.split_file_exists,
            "existing AI file was not recorded in the editor baseline");

    require(std::filesystem::remove(ai_path), "could not simulate external AI file deletion");
    pubg::AiAimSettingsDocumentSnapshot missing;
    require(config.readAiAimSettingsDocument(missing, error) && !missing.split_file_exists,
            "missing AI file was not recorded in the new snapshot");
    require(missing.document == expected.document,
            "deletion regression must exercise identical in-memory fallback content");
    require(!config.saveAiAimSettings(validAiSettings(144), expected, error) && !error.empty(),
            "stale editor silently recreated an externally deleted AI file");
    require(!std::filesystem::exists(ai_path) && config.aiAimSettings().algorithm.target_fps == 120,
            "rejected deletion conflict changed disk or memory");
    require(config.saveAiAimSettings(validAiSettings(144), missing, error),
            "freshly reloaded missing-file baseline could not explicitly create the AI file");
}

void testExternalCreationConflictsEvenWithIdenticalDocument() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", Json{{"ai_aim", validAiSettings(120)}}.dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "legacy main config failed to load");
    pubg::AiAimSettingsDocumentSnapshot expected;
    std::string error;
    require(config.readAiAimSettingsDocument(expected, error) && !expected.split_file_exists,
            "missing split file was not recorded in the editor baseline");

    const std::string external = expected.document.dump(2);
    writeText(ai_path, external);
    require(!config.saveAiAimSettings(validAiSettings(144), expected, error) && !error.empty(),
            "stale editor overwrote an externally created file with identical contents");
    require(readText(ai_path) == external && config.aiAimSettings().algorithm.target_fps == 120,
            "rejected creation conflict changed disk or memory");
    pubg::AiAimSettingsDocumentSnapshot reloaded;
    require(config.readAiAimSettingsDocument(reloaded, error) && reloaded.split_file_exists,
            "reload did not record externally created split file");
    require(config.saveAiAimSettings(validAiSettings(144), reloaded, error),
            "freshly reloaded created-file baseline could not explicitly save");
}

void testMalformedOrUnreadableCurrentFileCannotBeOverwritten() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, validAiSettings(120).dump(2));
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    const pubg::AiAimSettingsDocumentSnapshot expected{validAiSettings(120), true};
    const std::string malformed = "{} trailing garbage";
    writeText(ai_path, malformed);
    auto snapshot = expected;
    std::string error;
    require(!config.readAiAimSettingsDocument(snapshot, error) && !error.empty(),
            "malformed editor document silently became defaults");
    require(snapshot == expected, "failed editor read replaced the caller's snapshot");
    require(!config.saveAiAimSettings(validAiSettings(144), expected, error),
            "explicit save overwrote a malformed external file");
    require(readText(ai_path) == malformed, "malformed external file changed after save failure");

    Json unsupported = validAiSettings(120);
    unsupported["schema_version"] = 2;
    const std::string unsupported_text = unsupported.dump(2);
    writeText(ai_path, unsupported_text);
    require(config.readAiAimSettingsDocument(snapshot, error),
            "valid JSON with unsupported schema should be returned for typed validation");
    require(!pubg::parseAiAimSettings(snapshot.document).valid(), "unsupported schema was accepted");
    require(!config.saveAiAimSettings(validAiSettings(144), snapshot, error),
            "editor overwrote an existing invalid/unsupported schema");
    require(readText(ai_path) == unsupported_text,
            "unsupported external document changed after save failure");
}

#ifdef _WIN32
void testAtomicReplacementFailureKeepsDiskAndMemory() {
    TemporaryConfigRoot root;
    const auto config_dir = root.path() / "config";
    const auto ai_path = config_dir / "ai_aim_settings.json";
    const std::string original = validAiSettings(120).dump(2);
    writeText(config_dir / "config.json", "{}");
    writeText(ai_path, original);
    pubg::Config config{pubg::ResourcePaths(root.path())};
    require(config.load(), "main config failed to load");
    HANDLE protected_file = CreateFileW(ai_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(protected_file != INVALID_HANDLE_VALUE, "could not protect AI file for failure test");
    const pubg::ScopeExit close_file([&]() noexcept { CloseHandle(protected_file); });
    std::string error;
    const pubg::AiAimSettingsDocumentSnapshot expected{validAiSettings(120), true};
    require(!config.saveAiAimSettings(validAiSettings(144), expected, error),
            "atomic replacement unexpectedly succeeded for a protected target");
    require(!error.empty() && readText(ai_path) == original,
            "failed atomic replacement damaged the original document");
    require(config.aiAimSettings().algorithm.target_fps == 120,
            "failed atomic replacement modified the in-memory settings");
    for (const auto& entry : std::filesystem::directory_iterator(config_dir)) {
        require(entry.path().filename().string().find(".tmp.") == std::string::npos,
                "failed atomic replacement leaked its temporary file");
    }
}
#endif

} // namespace

int main() {
    try {
        testMalformedAiFileRemainsInvalidAndIsNotOverwritten();
        testExternalAiEditIsNotOverwrittenByOrdinarySave();
        testLegacyEmbeddedAiSettingsRemainWhenSplitFileIsMissing();
        testEditorReadsLatestDiskWithoutChangingRuntimeSnapshot();
        testExplicitSaveUpdatesOnlyAiFileAndMemory();
        testExplicitSaveRejectsExternalEditsAndInvalidDocuments();
        testEditorCreatesSplitFileFromLegacyOrDefaultsOnlyExplicitly();
        testExternalDeletionConflictsEvenWhenMemoryMatchesBaseline();
        testExternalCreationConflictsEvenWithIdenticalDocument();
        testMalformedOrUnreadableCurrentFileCannotBeOverwritten();
#ifdef _WIN32
        testAtomicReplacementFailureKeepsDiskAndMemory();
#endif
        std::cout << "AiAim config persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AiAim config persistence tests failed: " << error.what() << '\n';
        return 1;
    }
}
