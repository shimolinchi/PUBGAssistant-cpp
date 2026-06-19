#include "WeaponDetector.hpp"

#include <iostream>

namespace pubg {

WeaponDetector::WeaponDetector(Config& config, RegionManager& regions, int fps, double threshold)
    : config_(config), regions_(regions), fps_(fps), threshold_(threshold) {
    templates_ = TemplateMatcher::loadWeaponTemplates(config_.paths().templatesDir() / "weapons");
    loadTargetSize();
}

WeaponDetector::~WeaponDetector() {
    setEnabled(false);
}

void WeaponDetector::loadTargetSize() {
    const auto scaling = config_.read([](const Json& data) {
        return data.value("region_scaling_settings", Json::object())
            .value("weapon_region", Json::object());
    });
    if (!scaling.empty()) {
        target_w_ = scaling.value("width", target_w_);
        target_h_ = scaling.value("height", target_h_);
    } else if (!templates_.empty()) {
        target_w_ = templates_.front().edges.cols;
        target_h_ = templates_.front().edges.rows;
    }
}

void WeaponDetector::setEnabled(bool enabled, Callback cb) {
    if (cb) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(cb);
    }
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        worker_ = std::thread(&WeaponDetector::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

void WeaponDetector::updatePrimaryWeapons(std::optional<std::string> weapon1, std::optional<std::string> weapon2) {
    std::lock_guard lock(mutex_);
    primary_weapons_[0] = std::move(weapon1);
    primary_weapons_[1] = std::move(weapon2);
}

std::pair<std::string, double> WeaponDetector::currentWeapon() const {
    std::lock_guard lock(mutex_);
    return {current_weapon_, current_score_};
}

WeaponDetector::Callback WeaponDetector::callbackCopy() const {
    std::lock_guard lock(mutex_);
    return callback_;
}

std::pair<std::string, double> WeaponDetector::identifyOnce(ScreenCapture& capture) {
    const auto rect = regions_.getRealRegion("weapon_region");
    if (!rect || templates_.empty()) {
        return {"", 0.0};
    }
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return {"", 0.0};
    }
    if (bgr.cols != target_w_ || bgr.rows != target_h_) {
        cv::resize(bgr, bgr, {target_w_, target_h_});
    }
    cv::Mat current = TemplateMatcher::preprocessWeapon(bgr);
    if (cv::countNonZero(current) < 5) {
        return {"", 0.0};
    }

    std::vector<std::string> candidates;
    {
        std::lock_guard lock(mutex_);
        for (const auto& w : primary_weapons_) {
            if (w) {
                candidates.push_back(*w);
            }
        }
    }
    candidates.insert(candidates.end(), special_weapons_.begin(), special_weapons_.end());

    std::string best;
    double best_score = 0.0;
    for (const auto& tpl : templates_) {
        if (std::find(candidates.begin(), candidates.end(), tpl.name) == candidates.end()) {
            continue;
        }
        if (tpl.edges.rows > current.rows || tpl.edges.cols > current.cols) {
            continue;
        }
        cv::Mat res;
        cv::matchTemplate(current, tpl.edges, res, cv::TM_CCORR_NORMED, tpl.mask);
        double max_val = 0.0;
        cv::minMaxLoc(res, nullptr, &max_val);
        if (std::isfinite(max_val) && max_val > best_score) {
            best_score = max_val;
            best = tpl.name;
        }
    }
    const double th = best == "Grenade" ? 0.70 : threshold_;
    if (!best.empty() && best_score >= th) {
        return {best, best_score};
    }
    return {"", best_score};
}

void WeaponDetector::run() {
    ScreenCapture capture;
    while (!stop_) {
        const double start = nowSeconds();
        auto [weapon, score] = identifyOnce(capture);
        std::string notify_weapon;
        double notify_score = 0.0;
        bool should_notify = false;

        {
            std::lock_guard lock(mutex_);
            if (!weapon.empty()) {
                if (pending_weapon_ == weapon) {
                    pending_counter_++;
                    if (pending_counter_ >= 2 && current_weapon_ != weapon) {
                        current_weapon_ = weapon;
                        current_score_ = score;
                        notify_weapon = weapon;
                        notify_score = score;
                        should_notify = true;
                    }
                } else {
                    pending_weapon_ = weapon;
                    pending_counter_ = 1;
                }
            } else {
                pending_weapon_.clear();
                pending_counter_ = 0;
                if (!current_weapon_.empty()) {
                    current_weapon_.clear();
                    current_score_ = 0.0;
                    should_notify = true;
                }
            }
        }

        if (should_notify) {
            if (auto cb = callbackCopy()) {
                cb(notify_weapon, notify_score);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

} // namespace pubg
