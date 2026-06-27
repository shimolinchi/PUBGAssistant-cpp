#pragma once

#include "Common.hpp"

namespace pubg {

// 一个 HUD 绘制命令。OverlayWindow 每次重绘时按命令列表画线、框、圆和文字。
// 这样各业务模块不直接操作 Win32 GDI，只生成抽象绘制指令。
struct OverlayCommand {
    enum class Type { Line, Rect, RoundedRect, Circle, Text, TextCenter, Clear } type = Type::Clear;
    double x1 = 0;
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
    double radius = 0;
    std::string text;
    cv::Scalar bgr{255, 255, 255};
    int width = 2;
    int font_size = 18;
    int alpha = 255;
};

// Win32 透明置顶 HUD 窗口，对应 Python 版多个 Tkinter Toplevel overlay。
// 当前使用黑色 colorkey 透明，支持点击穿透和基础 GDI 绘制。
class OverlayWindow {
public:
    // 构造时不创建窗口，create 成功后才有 hwnd。
    OverlayWindow();

    // 析构时销毁 Win32 窗口。
    ~OverlayWindow();

    // 创建全屏或指定尺寸 overlay。click_through=true 时鼠标事件穿透到游戏窗口。
    bool create(const std::wstring& title, int width, int height, bool click_through = true);
    bool create(const std::wstring& title, int width, int height, bool click_through,
                bool exclude_from_capture);
    bool createAt(const std::wstring& title, int left, int top, int width, int height,
                  bool click_through = true, bool exclude_from_capture = true);

    // 显示或隐藏窗口。
    void show(bool visible);

    // 替换当前绘制命令并触发重绘。
    void setCommands(std::vector<OverlayCommand> commands);

    // 清空 HUD 内容。
    void clear();

    // 主动关闭底层 Win32 窗口。退出时先调用它，避免后台线程残留的重绘消息继续访问旧句柄。
    void close();

    // 处理本线程窗口消息。识别/HUD 线程需要周期调用，否则窗口不刷新。
    void pumpMessages();

    // 窗口是否已经成功创建。
    [[nodiscard]] bool created() const noexcept;

private:
#ifdef _WIN32
    // Win32 窗口过程，转发 WM_PAINT 到 paint()。
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // 根据 commands_ 执行实际 GDI 绘制。
    void paint();
    void renderLayered();
    void requestRender();
    bool isOwnerThread() const;
    HWND hwnd_ = nullptr;
    DWORD owner_thread_id_ = 0;
    std::atomic_bool destroying_{false};
#endif
    std::mutex mutex_;
    std::vector<OverlayCommand> commands_;
    int width_ = 0;
    int height_ = 0;
    int left_ = 0;
    int top_ = 0;
};

} // namespace pubg
