#pragma once

#include "Ballistics.hpp"
#include "Config.hpp"
#include "ElevationRadar.hpp"
#include "LargeMapRadar.hpp"
#include "MinimapRadar.hpp"
#include "RegionManager.hpp"
#include "ScreenCapture.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pubg {

class MortarAutoAim {
public:
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;

    MortarAutoAim(Config& config, RegionManager& regions, MinimapRadar& minimap, LargeMapRadar& large_map,
                  ElevationRadar& elevation, MessageCallback message_callback = {});
    ~MortarAutoAim();

    void trigger(const std::string& selected_color);
    void shutdown();

private:
    struct Step {
        int presses = 0;
        double distance = 121.0;
    };

    void run(std::string selected_color);
    void alignDirection(const std::string& selected_color, std::atomic_bool& stop_requested);
    [[nodiscard]] std::optional<double> selectedMarkerOffset(const std::string& selected_color) const;
    [[nodiscard]] std::optional<double> targetDistance(const std::string& selected_color) const;
    [[nodiscard]] std::vector<Step> loadSteps() const;
    [[nodiscard]] int resetHoldMs() const;
    [[nodiscard]] int keyDelayMs() const;
    [[nodiscard]] int directionMaxMs() const;
    [[nodiscard]] double directionTolerancePx() const;
    [[nodiscard]] double directionKp() const;
    [[nodiscard]] double directionKi() const;
    [[nodiscard]] double directionKd() const;
    [[nodiscard]] int directionStepDelayMs() const;
    [[nodiscard]] double directionMaxStepPx() const;
    [[nodiscard]] Step nearestStep(double distance) const;
    static void tapKey(int vk, int delay_ms);
    static void holdKey(int vk, int hold_ms);
    static void wheelDown(int delay_ms);

    Config& config_;
    RegionManager& regions_;
    MinimapRadar& minimap_;
    LargeMapRadar& large_map_;
    ElevationRadar& elevation_;
    Ballistics ballistics_;
    MessageCallback message_callback_;
    std::atomic_bool running_{false};
    std::thread worker_;
};

} // namespace pubg
