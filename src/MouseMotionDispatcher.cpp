#include "MouseMotionDispatcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace pubg {
namespace {

constexpr std::size_t kSourceCount = static_cast<std::size_t>(MotionSource::Count);

constexpr std::size_t sourceIndex(MotionSource source) noexcept {
    return static_cast<std::size_t>(source);
}

constexpr bool validSource(MotionSource source) noexcept {
    return sourceIndex(source) < kSourceCount;
}

constexpr std::uint32_t sourceBit(MotionSource source) noexcept {
    return std::uint32_t{1} << static_cast<unsigned>(source);
}

int roundedAndClamped(double value) noexcept {
    constexpr int lower = std::numeric_limits<int>::min();
    constexpr int upper = std::numeric_limits<int>::max();
    if (!std::isfinite(value)) {
        return std::isnan(value) ? 0 : (value < 0.0 ? lower : upper);
    }
    if (value <= static_cast<double>(lower)) {
        return lower;
    }
    if (value >= static_cast<double>(upper)) {
        return upper;
    }
    return static_cast<int>(std::llround(value));
}

int clampedToInt(long long value) noexcept {
    constexpr int lower = std::numeric_limits<int>::min();
    constexpr int upper = std::numeric_limits<int>::max();
    if (value <= static_cast<long long>(lower)) {
        return lower;
    }
    if (value >= static_cast<long long>(upper)) {
        return upper;
    }
    return static_cast<int>(value);
}

} // namespace

namespace detail {

class MouseMotionDispatcherState final {
public:
    explicit MouseMotionDispatcherState(std::shared_ptr<IMouseMotionBackend> backend)
        : backend_(std::move(backend)) {
        if (!backend_) {
            throw std::invalid_argument("mouse-motion backend must not be null");
        }
        snapshot_.running = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~MouseMotionDispatcherState() { shutdown(); }

    void configure(MotionSource source, MotionPolicy policy) noexcept {
        std::lock_guard lock(mutex_);
        sources_[sourceIndex(source)].policy = policy;
    }

    void open(MotionSource source) noexcept {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        SourceState& state = sources_[sourceIndex(source)];
        ++state.epoch;
        state.open = true;
        state.pending = {};
        state.residual_x = 0.0;
        state.residual_y = 0.0;
        state.last_sequence = 0;
        cv_.notify_all();
    }

    void closeAndFence(MotionSource source) noexcept {
        std::unique_lock lock(mutex_);
        SourceState& state = sources_[sourceIndex(source)];
        const std::uint64_t closing_epoch = state.epoch;
        ++state.epoch;
        state.open = false;
        state.pending = {};
        state.residual_x = 0.0;
        state.residual_y = 0.0;
        cv_.notify_all();
        const std::uint32_t bit = sourceBit(source);
        const std::size_t index = sourceIndex(source);
        fence_cv_.wait(lock, [&] {
            // A concurrent open() starts a strictly newer epoch. Its work is not
            // part of this fence and must neither delay nor be reset by it.
            return (in_flight_sources_ & bit) == 0 ||
                   in_flight_epochs_[index] > closing_epoch;
        });
    }

    bool isOpen(MotionSource source) const noexcept {
        std::lock_guard lock(mutex_);
        return !stopping_ && sources_[sourceIndex(source)].open;
    }

    bool submit(MotionSource source, const MotionRequest& request) noexcept {
        if (!std::isfinite(request.dx) || !std::isfinite(request.dy) ||
            request.max_age < std::chrono::milliseconds::zero()) {
            return false;
        }

        std::lock_guard lock(mutex_);
        SourceState& state = sources_[sourceIndex(source)];
        if (stopping_ || !state.open ||
            (exclusive_source_.has_value() && *exclusive_source_ != source)) {
            ++snapshot_.dropped_closed;
            return false;
        }
        if (request.sequence != 0 && request.sequence <= state.last_sequence) {
            ++snapshot_.dropped_out_of_order;
            return false;
        }
        if (request.sequence != 0) {
            state.last_sequence = request.sequence;
        }

        if (state.policy == MotionPolicy::Latest) {
            if (state.pending.present) {
                ++snapshot_.replaced_latest;
            }
            state.pending = Pending::from(request, state.epoch);
        } else if (state.pending.present) {
            state.pending.dx += request.dx;
            state.pending.dy += request.dy;
            state.pending.sequence = request.sequence;
            state.pending.produced_at = std::min(state.pending.produced_at, request.produced_at);
            state.pending.max_age = std::min(state.pending.max_age, request.max_age);
        } else {
            state.pending = Pending::from(request, state.epoch);
        }
        ++snapshot_.submitted;
        cv_.notify_one();
        return true;
    }

    std::uint64_t acquireExclusive(MotionSource source) noexcept {
        std::unique_lock lock(mutex_);
        exclusive_cv_.wait(lock, [&] { return stopping_ || !exclusive_source_.has_value(); });
        if (stopping_) {
            return 0;
        }

        exclusive_source_ = source;
        const std::uint32_t allowed = sourceBit(source);
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            if (index == sourceIndex(source)) {
                continue;
            }
            sources_[index].pending = {};
        }
        cv_.notify_all();
        fence_cv_.wait(lock, [&] { return (in_flight_sources_ & ~allowed) == 0; });
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            if (index == sourceIndex(source)) {
                continue;
            }
            sources_[index].residual_x = 0.0;
            sources_[index].residual_y = 0.0;
        }
        return ++last_lease_id_;
    }

