#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace pubg {

// Coordinates concurrently built render snapshots. Call issue() before taking
// a snapshot, then publishIfLatest() after all source locks have been released.
// An older snapshot can never overwrite a newer published frame.
class LatestRenderPublication final {
public:
    using Revision = std::uint64_t;

    [[nodiscard]] Revision issue() noexcept {
        return requested_revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    template <typename Publisher>
    bool publishIfLatest(Revision revision, Publisher&& publisher) {
        std::lock_guard lock(publish_mutex_);
        if (revision != requested_revision_.load(std::memory_order_acquire)) {
            return false;
        }
        std::invoke(std::forward<Publisher>(publisher));
        return true;
    }

private:
    std::atomic<Revision> requested_revision_{0};
    std::mutex publish_mutex_;
};

} // namespace pubg
