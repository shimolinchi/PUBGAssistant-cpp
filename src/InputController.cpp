#include "InputController.hpp"

namespace pubg {

void InputController::moveMouseRelative(int dx, int dy) {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
#else
    (void)dx;
    (void)dy;
#endif
}

void InputController::keyDown(int vk) {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    SendInput(1, &input, sizeof(INPUT));
#else
    (void)vk;
#endif
}

void InputController::keyUp(int vk) {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
#else
    (void)vk;
#endif
}

void InputController::mouseLeftDown() {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
#endif
}

void InputController::mouseLeftUp() {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
#endif
}

void InputController::mouseRightDown() {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &input, sizeof(INPUT));
#endif
}

void InputController::mouseRightUp() {
#ifdef _WIN32
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &input, sizeof(INPUT));
#endif
}

void InputController::pressVirtualKey(int vk) {
    if (vk == VK_LBUTTON) {
        mouseLeftDown();
    } else if (vk == VK_RBUTTON) {
        mouseRightDown();
    } else {
        keyDown(vk);
    }
}

void InputController::releaseVirtualKey(int vk) {
    if (vk == VK_LBUTTON) {
        mouseLeftUp();
    } else if (vk == VK_RBUTTON) {
        mouseRightUp();
    } else {
        keyUp(vk);
    }
}

void InputController::mouseWheel(int notches) {
#ifdef _WIN32
    if (notches == 0) {
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(notches * WHEEL_DELTA));
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &input, sizeof(INPUT));
#else
    (void)notches;
#endif
}

bool InputController::isKeyDown(int vk) {
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)vk;
    return false;
#endif
}

bool InputController::isLeftMouseDown() {
#ifdef _WIN32
    return isKeyDown(VK_LBUTTON);
#else
    return false;
#endif
}

std::pair<int, int> InputController::cursorPosition() {
#ifdef _WIN32
    POINT pt{};
    if (GetCursorPos(&pt)) {
        return {static_cast<int>(pt.x), static_cast<int>(pt.y)};
    }
#endif
    return {0, 0};
}

int InputController::parseVirtualKey(const std::string& key) {
    std::string k;
    for (char c : key) {
        if (c != '<' && c != '>') {
            k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (k == "end") return VK_END;
    if (k == "delete" || k == "del") return VK_DELETE;
    if (k == "home") return VK_HOME;
    if (k == "tab") return VK_TAB;
    if (k == "enter" || k == "return") return VK_RETURN;
    if (k == "space") return VK_SPACE;
    if (k == "alt" || k == "alt_l" || k == "alt_r" || k == "menu") return VK_MENU;
    if (k == "ctrl" || k == "control" || k == "ctrl_l" || k == "ctrl_r") return VK_CONTROL;
    if (k == "shift" || k == "shift_l" || k == "shift_r") return VK_SHIFT;
    if (k == "left") return VK_LEFT;
    if (k == "up") return VK_UP;
    if (k == "right") return VK_RIGHT;
    if (k == "down") return VK_DOWN;
    if (k == "mouse_left" || k == "lbutton") return VK_LBUTTON;
    if (k == "mouse_right" || k == "rbutton") return VK_RBUTTON;
    if (k == "mouse_middle" || k == "mbutton") return VK_MBUTTON;
    if (k == "f1") return VK_F1;
    if (k == "f2") return VK_F2;
    if (k == "f3") return VK_F3;
    if (k == "f4") return VK_F4;
    if (k == "f5") return VK_F5;
    if (k == "f6") return VK_F6;
    if (k == "f7") return VK_F7;
    if (k == "f8") return VK_F8;
    if (k == "f9") return VK_F9;
    if (k == "f10") return VK_F10;
    if (k == "f11") return VK_F11;
    if (k == "f12") return VK_F12;
    if (k.size() == 1) {
    #ifdef _WIN32
        SHORT vk = VkKeyScanA(k[0]);
        return vk == -1 ? 0 : (vk & 0xff);
    #else
        if (k[0] >= 'a' && k[0] <= 'z') return static_cast<int>(std::toupper(static_cast<unsigned char>(k[0])));
        if (k[0] >= '0' && k[0] <= '9') return static_cast<int>(k[0]);
    #endif
    }
    return 0;
}

} // namespace pubg
