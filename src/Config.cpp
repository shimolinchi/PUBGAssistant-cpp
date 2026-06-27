#include "Config.hpp"

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
    active_region_w_ = width;
    active_region_h_ = height;

    Json profile;
    const bool loaded = loadOptionalJsonFile(paths_.regionConfigFile(width, height), profile) && profile.is_object();
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
}

Json Config::baseConfigWithoutSplitSections() const {
    Json base = data_;
    base.erase("map_data");
    base.erase("recoil_settings");
    base.erase("real_regions");
    base.erase("real_scales");
    base.erase("region_scaling_settings");
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

} // namespace pubg
