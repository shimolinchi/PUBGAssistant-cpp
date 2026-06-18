#include "EquipmentDetector.hpp"

#include <thread>

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
                cv::Mat resized;
                cv::resize(img, resized, {28, 28});
                number_templates_[i] = resized;
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

double EquipmentDetector::thresholdFor(const std::string& key) {
    if (key == "names") return 0.55;
    if (key == "scopes") return 0.65;
    if (key == "grips") return 0.4;
    if (key == "muzzles") return 0.4;
    if (key == "stocks") return 0.4;
    return 0.0;
}

void EquipmentDetector::setEnabled(bool enabled, Callback cb) {
    if (cb) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(cb);
    }
    if (enabled && !enabled_) {
        stop_ = false;
        enabled_ = true;
        active_ = false;
        ++session_id_;
        scan_requested_ = false;
        {
            std::lock_guard lock(mutex_);
            confirming_until_time_ = 0.0;
            consecutive_no_numbers_ = 0;
        }
        emitStatus("closed");
        worker_ = std::thread(&EquipmentDetector::run, this);
    } else if (!enabled && enabled_) {
        stop_ = true;
        enabled_ = false;
        active_ = false;
        ++session_id_;
        scan_requested_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::lock_guard lock(mutex_);
            confirming_until_time_ = 0.0;
            consecutive_no_numbers_ = 0;
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
    cv::Mat resized;
    cv::resize(bgr, resized, {32, 32});
    bgr = resized;
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
            cv::Mat resized;
            cv::resize(bgr, resized, {w, h});
            bgr = resized;
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            auto [name, score] = TemplateMatcher::matchGray(gray, name_templates_);
            info.name_score = score;
            if (score >= thresholdFor("names")) {
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
        cv::Mat resized;
        cv::resize(bgr, resized, {50, 50});
        bgr = resized;
        return TemplateMatcher::matchMasked(bgr, templates);
    };

    auto [scope, scope_score] = detectPart("scope", scope_templates_);
    if (scope_score >= thresholdFor("scopes")) info.scope = scope;
    info.scope_score = scope_score;

    auto [grip, grip_score] = detectPart("grip", grip_templates_);
    if (grip_score >= thresholdFor("grips")) info.grip = grip;
    info.grip_score = grip_score;

    auto [muzzle, muzzle_score] = detectPart("muzzle", muzzle_templates_);
    if (muzzle_score >= thresholdFor("muzzles")) info.muzzle = muzzle;
    info.muzzle_score = muzzle_score;

    auto [stock, stock_score] = detectPart("stock", stock_templates_);
    if (stock_score >= thresholdFor("stocks")) info.stock = stock;
    info.stock_score = stock_score;

    return info;
}

void EquipmentDetector::requestEquipmentConfirmation() {
    if (!enabled_) {
        return;
    }

    active_ = true;
    ++session_id_;
    scan_requested_ = true;
    {
        std::lock_guard lock(mutex_);
        confirming_until_time_ = nowSeconds() + 1.2;
        consecutive_no_numbers_ = 0;
    }
    emitStatus("confirming");
}

void EquipmentDetector::requestScan() {
    // 鼠标左键松开等外部事件触发：只在装备栏激活期间强制扫一次。
    if (enabled_ && active_) {
        scan_requested_.store(true);
    }
}

void EquipmentDetector::run() {
    ScreenCapture capture;
    bool was_open = false;
    while (!stop_) {
        const double start = nowSeconds();
        if (!active_) {
            was_open = false;
            scan_requested_.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
            continue;
        }
        const std::uint64_t session_id = session_id_.load();

        const bool n1 = detectWeaponNumber(capture, 1);
        const bool n2 = detectWeaponNumber(capture, 2);
        if (!active_ || session_id != session_id_.load()) {
            was_open = false;
            scan_requested_.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
            continue;
        }
        const bool equipment_open = n1 || n2;
        const bool forced = scan_requested_.exchange(false);

        if (equipment_open) {
            double confirming_until = 0.0;
            {
                std::lock_guard lock(mutex_);
                confirming_until = confirming_until_time_;
                confirming_until_time_ = 0.0;
                last_detected_time_ = nowSeconds();
                consecutive_no_numbers_ = 0;
            }
            if (!active_ || session_id != session_id_.load()) {
                was_open = false;
                continue;
            }
            emitStatus("opened");
            if (!was_open || forced || confirming_until > start) {
                scanCurrentEquipment(capture, session_id);
            }
        } else {
            double confirming_until = 0.0;
            int missing_count = 0;
            {
                std::lock_guard lock(mutex_);
                confirming_until = confirming_until_time_;
                if (confirming_until <= nowSeconds()) {
                    missing_count = ++consecutive_no_numbers_;
                }
            }
            const double now = nowSeconds();
            if (confirming_until > now) {
                scan_requested_ = true;
                if (active_ && session_id == session_id_.load()) {
                    emitStatus("confirming");
                }
            } else if (missing_count >= 4) {
                active_ = false;
                ++session_id_;
                scan_requested_ = false;
                {
                    std::lock_guard lock(mutex_);
                    confirming_until_time_ = 0.0;
                    consecutive_no_numbers_ = 0;
                }
                emitStatus("closed");
            } else {
                if (active_ && session_id == session_id_.load()) {
                    emitStatus("confirming");
                }
            }
        }
        was_open = equipment_open;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMsForFps(fps_, nowSeconds() - start)));
    }
}

void EquipmentDetector::scanCurrentEquipment(ScreenCapture& capture, std::uint64_t session_id) {
    if (!active_ || session_id != session_id_.load()) {
        return;
    }
    WeaponSlotInfo slot1;
    WeaponSlotInfo slot2;
    std::thread slot2_worker([&] {
        ScreenCapture slot2_capture;
        slot2 = detectWeapon(slot2_capture, 2);
    });
    slot1 = detectWeapon(capture, 1);
    slot2_worker.join();
    if (!active_ || session_id != session_id_.load()) {
        return;
    }
    // 只用识别到武器名的槽位覆盖旧结果；空结果（多为装备栏开合过渡帧）保留上次的好数据，
    // 避免“再次按 Tab 关闭时武器/配件凭空消失”。
    bool changed = false;
    std::map<int, WeaponSlotInfo> snapshot;
    {
        std::lock_guard lock(mutex_);
        if (!slot1.name.empty()) { current_[1] = slot1; changed = true; }
        if (!slot2.name.empty()) { current_[2] = slot2; changed = true; }
        if (changed) {
            last_detected_time_ = nowSeconds();
            snapshot = current_;
        }
    }
    if (changed) {
        if (auto cb = callbackCopy()) {
            cb(snapshot);
        }
    }
}

} // namespace pubg
