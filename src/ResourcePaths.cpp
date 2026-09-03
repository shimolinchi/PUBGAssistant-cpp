#include "ResourcePaths.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pubg {

static std::filesystem::path currentExecutableDir() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(len);
    if (!buffer.empty()) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

ResourcePaths::ResourcePaths(std::filesystem::path executable_path) {
    if (executable_path.empty()) {
        executable_path = currentExecutableDir();
    }
    root_ = std::filesystem::absolute(executable_path);

    // Debugging from the project root should work without copying files first.
    if (!std::filesystem::exists(root_ / "config" / "config.json")) {
        auto probe = std::filesystem::current_path();
        if (std::filesystem::exists(probe / "config" / "config.json")) {
            root_ = probe;
        }
    }
}

std::filesystem::path ResourcePaths::configDir() const {
    return root_ / "config";
}

std::filesystem::path ResourcePaths::configFile() const {
    return configDir() / "config.json";
}

std::filesystem::path ResourcePaths::mapDataFile() const {
    return configDir() / "map_data.json";
}

std::filesystem::path ResourcePaths::recoilSettingsFile() const {
    return configDir() / "recoil_settings.json";
}

std::filesystem::path ResourcePaths::specialAssistantsFile() const {
    return configDir() / "special_assistants.json";
}

std::filesystem::path ResourcePaths::aiAimSettingsFile() const {
    return configDir() / "ai_aim_settings.json";
}

std::filesystem::path ResourcePaths::aiAimRuntimeDir() const {
    return root_ / "ai_aim_runtime";
}

std::filesystem::path ResourcePaths::regionConfigDir() const {
    return configDir() / "regions";
}

std::pair<int, int> ResourcePaths::normalizeRegionResolution(int width, int height) const {
    width = std::max(1, width);
    height = std::max(1, height);

    const auto dir = regionConfigDir();
    auto existsProfile = [&](int w, int h) {
        std::error_code ec;
        return std::filesystem::exists(dir / (std::to_string(w) + "_" + std::to_string(h) + ".json"), ec);
    };

    // Windows/Qt DPI can occasionally produce an off-by-one physical size such as 1921x1080.
    // If an adjacent calibrated profile exists, always use it for both reading and saving.
    if ((width % 2) != 0 && width > 1 && existsProfile(width - 1, height)) {
        return {width - 1, height};
    }
    if ((height % 2) != 0 && height > 1 && existsProfile(width, height - 1)) {
        return {width, height - 1};
    }
    if (existsProfile(width, height)) {
        return {width, height};
    }

    const std::pair<int, int> candidates[] = {
        {width - 1, height},
        {width + 1, height},
        {width, height - 1},
        {width, height + 1},
        {width - 1, height - 1},
        {width + 1, height + 1},
        {width - 1, height + 1},
        {width + 1, height - 1},
    };
    for (const auto& [w, h] : candidates) {
        if (w > 0 && h > 0 && existsProfile(w, h)) {
            return {w, h};
        }
    }
    return {width, height};
}

std::filesystem::path ResourcePaths::regionConfigFile(int width, int height) const {
    const auto [normalized_w, normalized_h] = normalizeRegionResolution(width, height);
    return regionConfigDir() / (std::to_string(normalized_w) + "_" + std::to_string(normalized_h) + ".json");
}

std::filesystem::path ResourcePaths::templatesDir() const {
    return root_ / "assets" / "templates";
}

std::filesystem::path ResourcePaths::templatePath(const std::string& relative) const {
    return templatesDir() / std::filesystem::path(relative);
}

std::filesystem::path ResourcePaths::iconFile() const {
    return root_ / "icon.ico";
}

std::filesystem::path ResourcePaths::readmePdfFile() const {
    return root_ / "README.pdf";
}

} // namespace pubg
