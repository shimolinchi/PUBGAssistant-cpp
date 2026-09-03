#include "Config.hpp"
#include "ScopeExit.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <system_error>

namespace pubg {

Config::Config(ResourcePaths paths) : paths_(std::move(paths)) {}

bool Config::load() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!loadJsonFile(paths_.configFile(), data_)) {
        data_ = Json::object();
        return false;
    }
    mergeSplitConfig();
    return true;
}

bool Config::loadRegionProfile(int width, int height) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto [normalized_w, normalized_h] = paths_.normalizeRegionResolution(width, height);
    active_region_w_ = normalized_w;
    active_region_h_ = normalized_h;

    Json profile;
    const bool loaded = loadOptionalJsonFile(paths_.regionConfigFile(normalized_w, normalized_h), profile) && profile.is_object();
    if (loaded) {
        data_["real_regions"] = profile.value("real_regions", Json::object());
        data_["real_scales"] = profile.value("real_scales", Json::object());
        data_["region_scaling_settings"] = profile.value("region_scaling_settings", Json::object());
    } else {
        if (!data_.contains("real_regions") || !data_["real_regions"].is_object()) {
            data_["real_regions"] = Json::object();
        }
        if (!data_.contains("real_scales") || !data_["real_scales"].is_object()) {
            data_["real_scales"] = Json::object();
        }
        if (!data_.contains("region_scaling_settings") || !data_["region_scaling_settings"].is_object()) {
            data_["region_scaling_settings"] = Json::object();
        }
    }
    return loaded;
}

bool Config::save() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::filesystem::create_directories(paths_.configDir());
    const bool ok_main = saveJsonFile(paths_.configFile(), baseConfigWithoutSplitSections());
    const bool ok_map = data_.contains("map_data")
        ? saveJsonFile(paths_.mapDataFile(), data_["map_data"])
        : true;
    const bool ok_recoil = data_.contains("recoil_settings")
        ? saveJsonFile(paths_.recoilSettingsFile(), data_["recoil_settings"])
        : true;
    const bool ok_special = saveJsonFile(paths_.specialAssistantsFile(), specialAssistantsConfig());
    const bool ok_region = hasActiveRegionProfile()
        ? saveJsonFile(paths_.regionConfigFile(active_region_w_, active_region_h_), regionProfileConfig())
        : true;
    return ok_main && ok_map && ok_recoil && ok_special && ok_region;
}

bool Config::saveRegionProfile() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!hasActiveRegionProfile()) {
        return false;
    }
    return saveJsonFile(paths_.regionConfigFile(active_region_w_, active_region_h_), regionProfileConfig());
}

bool Config::loadJsonFile(const std::filesystem::path& path, Json& out) const {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[config] cannot open " << path << "\n";
        out = Json::object();
        return false;
    }
    try {
        in >> out;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[config] parse failed in " << path << ": " << e.what() << "\n";
        out = Json::object();
        return false;
    }
}

bool Config::loadOptionalJsonFile(const std::filesystem::path& path, Json& out) const {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        out = Json::object();
        return false;
    }
    return loadJsonFile(path, out);
}

bool Config::saveJsonFile(const std::filesystem::path& path, const Json& data) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) {
        std::cerr << "[config] cannot write " << path << "\n";
        return false;
    }
    out << data.dump(4);
    return true;
}

void Config::mergeSplitConfig() {
    Json split;
    if (loadOptionalJsonFile(paths_.mapDataFile(), split)) {
        data_["map_data"] = std::move(split);
    }
    if (loadOptionalJsonFile(paths_.recoilSettingsFile(), split)) {
        data_["recoil_settings"] = std::move(split);
    }
    if (loadOptionalJsonFile(paths_.specialAssistantsFile(), split) && split.is_object()) {
        for (auto it = split.begin(); it != split.end(); ++it) {
            data_[it.key()] = it.value();
        }
    }
    std::error_code ai_aim_error;
    if (std::filesystem::exists(paths_.aiAimSettingsFile(), ai_aim_error) && !ai_aim_error) {
        if (loadJsonFile(paths_.aiAimSettingsFile(), split)) {
            data_["ai_aim"] = std::move(split);
        } else {
            // Preserve a malformed-file signal for typed validation instead of silently
            // turning a parse error into valid defaults.
            data_["ai_aim"] = Json{};
        }
    }
}

