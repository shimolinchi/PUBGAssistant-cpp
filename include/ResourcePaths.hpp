#pragma once

#include "Common.hpp"

namespace pubg {

// 管理 C++ 版运行时资源路径。
// 目标是让 VS 调试目录和 Release exe 目录都能找到 config/ 与 assets/templates/。
class ResourcePaths {
public:
    // executable_path 为空时自动取当前 exe 所在目录；如果从项目根运行，会回退到当前工作目录。
    explicit ResourcePaths(std::filesystem::path executable_path = {});

    // 当前推断出的项目/运行根目录。
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // 配置目录：root/config。
    [[nodiscard]] std::filesystem::path configDir() const;

    // 主配置文件路径：root/config/config.json。
    [[nodiscard]] std::filesystem::path configFile() const;

    // 地图点位配置路径：root/config/map_data.json。
    [[nodiscard]] std::filesystem::path mapDataFile() const;

    // 压枪参数配置路径：root/config/recoil_settings.json。
    [[nodiscard]] std::filesystem::path recoilSettingsFile() const;

    // 特殊武器/投掷物助手配置路径：root/config/special_assistants.json。
    [[nodiscard]] std::filesystem::path specialAssistantsFile() const;

    // 模板根目录：root/assets/templates。
    [[nodiscard]] std::filesystem::path templatesDir() const;

    // 获取模板子路径，例如 templatePath("weapons")。
    [[nodiscard]] std::filesystem::path templatePath(const std::string& relative) const;

    // 应用图标路径：root/icon.ico。
    [[nodiscard]] std::filesystem::path iconFile() const;

private:
    std::filesystem::path root_;
};

} // namespace pubg
