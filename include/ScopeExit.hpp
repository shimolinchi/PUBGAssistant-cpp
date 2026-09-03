#pragma once

#include <type_traits>
#include <utility>

namespace pubg {

// Executes a small, non-allocating cleanup action when the current scope exits.
// Cleanup functions must not throw: a throwing destructor cannot safely continue
// stack unwinding or object teardown.
template <typename Function>
class ScopeExit final {
public:
    explicit ScopeExit(Function function) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_(std::move(function)) {}

    ~ScopeExit() noexcept {
        if (active_) {
            function_();
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    void dismiss() noexcept { active_ = false; }

private:
    Function function_;
    bool active_ = true;
};

template <typename Function>
ScopeExit(Function) -> ScopeExit<std::decay_t<Function>>;

} // namespace pubg
