#include "EquipmentDetector.hpp"

namespace pubg {

EquipmentDetector::EquipmentDetector(Config& config, RegionManager& regions, int fps, double idle_timeout)
    : config_(config), regions_(regions), fps_(fps), idle_timeout_(idle_timeout) {
    const auto root = config_.paths().templatesDir() / "equipments";
    name_templates_ = TemplateMatcher::loadGrayTemplates(root / "names");
    scope_templates_ = TemplateMatcher::loadMaskedTemplates(root / "scopes");
    grip_templates_ = TemplateMatcher::loadMaskedTemplates(root / "grips");
    muzzle_templates_ = TemplateMatcher::loadMaskedTemplates(root / "muzzles");
    stock_templates_ = TemplateMatcher::loadMaskedTemplates(root / "stocks");
    for (int i = 1; i <= 2; ++i) {
        const auto dir = root / "numbers" / std::to_string(i);
        if (!std::filesystem::exists(dir)) {
            continue;
        }
        for (const auto& file : std::filesystem::directory_iterator(dir)) {
            if (file.path().extension() != ".png") {
                continue;
            }
            cv::Mat img = cv::imread(file.path().string(), cv::IMREAD_GRAYSCALE);
            if (!img.empty()) {
                cv::resize(img, img, {28, 28});
                number_templates_[i] = img;
                break;
            }
        }
    }
    current_[1] = {};
    current_[2] = {};
}

EquipmentDetector::~EquipmentDetector() {
    setEnabled(false);
}

void EquipmentDetector::setEnabled(bool enabled, Callback cb) {
    if (cb) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(cb);
    }
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        emitStatus("closed");
        worker_ = std::thread(&EquipmentDetector::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        emitStatus("closed");
    }
}

void EquipmentDetector::setStatusCallback(StatusCallback cb) {
    std::lock_guard lock(mutex_);
    status_callback_ = std::move(cb);
}

EquipmentDetector::Callback EquipmentDetector::callbackCopy() const {
    std::lock_guard lock(mutex_);
    return callback_;
}

void EquipmentDetector::emitStatus(const std::string& status) {
    StatusCallback cb;
    {
        std::lock_guard lock(mutex_);
        if (status == last_status_) {
            return;
        }
        last_status_ = status;
        cb = status_callback_;
    }
    if (cb) {
        cb(status);
    }
}

std::map<int, WeaponSlotInfo> EquipmentDetector::currentWeapons() const {
    std::lock_guard lock(mutex_);
    return current_;
}

std::pair<int, int> EquipmentDetector::nameTargetSize(const std::string& region_key) const {
    int w = 237;
    int h = 36;
    if (!name_templates_.empty() && !name_templates_.begin()->second.empty()) {
        w = name_templates_.begin()->second.front().cols;
        h = name_templates_.begin()->second.front().rows;
    }
    config_.read([&](const Json& data) {
        if (data.contains("region_scaling_settings") && data["region_scaling_settings"].contains(region_key)) {
            const auto& r = data["region_scaling_settings"][region_key];
            w = r.value("width", w);
            h = r.value("height", h);
        }
    });
    return {w, h};
}

bool EquipmentDetector::detectWeaponNumber(ScreenCapture& capture, int slot) {
    const auto rect = regions_.getRealRegion("weapon" + std::to_string(slot) + "_number_region");
    if (!rect || number_templates_.empty()) {
        return false;
    }
    cv::Mat bgr = capture.grabBgr(*rect);
    if (bgr.empty()) {
        return false;
    }
    cv::resize(bgr, bgr, {32, 32});
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    for (const auto& [_, tpl] : number_templates_) {
        if (tpl.rows > gray.rows || tpl.cols > gray.cols) {
            continue;
        }
        cv::Mat res;
        cv::matchTemplate(gray, tpl, res, cv::TM_CCOEFF_NORMED);
        double max_val = 0.0;
        cv::minMaxLoc(res, nullptr, &max_val);
        if (max_val >= 0.5) {
            return true;
        }
    }
    return false;
}

bool EquipmentDetector::detectAnyNumber(ScreenCapture& capture) {
    return detectWeaponNumber(capture, 1) || detectWeaponNumber(capture, 2);
}

