#include "StatusHudLayout.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const auto original = pubg::statusHudLayout(13.0, 25.0);
        require(original.switch_stride == 35.0 && original.switches_y == 33.0 &&
                original.weapons_y == 58.0 && original.line_gap == 25.0,
                "legacy layout changed at the default font size");
        const auto fine = pubg::statusHudLayout(13.25, 25.0);
        require(fine.font_size == 13.25 && fine.switch_stride > original.switch_stride,
                "fractional font size or layout growth was truncated");
        const auto large = pubg::statusHudLayout(22.0, 18.0);
        require(large.switch_stride > 44.0 && large.line_gap >= 28.0 &&
                large.weapons_y - large.switches_y >= 28.0,
                "large HUD text overlaps the adjacent label or line");
        const auto invalid = pubg::statusHudLayout(std::numeric_limits<double>::quiet_NaN(),
                                                  std::numeric_limits<double>::infinity());
        require(invalid.font_size == 13.0 && invalid.line_gap == 25.0,
                "invalid layout values did not fall back to defaults");
        require(pubg::statusHudLayout(1000, 1000).font_size == 22.0 &&
                pubg::statusHudLayout(-1, -1).font_size == 10.0,
                "font size escaped the existing bounds");
        std::cout << "Status HUD layout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
