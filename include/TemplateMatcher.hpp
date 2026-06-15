#pragma once

#include "Config.hpp"

namespace pubg {

// 带透明掩码的模板。配件模板通常是 BGRA PNG，alpha 通道会转换成 mask。
struct MaskedTemplate {
    std::string name;
    cv::Mat image;
    cv::Mat mask;
};

// 手持武器模板。Python 版 weapon_detector.py 会先做边缘提取，这里保存边缘图和匹配 mask。
struct WeaponTemplate {
    std::string name;
    cv::Mat edges;
    cv::Mat mask;
};

// OpenCV 模板加载和匹配工具类。
// 这个类不保存状态，只把 Python 版多个 detector 中重复的模板处理逻辑集中起来。
class TemplateMatcher {
public:
    // 武器图像预处理：BGR -> 灰度 -> 高斯模糊 -> Canny -> 膨胀。
    // 对应 Python WeaponDetector._preprocess_image。
    static cv::Mat preprocessWeapon(const cv::Mat& bgr);

    // 递归读取 templates/weapons/<WeaponName>/*.png，生成武器边缘模板。
    static std::vector<WeaponTemplate> loadWeaponTemplates(const std::filesystem::path& dir);

    // 读取 templates/.../<ItemName>/*.png 并转灰度。
    // 用于装备栏武器名、姿势等灰度模板匹配。
    static std::unordered_map<std::string, std::vector<cv::Mat>> loadGrayTemplates(const std::filesystem::path& dir);

    // 读取 BGRA/BGR 模板，并把 alpha 通道转成 mask。
    // 用于倍镜、握把、枪口、枪托等配件模板匹配。
    static std::unordered_map<std::string, std::vector<MaskedTemplate>> loadMaskedTemplates(const std::filesystem::path& dir);

    // 读取标点 PNG 的 alpha 通道并二值化。
    // 小地图/测高标点检测用它匹配图标形状。
    static std::vector<cv::Mat> loadAlphaBinaryTemplates(const std::filesystem::path& dir);

    // 在灰度 ROI 中匹配一组模板，返回最佳名称和分数。
    // 调用方负责和自身阈值比较。
    static std::pair<std::string, double> matchGray(
        const cv::Mat& roi_gray,
        const std::unordered_map<std::string, std::vector<cv::Mat>>& templates);

    // 在彩色 ROI 中使用 mask 匹配一组模板，返回最佳名称和分数。
    // 主要用于装备栏配件区域。
    static std::pair<std::string, double> matchMasked(
        const cv::Mat& roi_bgr,
        const std::unordered_map<std::string, std::vector<MaskedTemplate>>& templates);
};

} // namespace pubg
