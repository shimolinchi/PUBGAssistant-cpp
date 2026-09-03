#pragma once

#include <utility>

namespace pubg {

// Owner-thread shutdown phase. begin() returning false makes repeated or
// re-entrant stop calls harmless; resources may only be released after
// markStopped() publishes successful mandatory quiescence.
class ShutdownLifecycle final {
public:
    enum class Phase {
        Running,
        Stopping,
        Stopped,
    };

    [[nodiscard]] bool begin() noexcept {
        if (phase_ != Phase::Running) {
            return false;
        }
        phase_ = Phase::Stopping;
        return true;
    }

    void markStopped() noexcept {
        if (phase_ == Phase::Stopping) {
            phase_ = Phase::Stopped;
        }
    }

    [[nodiscard]] bool stopped() const noexcept { return phase_ == Phase::Stopped; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

private:
    Phase phase_ = Phase::Running;
};

// Runs every independent mandatory shutdown action while remembering whether
// quiescence was fully proven. Callers must not release worker-owned resources
// unless succeeded() is true.
class ShutdownSequence final {
public:
    template <typename Action>
    void runMandatory(Action&& action) noexcept {
        try {
            std::forward<Action>(action)();
        } catch (...) {
            succeeded_ = false;
        }
    }

    [[nodiscard]] bool succeeded() const noexcept { return succeeded_; }

private:
    bool succeeded_ = true;
};

} // namespace pubg
