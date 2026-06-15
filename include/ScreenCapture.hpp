#pragma once

#include "Common.hpp"

namespace pubg {

// 屏幕截图封装。Windows 上使用 GDI BitBlt + CAPTUREBLT 截取指定物理像素区域。
// 对应 Python 版各模块中的 mss.grab(rect)。
class ScreenCapture {
public:
    // 获取桌面 DC。一个 ScreenCapture 对象可以在同一线程循环里重复截图。
    ScreenCapture();

    // 释放桌面 DC。
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // 截取 rect 区域并返回 BGR 格式 cv::Mat。
    // 返回空 Mat 表示区域无效、非 Windows 环境或截图失败。
    [[nodiscard]] cv::Mat grabBgr(const Rect& rect) const;

private:
#ifdef _WIN32
    HDC screen_dc_ = nullptr;
#endif
};

} // namespace pubg
