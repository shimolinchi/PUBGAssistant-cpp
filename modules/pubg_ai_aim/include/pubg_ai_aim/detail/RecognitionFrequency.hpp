#pragma once

#include "pubg_ai_aim/AimTypes.hpp"

#include <optional>

namespace pubg::ai_aim::detail {

// Worker completion rate, not capture sequence deltas, UI polls or target_fps.
// Half-second averages keep the HUD stable. No allocation or extra worker is
// needed; the owning module serializes record/reset/read with its state mutex.
class RecognitionFrequency {
public:
    void reset() noexcept { *this = RecognitionFrequency{}; }

    void record(Clock::time_point now) noexcept {
        if (!last_completion_ || now < *last_completion_ ||
            now - *last_completion_ >= stale_after) {
            reset();
            window_start_ = now;
            last_completion_ = now;
            return; // The first completion is the baseline, not an interval.
        }
        last_completion_ = now;
        ++completed_intervals_;
        const auto elapsed = now - window_start_;
        if (elapsed >= sample_window) {
            hertz_ = static_cast<double>(completed_intervals_) /
                std::chrono::duration<double>(elapsed).count();
            window_start_ = now;
            completed_intervals_ = 0;
        }
    }

    [[nodiscard]] double hertz(Clock::time_point now) const noexcept {
        if (!last_completion_ || now < *last_completion_ ||
            now - *last_completion_ >= stale_after) return 0.0;
        return hertz_;
    }

private:
    static constexpr auto sample_window = std::chrono::milliseconds(500);
    static constexpr auto stale_after = std::chrono::seconds(1);
    Clock::time_point window_start_{};
    std::optional<Clock::time_point> last_completion_;
    std::uint64_t completed_intervals_ = 0;
    double hertz_ = 0.0;
};

} // namespace pubg::ai_aim::detail
