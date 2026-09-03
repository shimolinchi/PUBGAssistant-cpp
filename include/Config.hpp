#pragma once

#include <mutex>
#include <shared_mutex>
#include <utility>

#include "AiAimSettings.hpp"
#include "Common.hpp"
#include "ResourcePaths.hpp"

namespace pubg {

// 编辑基线同时保存文档和本次读取时分文件是否存在；这是内存元数据，不写入 JSON。
struct AiAimSettingsDocumentSnapshot {
    Json document = Json::object();
    bool split_file_exists = false;

    bool operator==(const AiAimSettingsDocumentSnapshot&) const = default;
};

// 全局配置管理器，对应 Python 版中各模块直接读取的配置数据。
// 磁盘上会把大块配置拆到多个 JSON 文件，内存中仍合并为一份完整配置供各模块读取。
class Config {
public:
    // paths 决定 config/*.json 和 assets/templates 的查找根目录。
    explicit Config(ResourcePaths paths);

    // 读取主配置并合并 map_data/recoil_settings/special_assistants/ai_aim 分文件。
    // 如果分文件不存在，会保留主配置里同名字段，便于兼容旧版单文件配置。
    bool load();
    bool loadRegionProfile(int width, int height);

    // 将 UI 可编辑配置按职责写回多个 JSON 文件。AI 参数独立编辑、下次启动生效；
    // 普通保存不会重写 ai_aim_settings.json，避免覆盖运行期间的外部编辑。
    bool save();
    bool saveRegionProfile() const;
    [[nodiscard]] bool hasActiveRegionProfile() const noexcept { return active_region_w_ > 0 && active_region_h_ > 0; }
    [[nodiscard]] std::pair<int, int> activeRegionResolution() const noexcept { return {active_region_w_, active_region_h_}; }

    // 原始 JSON 不再向外暴露裸引用。读取用 read()，写入用 write()，避免后台线程和 UI 线程数据竞争。

    // 在共享锁保护下读取配置。fn 接收 const Json&，其返回值会被透传。
    // 所有后台线程的每帧配置读取都应走这里，与 UI 线程的 write()/save() 互斥。
    template <class Fn>
    auto read(Fn&& fn) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return fn(static_cast<const Json&>(data_));
    }

    // 在独占锁保护下修改配置。fn 接收 Json&。修改后如需落盘请在返回后再调用 save()。
    template <class Fn>
    auto write(Fn&& fn) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return fn(data_);
    }

    // 返回资源路径管理器，用于定位模板目录和配置文件。
    [[nodiscard]] const ResourcePaths& paths() const noexcept { return paths_; }

    // 解析 pnt_colors，返回小地图/大地图/测高标点使用的 HSV 阈值和显示颜色。
    [[nodiscard]] std::vector<MarkerColor> markerColors() const;

    // 返回颜色名到 hex 颜色的映射，HUD 绘制时按 Yellow/Orange/Blue/Green 取色。
    [[nodiscard]] std::unordered_map<std::string, std::string> markerHex() const;

    // 解析 hotkeys 字段。App 和快捷键设置页会用它注册全局轮询热键。
    [[nodiscard]] std::unordered_map<std::string, std::string> hotkeys() const;

    // 返回内存中的 AI 配置快照；保存成功后由宿主将该快照应用到运行模块。
    // JSON 映射与校验只在 AiAimSettings 中实现。
    [[nodiscard]] AiAimSettings aiAimSettings() const;

    // 编辑器读取磁盘最新原文；只有分文件缺失时才退回内存旧版配置/默认空对象。
    // JSON 语法或 I/O 失败返回 false；schema/类型校验由 parseAiAimSettings 统一处理。
    [[nodiscard]] bool readAiAimSettingsDocument(
        AiAimSettingsDocumentSnapshot& snapshot, std::string& error) const;

    // 显式保存 AI 分文件：校验后同时检查 expected 的文档和分文件存在性，
    // 用同目录临时文件原子替换，成功后才更新内存。普通 save() 不调用此接口。
    [[nodiscard]] bool saveAiAimSettings(
        const Json& document, const AiAimSettingsDocumentSnapshot& expected, std::string& error);

private:
    bool loadJsonFile(const std::filesystem::path& path, Json& out) const;
    bool loadOptionalJsonFile(const std::filesystem::path& path, Json& out) const;
    bool saveJsonFile(const std::filesystem::path& path, const Json& data) const;
    bool readAiAimSettingsDocumentUnlocked(
        AiAimSettingsDocumentSnapshot& snapshot, std::string& error) const;
    void mergeSplitConfig();
    [[nodiscard]] Json baseConfigWithoutSplitSections() const;
    [[nodiscard]] Json regionProfileConfig() const;
    [[nodiscard]] Json specialAssistantsConfig() const;
    // markerColors() 的无锁实现，供已持有 mutex_ 的 markerColors()/markerHex() 复用，避免共享锁递归。
    [[nodiscard]] std::vector<MarkerColor> markerColorsUnlocked() const;

    ResourcePaths paths_;
    Json data_;
    int active_region_w_ = 0;
    int active_region_h_ = 0;
    // 保护 data_：后台识别线程读、UI 线程写。read()/save() 取共享锁，write()/load() 取独占锁。
    mutable std::shared_mutex mutex_;
};

} // namespace pubg
