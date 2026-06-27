#include "OverlayWindow.hpp"

#ifdef _WIN32
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#endif

namespace pubg {

#ifdef _WIN32
namespace {
ULONG_PTR g_gdiplus_token = 0;
std::mutex g_gdiplus_mutex;
constexpr UINT kOverlayRenderMessage = WM_APP + 0x431;
constexpr UINT kOverlayShowMessage = WM_APP + 0x432;

void ensureGdiplus() {
    std::lock_guard lock(g_gdiplus_mutex);
    if (g_gdiplus_token) {
        return;
    }
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
}

Gdiplus::Color gdipColor(const cv::Scalar& bgr, int alpha) {
    return Gdiplus::Color(
        std::clamp(alpha, 0, 255),
        std::clamp(static_cast<int>(std::round(bgr[2])), 0, 255),
        std::clamp(static_cast<int>(std::round(bgr[1])), 0, 255),
        std::clamp(static_cast<int>(std::round(bgr[0])), 0, 255));
}

void addRoundedRectPath(Gdiplus::GraphicsPath& path, float x1, float y1, float x2, float y2, float radius) {
    const float d = std::max(0.0f, radius * 2.0f);
    if (d <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x1, y1, x2 - x1, y2 - y1));
        return;
    }
    path.AddArc(x1, y1, d, d, 180.0f, 90.0f);
    path.AddArc(x2 - d, y1, d, d, 270.0f, 90.0f);
    path.AddArc(x2 - d, y2 - d, d, d, 0.0f, 90.0f);
    path.AddArc(x1, y2 - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

const wchar_t* fontFamilyName() {
    return L"Microsoft YaHei";
}
} // namespace
#endif

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    close();
}

