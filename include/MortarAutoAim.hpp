#pragma once

#include "Config.hpp"
#include "LargeMapRadar.hpp"
#include "MinimapRadar.hpp"

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

    MortarAutoAim(Config& config, MinimapRadar& minimap, LargeMapRadar& large_map, MessageCallback message_callback = {});
    ~MortarAutoAim();

    void trigger(const std::string& selected_color);
    void shutdown();

private:
    struct Step {
        int presses = 0;
        double distance = 121.0;
    };

    void run(std::string selected_color);
    [[nodiscard]] std::optional<double> targetDistance(const std::string& selected_color) const;
    [[nodiscard]] std::vector<Step> loadSteps() const;
    [[nodiscard]] int resetHoldMs() const;
    [[nodiscard]] int keyDelayMs() const;
    [[nodiscard]] Step nearestStep(double distance) const;
    static void tapKey(int vk, int delay_ms);
    static void holdKey(int vk, int hold_ms);
    static void wheelDown(int delay_ms);

    Config& config_;
    MinimapRadar& minimap_;
    LargeMapRadar& large_map_;
    MessageCallback message_callback_;
    std::atomic_bool running_{false};
    std::thread worker_;
};

} // namespace pubg
