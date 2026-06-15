#include "TemplateMatcher.hpp"

#include <iostream>

namespace pubg {

cv::Mat TemplateMatcher::preprocessWeapon(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, {3, 3}, 0);
    cv::Mat edges;
    cv::Canny(gray, edges, 50, 150);
    cv::dilate(edges, edges, cv::Mat::ones(2, 2, CV_8U), {-1, -1}, 1);
    return edges;
}

std::vector<WeaponTemplate> TemplateMatcher::loadWeaponTemplates(const std::filesystem::path& dir) {
    std::vector<WeaponTemplate> out;
    if (!std::filesystem::exists(dir)) {
        std::cerr << "[templates] missing weapon dir " << dir << "\n";
        return out;
    }
    for (const auto& weapon_dir : std::filesystem::directory_iterator(dir)) {
        if (!weapon_dir.is_directory()) {
            continue;
        }
        const std::string weapon = weapon_dir.path().filename().string();
        for (const auto& file : std::filesystem::directory_iterator(weapon_dir.path())) {
            if (!file.is_regular_file() || file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_COLOR);
            if (img.empty()) {
                continue;
            }
            cv::Mat edges = preprocessWeapon(img);
            cv::Mat mask;
            cv::dilate(edges, mask, cv::Mat::ones(5, 5, CV_8U), {-1, -1}, 1);
            if (cv::countNonZero(mask) == 0) {
                continue;
            }
            out.push_back({weapon, edges, mask});
        }
    }
    std::cerr << "[templates] weapon templates: " << out.size() << "\n";
    return out;
}

std::unordered_map<std::string, std::vector<cv::Mat>> TemplateMatcher::loadGrayTemplates(const std::filesystem::path& dir) {
    std::unordered_map<std::string, std::vector<cv::Mat>> out;
    if (!std::filesystem::exists(dir)) {
        return out;
    }
    for (const auto& item_dir : std::filesystem::directory_iterator(dir)) {
        if (!item_dir.is_directory()) {
            continue;
        }
        const std::string name = item_dir.path().filename().string();
        for (const auto& file : std::filesystem::directory_iterator(item_dir.path())) {
            if (!file.is_regular_file() || file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_UNCHANGED);
            if (img.empty()) {
                continue;
            }
            cv::Mat gray;
            if (img.channels() == 4) {
                cv::cvtColor(img, gray, cv::COLOR_BGRA2GRAY);
            } else if (img.channels() == 3) {
                cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
            } else {
                gray = img;
            }
            out[name].push_back(gray);
        }
    }
    return out;
}

std::unordered_map<std::string, std::vector<MaskedTemplate>> TemplateMatcher::loadMaskedTemplates(const std::filesystem::path& dir) {
    std::unordered_map<std::string, std::vector<MaskedTemplate>> out;
    if (!std::filesystem::exists(dir)) {
        return out;
    }
    for (const auto& item_dir : std::filesystem::directory_iterator(dir)) {
        if (!item_dir.is_directory()) {
            continue;
        }
        const std::string name = item_dir.path().filename().string();
        for (const auto& file : std::filesystem::directory_iterator(item_dir.path())) {
            if (!file.is_regular_file() || file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_UNCHANGED);
            if (img.empty()) {
                continue;
            }
            cv::Mat bgr;
            cv::Mat mask;
            if (img.channels() == 4) {
                std::vector<cv::Mat> channels;
                cv::split(img, channels);
                cv::merge(std::vector<cv::Mat>{channels[0], channels[1], channels[2]}, bgr);
                cv::threshold(channels[3], mask, 1, 255, cv::THRESH_BINARY);
            } else {
                bgr = img;
                mask = cv::Mat(img.rows, img.cols, CV_8U, cv::Scalar(255));
            }
            out[name].push_back({name, bgr, mask});
        }
    }
    return out;
}

std::vector<cv::Mat> TemplateMatcher::loadAlphaBinaryTemplates(const std::filesystem::path& dir) {
    std::vector<cv::Mat> out;
    auto loadFrom = [&out](const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return;
        }
        for (const auto& file : std::filesystem::directory_iterator(path)) {
            if (!file.is_regular_file() || file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_UNCHANGED);
            if (img.empty() || img.channels() != 4) {
                continue;
            }
            std::vector<cv::Mat> channels;
            cv::split(img, channels);
            cv::Mat binary;
            cv::threshold(channels[3], binary, 128, 255, cv::THRESH_BINARY);
            out.push_back(binary);
        }
    };
    loadFrom(dir);
    return out;
}

std::pair<std::string, double> TemplateMatcher::matchGray(
    const cv::Mat& roi_gray,
    const std::unordered_map<std::string, std::vector<cv::Mat>>& templates) {
    std::string best;
    double best_score = 0.0;
    for (const auto& [name, list] : templates) {
        for (const auto& tpl : list) {
            if (tpl.rows > roi_gray.rows || tpl.cols > roi_gray.cols) {
                continue;
            }
            cv::Mat res;
            cv::matchTemplate(roi_gray, tpl, res, cv::TM_CCOEFF_NORMED);
            double max_val = 0.0;
            cv::minMaxLoc(res, nullptr, &max_val);
            if (std::isfinite(max_val) && max_val > best_score) {
                best_score = max_val;
                best = name;
            }
        }
    }
    return {best, best_score};
}

std::pair<std::string, double> TemplateMatcher::matchMasked(
    const cv::Mat& roi_bgr,
    const std::unordered_map<std::string, std::vector<MaskedTemplate>>& templates) {
    std::string best;
    double best_score = 0.0;
    for (const auto& [name, list] : templates) {
        for (const auto& tpl : list) {
            if (tpl.image.rows > roi_bgr.rows || tpl.image.cols > roi_bgr.cols) {
                continue;
            }
            cv::Mat res;
            cv::matchTemplate(roi_bgr, tpl.image, res, cv::TM_CCOEFF_NORMED, tpl.mask);
            double max_val = 0.0;
            cv::minMaxLoc(res, nullptr, &max_val);
            if (std::isfinite(max_val) && max_val > best_score) {
                best_score = max_val;
                best = name;
            }
        }
    }
    return {best, best_score};
}

} // namespace pubg
