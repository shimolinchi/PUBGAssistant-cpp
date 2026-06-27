#pragma once

#include "Common.hpp"

namespace pubg {

class ResourcePaths {
public:
    explicit ResourcePaths(std::filesystem::path executable_path = {});

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    [[nodiscard]] std::filesystem::path configDir() const;
    [[nodiscard]] std::filesystem::path configFile() const;
    [[nodiscard]] std::filesystem::path mapDataFile() const;
    [[nodiscard]] std::filesystem::path recoilSettingsFile() const;
    [[nodiscard]] std::filesystem::path specialAssistantsFile() const;
    [[nodiscard]] std::filesystem::path regionConfigDir() const;
    [[nodiscard]] std::filesystem::path regionConfigFile(int width, int height) const;

    [[nodiscard]] std::filesystem::path templatesDir() const;
    [[nodiscard]] std::filesystem::path templatePath(const std::string& relative) const;
    [[nodiscard]] std::filesystem::path iconFile() const;
    [[nodiscard]] std::filesystem::path readmePdfFile() const;

private:
    std::filesystem::path root_;
};

} // namespace pubg
