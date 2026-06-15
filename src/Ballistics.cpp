#include "Ballistics.hpp"

namespace pubg {

double Interpolator::sample(const std::vector<double>& xs, const std::vector<double>& ys, double x, double fallback) {
    if (xs.empty() || ys.empty() || xs.size() != ys.size()) {
        return fallback;
    }
    if (x <= xs.front()) {
        return ys.front();
    }
    if (x >= xs.back()) {
        return ys.back();
    }
    for (size_t i = 1; i < xs.size(); ++i) {
        if (x <= xs[i]) {
            const double t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
        }
    }
    return ys.back();
}

static std::vector<double> vec(const Json& j, const std::string& key) {
    std::vector<double> out;
    if (!j.contains(key) || !j[key].is_array()) {
        return out;
    }
    for (const auto& item : j[key]) {
        out.push_back(item.get<double>());
    }
    return out;
}

Ballistics::Ballistics(Config& config) : config_(config) {}

double Ballistics::rocketRatio(double distance) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("rocket_config", Json::object());
    });
    return Interpolator::sample(vec(c, "calib_dists"), vec(c, "calib_ratios"), distance, 0.0);
}

double Ballistics::vssDropRatio(double distance) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("vss_config", Json::object());
    });
    return Interpolator::sample(vec(c, "calib_dists"), vec(c, "calib_drops_ratio"), distance, 0.0);
}

double Ballistics::crossbowDropRatio(double distance) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("crossbow_config", Json::object());
    });
    return Interpolator::sample(vec(c, "calib_dists"), vec(c, "calib_drops_ratio"), distance, 0.0);
}

double Ballistics::throwableElevationRatio(double distance, bool jump_throw) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    return Interpolator::sample(
        vec(c, jump_throw ? "jump_calib_dists" : "calib_dists"),
        vec(c, jump_throw ? "jump_calib_elevations_ratio" : "calib_elevations_ratio"),
        distance,
        0.0);
}

double Ballistics::throwableCookTime(double distance, bool jump_throw) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("throwables_config", Json::object());
    });
    return Interpolator::sample(
        vec(c, jump_throw ? "jump_calib_dists" : "calib_dists"),
        vec(c, jump_throw ? "jump_calib_times" : "calib_times"),
        distance,
        0.0);
}

double Ballistics::mortarTrueDistance(double horizontal_distance, double elevation_ratio) const {
    const auto c = config_.read([](const Json& root) {
        return root.value("mortar_config", Json::object());
    });
    const double a = c.value("a_param", 0.2);
    const double b = c.value("b_param", 0.2);
    const double vertical = (elevation_ratio - 0.5) * 100.0 * a;
    return std::sqrt(horizontal_distance * horizontal_distance + vertical * vertical) + b;
}

} // namespace pubg
