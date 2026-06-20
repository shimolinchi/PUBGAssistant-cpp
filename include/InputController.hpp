#pragma once

#include "Common.hpp"

namespace pubg {

// Windows 输入控制封装，对应 Python 版 pynput + ctypes.mouse_event 的职责。
// 当前实现使用 SendInput 和 GetAsyncKeyState，便于先验证压枪与热键行为。
class InputController {
public:
    // 相对移动鼠标。dx/dy 单位是 Windows 鼠标事件单位，压枪主要使用 dy。
    static void moveMouseRelative(int dx, int dy);

    // 按下一个虚拟键码，例如 VK_END。
    static void keyDown(int vk);

    // 松开一个虚拟键码。
    static void keyUp(int vk);

    // 按下鼠标左键。投掷物自动捏雷/释放用鼠标事件而不是键盘 VK。
    static void mouseLeftDown();

    // 松开鼠标左键。
    static void mouseLeftUp();

    // 按下鼠标右键。SG 闪身喷腰射瞄准使用。
    static void mouseRightDown();

    // 松开鼠标右键。
    static void mouseRightUp();

    // 按下/松开任意虚拟键，鼠标键会自动走 mouse event。
    static void pressVirtualKey(int vk);
    static void releaseVirtualKey(int vk);

    // 滚动鼠标滚轮。positive 表示向上，negative 表示向下；notches 是滚动格数。
    static void mouseWheel(int notches);

    // 查询某个虚拟键当前是否处于按下状态。
    static bool isKeyDown(int vk);

    // 查询鼠标左键物理状态。压枪循环用它判断是否正在开火。
    static bool isLeftMouseDown();

    // 读取当前鼠标位置，返回屏幕物理像素坐标。F4 大地图一次测距点击使用。
    static std::pair<int, int> cursorPosition();

    // 将配置里的字符串按键转成 Win32 虚拟键码。
    // 支持 end/delete/home/tab/space/alt/ctrl/shift/f1-f12/mouse 按键和单字符键。
    static int parseVirtualKey(const std::string& key);
};

} // namespace pubg