    void releaseExclusive(MotionSource source, std::uint64_t lease_id) noexcept {
        std::lock_guard lock(mutex_);
        if (!exclusive_source_.has_value() || *exclusive_source_ != source ||
            lease_id == 0 || lease_id != last_lease_id_) {
            return;
        }
        exclusive_source_.reset();
        exclusive_cv_.notify_all();
        cv_.notify_all();
    }

    MouseMotionDispatcherSnapshot snapshot() const noexcept {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept {
        {
            std::unique_lock lock(mutex_);
            if (!stopping_) {
                stopping_ = true;
                snapshot_.running = false;
                exclusive_source_.reset();
                for (SourceState& source : sources_) {
                    source.open = false;
                    ++source.epoch;
                    source.pending = {};
                    source.residual_x = 0.0;
                    source.residual_y = 0.0;
                }
            }
            cv_.notify_all();
            exclusive_cv_.notify_all();
            fence_cv_.wait(lock, [&] { return in_flight_sources_ == 0; });
        }
        std::lock_guard join_lock(join_mutex_);
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

private:
    struct Pending {
        bool present = false;
        double dx = 0.0;
        double dy = 0.0;
        std::uint64_t sequence = 0;
        std::uint64_t epoch = 0;
        MotionClock::time_point produced_at{};
        std::chrono::milliseconds max_age{0};

        static Pending from(const MotionRequest& request, std::uint64_t epoch) noexcept {
            return {
                true,
                request.dx,
                request.dy,
                request.sequence,
                epoch,
                request.produced_at,
                request.max_age,
            };
        }
    };

    struct SourceState {
        bool open = false;
        MotionPolicy policy = MotionPolicy::Latest;
        std::uint64_t epoch = 0;
        std::uint64_t last_sequence = 0;
        Pending pending{};
        double residual_x = 0.0;
        double residual_y = 0.0;
    };

    bool hasPendingLocked() const noexcept {
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            const MotionSource source = static_cast<MotionSource>(index);
            if (sources_[index].pending.present &&
                (!exclusive_source_.has_value() || *exclusive_source_ == source)) {
                return true;
            }
        }
        return false;
    }

    void workerLoop() noexcept {
        std::unique_lock lock(mutex_);
        for (;;) {
            cv_.wait(lock, [&] { return stopping_ || hasPendingLocked(); });
            if (stopping_) {
                break;
            }

            const MotionClock::time_point now = MotionClock::now();
            long long combined_x = 0;
            long long combined_y = 0;
            std::uint32_t batch_sources = 0;

            for (std::size_t index = 0; index < sources_.size(); ++index) {
                const MotionSource source = static_cast<MotionSource>(index);
                if (exclusive_source_.has_value() && *exclusive_source_ != source) {
                    continue;
                }
                SourceState& state = sources_[index];
                if (!state.pending.present) {
                    continue;
                }
                Pending pending = state.pending;
                state.pending = {};

                if (!state.open || pending.epoch != state.epoch) {
                    ++snapshot_.dropped_closed;
                    continue;
                }
                if (pending.produced_at == MotionClock::time_point{} ||
                    now - pending.produced_at > pending.max_age) {
                    state.residual_x = 0.0;
                    state.residual_y = 0.0;
                    ++snapshot_.dropped_stale;
                    continue;
                }

                const double source_x = pending.dx + state.residual_x;
                const double source_y = pending.dy + state.residual_y;
                const int rounded_x = roundedAndClamped(source_x);
                const int rounded_y = roundedAndClamped(source_y);
                state.residual_x = source_x - static_cast<double>(rounded_x);
                state.residual_y = source_y - static_cast<double>(rounded_y);
                combined_x += rounded_x;
                combined_y += rounded_y;
                batch_sources |= sourceBit(source);
                in_flight_epochs_[index] = pending.epoch;
            }

            if (batch_sources == 0 || (combined_x == 0 && combined_y == 0)) {
                continue;
            }

            const int output_x = clampedToInt(combined_x);
            const int output_y = clampedToInt(combined_y);
            in_flight_sources_ = batch_sources;
            const std::shared_ptr<IMouseMotionBackend> backend = backend_;
            lock.unlock();

            std::uint32_t error_code = 0;
            const bool sent = backend && backend->moveRelative(output_x, output_y, error_code);

            lock.lock();
            in_flight_sources_ = 0;
            in_flight_epochs_.fill(0);
            if (sent) {
                ++snapshot_.sent_batches;
                snapshot_.last_error_code = 0;
            } else {
                ++snapshot_.backend_failures;
                snapshot_.last_error_code = error_code;
            }
            fence_cv_.notify_all();
        }
        in_flight_sources_ = 0;
        in_flight_epochs_.fill(0);
        fence_cv_.notify_all();
    }

    mutable std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable cv_;
    std::condition_variable fence_cv_;
    std::condition_variable exclusive_cv_;
    bool stopping_ = false;
    std::array<SourceState, kSourceCount> sources_{};
    std::optional<MotionSource> exclusive_source_;
    std::uint64_t last_lease_id_ = 0;
    std::uint32_t in_flight_sources_ = 0;
    std::array<std::uint64_t, kSourceCount> in_flight_epochs_{};
    std::shared_ptr<IMouseMotionBackend> backend_;
    MouseMotionDispatcherSnapshot snapshot_{};
    std::thread worker_;
};

} // namespace detail

