#include "LatestRenderPublication.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class PromiseSignal final {
public:
    explicit PromiseSignal(std::promise<void>& promise) noexcept
        : promise_(promise) {}

    ~PromiseSignal() { signal(); }

    PromiseSignal(const PromiseSignal&) = delete;
    PromiseSignal& operator=(const PromiseSignal&) = delete;

    void signal() {
        if (!signaled_) {
            promise_.set_value();
            signaled_ = true;
        }
    }

private:
    std::promise<void>& promise_;
    bool signaled_ = false;
};

void testOlderSnapshotCannotOverwriteLatestPublication() {
    pubg::LatestRenderPublication publication;
    const auto old_revision = publication.issue();
    const auto latest_revision = publication.issue();
    std::string visible_frame;

    require(publication.publishIfLatest(latest_revision, [&] {
        visible_frame = "cleared";
    }), "latest render snapshot was rejected");
    require(!publication.publishIfLatest(old_revision, [&] {
        visible_frame = "stale AI error";
    }), "older render snapshot was published after the latest snapshot");
    require(visible_frame == "cleared",
            "older render snapshot overwrote the cleared HUD frame");
}

void testNewRevisionWinsWhenIssuedDuringPublication() {
    pubg::LatestRenderPublication publication;
    const auto old_revision = publication.issue();
    std::promise<void> old_publisher_entered;
    std::promise<void> release_old_publisher;
    auto release_old = release_old_publisher.get_future().share();
    std::string visible_frame;

    auto old_publish = std::async(std::launch::async, [&] {
        return publication.publishIfLatest(old_revision, [&] {
            old_publisher_entered.set_value();
            release_old.wait();
            visible_frame = "old";
        });
    });
    PromiseSignal release_old_on_exit(release_old_publisher);
    require(old_publisher_entered.get_future().wait_for(2s) ==
                std::future_status::ready,
            "old publisher did not enter its publication section");

    const auto latest_revision = publication.issue();
    auto latest_publish = std::async(std::launch::async, [&] {
        return publication.publishIfLatest(latest_revision, [&] {
            visible_frame = "latest";
        });
    });
    require(latest_publish.wait_for(30ms) == std::future_status::timeout,
            "latest publisher bypassed the active publication section");

    release_old_on_exit.signal();
    require(old_publish.wait_for(2s) == std::future_status::ready &&
                old_publish.get(),
            "active old publisher did not finish normally");
    require(latest_publish.wait_for(2s) == std::future_status::ready &&
                latest_publish.get(),
            "latest publisher did not run after the active publication");
    require(visible_frame == "latest",
            "an in-flight old publication remained visible after a newer revision");
}

} // namespace

int main() {
    try {
        testOlderSnapshotCannotOverwriteLatestPublication();
        testNewRevisionWinsWhenIssuedDuringPublication();
        std::cout << "Latest render publication tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Latest render publication tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
