#pragma once

#include "Config.hpp"
#include "OverlayWindow.hpp"

namespace pubg {

class RegionManager {
public:
    struct DisplayInfo {
        int left = 0;
        int top = 0;
        int width = 1920;
        int height = 1080;
        int qt_left = 0;
        int qt_top = 0;
        int qt_width = 1920;
        int qt_height = 1080;
        double device_pixel_ratio = 1.0;
        std::string name;
    };

    explicit RegionManager(Config& config, DisplayInfo display);

    [[nodiscard]] std::optional<Rect> getRealRegion(const std::string& name) const;
    [[nodiscard]] double getRealScale(const std::string& name, double fallback = 0.0) const;

    [[nodiscard]] int screenWidth() const noexcept { return screen_w_; }
    [[nodiscard]] int screenHeight() const noexcept { return screen_h_; }
    [[nodiscard]] int screenLeft() const noexcept { return screen_left_; }
    [[nodiscard]] int screenTop() const noexcept { return screen_top_; }
    [[nodiscard]] int qtScreenLeft() const noexcept { return qt_screen_left_; }
    [[nodiscard]] int qtScreenTop() const noexcept { return qt_screen_top_; }
    [[nodiscard]] int qtScreenWidth() const noexcept { return qt_screen_w_; }
    [[nodiscard]] int qtScreenHeight() const noexcept { return qt_screen_h_; }
    [[nodiscard]] double devicePixelRatio() const noexcept { return device_pixel_ratio_; }

    void setRealRegion(const std::string& name, Rect rect);
    void setRealScale(const std::string& name, double value);
    bool createOverlay(OverlayWindow& overlay, const std::wstring& title, bool click_through = true,
                       bool exclude_from_capture = true) const;

    void syncCrosshairRegion();
    void syncCompassRegion();

private:
    Config& config_;
    int screen_left_ = 0;
    int screen_top_ = 0;
    int screen_w_ = 1920;
    int screen_h_ = 1080;
    int qt_screen_left_ = 0;
    int qt_screen_top_ = 0;
    int qt_screen_w_ = 1920;
    int qt_screen_h_ = 1080;
    double device_pixel_ratio_ = 1.0;
};

} // namespace pubg
