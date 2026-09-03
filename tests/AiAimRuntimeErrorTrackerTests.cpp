#include "AiAimRuntimeErrorTracker.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testErrorRecoveryAndRepeatedErrorEdges() {
    pubg::AiAimRuntimeErrorTracker tracker;

    const auto first = tracker.observe("inference failed");
    require(first.changed && !first.recovered &&
            first.active_error == "inference failed",
            "first runtime error was not reported");

    const auto duplicate = tracker.observe("inference failed");
    require(!duplicate.changed, "duplicate runtime error was reported twice");

    const auto recovered = tracker.observe({});
    require(recovered.changed && recovered.recovered &&
            recovered.active_error.empty(),
            "runtime recovery edge was not reported");

    const auto repeated = tracker.observe("inference failed");
    require(repeated.changed && !repeated.recovered,
            "same runtime error was not reported after recovery");
}

void testResetStartsAFreshObservationSession() {
    pubg::AiAimRuntimeErrorTracker tracker;
    (void)tracker.observe("capture failed");
    tracker.reset();
    const auto after_reset = tracker.observe("capture failed");
    require(after_reset.changed && !after_reset.recovered,
            "reset retained an error from the previous module session");
}

void testNewSessionReportsTheSameErrorAgain() {
    pubg::AiAimRuntimeErrorTracker tracker;
    const auto first = tracker.observe("capture failed", 4);
    const auto next_session = tracker.observe("capture failed", 5);
    require(first.changed && next_session.changed,
            "a new module session inherited the previous error edge");
}

void testRuntimeSampleMustBelongToTheActiveSession() {
    require(pubg::shouldAcceptAiAimRuntimeSample(true, 7, 7),
            "the active session's runtime sample was rejected");
    require(!pubg::shouldAcceptAiAimRuntimeSample(false, 7, 7),
            "a runtime sample was accepted after the module was disabled");
    require(!pubg::shouldAcceptAiAimRuntimeSample(true, 7, 8),
            "a stale runtime sample was accepted by a newer session");
}

} // namespace

int main() {
    try {
        testErrorRecoveryAndRepeatedErrorEdges();
        testResetStartsAFreshObservationSession();
        testNewSessionReportsTheSameErrorAgain();
        testRuntimeSampleMustBelongToTheActiveSession();
        std::cout << "AI aim runtime error tracker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "AI aim runtime error tracker tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