Json Config::baseConfigWithoutSplitSections() const {
    Json base = data_;
    base.erase("map_data");
    base.erase("recoil_settings");
    base.erase("real_regions");
    base.erase("real_scales");
    base.erase("region_scaling_settings");
    std::error_code ai_aim_error;
    if (std::filesystem::exists(paths_.aiAimSettingsFile(), ai_aim_error) && !ai_aim_error) {
        base.erase("ai_aim");
    }
    static constexpr std::array<const char*, 6> special_keys = {
        "throwables_config",
        "rocket_config",
        "vss_config",
        "crossbow_config",
        "c4_config",
        "mortar_config",
    };
    for (const auto* key : special_keys) {
        base.erase(key);
    }
    return base;
}

Json Config::regionProfileConfig() const {
    Json out = Json::object();
    out["resolution"] = Json{{"width", active_region_w_}, {"height", active_region_h_}};
    out["real_regions"] = data_.value("real_regions", Json::object());
    out["real_scales"] = data_.value("real_scales", Json::object());
    out["region_scaling_settings"] = data_.value("region_scaling_settings", Json::object());
    return out;
}

Json Config::specialAssistantsConfig() const {
    Json out = Json::object();
    static constexpr std::array<const char*, 6> special_keys = {
        "throwables_config",
        "rocket_config",
        "vss_config",
        "crossbow_config",
        "c4_config",
        "mortar_config",
    };
    for (const auto* key : special_keys) {
        if (data_.contains(key)) {
            out[key] = data_[key];
        }
    }
    return out;
}

std::vector<MarkerColor> Config::markerColors() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return markerColorsUnlocked();
}

std::vector<MarkerColor> Config::markerColorsUnlocked() const {
    static const std::vector<std::pair<std::string, Json>> fallback = {
        {"Yellow", Json{{"lower", {26, 150, 160}}, {"upper", {30, 255, 255}}, {"hex", "#E3D43C"}}},
        {"Orange", Json{{"lower", {10, 160, 160}}, {"upper", {14, 255, 255}}, {"hex", "#B3500D"}}},
        {"Blue", Json{{"lower", {110, 120, 160}}, {"upper", {114, 255, 255}}, {"hex", "#1A3EA3"}}},
        {"Green", Json{{"lower", {78, 150, 120}}, {"upper", {82, 255, 255}}, {"hex", "#109166"}}},
    };

    std::vector<MarkerColor> colors;
    const Json* source = nullptr;
    const std::string mode = data_.value("pnt_color_mode", std::string("normal"));
    if (data_.contains("pnt_color_modes") &&
        data_["pnt_color_modes"].is_object() &&
        data_["pnt_color_modes"].contains(mode) &&
        data_["pnt_color_modes"][mode].is_object()) {
        source = &data_["pnt_color_modes"][mode];
    }

    auto addColor = [&colors](const std::string& name, const Json& item) {
        if (!item.contains("lower") || !item.contains("upper")) {
            return;
        }
        auto lower = item["lower"];
        auto upper = item["upper"];
        if (!lower.is_array() || !upper.is_array() || lower.size() < 3 || upper.size() < 3) {
            return;
        }
        colors.push_back(MarkerColor{
            name,
            cv::Scalar(lower[0].get<int>(), lower[1].get<int>(), lower[2].get<int>()),
            cv::Scalar(upper[0].get<int>(), upper[1].get<int>(), upper[2].get<int>()),
            item.value("hex", "#FFFFFF")
        });
    };

    if (source) {
        for (auto it = source->begin(); it != source->end(); ++it) {
            addColor(it.key(), it.value());
        }
    }
    if (colors.empty()) {
        for (const auto& [name, item] : fallback) {
            addColor(name, item);
        }
    }
    return colors;
}

std::unordered_map<std::string, std::string> Config::markerHex() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::unordered_map<std::string, std::string> out;
    for (const auto& color : markerColorsUnlocked()) {
        out[color.name] = color.hex;
    }
    return out;
}

std::unordered_map<std::string, std::string> Config::hotkeys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::unordered_map<std::string, std::string> out;
    if (!data_.contains("hotkeys") || !data_["hotkeys"].is_object()) {
        return out;
    }
    for (auto it = data_["hotkeys"].begin(); it != data_["hotkeys"].end(); ++it) {
        if (it.value().is_string()) {
            out[it.key()] = it.value().get<std::string>();
        }
    }
    return out;
}

AiAimSettings Config::aiAimSettings() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (data_.contains("ai_aim")) {
        return parseAiAimSettings(data_["ai_aim"]);
    }
    return parseAiAimSettings(Json::object());
}

bool Config::readAiAimSettingsDocument(
    AiAimSettingsDocumentSnapshot& snapshot, std::string& error) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return readAiAimSettingsDocumentUnlocked(snapshot, error);
}

