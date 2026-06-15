#pragma once

#include "RegionManager.hpp"
#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

namespace pubg {

// 当前手持武器识别模块，对应 Python 版 modules/weapon_detector.py。
// 它只识别屏幕下方 weapon_region 内的“当前手持武器图标”，并通过回调通知主控。
class WeaponDetector {
public:
    // 回调参数：武器名、匹配分数。武器名为空表示当前没有稳定识别到武器。
    using Callback = std::function<void(const std::string&, double)>;

    // config 提供模板路径和缩放尺寸，regions 提供 weapon_region。
    WeaponDetector(Config& config, RegionManager& regions, int fps = 30, double threshold = 0.65);

    // 停止后台线程。
    ~WeaponDetector();

    // 开关识别线程。cb 非空时更新回调函数。
    void setEnabled(bool enabled, Callback cb = {});

    // 更新装备栏识别出的两把主武器。
    // 手持识别只在这两把主武器 + 特殊武器列表中找候选，减少误报。
    void updatePrimaryWeapons(std::optional<std::string> weapon1, std::optional<std::string> weapon2);

    // 返回当前稳定识别到的武器名和分数。
    [[nodiscard]] std::pair<std::string, double> currentWeapon() const;

private:
    // 截一帧 weapon_region 并完成一次模板匹配。
    std::pair<std::string, double> identifyOnce(ScreenCapture& capture);

    // 后台识别循环：按 fps 截图、匹配，并用连续帧确认避免抖动。
    void run();

    // 线程安全地复制当前回调，避免后台线程和 UI 线程同时访问 std::function。
    Callback callbackCopy() const;

    // 从 region_scaling_settings.weapon_region 读取目标缩放尺寸。
    void loadTargetSize();

    // 外部依赖：配置用于读取模板路径和缩放尺寸；RegionManager 提供截图区域。
    Config& config_;
    RegionManager& regions_;

    // 识别频率和阈值。target_w_/target_h_ 用于把截图缩放到和模板一致的尺寸。
    int fps_ = 30;
    double threshold_ = 0.65;
    int target_w_ = 160;
    int target_h_ = 50;

    // 已加载的手持武器模板，以及当前允许参与匹配的主武器/特殊武器候选。
    std::vector<WeaponTemplate> templates_;
    std::vector<std::optional<std::string>> primary_weapons_{2};
    std::vector<std::string> special_weapons_{"Rocket", "Grenade", "VSS", "Crossbow", "C4"};

    // 线程和回调状态。
    mutable std::mutex mutex_;
    std::atomic_bool enabled_{false};
    std::atomic_bool stop_{false};
    std::thread worker_;
    Callback callback_;

    // 当前稳定识别结果，以及用于连续帧确认的 pending 状态。
    std::string current_weapon_;
    double current_score_ = 0.0;
    std::string pending_weapon_;
    int pending_counter_ = 0;
};

} // namespace pubg
