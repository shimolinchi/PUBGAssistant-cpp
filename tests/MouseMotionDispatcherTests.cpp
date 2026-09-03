#include "MouseMotionDispatcher.hpp"
#include "MouseMotionTestSupport.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using pubg::MotionClock;
using pubg::MotionPolicy;
using pubg::MotionSource;
using RecordingBackend = pubg::test::RecordingMouseBackend;
using pubg::test::require;
using pubg::test::waitUntil;

void testLatestReplacesPending() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->setBlocked(true);
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    ai->open();

    require(ai->submit(1.0, 0.0, 1, MotionClock::now(), 5s), "first AI command rejected");
    require(backend->waitForCalls(1), "first AI command was not dispatched");
    require(ai->submit(2.0, 0.0, 2, MotionClock::now(), 5s), "second AI command rejected");
    require(ai->submit(3.0, 0.0, 3, MotionClock::now(), 5s), "third AI command rejected");
    require(ai->submit(4.0, 0.0, 4, MotionClock::now(), 5s), "fourth AI command rejected");

    backend->setBlocked(false);
    require(backend->waitForCalls(2), "latest AI command was not dispatched");
    ai->closeAndFence();
    const auto calls = backend->calls();
    require(calls.size() == 2, "latest policy emitted more than one pending AI command");
    require(calls[1] == std::pair<int, int>{4, 0}, "latest policy did not keep newest AI command");
    require(dispatcher->snapshot().replaced_latest >= 2, "latest replacement was not counted");
}

void testNullBackendIsRejected() {
    bool rejected = false;
    try {
        (void)pubg::makeMouseMotionDispatcher(std::shared_ptr<pubg::IMouseMotionBackend>{});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "dispatcher core accepted a null injected backend");
}

void testCloseAndFenceWaitsAndRejectsLateSubmit() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->setBlocked(true);
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    ai->open();
    require(ai->submit(5.0, 0.0, 1, MotionClock::now(), 5s),
            "AI command rejected before close test");
    require(backend->waitForCalls(1), "backend did not enter blocked call");

    auto close = std::async(std::launch::async, [&] { ai->closeAndFence(); });
    require(waitUntil([&] { return !ai->isOpen(); }),
            "closeAndFence did not close the channel");
    require(close.wait_for(0ms) == std::future_status::timeout,
            "closeAndFence returned while backend call was in flight");
    require(!ai->submit(9.0, 0.0, 2), "closed AI channel accepted a late command");

    backend->setBlocked(false);
    require(close.wait_for(2s) == std::future_status::ready, "closeAndFence did not finish");
    close.get();
    std::this_thread::sleep_for(20ms);
    require(backend->calls().size() == 1, "a command escaped after closeAndFence");
}

void testRecoilAndAiAreAdditive() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->setBlocked(true);
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    auto recoil = dispatcher->createChannel(MotionSource::Recoil, MotionPolicy::Accumulate);
    ai->open();
    recoil->open();

    require(ai->submit(1.0, 0.0, 1, MotionClock::now(), 5s), "priming AI command rejected");
    require(backend->waitForCalls(1), "priming AI command did not reach backend");
    require(ai->submit(2.0, 3.0, 2, MotionClock::now(), 5s), "pending AI command rejected");
    require(recoil->submit(0.0, 4.0, 1, MotionClock::now(), 5s), "first recoil command rejected");
    require(recoil->submit(0.0, 5.0, 2, MotionClock::now(), 5s), "second recoil command rejected");

    backend->setBlocked(false);
    require(backend->waitForCalls(2), "combined AI/recoil command was not emitted");
    ai->closeAndFence();
    recoil->closeAndFence();
    const auto calls = backend->calls();
    require(calls.size() == 2, "unexpected number of additive backend calls");
    require(calls[1] == std::pair<int, int>{2, 12}, "AI and recoil deltas were not added");
}

void testMortarExclusiveRejectsOtherSources() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    auto recoil = dispatcher->createChannel(MotionSource::Recoil, MotionPolicy::Accumulate);
    auto mortar = dispatcher->createChannel(MotionSource::Mortar, MotionPolicy::Latest);
    ai->open();
    recoil->open();
    mortar->open();

    {
        auto lease = dispatcher->acquireExclusive(MotionSource::Mortar);
        require(static_cast<bool>(lease), "failed to acquire Mortar exclusive lease");
        require(!ai->submit(30.0, 0.0, 1), "AI command accepted during Mortar exclusive lease");
        require(!recoil->submit(0.0, 30.0, 1), "recoil accepted during Mortar exclusive lease");
        require(mortar->submit(7.0, 0.0, 1), "Mortar command rejected during its lease");
        require(backend->waitForCalls(1), "Mortar command was not dispatched");
    }

    require(ai->submit(2.0, 0.0, 1), "AI did not resume after Mortar lease");
    require(backend->waitForCalls(2), "resumed AI command was not dispatched");
    ai->closeAndFence();
    recoil->closeAndFence();
    mortar->closeAndFence();
    const auto calls = backend->calls();
    require(calls[0] == std::pair<int, int>{7, 0}, "non-Mortar motion escaped exclusive lease");
    require(calls[1] == std::pair<int, int>{2, 0}, "AI did not resume cleanly after lease");
}

