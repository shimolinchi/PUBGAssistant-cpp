#include "ScreenCapture.hpp"

#include <stdexcept>

namespace pubg {

ScreenCapture::ScreenCapture() {
#ifdef _WIN32
    screen_dc_ = GetDC(nullptr);
#endif
}

ScreenCapture::~ScreenCapture() {
#ifdef _WIN32
    if (screen_dc_) {
        ReleaseDC(nullptr, screen_dc_);
    }
#endif
}

cv::Mat ScreenCapture::grabBgr(const Rect& rect) const {
    if (!rect.valid()) {
        return {};
    }
#ifdef _WIN32
    if (!screen_dc_) {
        return {};
    }
    HDC mem_dc = CreateCompatibleDC(screen_dc_);
    if (!mem_dc) {
        return {};
    }
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc_, rect.width, rect.height);
    if (!bitmap) {
        DeleteDC(mem_dc);
        return {};
    }
    HGDIOBJ old = SelectObject(mem_dc, bitmap);
    if (!old) {
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        return {};
    }

    if (!BitBlt(mem_dc, 0, 0, rect.width, rect.height, screen_dc_, rect.left, rect.top, SRCCOPY)) {
        SelectObject(mem_dc, old);
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        return {};
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rect.width;
    bi.bmiHeader.biHeight = -rect.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    cv::Mat bgra(rect.height, rect.width, CV_8UC4);
    const int rows = GetDIBits(mem_dc, bitmap, 0, rect.height, bgra.data, &bi, DIB_RGB_COLORS);

    SelectObject(mem_dc, old);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);

    if (rows != rect.height) {
        return {};
    }

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
#else
    (void)rect;
    return {};
#endif
}

} // namespace pubg
