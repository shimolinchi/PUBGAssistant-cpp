#pragma once

#include "Ballistics.hpp"
#include "ElevationRadar.hpp"
#include "MinimapRadar.hpp"
#include "OverlayWindow.hpp"
#include "ScreenCapture.hpp"

#include <optional>
#include <utility>

namespace pubg {

class SpecialAssistants {
public:
    SpecialAssistants(Config& config, RegionManager& regions, MinimapRadar& minimap, ElevationRadar& elevation);
    ~SpecialAssistants();

    void setDisplayEnabled(bool enabled);
    void setCurrentWeapon(const std::string& weapon);
    void setManualEnabled(const std::string& key, bool enabled);
    void setMarkerHex(std::unordered_map<std::string, std::string> hex);
    void shutdown();

private:
    void run();
    void drawRocket(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                    std::vector<OverlayCommand>& cmds);
    void drawVss(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                 std::vector<OverlayCommand>& cmds);
    void drawCrossbow(const DistanceMap& dists, const std::unordered_map<std::string, std::string>& hex,
                      std::vector<OverlayCommand>& cmds);
    [[nodiscard]] std::optional<std::pair<double, double>> detectCrosshairCenter(ScreenCapture& capture) const;
    [[nodiscard]] std::optional<std::pair<double, double>> cachedCrosshairCenter();
    void drawMortar(const DistanceMap& dists, const ElevationMap& elevs,
                    const std::unordered_map<std::string, std::string>& hex,
                    std::vector<OverlayCommand>& cmds);

    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    ElevationRadar& elevation_;

    Ballistics ballistics_;
    std::unordered_map<std::string, std::string> hex_;

    std::atomic_bool running_{true};
    std::atomic_bool display_enabled_{false};
    std::thread worker_;

    std::mutex mutex_;
    std::string current_weapon_;
    std::unordered_map<std::string, bool> manual_;
    std::optional<std::pair<double, double>> cached_crosshair_center_;
    double last_crosshair_sample_ = 0.0;

    OverlayWindow overlay_;
};

} // namespace pubg