bool Config::readAiAimSettingsDocumentUnlocked(
    AiAimSettingsDocumentSnapshot& snapshot, std::string& error) const {
    error.clear();
    try {
        const auto path = paths_.aiAimSettingsFile();
        std::error_code status_error;
        const bool exists = std::filesystem::exists(path, status_error);
        if (status_error) {
            error = "Cannot inspect AI settings: " + status_error.message();
            return false;
        }
        if (!exists) {
            snapshot = {data_.contains("ai_aim") ? data_["ai_aim"] : Json::object(), false};
            return true;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "Cannot read AI settings file: " + path.string();
            return false;
        }
        // Parse the entire stream, including rejecting non-whitespace trailing data.
        Json loaded = Json::parse(input);
        if (input.bad()) {
            error = "I/O error while reading AI settings file";
            return false;
        }
        snapshot = {std::move(loaded), true};
        return true;
    } catch (const std::exception& failure) {
        error = std::string("Cannot read AI settings: ") + failure.what();
        return false;
    }
}

bool Config::saveAiAimSettings(
    const Json& document, const AiAimSettingsDocumentSnapshot& expected, std::string& error) {
    error.clear();
    const auto settings = parseAiAimSettings(document);
    if (!settings.valid()) {
        error = settings.validation_error;
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    try {
        AiAimSettingsDocumentSnapshot current;
        if (!readAiAimSettingsDocumentUnlocked(current, error)) {
            return false;
        }
        if (current != expected) {
            error = "AI settings changed outside this editor; reopen the settings before saving";
            return false;
        }
        const auto current_settings = parseAiAimSettings(current.document);
        if (!current_settings.valid()) {
            error = "Existing AI settings are invalid; the editor cannot overwrite them: " +
                current_settings.validation_error;
            return false;
        }

        // Allocate the future in-memory state before writing, so allocation failures
        // cannot leave disk updated while the caller receives a save failure.
        Json saved_data = data_;
        saved_data["ai_aim"] = document;
        const std::string serialized = document.dump(4);
        // nlohmann serializes non-finite floating values as null. Reject such
        // in-memory inputs rather than successfully persisting an invalid document.
        const auto serialized_settings = parseAiAimSettings(Json::parse(serialized));
        if (!serialized_settings.valid()) {
            error = serialized_settings.validation_error;
            return false;
        }
        const auto path = paths_.aiAimSettingsFile();
        std::filesystem::create_directories(path.parent_path());
        static std::atomic_uint64_t next_temporary_id{0};
        auto temporary = path;
        temporary += ".tmp." + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
            std::to_string(next_temporary_id.fetch_add(1, std::memory_order_relaxed));
        bool temporary_created = false;
        const ScopeExit cleanup([&]() noexcept {
            if (temporary_created) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
        });

#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "Cannot create temporary AI settings: " +
                std::system_category().message(static_cast<int>(GetLastError()));
            return false;
        }
        temporary_created = true;
        {
            const ScopeExit close_file([&]() noexcept { CloseHandle(file); });
            std::size_t written = 0;
            while (written < serialized.size()) {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                    serialized.size() - written, 1024 * 1024));
                DWORD count = 0;
                if (!WriteFile(file, serialized.data() + written, chunk, &count, nullptr) ||
                    count == 0) {
                    error = "Cannot write temporary AI settings";
                    return false;
                }
                written += count;
            }
            if (!FlushFileBuffers(file)) {
                error = "Cannot flush temporary AI settings";
                return false;
            }
        }
#else
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "Cannot create temporary AI settings";
                return false;
            }
            temporary_created = true;
            output << serialized;
            output.flush();
            output.close();
            if (!output) {
                error = "Cannot write temporary AI settings";
                return false;
            }
        }
#endif

        // Recheck after staging the write as well; an external edit while the
        // temporary file was being written must not be silently replaced.
        if (!readAiAimSettingsDocumentUnlocked(current, error)) {
            return false;
        }
        if (current != expected) {
            error = "AI settings changed outside this editor; reopen the settings before saving";
            return false;
        }
#ifdef _WIN32
        // A baseline without a split file may create one, but must not replace
        // a file another editor creates after the final optimistic check.
        const DWORD move_flags = MOVEFILE_WRITE_THROUGH |
            (expected.split_file_exists ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         move_flags)) {
            error = "Cannot replace AI settings file: " +
                std::system_category().message(static_cast<int>(GetLastError()));
            return false;
        }
#else
        std::filesystem::rename(temporary, path);
#endif
        temporary_created = false;
        data_.swap(saved_data);
        return true;
    } catch (const std::exception& failure) {
        error = std::string("Cannot save AI settings: ") + failure.what();
        return false;
    }
}

} // namespace pubg
