#pragma once

#include <algorithm>
#include <cmath>

namespace pubg {

struct StatusHudLayout {
    double font_size;
    double switch_stride;
    double marker_circle_offset;
    double switches_y;
    double weapons_y;
    double line_gap;
};

// Preserve fractional sizes through the GDI+ renderer and grow label spacing
// with the font. The legacy 13px/25px layout remains unchanged.
[[nodiscard]] inline StatusHudLayout statusHudLayout(double font_size, double line_gap) noexcept {
    font_size = std::clamp(std::isfinite(font_size) ? font_size : 13.0, 10.0, 22.0);
    line_gap = std::clamp(std::isfinite(line_gap) ? line_gap : 25.0, 18.0, 40.0);
    const double growth = std::max(0.0, font_size - 13.0);
    return {font_size,
            2.0 * font_size + 9.0,
            118.0 * font_size / 13.0,
            33.0 + growth,
            58.0 + 2.0 * growth,
            std::max(line_gap, font_size + 6.0)};
}

} // namespace pubg
