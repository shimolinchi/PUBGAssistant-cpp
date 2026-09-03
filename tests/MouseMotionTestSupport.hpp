#pragma once

#include "MouseMotionDispatcher.hpp"
#include "ScopeExit.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace pubg::test {

using pubg::ScopeExit;

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
bool waitUntil(
    Predicate&& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = MotionClock::now() + timeout;
    while (!predicate() && MotionClock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

class RecordingMouseBackend final : public IMouseMotionBackend {
public:
    bool moveRelative(int dx, int dy, std::uint32_t& error_code) noexcept override {
        std::unique_lock lock(mutex_);
        calls_.emplace_back(dx, dy);
        cv_.notify_all();
        cv_.wait(lock, [&] { return !blocked_; });
        if (fail_next_) {
            fail_next_ = false;
            error_code = 1234;
            return false;
        }
        error_code = 0;
        return true;
    }

    void setBlocked(bool blocked) {
        {
            std::lock_guard lock(mutex_);
            blocked_ = blocked;
        }
        cv_.notify_all();
    }

    bool waitForCalls(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return calls_.size() >= count; });
    }

    std::size_t callCount() const {
        std::lock_guard lock(mutex_);
        return calls_.size();
    }

    std::vector<std::pair<int, int>> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    void failNext() {
        std::lock_guard lock(mutex_);
        fail_next_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::pair<int, int>> calls_;
    bool blocked_ = false;
    bool fail_next_ = false;
};

} // namespace pubg::test