void OverlayWindow::close() {
#ifdef _WIN32
    HWND hwnd = hwnd_;
    if (hwnd) {
        destroying_.store(true);
        if (isOwnerThread()) {
            DestroyWindow(hwnd);
        } else {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        hwnd_ = nullptr;
        owner_thread_id_ = 0;
    }
#endif
}

bool OverlayWindow::create(const std::wstring& title, int width, int height, bool click_through) {
    return create(title, width, height, click_through, true);
}

bool OverlayWindow::create(const std::wstring& title, int width, int height, bool click_through,
                           bool exclude_from_capture) {
    return createAt(title, 0, 0, width, height, click_through, exclude_from_capture);
}

bool OverlayWindow::createAt(const std::wstring& title, int left, int top, int width, int height,
                             bool click_through, bool exclude_from_capture) {
    close();
#ifdef _WIN32
    destroying_.store(false);
#endif
    left_ = left;
    top_ = top;
    width_ = width;
    height_ = height;
#ifdef _WIN32
    ensureGdiplus();
    const wchar_t* cls = L"PUBGAssistant-cppOverlay";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayWindow::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (click_through) {
        ex |= WS_EX_TRANSPARENT;
    }
    hwnd_ = CreateWindowExW(
        ex, cls, title.c_str(), WS_POPUP,
        left, top, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        return false;
    }
    owner_thread_id_ = GetCurrentThreadId();
    ShowWindow(hwnd_, SW_SHOW);
    if (exclude_from_capture) {
        constexpr DWORD kWdaExcludeFromCapture = 0x00000011;
        if (!SetWindowDisplayAffinity(hwnd_, kWdaExcludeFromCapture)) {
            SetWindowDisplayAffinity(hwnd_, WDA_MONITOR);
        }
    }
    renderLayered();
    return true;
#else
    (void)title;
    (void)left;
    (void)top;
    (void)click_through;
    (void)exclude_from_capture;
    return false;
#endif
}

void OverlayWindow::show(bool visible) {
#ifdef _WIN32
    if (hwnd_ && !destroying_.load()) {
        if (isOwnerThread()) {
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        } else {
            PostMessageW(hwnd_, kOverlayShowMessage, visible ? 1 : 0, 0);
        }
    }
#else
    (void)visible;
#endif
}

void OverlayWindow::setCommands(std::vector<OverlayCommand> commands) {
#ifdef _WIN32
    if (destroying_.load()) {
        return;
    }
#endif
    {
        std::lock_guard lock(mutex_);
        commands_ = std::move(commands);
    }
#ifdef _WIN32
    if (hwnd_) {
        requestRender();
    }
#endif
}

void OverlayWindow::clear() {
    setCommands({OverlayCommand{OverlayCommand::Type::Clear}});
}

void OverlayWindow::pumpMessages() {
#ifdef _WIN32
    if (!isOwnerThread()) {
        return;
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

bool OverlayWindow::created() const noexcept {
#ifdef _WIN32
    return hwnd_ != nullptr;
#else
    return false;
#endif
}

#ifdef _WIN32
bool OverlayWindow::isOwnerThread() const {
    return owner_thread_id_ == 0 || GetCurrentThreadId() == owner_thread_id_;
}

void OverlayWindow::requestRender() {
    if (!hwnd_ || destroying_.load()) {
        return;
    }
    if (isOwnerThread()) {
        renderLayered();
    } else {
        PostMessageW(hwnd_, kOverlayRenderMessage, 0, 0);
    }
}

void OverlayWindow::renderLayered() {
    if (!hwnd_ || destroying_.load() || width_ <= 0 || height_ <= 0) {
        return;
    }
    ensureGdiplus();
    std::lock_guard render_lock(g_gdiplus_mutex);

    std::vector<OverlayCommand> commands;
    {
        std::lock_guard lock(mutex_);
        commands = commands_;
    }

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return;
    }
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(nullptr, screen_dc);
        return;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = -height_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(mem_dc, dib);
    if (!old_bitmap) {
        DeleteObject(dib);
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return;
    }

    Gdiplus::Graphics graphics(mem_dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    for (const auto& cmd : commands) {
        if (cmd.type == OverlayCommand::Type::Clear) {
            continue;
        }
        const auto color = gdipColor(cmd.bgr, cmd.alpha);
        Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(std::max(1, cmd.width)));
        Gdiplus::SolidBrush brush(color);
        switch (cmd.type) {
        case OverlayCommand::Type::Line:
            graphics.DrawLine(&pen,
                              static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1),
                              static_cast<Gdiplus::REAL>(cmd.x2), static_cast<Gdiplus::REAL>(cmd.y2));
            break;
        case OverlayCommand::Type::Rect:
            if (cmd.width > 0) {
                graphics.DrawRectangle(&pen,
                                       static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1),
                                       static_cast<Gdiplus::REAL>(cmd.x2 - cmd.x1), static_cast<Gdiplus::REAL>(cmd.y2 - cmd.y1));
            } else {
                graphics.FillRectangle(&brush,
                                       static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1),
                                       static_cast<Gdiplus::REAL>(cmd.x2 - cmd.x1), static_cast<Gdiplus::REAL>(cmd.y2 - cmd.y1));
            }
            break;
        case OverlayCommand::Type::RoundedRect: {
            Gdiplus::GraphicsPath path;
            addRoundedRectPath(path, static_cast<float>(cmd.x1), static_cast<float>(cmd.y1),
                               static_cast<float>(cmd.x2), static_cast<float>(cmd.y2),
                               static_cast<float>(cmd.radius));
            if (cmd.width > 0) {
                graphics.DrawPath(&pen, &path);
            } else {
                graphics.FillPath(&brush, &path);
            }
            break;
        }
        case OverlayCommand::Type::Circle:
            if (cmd.width > 0) {
                graphics.DrawEllipse(&pen,
                                     static_cast<Gdiplus::REAL>(cmd.x1 - cmd.radius),
                                     static_cast<Gdiplus::REAL>(cmd.y1 - cmd.radius),
                                     static_cast<Gdiplus::REAL>(cmd.radius * 2.0),
                                     static_cast<Gdiplus::REAL>(cmd.radius * 2.0));
            } else {
                graphics.FillEllipse(&brush,
                                     static_cast<Gdiplus::REAL>(cmd.x1 - cmd.radius),
                                     static_cast<Gdiplus::REAL>(cmd.y1 - cmd.radius),
                                     static_cast<Gdiplus::REAL>(cmd.radius * 2.0),
                                     static_cast<Gdiplus::REAL>(cmd.radius * 2.0));
            }
            break;
        case OverlayCommand::Type::Text:
        case OverlayCommand::Type::TextCenter: {
            const auto wide = widen(cmd.text);
            Gdiplus::FontFamily family(fontFamilyName());
            const Gdiplus::FontFamily* font_family = family.IsAvailable()
                ? &family
                : Gdiplus::FontFamily::GenericSansSerif();
            Gdiplus::Font font(font_family, static_cast<Gdiplus::REAL>(cmd.font_size), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            if (font.GetLastStatus() != Gdiplus::Ok) {
                break;
            }
            Gdiplus::StringFormat format;
            if (cmd.type == OverlayCommand::Type::TextCenter) {
                format.SetAlignment(Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::RectF rect(static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1),
                                    static_cast<Gdiplus::REAL>(cmd.x2 - cmd.x1), static_cast<Gdiplus::REAL>(cmd.y2 - cmd.y1));
                graphics.DrawString(wide.c_str(), static_cast<INT>(wide.size()), &font, rect, &format, &brush);
            } else {
                Gdiplus::PointF point(static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1));
                graphics.DrawString(wide.c_str(), static_cast<INT>(wide.size()), &font, point, &brush);
            }
            break;
        }
        case OverlayCommand::Type::Clear:
            break;
        }
    }

    POINT src{0, 0};
    POINT dst{left_, top_};
    SIZE size{width_, height_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen_dc, &dst, &size, mem_dc, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem_dc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
}

LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (msg) {
    case WM_PAINT:
        if (self) {
            self->paint();
            return 0;
        }
        break;
    case kOverlayRenderMessage:
        if (self && !self->destroying_.load()) {
            self->renderLayered();
            return 0;
        }
        break;
    case kOverlayShowMessage:
        if (self && !self->destroying_.load()) {
            ShowWindow(hwnd, wp ? SW_SHOW : SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (self) {
            self->destroying_.store(true);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (self) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            self->owner_thread_id_ = 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OverlayWindow::paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    HDC mem_dc = CreateCompatibleDC(hdc);
    HBITMAP mem_bitmap = CreateCompatibleBitmap(hdc, width_, height_);
    HGDIOBJ old_bitmap = SelectObject(mem_dc, mem_bitmap);

    RECT rc{0, 0, width_, height_};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem_dc, &rc, black);
    DeleteObject(black);

    std::vector<OverlayCommand> commands;
    {
        std::lock_guard lock(mutex_);
        commands = commands_;
    }

    SetBkMode(mem_dc, TRANSPARENT);
    for (const auto& cmd : commands) {
        COLORREF color = RGB(static_cast<int>(cmd.bgr[2]), static_cast<int>(cmd.bgr[1]), static_cast<int>(cmd.bgr[0]));
        HPEN pen = CreatePen(PS_SOLID, cmd.width, color);
        HGDIOBJ old_pen = SelectObject(mem_dc, pen);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ old_brush = SelectObject(mem_dc, brush);

        switch (cmd.type) {
        case OverlayCommand::Type::Line:
            MoveToEx(mem_dc, static_cast<int>(cmd.x1), static_cast<int>(cmd.y1), nullptr);
            LineTo(mem_dc, static_cast<int>(cmd.x2), static_cast<int>(cmd.y2));
            break;
        case OverlayCommand::Type::Rect:
            SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
            Rectangle(mem_dc, static_cast<int>(cmd.x1), static_cast<int>(cmd.y1), static_cast<int>(cmd.x2), static_cast<int>(cmd.y2));
            break;
        case OverlayCommand::Type::RoundedRect:
            if (cmd.width <= 0) {
                SelectObject(mem_dc, GetStockObject(NULL_PEN));
            }
            RoundRect(mem_dc,
                      static_cast<int>(cmd.x1), static_cast<int>(cmd.y1),
                      static_cast<int>(cmd.x2), static_cast<int>(cmd.y2),
                      static_cast<int>(cmd.radius * 2), static_cast<int>(cmd.radius * 2));
            break;
        case OverlayCommand::Type::Circle:
            Ellipse(mem_dc,
                    static_cast<int>(cmd.x1 - cmd.radius), static_cast<int>(cmd.y1 - cmd.radius),
                    static_cast<int>(cmd.x1 + cmd.radius), static_cast<int>(cmd.y1 + cmd.radius));
            break;
        case OverlayCommand::Type::Text: {
            HFONT font = CreateFontW(-cmd.font_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei");
            HGDIOBJ old_font = SelectObject(mem_dc, font);
            SetTextColor(mem_dc, color);
            auto text = widen(cmd.text);
            TextOutW(mem_dc, static_cast<int>(cmd.x1), static_cast<int>(cmd.y1), text.c_str(), static_cast<int>(text.size()));
            SelectObject(mem_dc, old_font);
            DeleteObject(font);
            break;
        }
        case OverlayCommand::Type::TextCenter: {
            HFONT font = CreateFontW(-cmd.font_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei");
            HGDIOBJ old_font = SelectObject(mem_dc, font);
            SetTextColor(mem_dc, color);
            auto text = widen(cmd.text);
            RECT text_rect{static_cast<LONG>(cmd.x1), static_cast<LONG>(cmd.y1),
                           static_cast<LONG>(cmd.x2), static_cast<LONG>(cmd.y2)};
            DrawTextW(mem_dc, text.c_str(), static_cast<int>(text.size()), &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(mem_dc, old_font);
            DeleteObject(font);
            break;
        }
        case OverlayCommand::Type::Clear:
            break;
        }

        SelectObject(mem_dc, old_pen);
        SelectObject(mem_dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    BitBlt(hdc, 0, 0, width_, height_, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(mem_bitmap);
    DeleteDC(mem_dc);
    EndPaint(hwnd_, &ps);
}
#endif

} // namespace pubg
