#pragma once

#include <mutex>
#include <shared_mutex>
#include <utility>

#include "Common.hpp"
#include "ResourcePaths.hpp"

namespace pubg {

// 全局配置管理器，对应 Python 版中各模块直接读取的配置数据。
// 磁盘上会把大块配置拆到多个 JSON 文件，内存中仍合并为一份完整配置供各模块读取。
class Config {
public:
    // paths 决定 config/*.json 和 assets/templates 的查找根目录。
    explicit Config(ResourcePaths paths);

    // 读取主配置并合并 map_data/recoil_settings/special_assistants 分文件。
    // 如果分文件不存在，会保留主配置里同名字段，便于兼容旧版单文件配置。
    bool load();
    bool loadRegionProfile(int width, int height);

    // 将当前 data_ 按职责写回多个 JSON 文件。主配置不再保存地图点位、压枪和特殊助手大字段。
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

private:
    bool loadJsonFile(const std::filesystem::path& path, Json& out) const;
    bool loadOptionalJsonFile(const std::filesystem::path& path, Json& out) const;
    bool saveJsonFile(const std::filesystem::path& path, const Json& data) const;
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
