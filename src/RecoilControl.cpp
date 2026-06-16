#include "RecoilControl.hpp"

#include "BuildConfig.hpp"

namespace pubg {

RecoilControl::RecoilControl(Config& config, RegionManager* regions) : config_(config), regions_(regions) {
    loadConfig();
#if PUBG_ENABLE_INPUT_CONTROL
    worker_ = std::thread(&RecoilControl::workerLoop, this);
#endif
}

RecoilControl::~RecoilControl() {
    running_ = false;
#if PUBG_ENABLE_INPUT_CONTROL
    {
        std::lock_guard lock(mutex_);
        stopSrBreathTrackingLocked();
        firing_ = false;
        fire_start_ = 0.0;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    InputController::keyUp(fire_vk_);
#endif
}

std::vector<double> RecoilControl::normalizeCurve(const Json& value, double fallback) {
    std::vector<double> out;
    if (value.is_array()) {
        for (const auto& item : value) {
            out.push_back(item.get<double>());
        }
    } else if (value.is_number()) {
        out.push_back(value.get<double>());
    }
    if (out.empty()) {
        out.push_back(fallback);
    }
    return out;
}

double RecoilControl::sampleCurve(const std::vector<double>& curve, double elapsed, double step) {
    if (curve.empty()) {
        return 0.0;
    }
    if (curve.size() == 1) {
        return curve[0];
    }
    const double pos = std::max(0.0, elapsed) / std::max(0.001, step);
    const auto left = static_cast<size_t>(pos);
    if (left >= curve.size() - 1) {
        return curve.back();
    }
    const double t = pos - static_cast<double>(left);
    return curve[left] + (curve[left + 1] - curve[left]) * t;
}

void RecoilControl::loadConfig() {
    const auto data = config_.read([](const Json& root) {
        return Json{
            {"recoil_settings", root.value("recoil_settings", Json::object())},
            {"hotkeys", root.value("hotkeys", Json::object())},
        };
    });
    const auto rc = data.value("recoil_settings", Json::object());
    const auto hotkeys = data.value("hotkeys", Json::object());
    fire_vk_ = InputController::parseVirtualKey(hotkeys.value("fire_key", rc.value("fire_key", "end")));
    if (!fire_vk_) {
        fire_vk_ = VK_END;
    }
    recoil_delay_ = rc.value("recoil_delay", 0.02);
    recoil_curve_step_ = rc.value("recoil_curve_step", 0.4);
    const auto sr_cfg = rc.value("sr_breath_control", Json::object());
    sr_scope_delay_ = sr_cfg.value("scope_delay", sr_scope_delay_);
    sr_track_interval_ = sr_cfg.value("track_interval", sr_track_interval_);
    sr_move_scale_ = sr_cfg.value("move_scale", sr_move_scale_);
    sr_max_step_ = sr_cfg.value("max_step", sr_max_step_);
    sr_miss_limit_ = sr_cfg.value("miss_limit", sr_miss_limit_);
    sr_min_confidence_ = sr_cfg.value("min_confidence", sr_min_confidence_);
    sr_invert_y_ = sr_cfg.value("invert_y", sr_invert_y_);
    sr_probe_seconds_ = sr_cfg.value("probe_seconds", sr_probe_seconds_);
    sr_scope_lost_seconds_ = sr_cfg.value("scope_lost_seconds", sr_scope_lost_seconds_);
    sr_scope_confirm_frames_ = sr_cfg.value("scope_confirm_frames", sr_scope_confirm_frames_);
    sr_tracker_config_ = sr_cfg.value("edge_tracker", Json::object());

    weapon_configs_.clear();
    const auto weapons = rc.value("weapons", Json::object());
    for (auto it = weapons.begin(); it != weapons.end(); ++it) {
        weapon_configs_[it.key()] = it.value();
    }

    stance_multipliers_.clear();
    auto stances = rc.value("stance_multipliers", Json::object());
    for (auto it = stances.begin(); it != stances.end(); ++it) {
        for (auto jt = it.value().begin(); jt != it.value().end(); ++jt) {
            stance_multipliers_[it.key()][jt.key()] = jt.value().get<double>();
        }
    }
    if (stance_multipliers_.empty()) {
        stance_multipliers_["ar"] = {{"stand", 1.0}, {"squat", 0.8}, {"lie", 0.6}};
        stance_multipliers_["smg"] = {{"stand", 1.0}, {"squat", 0.8}, {"lie", 0.7}};
        stance_multipliers_["lmg"] = {{"stand", 1.0}, {"squat", 0.4}, {"lie", 0.2}};
        stance_multipliers_["dmr"] = {{"stand", 1.0}, {"squat", 0.8}, {"lie", 0.6}};
    }

    auto loadCurves = [](const Json& j) {
        std::unordered_map<std::string, std::vector<double>> out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            out[it.key()] = RecoilControl::normalizeCurve(it.value(), 1.0);
        }
        return out;
    };
    scope_curves_ = loadCurves(rc.value("scope_multipliers", Json::object()));
    grip_curves_ = loadCurves(rc.value("grip_multipliers", Json::object()));
    muzzle_curves_ = loadCurves(rc.value("muzzle_multipliers", Json::object()));
    stock_curves_ = loadCurves(rc.value("stock_multipliers", Json::object()));
}

void RecoilControl::reloadConfig() {
    std::lock_guard lock(mutex_);
    loadConfig();
}

bool RecoilControl::isFiring() const {
    std::lock_guard lock(mutex_);
    return firing_;
}

void RecoilControl::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        std::lock_guard lock(mutex_);
        firing_ = false;
        fire_start_ = 0.0;
#if PUBG_ENABLE_INPUT_CONTROL
        InputController::keyUp(fire_vk_);
#endif
        stopSrBreathTrackingLocked();
    }
}

