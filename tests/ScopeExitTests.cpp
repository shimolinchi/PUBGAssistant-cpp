#include "ScopeExit.hpp"

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

struct RecordedWindow final {
    explicit RecordedWindow(std::vector<std::string>& events)
        : events(events) {}

    ~RecordedWindow() { events.emplace_back("destroy-window"); }

    std::vector<std::string>& events;
};

void exerciseWindowLifetime(bool throw_during_run) {
    std::vector<std::string> events;
    try {
        RecordedWindow window(events);
        bool bound = true;
        auto lifetime = pubg::ScopeExit([&]() noexcept {
            events.emplace_back("stop-producers");
            bound = false;
            events.emplace_back("clear-window-binding");
        });
        if (throw_during_run) {
            throw std::runtime_error("simulated startup failure");
        }
        require(bound, "window binding must remain active while the run scope is active");
    } catch (const std::runtime_error&) {
    }

    require(events == std::vector<std::string>{
        "stop-producers", "clear-window-binding", "destroy-window"},
        "runtime producers and binding must be cleared before window destruction");
}

void testDismiss() {
    bool called = false;
    {
        auto guard = pubg::ScopeExit([&]() noexcept { called = true; });
        guard.dismiss();
    }
    require(!called, "dismissed scope cleanup must not run");
}

} // namespace

int main() {
    try {
        exerciseWindowLifetime(false);
        exerciseWindowLifetime(true);
        testDismiss();
        std::cout << "ScopeExit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ScopeExit tests failed: " << error.what() << '\n';
        return 1;
    }
}