void testFiniteMotionClampsPerSourceAndBatch() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->setBlocked(true);
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    auto recoil = dispatcher->createChannel(MotionSource::Recoil, MotionPolicy::Accumulate);
    ai->open();
    recoil->open();

    require(ai->submit(1.0, 0.0, 1, MotionClock::now(), 5s),
            "priming command rejected before clamp test");
    require(backend->waitForCalls(1), "priming command did not reach clamp-test backend");

    const double above_max = static_cast<double>(std::numeric_limits<int>::max()) + 4096.0;
    const double below_min = static_cast<double>(std::numeric_limits<int>::min()) - 4096.0;
    require(ai->submit(above_max, below_min, 2, MotionClock::now(), 5s),
            "AI boundary command rejected");
    require(recoil->submit(above_max, below_min, 1, MotionClock::now(), 5s),
            "recoil boundary command rejected");

    backend->setBlocked(false);
    require(backend->waitForCalls(2), "clamped batch was not dispatched");
    ai->closeAndFence();
    recoil->closeAndFence();

    const auto calls = backend->calls();
    require(calls.size() == 2, "boundary inputs emitted an unexpected backend call count");
    require(calls[1] == std::pair<int, int>{
                            std::numeric_limits<int>::max(),
                            std::numeric_limits<int>::min()},
            "finite boundary inputs were not saturated to the int range");
}

void testStaleAndBackendFailureAreReported() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    ai->open();

    require(ai->submit(3.0, 0.0, 1, MotionClock::now() - 100ms, 10ms),
            "stale command was rejected before dispatcher accounting");
    const auto stale_deadline = MotionClock::now() + 2s;
    while (dispatcher->snapshot().dropped_stale == 0 && MotionClock::now() < stale_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(dispatcher->snapshot().dropped_stale == 1, "stale command was not dropped");

    backend->failNext();
    require(ai->submit(3.0, 0.0, 2), "backend-failure command rejected");
    require(backend->waitForCalls(1), "backend failure path was not called");
    ai->closeAndFence();
    const auto snapshot = dispatcher->snapshot();
    require(snapshot.backend_failures == 1, "backend failure was not counted");
    require(snapshot.last_error_code == 1234, "backend error code was not retained");
}

void testConcurrentShutdownIsIdempotent() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->setBlocked(true);
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    auto ai = dispatcher->createChannel(MotionSource::AiAim, MotionPolicy::Latest);
    ai->open();
    require(ai->submit(6.0, 0.0, 1, MotionClock::now(), 5s),
            "AI command rejected before concurrent shutdown test");
    require(backend->waitForCalls(1), "backend did not enter concurrent shutdown test");

    auto first = std::async(std::launch::async, [&] { dispatcher->shutdown(); });
    auto second = std::async(std::launch::async, [&] { dispatcher->shutdown(); });
    require(first.wait_for(30ms) == std::future_status::timeout,
            "first shutdown ignored an in-flight backend call");
    require(second.wait_for(30ms) == std::future_status::timeout,
            "second shutdown ignored an in-flight backend call");

    backend->setBlocked(false);
    require(first.wait_for(2s) == std::future_status::ready, "first shutdown did not finish");
    require(second.wait_for(2s) == std::future_status::ready, "second shutdown did not finish");
    first.get();
    second.get();
    require(!dispatcher->snapshot().running, "dispatcher remained running after shutdown");
}

} // namespace

int main() {
    try {
        testLatestReplacesPending();
        testNullBackendIsRejected();
        testCloseAndFenceWaitsAndRejectsLateSubmit();
        testRecoilAndAiAreAdditive();
        testMortarExclusiveRejectsOtherSources();
        testFiniteMotionClampsPerSourceAndBatch();
        testStaleAndBackendFailureAreReported();
        testConcurrentShutdownIsIdempotent();
        std::cout << "MouseMotionDispatcher tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MouseMotionDispatcher tests failed: " << error.what() << '\n';
        return 1;
    }
}
