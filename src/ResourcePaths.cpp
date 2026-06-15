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

std::filesystem::path ResourcePaths::templatesDir() const {
    return root_ / "assets" / "templates";
}

std::filesystem::path ResourcePaths::templatePath(const std::string& relative) const {
    return templatesDir() / std::filesystem::path(relative);
}

std::filesystem::path ResourcePaths::iconFile() const {
    return root_ / "icon.ico";
}

} // namespace pubg