void RecoilControl::updateCurrentWeapon(const std::string& weapon) {
    std::lock_guard lock(mutex_);
    if (weapon == current_weapon_) {
        return;
    }
    current_weapon_ = weapon;
    firing_ = false;
    fire_start_ = 0.0;
#if PUBG_ENABLE_INPUT_CONTROL
    InputController::keyUp(fire_vk_);
#endif
    auto it = weapon_configs_.find(weapon);
    if (it == weapon_configs_.end()) {
        weapon_type_ = "ar";
        recoil_curve_.clear();
        auto_fire_ = false;
        stopSrBreathTrackingLocked();
        return;
    }
    weapon_type_ = it->second.value("type", "ar");
    auto_fire_ = it->second.value("auto_fire", false);
    recoil_curve_ = weapon_type_ == "sr" ? std::vector<double>{} :
        normalizeCurve(it->second.contains("recoil_curve") ? it->second["recoil_curve"] : it->second.value("base", Json(0.0)), 0.0);
}

void RecoilControl::updateAttachments(const WeaponSlotInfo& info) {
    std::lock_guard lock(mutex_);
    scope_ = info.scope.empty() ? "hip" : info.scope;
    grip_ = info.grip;
    muzzle_ = info.muzzle;
    stock_ = info.stock;
}

void RecoilControl::updateStance(const std::string& stance) {
    std::lock_guard lock(mutex_);
    if (!stance.empty()) {
        stance_ = stance;
    }
}

double RecoilControl::multiplier(const std::unordered_map<std::string, std::vector<double>>& map, const std::string& key, double elapsed) const {
    if (key.empty()) {
        return 1.0;
    }
    auto it = map.find(key);
    if (it == map.end()) {
        return 1.0;
    }
    return sampleCurve(it->second, elapsed, recoil_curve_step_);
}