WeaponSlotInfo EquipmentDetector::detectWeapon(ScreenCapture& capture, int slot) {
    WeaponSlotInfo info;
    const std::string prefix = "weapon" + std::to_string(slot);
    const std::string name_region = prefix + "_name_region";
    if (const auto rect = regions_.getRealRegion(name_region)) {
        cv::Mat bgr = capture.grabBgr(*rect);
        if (!bgr.empty()) {
            auto [w, h] = nameTargetSize(name_region);
            cv::resize(bgr, bgr, {w, h});
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            auto [name, score] = TemplateMatcher::matchGray(gray, name_templates_);
            info.name_score = score;
            if (score >= thresholds_["names"]) {
                info.name = name;
            }
        }
    }

    if (info.name.empty()) {
        return info;
    }

    auto detectPart = [&](const std::string& key,
                          const std::unordered_map<std::string, std::vector<MaskedTemplate>>& templates) {
        const auto rect = regions_.getRealRegion(prefix + "_" + key + "_region");
        if (!rect) {
            return std::pair<std::string, double>{"", 0.0};
        }
        cv::Mat bgr = capture.grabBgr(*rect);
        if (bgr.empty()) {
            return std::pair<std::string, double>{"", 0.0};
        }
        cv::resize(bgr, bgr, {50, 50});
        return TemplateMatcher::matchMasked(bgr, templates);
    };

    auto [scope, scope_score] = detectPart("scope", scope_templates_);
    if (scope_score >= thresholds_["scopes"]) info.scope = scope;
    info.scope_score = scope_score;

    auto [grip, grip_score] = detectPart("grip", grip_templates_);
    if (grip_score >= thresholds_["grips"]) info.grip = grip;
    info.grip_score = grip_score;

    auto [muzzle, muzzle_score] = detectPart("muzzle", muzzle_templates_);
    if (muzzle_score >= thresholds_["muzzles"]) info.muzzle = muzzle;
    info.muzzle_score = muzzle_score;

    auto [stock, stock_score] = detectPart("stock", stock_templates_);
    if (stock_score >= thresholds_["stocks"]) info.stock = stock;
    info.stock_score = stock_score;

    return info;
}

void EquipmentDetector::onTabPress() {
    ScreenCapture capture;
    if (!detectAnyNumber(capture)) {
        {
            std::lock_guard lock(mutex_);
            confirming_until_time_ = nowSeconds() + 0.35;
        }
        emitStatus("confirming");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        confirming_until_time_ = 0.0;
    }
    emitStatus("opened");
    std::map<int, WeaponSlotInfo> next;
    next[1] = detectWeapon(capture, 1);
    next[2] = detectWeapon(capture, 2);
    {
        std::lock_guard lock(mutex_);
        current_ = next;
        last_detected_time_ = nowSeconds();
    }
    if (auto cb = callbackCopy()) {
        cb(next);
    }
}

void EquipmentDetector::run() {
    ScreenCapture capture;
    while (!stop_) {
        const double start = nowSeconds();
        if (detectAnyNumber(capture)) {
            {
                std::lock_guard lock(mutex_);
                confirming_until_time_ = 0.0;
            }
            emitStatus("opened");
            std::map<int, WeaponSlotInfo> next;
            next[1] = detectWeapon(capture, 1);
            next[2] = detectWeapon(capture, 2);
            {
                std::lock_guard lock(mutex_);
                current_ = next;
                last_detected_time_ = nowSeconds();
            }
            if (auto cb = callbackCopy()) {
                cb(next);
            }
        } else {
            double last_seen = 0.0;
            double confirming_until = 0.0;
            {
                std::lock_guard lock(mutex_);
                last_seen = last_detected_time_;
                confirming_until = confirming_until_time_;
            }
            const double now = nowSeconds();
            if (confirming_until > now) {
                emitStatus("confirming");
            } else if (last_seen > 0.0 && now - last_seen > idle_timeout_) {
                {
                    std::lock_guard lock(mutex_);
                    current_[1] = {};
                    current_[2] = {};
                    confirming_until_time_ = 0.0;
                }
                emitStatus("closed");
            } else {
                emitStatus("closed");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

} // namespace pubg