MouseMotionChannel::MouseMotionChannel(
    std::shared_ptr<detail::MouseMotionDispatcherState> state,
    MotionSource source) noexcept
    : state_(std::move(state)), source_(source) {}

MouseMotionChannel::~MouseMotionChannel() { closeAndFence(); }

void MouseMotionChannel::open() noexcept {
    if (state_) {
        state_->open(source_);
    }
}

void MouseMotionChannel::closeAndFence() noexcept {
    if (state_) {
        state_->closeAndFence(source_);
    }
}

bool MouseMotionChannel::isOpen() const noexcept {
    return state_ && state_->isOpen(source_);
}

bool MouseMotionChannel::submit(const MotionRequest& request) noexcept {
    return state_ && state_->submit(source_, request);
}

bool MouseMotionChannel::submit(
    double dx,
    double dy,
    std::uint64_t sequence,
    MotionClock::time_point produced_at,
    std::chrono::milliseconds max_age) noexcept {
    return submit(MotionRequest{dx, dy, sequence, produced_at, max_age});
}

ExclusiveMotionLease::ExclusiveMotionLease(
    std::shared_ptr<detail::MouseMotionDispatcherState> state,
    MotionSource source,
    std::uint64_t lease_id) noexcept
    : state_(std::move(state)), source_(source), lease_id_(lease_id) {}

ExclusiveMotionLease::~ExclusiveMotionLease() { release(); }

ExclusiveMotionLease::ExclusiveMotionLease(ExclusiveMotionLease&& other) noexcept
    : state_(std::move(other.state_)), source_(other.source_), lease_id_(other.lease_id_) {
    other.lease_id_ = 0;
}

ExclusiveMotionLease& ExclusiveMotionLease::operator=(ExclusiveMotionLease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        source_ = other.source_;
        lease_id_ = other.lease_id_;
        other.lease_id_ = 0;
    }
    return *this;
}

ExclusiveMotionLease::operator bool() const noexcept {
    return state_ && lease_id_ != 0;
}

void ExclusiveMotionLease::release() noexcept {
    if (state_ && lease_id_ != 0) {
        state_->releaseExclusive(source_, lease_id_);
    }
    lease_id_ = 0;
    state_.reset();
}

MouseMotionDispatcher::MouseMotionDispatcher(std::shared_ptr<IMouseMotionBackend> backend)
    : state_(std::make_shared<detail::MouseMotionDispatcherState>(std::move(backend))) {}

MouseMotionDispatcher::~MouseMotionDispatcher() { shutdown(); }

std::shared_ptr<MouseMotionChannel> MouseMotionDispatcher::createChannel(
    MotionSource source,
    MotionPolicy policy) {
    if (!validSource(source)) {
        throw std::invalid_argument("invalid mouse-motion source");
    }
    if (policy != MotionPolicy::Latest && policy != MotionPolicy::Accumulate) {
        throw std::invalid_argument("invalid mouse-motion policy");
    }
    state_->configure(source, policy);
    return std::shared_ptr<MouseMotionChannel>(new MouseMotionChannel(state_, source));
}

ExclusiveMotionLease MouseMotionDispatcher::acquireExclusive(MotionSource source) noexcept {
    if (!state_ || !validSource(source)) {
        return {};
    }
    const std::uint64_t lease_id = state_->acquireExclusive(source);
    return ExclusiveMotionLease(state_, source, lease_id);
}

MouseMotionDispatcherSnapshot MouseMotionDispatcher::snapshot() const noexcept {
    return state_ ? state_->snapshot() : MouseMotionDispatcherSnapshot{};
}

void MouseMotionDispatcher::shutdown() noexcept {
    if (state_) {
        state_->shutdown();
    }
}

std::shared_ptr<MouseMotionDispatcher> makeMouseMotionDispatcher(
    std::shared_ptr<IMouseMotionBackend> backend) {
    return std::make_shared<MouseMotionDispatcher>(std::move(backend));
}

} // namespace pubg