double RecoilControl::calculateStrength(double elapsed) {
    if (current_weapon_.empty() || recoil_curve_.empty()) {
        return 0.0;
    }
    double stance_mult = 1.0;
    if (auto wt = stance_multipliers_.find(weapon_type_); wt != stance_multipliers_.end()) {
        if (auto st = wt->second.find(stance_); st != wt->second.end()) {
            stance_mult = st->second;
        }
    }
    return sampleCurve(recoil_curve_, elapsed, recoil_curve_step_)
        * multiplier(scope_curves_, scope_, elapsed)
        * multiplier(grip_curves_, grip_, elapsed)
        * multiplier(muzzle_curves_, muzzle_, elapsed)
        * multiplier(stock_curves_, stock_, elapsed)
        * stance_mult;
}

void RecoilControl::workerLoop() {
#if PUBG_ENABLE_INPUT_CONTROL
    bool last_left = false;
    bool last_right = false;
    ScreenCapture capture;
    while (running_) {
        double delay_seconds = 0.02;
        const bool left = InputController::isLeftMouseDown();
        const bool right = InputController::isKeyDown(VK_RBUTTON);
        {
            std::lock_guard lock(mutex_);
            delay_seconds = sr_breath_enabled_ ? sr_track_interval_ : recoil_delay_;
            if (right && !last_right) {
                if (sr_breath_enabled_) {
                    stopSrBreathTrackingLocked();
                } else {
                    startSrBreathTrackingLocked();
                }
            }
            if (left && !last_left) {
                InputController::keyDown(fire_vk_);
                if (enabled_ && !current_weapon_.empty() && weapon_type_ != "sr") {
                    if (weapon_type_ == "dmr") {
                        const int strength = static_cast<int>(std::round(calculateStrength(0.0)));
                        if (strength > 0) {
                            InputController::moveMouseRelative(0, strength);
                        }
                    } else {
                        firing_ = true;
                        fire_start_ = nowSeconds();
                    }
                }
            } else if (!left && last_left) {
                firing_ = false;
                fire_start_ = 0.0;
                InputController::keyUp(fire_vk_);
            }

            if (enabled_ && firing_ && !current_weapon_.empty() && weapon_type_ != "dmr" && weapon_type_ != "sr") {
                const double t = nowSeconds() - fire_start_;
                const int strength = static_cast<int>(std::round(calculateStrength(t)));
                if (auto_fire_) {
                    InputController::keyDown(fire_vk_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    InputController::keyUp(fire_vk_);
                }
                if (strength > 0) {
                    InputController::moveMouseRelative(0, strength);
                }
            }
        }
        applySrBreathControl(capture);
        last_left = left;
        last_right = right;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(std::max(1.0, delay_seconds * 1000.0))));
    }
#endif
}

std::string RecoilControl::scopeEdgeRegionName() const {
    if (scope_ == "6" || scope_ == "6x" || scope_ == "x6") return "scope_top_edge_6x_region";
    if (scope_ == "8" || scope_ == "8x" || scope_ == "x8") return "scope_top_edge_8x_region";
    return "scope_top_edge_4x_region";
}

void RecoilControl::startSrBreathTrackingLocked() {
    sr_breath_enabled_ = enabled_ && weapon_type_ == "sr" && !current_weapon_.empty();
    const double now = nowSeconds();
    sr_scope_ready_time_ = now + std::max(0.0, sr_scope_delay_);
    sr_probe_until_ = sr_scope_ready_time_ + std::max(0.1, sr_probe_seconds_);
    sr_miss_count_ = 0;
    sr_edge_hit_streak_ = 0;
    sr_scope_active_ = false;
    sr_last_confirmed_edge_time_ = 0.0;
    sr_last_track_time_ = 0.0;
    if (!sr_breath_enabled_) {
        stopSrBreathTrackingLocked();
        return;
    }
    if (auto* tracker = ensureSrTrackerLocked()) {
        tracker->reset();
    }
}

