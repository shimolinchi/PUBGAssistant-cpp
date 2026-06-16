#pragma once

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>

// 在 macOS/Linux 上做语法检查时没有 Windows 的 VK_* 常量。
// 这些 fallback 只用于让非 Windows 环境能解析头文件；实际运行仍以 Win32 定义为准。
#ifndef _WIN32
#define VK_END 0x23
#define VK_DELETE 0x2E
#define VK_HOME 0x24
#define VK_TAB 0x09
#define VK_SPACE 0x20
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#endif

namespace pubg {

// 屏幕区域，单位是 Windows 物理像素。
// 对应 Python 版 config.json 里的 real_regions 项。
struct Rect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;

    // 判断该截图区域是否有有效尺寸。截图和模板识别前都会用这个做保护。
    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0;
    }
};

// 一个游戏标点颜色的 HSV 阈值配置。
// 对应 config.json 中 pnt_colors 的 Yellow/Orange/Blue/Green 项。
struct MarkerColor {
    std::string name;
    cv::Scalar lower_hsv;
    cv::Scalar upper_hsv;
    std::string hex = "#FFFFFF";
};

// 小地图检测到的单个标点。
// x/y 是区域内局部坐标；distance_m 是换算后的游戏内距离；confidence 是模板匹配置信度。
struct TargetPoint {
    std::string color_name;
    std::string hex = "#FFFFFF";
    double x = 0.0;
    double y = 0.0;
    double distance_m = 0.0;
    double confidence = 1.0;
};

// 装备栏中一个武器槽位的识别结果。
// name/scope/grip/muzzle/stock 与 Python 版 EquipmentDetector 返回的字段保持一致。
struct WeaponSlotInfo {
    std::string name;
    double name_score = 0.0;
    std::string scope;
    double scope_score = 0.0;
    std::string grip;
    double grip_score = 0.0;
    std::string muzzle;
    double muzzle_score = 0.0;
    std::string stock;
    double stock_score = 0.0;
};

// 标点颜色名到距离/高度的映射，例如 {"Yellow": 123.4}。
using DistanceMap = std::unordered_map<std::string, double>;
using ElevationMap = std::unordered_map<std::string, double>;
using Json = nlohmann::json;

// 返回程序启动后的单调时间，单位秒。
// 用于线程循环、压枪曲线采样、超时判断，避免受系统时间调整影响。
inline double nowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// 根据目标 FPS 和本帧耗时计算剩余 sleep 毫秒数。
// 各识别线程用它保持接近 Python 版 fps 参数的节奏。
inline int sleepMsForFps(int fps, double elapsed_seconds) {
    if (fps <= 0) {
        return 1;
    }
    const double frame = 1.0 / static_cast<double>(fps);
    const double remain = frame - elapsed_seconds;
    return remain > 0.0 ? static_cast<int>(remain * 1000.0) : 1;
}

// 将 "#RRGGBB" 转成 OpenCV/GDI 使用的 BGR 颜色。
// HUD 绘制和标点颜色显示共用这个函数。
inline cv::Scalar hexToBgr(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') {
        return {255, 255, 255};
    }
    const int r = std::stoi(hex.substr(1, 2), nullptr, 16);
    const int g = std::stoi(hex.substr(3, 2), nullptr, 16);
    const int b = std::stoi(hex.substr(5, 2), nullptr, 16);
    return {static_cast<double>(b), static_cast<double>(g), static_cast<double>(r)};
}

inline cv::Scalar dimBgr(const cv::Scalar& bgr, double ratio) {
    ratio = std::clamp(ratio, 0.0, 1.0);
    return {bgr[0] * ratio, bgr[1] * ratio, bgr[2] * ratio};
}

// UTF-8 字符串转宽字符串。
// Win32 TextOutW/CreateWindowW 需要 wchar_t；非 Windows 下只做简单转换，便于编译检查。
inline std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
#ifdef _WIN32
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size - 1);
    return wide;
#else
    return std::wstring(text.begin(), text.end());
#endif
}

} // namespace pubg
