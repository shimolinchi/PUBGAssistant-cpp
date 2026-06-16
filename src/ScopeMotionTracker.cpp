#include "ScopeMotionTracker.hpp"

namespace pubg {

ScopeMotionTracker::ScopeMotionTracker(RegionManager& regions, std::string region_name, Json config)
    : regions_(regions), region_name_(std::move(region_name)) {
    black_threshold_ = config.value("black_threshold", black_threshold_);
    min_bright_ratio_ = config.value("min_bright_ratio", min_bright_ratio_);
    min_gradient_ = config.value("min_gradient", min_gradient_);
    max_edge_jump_ = config.value("max_edge_jump", max_edge_jump_);
    min_points_ = config.value("min_points", min_points_);
}

void ScopeMotionTracker::setRegionName(std::string region_name) {
    region_name_ = std::move(region_name);
    reset();
}

void ScopeMotionTracker::reset() {
    last_edge_y_.reset();
    last_confidence_ = 0.0;
}

std::tuple<double, double, bool> ScopeMotionTracker::detectMotion(ScreenCapture& capture) {
    auto rect = regions_.getRealRegion(region_name_);
    if (!rect) return {0.0, 0.0, false};
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) return {0.0, 0.0, false};
    auto [edge_y, confidence, found] = detectTopEdge(bgr, *rect);
    if (!found) {
        last_confidence_ = 0.0;
        return {0.0, confidence, false};
    }
    if (!last_edge_y_) {
        last_edge_y_ = edge_y;
        last_confidence_ = confidence;
        return {0.0, confidence, true};
    }
    const double dy = edge_y - *last_edge_y_;
    if (std::abs(dy) > max_edge_jump_) {
        last_edge_y_ = edge_y;
        last_confidence_ = 0.0;
        return {0.0, 0.0, false};
    }
    last_edge_y_ = edge_y;
    last_confidence_ = confidence;
    return {dy, confidence, true};
}

std::tuple<double, double, bool> ScopeMotionTracker::detectTopEdge(const cv::Mat& frame, const Rect& rect) const {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred_gray;
    cv::GaussianBlur(gray, blurred_gray, {3, 3}, 0.0);
    gray = blurred_gray;
    const int h = gray.rows;
    const int w = gray.cols;
    if (h < 12 || w < 6) {
        return {0.0, 0.0, false};
    }

    std::vector<cv::Point2d> col_edges;
    std::vector<double> col_scores;
    col_edges.reserve(static_cast<size_t>(w));
    col_scores.reserve(static_cast<size_t>(w));

    for (int x = 0; x < w; ++x) {
        cv::Mat column(h, 1, CV_32F);
        for (int y = 0; y < h; ++y) {
            column.at<float>(y, 0) = static_cast<float>(gray.at<uchar>(y, x));
        }
        cv::Mat smooth;
        cv::GaussianBlur(column, smooth, {1, 9}, 0.0);
        cv::Mat bright(h, 1, CV_32F);
        for (int y = 0; y < h; ++y) {
            bright.at<float>(y, 0) = smooth.at<float>(y, 0) > static_cast<float>(black_threshold_) ? 1.0f : 0.0f;
        }
        cv::Mat bright_ratio;
        cv::GaussianBlur(bright, bright_ratio, {1, 11}, 0.0);

        int candidate = -1;
        double candidate_grad = 0.0;
        for (int y = 0; y < h - 1; ++y) {
            const double grad = static_cast<double>(bright_ratio.at<float>(y + 1, 0) - bright_ratio.at<float>(y, 0));
            if (grad >= min_gradient_ && bright_ratio.at<float>(y + 1, 0) >= min_bright_ratio_) {
                candidate = y;
                candidate_grad = grad;
                break;
            }
        }
        if (candidate < 0) {
            continue;
        }
        const int y = candidate + 1;
        if (y < 2 || y > h - 3) {
            continue;
        }
        col_edges.emplace_back(static_cast<double>(x), static_cast<double>(y));
        col_scores.push_back(candidate_grad);
    }

    if (static_cast<int>(col_edges.size()) < min_points_) {
        return {0.0, 0.0, false};
    }

    std::vector<double> ys;
    ys.reserve(col_edges.size());
    for (const auto& p : col_edges) {
        ys.push_back(p.y);
    }
    const double median_y = median(ys);
    std::vector<double> abs_deviation;
    abs_deviation.reserve(ys.size());
    for (double y : ys) {
        abs_deviation.push_back(std::abs(y - median_y));
    }
    const double mad = std::max(1.0, median(abs_deviation));
    const double keep_threshold = std::max(6.0, 3.5 * mad);

    std::vector<cv::Point2d> inliers;
    inliers.reserve(col_edges.size());
    for (const auto& p : col_edges) {
        if (std::abs(p.y - median_y) <= keep_threshold) {
            inliers.push_back(p);
        }
    }
    if (static_cast<int>(inliers.size()) < min_points_) {
        return {0.0, 0.0, false};
    }

    double edge_y_local = 0.0;
    double min_x = inliers.front().x;
    double max_x = inliers.front().x;
    for (const auto& p : inliers) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
    }
    const double center_x = (static_cast<double>(w) - 1.0) / 2.0;

    if (inliers.size() >= 12 && max_x - min_x >= 6.0) {
        cv::Mat a(static_cast<int>(inliers.size()), 3, CV_64F);
        cv::Mat b(static_cast<int>(inliers.size()), 1, CV_64F);
        for (int i = 0; i < static_cast<int>(inliers.size()); ++i) {
            const double x = inliers[static_cast<size_t>(i)].x;
            a.at<double>(i, 0) = x * x;
            a.at<double>(i, 1) = x;
            a.at<double>(i, 2) = 1.0;
            b.at<double>(i, 0) = inliers[static_cast<size_t>(i)].y;
        }
        cv::Mat coeff;
        if (cv::solve(a, b, coeff, cv::DECOMP_QR)) {
            edge_y_local = coeff.at<double>(0, 0) * center_x * center_x +
                coeff.at<double>(1, 0) * center_x +
                coeff.at<double>(2, 0);
        } else {
            std::vector<double> inlier_ys;
            for (const auto& p : inliers) inlier_ys.push_back(p.y);
            edge_y_local = median(inlier_ys);
        }
    } else {
        std::vector<double> inlier_ys;
        for (const auto& p : inliers) inlier_ys.push_back(p.y);
        edge_y_local = median(inlier_ys);
    }

    if (edge_y_local < 0.0 || edge_y_local >= static_cast<double>(h)) {
        return {0.0, 0.0, false};
    }

    const double inlier_ratio = static_cast<double>(inliers.size()) / std::max(1, w);
    double score = 0.0;
    for (double s : col_scores) {
        score += s;
    }
    score = col_scores.empty() ? 0.0 : score / static_cast<double>(col_scores.size());
    const double confidence = std::clamp(inlier_ratio * 1.5 + score, 0.0, 1.0);
    return {static_cast<double>(rect.top) + edge_y_local, confidence, confidence >= 0.25};
}

double ScopeMotionTracker::median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    double med = values[mid];
    if (values.size() % 2 == 0) {
        const auto max_left = std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
        med = (*max_left + med) * 0.5;
    }
    return med;
}

} // namespace pubg