ScopeMotionTracker* RecoilControl::ensureSrTrackerLocked() {
    if (!regions_) {
        return nullptr;
    }
    const std::string region_name = scopeEdgeRegionName();
    if (sr_tracker_) {
        if (region_name != sr_tracker_region_name_) {
            sr_tracker_->setRegionName(region_name);
            sr_tracker_region_name_ = region_name;
        }
        return sr_tracker_.get();
    }

    Json tracker_cfg = Json{
        {"min_gradient", 0.12},
        {"min_bright_ratio", 0.35},
        {"max_edge_jump", std::max(20.0, static_cast<double>(sr_max_step_) * 4.0)}
    };
    if (sr_tracker_config_.is_object()) {
        for (auto it = sr_tracker_config_.begin(); it != sr_tracker_config_.end(); ++it) {
            tracker_cfg[it.key()] = it.value();
        }
    }
    sr_tracker_region_name_ = region_name;
    sr_tracker_ = std::make_unique<ScopeMotionTracker>(*regions_, sr_tracker_region_name_, tracker_cfg);
    return sr_tracker_.get();
}

void RecoilControl::stopSrBreathTrackingLocked() {
    sr_breath_enabled_ = false;
    sr_scope_ready_time_ = 0.0;
    sr_miss_count_ = 0;
    sr_probe_until_ = 0.0;
    sr_scope_active_ = false;
    sr_edge_hit_streak_ = 0;
    sr_last_confirmed_edge_time_ = 0.0;
    sr_last_track_time_ = 0.0;
    sr_tracker_.reset();
    sr_tracker_region_name_.clear();
}

void RecoilControl::applySrBreathControl(ScreenCapture& capture) {
#if PUBG_ENABLE_INPUT_CONTROL
    std::lock_guard lock(mutex_);
    if (!sr_breath_enabled_) return;
    if (!enabled_ || weapon_type_ != "sr" || current_weapon_.empty()) {
        stopSrBreathTrackingLocked();
        return;
    }
    const double now = nowSeconds();
    if (now < sr_scope_ready_time_) return;
    if (now - sr_last_track_time_ < sr_track_interval_) return;
    sr_last_track_time_ = now;

    auto* tracker = ensureSrTrackerLocked();
    if (!tracker) {
        stopSrBreathTrackingLocked();
        return;
    }

    auto [dy, confidence, found] = tracker->detectMotion(capture);
    if (!found || confidence < sr_min_confidence_) {
        ++sr_miss_count_;
        sr_edge_hit_streak_ = 0;
        if (!sr_scope_active_ && now > sr_probe_until_) {
            stopSrBreathTrackingLocked();
        } else if (sr_scope_active_ && sr_last_confirmed_edge_time_ > 0.0 &&
            now - sr_last_confirmed_edge_time_ >= std::max(0.1, sr_scope_lost_seconds_)) {
            stopSrBreathTrackingLocked();
        }
        return;
    }

    sr_miss_count_ = 0;
    ++sr_edge_hit_streak_;
    const int confirm_frames = std::max(1, sr_scope_confirm_frames_);
    if (sr_edge_hit_streak_ >= confirm_frames) {
        if (!sr_scope_active_) {
            tracker->reset();
            sr_edge_hit_streak_ = 0;
            sr_scope_active_ = true;
            sr_last_confirmed_edge_time_ = now;
            return;
        }
        sr_last_confirmed_edge_time_ = now;
    } else if (sr_scope_active_) {
        return;
    }

    if (!sr_scope_active_) {
        if (now > sr_probe_until_) {
            stopSrBreathTrackingLocked();
        }
        return;
    }

    int move = static_cast<int>(std::round((sr_invert_y_ ? -dy : dy) * sr_move_scale_));
    const int max_step = std::max(1, sr_max_step_);
    move = std::max(-max_step, std::min(max_step, move));
    if (move != 0) InputController::moveMouseRelative(0, move);
#else
    (void)capture;
#endif
}

} // namespace pubg
