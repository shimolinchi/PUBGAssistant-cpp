#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace pubg {

struct AiAimRuntimeErrorTransition final {
    bool changed = false;
    bool recovered = false;
    std::string active_error;
};

[[nodiscard]] inline bool shouldAcceptAiAimRuntimeSample(
    const bool enabled,
    const std::uint64_t observed_session,
    const std::uint64_t current_session) noexcept {
    return enabled && observed_session == current_session;
}

// Tracks error edges reported by AimSnapshot. Duplicate samples are ignored,
// while a recovery clears the remembered value so the same error can be
// reported again if it returns later.
class AiAimRuntimeErrorTracker final {
public:
    [[nodiscard]] AiAimRuntimeErrorTransition observe(
        std::string error,
        const std::uint64_t session_revision = 0) {
        if (session_revision_ != session_revision) {
            active_error_.clear();
            session_revision_ = session_revision;
        }
        if (error == active_error_) {
            return {};
        }
        const bool recovered = !active_error_.empty() && error.empty();
        active_error_ = std::move(error);
        return {true, recovered, active_error_};
    }

    void reset() noexcept {
        active_error_.clear();
        session_revision_ = 0;
    }

private:
    std::string active_error_;
    std::uint64_t session_revision_ = 0;
};

} // namespace pubg
