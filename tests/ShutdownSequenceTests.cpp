#include "ShutdownSequence.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testSuccessAllowsRelease() {
    pubg::ShutdownSequence shutdown;
    int stopped = 0;
    shutdown.runMandatory([&] { ++stopped; });
    shutdown.runMandatory([&] { ++stopped; });

    require(shutdown.succeeded(), "all successful mandatory steps must prove quiescence");
    require(stopped == 2, "all successful mandatory steps must run");
}

void testFailureIsStickyAndIndependentStopsContinue() {
    pubg::ShutdownSequence shutdown;
    std::vector<std::string> events;
    shutdown.runMandatory([&] { events.emplace_back("hotkeys"); });
    shutdown.runMandatory([&] {
        events.emplace_back("detector-failed");
        throw std::runtime_error("simulated join failure");
    });
    shutdown.runMandatory([&] { events.emplace_back("dispatcher"); });

    require(!shutdown.succeeded(), "a mandatory failure must permanently reject resource release");
    require(events == std::vector<std::string>{
        "hotkeys", "detector-failed", "dispatcher"},
        "independent mandatory stop attempts must continue after one failure");
}

void testLifecycleSeparatesStoppingFromStopped() {
    pubg::ShutdownLifecycle lifecycle;
    require(lifecycle.phase() == pubg::ShutdownLifecycle::Phase::Running,
            "shutdown lifecycle must start in Running");
    require(lifecycle.begin(), "the owner must be able to begin shutdown once");
    require(lifecycle.phase() == pubg::ShutdownLifecycle::Phase::Stopping,
            "begin must publish Stopping before mandatory actions run");
    require(!lifecycle.stopped(), "Stopping must never authorize resource release");
    require(!lifecycle.begin(), "re-entrant shutdown must not start a second sequence");

    lifecycle.markStopped();
    require(lifecycle.stopped(), "successful mandatory quiescence must publish Stopped");
    require(!lifecycle.begin(), "completed shutdown must remain idempotent");
}

} // namespace

int main() {
    try {
        testSuccessAllowsRelease();
        testFailureIsStickyAndIndependentStopsContinue();
        testLifecycleSeparatesStoppingFromStopped();
        std::cout << "ShutdownSequence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ShutdownSequence tests failed: " << error.what() << '\n';
        return 1;
    }
}
