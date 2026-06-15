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
    HDC mem_dc = CreateCompatibleDC(screen_dc_);
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc_, rect.width, rect.height);
    HGDIOBJ old = SelectObject(mem_dc, bitmap);

    BitBlt(mem_dc, 0, 0, rect.width, rect.height, screen_dc_, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = rect.width;
    bi.biHeight = -rect.height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    cv::Mat bgra(rect.height, rect.width, CV_8UC4);
    GetDIBits(mem_dc, bitmap, 0, rect.height, bgra.data, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    SelectObject(mem_dc, old);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
#else
    (void)rect;
    return {};
#endif
}

} // namespace pubg
