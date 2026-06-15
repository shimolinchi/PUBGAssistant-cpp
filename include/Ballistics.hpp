#pragma once

#include "Config.hpp"

namespace pubg {

// 一维线性插值工具。
// Python 版里大量使用 np.interp 计算弹道曲线，C++ 版统一收敛到这里。
class Interpolator {
public:
    // 在 xs/ys 曲线上按 x 做线性插值。
    // xs 和 ys 长度不一致或为空时返回 fallback；x 超出范围时返回端点值。
    static double sample(const std::vector<double>& xs, const std::vector<double>& ys, double x, double fallback = 0.0);
};

// 特殊武器弹道/时间计算模块。
// 对应 Python 版 rocket/vss/crossbow/throwables/mortar 各助手中的配置曲线读取和插值计算。
class Ballistics {
public:
    // 持有 Config 引用，实时从 config.json 的 *_config 字段读取曲线。
    explicit Ballistics(Config& config);

    // 火箭筒：根据距离返回准星向下/向上标记的比例。
    // 读取 rocket_config.calib_dists 和 rocket_config.calib_ratios。
    double rocketRatio(double distance) const;

    // VSS：根据距离返回弹道下坠比例。
    // 读取 vss_config.calib_dists 和 vss_config.calib_drops_ratio。
    double vssDropRatio(double distance) const;

    // 十字弩：根据距离返回弹道下坠比例。
    // 读取 crossbow_config.calib_dists 和 crossbow_config.calib_drops_ratio。
    double crossbowDropRatio(double distance) const;

    // 投掷物：根据距离返回抬高比例。
    // jump_throw=false 使用普通投掷曲线；true 使用跳投曲线。
    double throwableElevationRatio(double distance, bool jump_throw = false) const;

    // 投掷物：根据距离返回捏雷/释放计时。
    // ThrowablesAssistant 会用它决定自动松开左键的时间。
    double throwableCookTime(double distance, bool jump_throw = false) const;

    // 迫击炮：根据小地图水平距离和垂直测高比例估算真实打击距离。
    // 读取 mortar_config.a_param/b_param；这是 Python 版高低差修正的简化迁移。
    double mortarTrueDistance(double horizontal_distance, double elevation_ratio) const;

private:
    Config& config_;
};

} // namespace pubg
