#include "MouseMotionDispatcher.hpp"

#include "InputController.hpp"

#include <mutex>

namespace pubg {
namespace {

class InputControllerMouseBackend final : public IMouseMotionBackend {
public:
    bool moveRelative(int dx, int dy, std::uint32_t& error_code) noexcept override {
        unsigned long native_error = 0;
        const bool moved = InputController::tryMoveMouseRelative(dx, dy, &native_error);
        error_code = static_cast<std::uint32_t>(native_error);
        return moved;
    }
};

} // namespace

std::shared_ptr<IMouseMotionBackend> makeInputControllerMouseBackend() {
    return std::make_shared<InputControllerMouseBackend>();
}

MouseMotionDispatcher::MouseMotionDispatcher()
    : MouseMotionDispatcher(makeInputControllerMouseBackend()) {}

std::shared_ptr<MouseMotionDispatcher> makeMouseMotionDispatcher() {
    return std::make_shared<MouseMotionDispatcher>();
}

std::shared_ptr<MouseMotionDispatcher> sharedDefaultMouseMotionDispatcher() {
    static std::mutex default_mutex;
    static std::weak_ptr<MouseMotionDispatcher> default_dispatcher;
    std::lock_guard lock(default_mutex);
    if (std::shared_ptr<MouseMotionDispatcher> existing = default_dispatcher.lock()) {
        return existing;
    }
    std::shared_ptr<MouseMotionDispatcher> created = makeMouseMotionDispatcher();
    default_dispatcher = created;
    return created;
}

} // namespace pubg
