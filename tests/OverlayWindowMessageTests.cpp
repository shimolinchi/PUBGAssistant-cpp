#include "OverlayWindow.hpp"
#include "ScopeExit.hpp"

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
namespace {

constexpr UINT kForeignMessage = WM_APP + 0x651;
int foreign_message_count = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

LRESULT CALLBACK foreignWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kForeignMessage) {
        ++foreign_message_count;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void dispatchForeignMessage(HWND foreign) {
    MSG message{};
    require(PeekMessageW(
                &message, foreign, kForeignMessage, kForeignMessage, PM_REMOVE) != FALSE,
            "foreign message was unexpectedly removed from the owner-thread queue");
    DispatchMessageW(&message);
}

void testPumpIsRestrictedToOverlayWindow() {
    const wchar_t* class_name = L"PUBGAssistantOverlayMessageIsolationTest";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = foreignWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    require(RegisterClassW(&window_class) != 0,
            "failed to register foreign test window class");
    auto unregister_class = pubg::ScopeExit([&]() noexcept {
        UnregisterClassW(class_name, GetModuleHandleW(nullptr));
    });

    HWND foreign = CreateWindowExW(
        0, class_name, L"foreign", WS_POPUP,
        0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(foreign != nullptr, "failed to create foreign test window");
    auto destroy_foreign = pubg::ScopeExit([&]() noexcept {
        if (IsWindow(foreign)) {
            DestroyWindow(foreign);
        }
    });

    pubg::OverlayWindow overlay;
    require(overlay.createAt(L"overlay-message-test", 0, 0, 16, 16, true, false),
            "failed to create overlay test window");
    overlay.show(false);

    foreign_message_count = 0;
    require(PostMessageW(foreign, kForeignMessage, 0, 0) != FALSE,
            "failed to post foreign test message");
    overlay.pumpMessages();
    require(foreign_message_count == 0,
            "overlay pump dispatched another window's callback");
    dispatchForeignMessage(foreign);
    require(foreign_message_count == 1,
            "foreign message did not dispatch through its own pump");

    overlay.close();
    require(PostMessageW(foreign, kForeignMessage, 0, 0) != FALSE,
            "failed to post foreign message after overlay close");
    overlay.pumpMessages();
    require(foreign_message_count == 1,
            "closed overlay pump dispatched the owner thread queue");
    dispatchForeignMessage(foreign);
    require(foreign_message_count == 2,
            "foreign message after overlay close was not preserved");
}

} // namespace
#endif

int main() {
#ifdef _WIN32
    try {
        testPumpIsRestrictedToOverlayWindow();
        std::cout << "OverlayWindow message tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OverlayWindow message tests failed: " << error.what() << '\n';
        return 1;
    }
#else
    std::cout << "OverlayWindow message tests skipped outside Windows\n";
    return 0;
#endif
}
