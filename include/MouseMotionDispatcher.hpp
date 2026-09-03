#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace pubg {

enum class MotionSource : std::uint8_t {
    AiAim = 0,
    Recoil,
    SrBreath,
    Mortar,
    Count,
};

enum class MotionPolicy : std::uint8_t {
    Latest,
    Accumulate,
};

using MotionClock = std::chrono::steady_clock;

struct MotionRequest {
    double dx = 0.0;
    double dy = 0.0;
    std::uint64_t sequence = 0;
    MotionClock::time_point produced_at = MotionClock::now();
    std::chrono::milliseconds max_age{50};
};

struct MouseMotionDispatcherSnapshot {
    bool running = false;
    std::uint64_t submitted = 0;
    std::uint64_t sent_batches = 0;
    std::uint64_t replaced_latest = 0;
    std::uint64_t dropped_closed = 0;
    std::uint64_t dropped_stale = 0;
    std::uint64_t dropped_out_of_order = 0;
    std::uint64_t backend_failures = 0;
    std::uint32_t last_error_code = 0;
};

class IMouseMotionBackend {
public:
    virtual ~IMouseMotionBackend() = default;

    // error_code is a Win32 error for the production backend and backend-defined
    // for test implementations. A successful call must set it to zero.
    virtual bool moveRelative(int dx, int dy, std::uint32_t& error_code) noexcept = 0;
};

namespace detail {
class MouseMotionDispatcherState;
}

class MouseMotionChannel final {
public:
    ~MouseMotionChannel();

    MouseMotionChannel(const MouseMotionChannel&) = delete;
    MouseMotionChannel& operator=(const MouseMotionChannel&) = delete;

    void open() noexcept;
    // Advances the source epoch, rejects/clears the closing epoch, and waits
    // until its backend call has finished. A concurrent later open() belongs to
    // a newer epoch and is deliberately outside this fence.
    void closeAndFence() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;

    [[nodiscard]] bool submit(const MotionRequest& request) noexcept;
    [[nodiscard]] bool submit(
        double dx,
        double dy,
        std::uint64_t sequence = 0,
        MotionClock::time_point produced_at = MotionClock::now(),
        std::chrono::milliseconds max_age = std::chrono::milliseconds{50}) noexcept;

private:
    friend class MouseMotionDispatcher;
    MouseMotionChannel(
        std::shared_ptr<detail::MouseMotionDispatcherState> state,
        MotionSource source) noexcept;

    std::shared_ptr<detail::MouseMotionDispatcherState> state_;
    MotionSource source_;
};

class ExclusiveMotionLease final {
public:
    ExclusiveMotionLease() noexcept = default;
    ~ExclusiveMotionLease();

    ExclusiveMotionLease(ExclusiveMotionLease&& other) noexcept;
    ExclusiveMotionLease& operator=(ExclusiveMotionLease&& other) noexcept;
    ExclusiveMotionLease(const ExclusiveMotionLease&) = delete;
    ExclusiveMotionLease& operator=(const ExclusiveMotionLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    void release() noexcept;

private:
    friend class MouseMotionDispatcher;
    ExclusiveMotionLease(
        std::shared_ptr<detail::MouseMotionDispatcherState> state,
        MotionSource source,
        std::uint64_t lease_id) noexcept;

    std::shared_ptr<detail::MouseMotionDispatcherState> state_;
    MotionSource source_ = MotionSource::AiAim;
    std::uint64_t lease_id_ = 0;
};

class MouseMotionDispatcher final {
public:
    MouseMotionDispatcher();
    explicit MouseMotionDispatcher(std::shared_ptr<IMouseMotionBackend> backend);
    ~MouseMotionDispatcher();

    MouseMotionDispatcher(const MouseMotionDispatcher&) = delete;
    MouseMotionDispatcher& operator=(const MouseMotionDispatcher&) = delete;

    [[nodiscard]] std::shared_ptr<MouseMotionChannel> createChannel(
        MotionSource source,
        MotionPolicy policy);
    [[nodiscard]] ExclusiveMotionLease acquireExclusive(MotionSource source) noexcept;
    [[nodiscard]] MouseMotionDispatcherSnapshot snapshot() const noexcept;

    // Rejects new work, discards every pending delta, waits for the backend call
    // currently in flight, and joins the single dispatch worker.
    void shutdown() noexcept;

private:
    std::shared_ptr<detail::MouseMotionDispatcherState> state_;
};

[[nodiscard]] std::shared_ptr<IMouseMotionBackend> makeInputControllerMouseBackend();
[[nodiscard]] std::shared_ptr<MouseMotionDispatcher> makeMouseMotionDispatcher();
[[nodiscard]] std::shared_ptr<MouseMotionDispatcher> makeMouseMotionDispatcher(
    std::shared_ptr<IMouseMotionBackend> backend);

// Compatibility path for modules constructed before App owns a dispatcher.
// The weak process-wide slot makes independently constructed Recoil and Mortar
// instances share one worker without extending its lifetime beyond its users.
[[nodiscard]] std::shared_ptr<MouseMotionDispatcher> sharedDefaultMouseMotionDispatcher();

} // namespace pubg
