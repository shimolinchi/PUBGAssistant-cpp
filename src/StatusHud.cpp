#include "StatusHud.hpp"

#include "AiAimHudIndicator.hpp"
#include "BuildConfig.hpp"
#include "StatusHudLayout.hpp"

#include <algorithm>
#include <sstream>

namespace pubg {

namespace {
const std::unordered_map<std::string, std::string> kSpecialNames{
    {"Rocket", "火箭筒"}, {"Grenade", "投掷物"}, {"VSS", "VSS"},
    {"Crossbow", "十字弩"}, {"C4", "C4"}, {"Mortar", "迫击炮"},
};

const std::unordered_map<std::string, std::string> kScopeNames{
    {"red_dot", "红点"}, {"holographic", "全息"}, {"2", "二倍"}, {"3", "三倍"},
    {"4", "四倍"}, {"6", "六倍"}, {"8", "八倍"}, {"multiple", "蛤蟆"},
};

const std::unordered_map<std::string, std::string> kGripNames{
    {"vertical", "垂直"}, {"half", "半截"}, {"tilted", "斜向"},
    {"light", "轻握"}, {"laser", "激光"}, {"thumb", "拇指"},
};

const std::unordered_map<std::string, std::string> kMuzzleNames{
    {"ar_dmr_compensator", "步枪补偿"}, {"ar_dmr_suppressor", "步枪消焰"},
    {"ar_dmr_silencer", "步枪消音"}, {"ar_dmr_braker", "制退"},
    {"dmr_sr_compensator", "狙补偿"}, {"dmr_sr_suppressor", "狙消焰"},
    {"dmr_sr_silencer", "狙消音"}, {"smg_compensator", "冲锋补偿"},
    {"smg_suppressor", "冲锋消焰"}, {"smg_silencer", "冲锋消音"},
};

const std::unordered_map<std::string, std::string> kStockNames{
    {"tactical", "战术"}, {"heavy", "重型"}, {"uzi", "微托"}, {"cheek_pad", "托腮板"},
};

const std::unordered_map<std::string, std::string> kStanceNames{
    {"stand", "站立"}, {"squat", "蹲下"}, {"lie", "趴下"},
};

std::string mapped(const std::unordered_map<std::string, std::string>& map, const std::string& value) {
    if (auto it = map.find(value); it != map.end()) {
        return it->second;
    }
    return value;
}

} // namespace

StatusHud::StatusHud(Config& config, RegionManager& regions) : config_(config), regions_(regions) {
    marker_hex_ = config_.markerHex();
    equipment_[1] = {};
    equipment_[2] = {};
    regions_.createOverlay(overlay_, L"PUBGAssistant Status", true);
    message_worker_ = std::thread(&StatusHud::messageLoop, this);
    render();
}

StatusHud::~StatusHud() {
    {
        std::lock_guard lock(message_mutex_);
        message_stop_ = true;
    }
    message_cv_.notify_all();
    if (message_worker_.joinable()) {
        message_worker_.join();
    }
}

void StatusHud::setSwitches(bool weapon_detection, bool display, bool recoil, bool ai_aim) {
    {
        std::lock_guard lock(mutex_);
        weapon_detection_enabled_ = weapon_detection;
        display_enabled_ = display;
        recoil_enabled_ = recoil;
        ai_aim_enabled_ = ai_aim;
        if (!display || !ai_aim) ai_detection_frame_.reset();
        if (!ai_aim) ai_recognition_frequency_text_ = aiAimRecognitionFrequencyText(false, 0.0);
    }
    render();
}

void StatusHud::setAiSnapshot(const ai_aim::AimSnapshot& snapshot,
                            bool boxes_enabled, int max_frame_age_ms) {
    {
        std::lock_guard lock(mutex_);
        auto frame = snapshot.detection_frame;
        const auto frequency_text = aiAimRecognitionFrequencyText(ai_aim_enabled_, snapshot.recognition_hz);
        const auto now = ai_aim::Clock::now();
        if (!boxes_enabled || !display_enabled_ || !ai_aim_enabled_ ||
            (frame && (frame->timestamp > now || now - frame->timestamp >
                std::chrono::milliseconds(std::clamp(max_frame_age_ms, 1, 150))))) {
            frame.reset();
        }
        if (ai_detection_frame_ == frame && ai_boxes_enabled_ == boxes_enabled &&
            ai_frame_max_age_ms_ == max_frame_age_ms &&
            ai_recognition_frequency_text_ == frequency_text) return;
        ai_detection_frame_ = std::move(frame);
        ai_recognition_frequency_text_ = frequency_text;
        ai_boxes_enabled_ = boxes_enabled;
        ai_frame_max_age_ms_ = max_frame_age_ms;
    }
    render();
}

void StatusHud::setEquipmentStatus(const std::string& status) {
    {
        std::lock_guard lock(mutex_);
        equipment_status_ = status;
    }
    render();
}

void StatusHud::setEquipment(const std::map<int, WeaponSlotInfo>& equipment_slots) {
    {
        std::lock_guard lock(mutex_);
        equipment_ = equipment_slots;
    }
    render();
}

void StatusHud::setCurrentWeapon(const std::string& weapon) {
    {
        std::lock_guard lock(mutex_);
        current_weapon_ = weapon;
    }
    render();
}

void StatusHud::setStance(const std::string& stance) {
    {
        std::lock_guard lock(mutex_);
        current_stance_ = stance;
    }
    render();
}

void StatusHud::setPeekDirection(int direction, bool visible) {
    {
        std::lock_guard lock(mutex_);
        peek_direction_ = direction >= 1 && direction <= 2 ? direction : 0;
        peek_direction_visible_ = visible;
    }
    render();
}

void StatusHud::setMarkerColor(const std::string& color) {
    {
        std::lock_guard lock(mutex_);
        marker_color_ = color;
    }
    render();
}

void StatusHud::reloadMarkerHex() {
    {
        std::lock_guard lock(mutex_);
        marker_hex_ = config_.markerHex();
    }
    render();
}

void StatusHud::setMarkerIndicatorVisible(bool visible) {
    {
        std::lock_guard lock(mutex_);
        marker_indicator_visible_ = visible;
    }
    render();
}

void StatusHud::showTemporaryMessage(const std::string& text, int duration_ms, const std::string& color_hex) {
    {
        std::lock_guard lock(message_mutex_);
        temporary_message_ = text;
        temporary_message_color_hex_ = color_hex;
        message_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, duration_ms));
    }
    message_cv_.notify_all();
    render();
}

void StatusHud::clearTemporaryMessageWithPrefix(const std::string& prefix) {
    bool cleared = false;
    {
        std::lock_guard lock(message_mutex_);
        if (!temporary_message_.empty() &&
            temporary_message_.rfind(prefix, 0) == 0) {
            temporary_message_.clear();
            message_deadline_ = {};
            cleared = true;
        }
    }
    if (!cleared) {
        return;
    }
    message_cv_.notify_all();
    render();
}

void StatusHud::messageLoop() {
    std::unique_lock lock(message_mutex_);
    while (!message_stop_) {
        if (message_deadline_ == std::chrono::steady_clock::time_point{}) {
            message_cv_.wait(lock, [this] {
                return message_stop_ || message_deadline_ != std::chrono::steady_clock::time_point{};
            });
            continue;
        }
        const auto deadline = message_deadline_;
        if (message_cv_.wait_until(lock, deadline, [this, deadline] {
                return message_stop_ || message_deadline_ != deadline;
            })) {
            continue;
        }
        message_deadline_ = {};
        temporary_message_.clear();
        lock.unlock();
        render();
        lock.lock();
    }
}

std::string StatusHud::displayWeaponName(const std::string& name) const {
    if (name.empty()) {
        return "无";
    }
    return mapped(kSpecialNames, name);
}

std::string StatusHud::formatWeapon(const WeaponSlotInfo& info) const {
    std::vector<std::string> parts;
    parts.push_back(displayWeaponName(info.name));
    if (!info.scope.empty()) parts.push_back(mapped(kScopeNames, info.scope));
    if (!info.grip.empty()) parts.push_back(mapped(kGripNames, info.grip));
    if (!info.muzzle.empty()) parts.push_back(mapped(kMuzzleNames, info.muzzle));
    if (!info.stock.empty()) parts.push_back(mapped(kStockNames, info.stock));

    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out << " | ";
        out << parts[i];
    }
    return out.str();
}

cv::Scalar StatusHud::statusColor(const std::string& name) const {
    if (name == "red") return hexToBgr("#E74C3C");
    if (name == "white") return hexToBgr("#FFFFFF");
    auto it = marker_hex_.find(name);
    if (it != marker_hex_.end()) {
        return hexToBgr(it->second);
    }
    return hexToBgr("#FFFFFF");
}

void StatusHud::render() {
    const auto render_revision = render_publication_.issue();
    std::string temporary_message;
    std::string temporary_message_color_hex;
    {
        std::lock_guard lock(message_mutex_);
        temporary_message = temporary_message_;
        temporary_message_color_hex = temporary_message_color_hex_;
    }
    std::vector<OverlayCommand> cmds;
    {
        std::lock_guard lock(mutex_);
        const Json display_cfg = config_.read([](const Json& data) {
            return data.value("ui_state", Json::object()).value("display", Json::object());
        });
        const AiAimTargetOverlayState target_state{
            display_enabled_, ai_aim_enabled_, ai_boxes_enabled_,
            regions_.screenLeft(), regions_.screenTop(),
            regions_.screenWidth(), regions_.screenHeight(), ai_frame_max_age_ms_};
        for (const auto& outline : aiAimTargetOutlines(ai_detection_frame_.get(), target_state)) {
            cmds.push_back({OverlayCommand::Type::Rect,
                outline.left, outline.top, outline.right, outline.bottom,
                0, "", cv::Scalar(0, outline.green, outline.red),
                outline.width, 18, outline.alpha});
        }
        const double base_x = 35.0;
        const double top_y = static_cast<double>(regions_.screenHeight()) - 300.0;
        const auto layout = statusHudLayout(display_cfg.value("status_font_size", 13.0),
                                            display_cfg.value("status_line_gap", 25.0));
        const double font = layout.font_size;
        const double line_gap = layout.line_gap;
        const int text_alpha = std::clamp(static_cast<int>(std::round(display_cfg.value("status_text_alpha", 1.0) * 255.0)), 0, 255);
        double y = top_y + 5.0;

        if (marker_indicator_visible_) {
            const auto hex = marker_hex_.count(marker_color_) ? marker_hex_[marker_color_] : "#FFFFFF";
            const auto bgr = hexToBgr(hex);
            cmds.push_back({OverlayCommand::Type::Text, base_x, y, 0, 0, 0, "当前使用标点：", bgr, 1, font, text_alpha});
            cmds.push_back({OverlayCommand::Type::Circle, base_x + layout.marker_circle_offset, y + font * 9.0 / 13.0, 0, 0, 6.0, "", bgr, 2, 18, text_alpha});
        }

        y = top_y + layout.switches_y;
        double x = base_x;
        cmds.push_back({OverlayCommand::Type::Text, x, y, 0, 0, 0, "识别", weapon_detection_enabled_ ? statusColor("Green") : statusColor("red"), 1, font, text_alpha});
        x += layout.switch_stride;
        cmds.push_back({OverlayCommand::Type::Text, x, y, 0, 0, 0, "测距", display_enabled_ ? statusColor("Green") : statusColor("red"), 1, font, text_alpha});
        x += layout.switch_stride;
#if PUBG_ENABLE_INPUT_CONTROL
        cmds.push_back({OverlayCommand::Type::Text, x, y, 0, 0, 0, "压枪", recoil_enabled_ ? statusColor("Green") : statusColor("red"), 1, font, text_alpha});
        x += layout.switch_stride;
        const auto ai_state = aiAimHudState(ai_aim_enabled_, current_weapon_);
        const auto ai_color = hexToBgr(aiAimHudColorHex(ai_state));
        cmds.push_back({OverlayCommand::Type::Text, x, y, 0, 0, 0, kAiAimHudLabel, ai_color, 1, font, text_alpha});
        x += layout.switch_stride;
#endif

        std::string eq_text = "装备栏关闭";
        cv::Scalar eq_color = statusColor("Blue");
        if (equipment_status_ == "opened") {
            eq_text = "武器识别中";
            eq_color = statusColor("Green");
        } else if (equipment_status_ == "confirming") {
            eq_text = "正在确认中";
            eq_color = statusColor("Orange");
        }
        cmds.push_back({OverlayCommand::Type::Text, x, y, 0, 0, 0, eq_text, eq_color, 1, font, text_alpha});

        y = top_y + layout.weapons_y;
        const auto white = statusColor("white");
        const auto w1 = equipment_.count(1) ? equipment_.at(1) : WeaponSlotInfo{};
        const auto w2 = equipment_.count(2) ? equipment_.at(2) : WeaponSlotInfo{};
        cmds.push_back({OverlayCommand::Type::Text, base_x, y, 0, 0, 0, "武器1: " + formatWeapon(w1), white, 1, font, text_alpha});
        y += line_gap;
        cmds.push_back({OverlayCommand::Type::Text, base_x, y, 0, 0, 0, "武器2: " + formatWeapon(w2), white, 1, font, text_alpha});
        y += line_gap;
        const std::string pose = current_stance_.empty() ? "未知" : mapped(kStanceNames, current_stance_);
        const std::string peek = peek_direction_ == 1 ? "左" : (peek_direction_ == 2 ? "右" : "???");
        std::string current_line = "当前: " + displayWeaponName(current_weapon_) + " | " + pose;
        if (peek_direction_visible_) {
            current_line += " | " + peek;
        }
#if PUBG_ENABLE_INPUT_CONTROL
        current_line += " | " + ai_recognition_frequency_text_;
#endif
        cmds.push_back({OverlayCommand::Type::Text, base_x, y, 0, 0, 0,
                        current_line,
                        white, 1, font, text_alpha});

        if (!temporary_message.empty()) {
            const double box_w = 360.0;
            const double box_h = 46.0;
            const double box_x = regions_.screenWidth() / 2.0 - box_w / 2.0;
            const double box_y = regions_.screenHeight() * 0.8 - box_h;
            const auto accent = hexToBgr(temporary_message_color_hex);
            cmds.push_back({OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                            10.0, "", accent, 0, 18, 89});
            cmds.push_back({OverlayCommand::Type::RoundedRect, box_x, box_y, box_x + box_w, box_y + box_h,
                            10.0, "", accent, 2, 18, 204});
            cmds.push_back({OverlayCommand::Type::TextCenter, box_x, box_y, box_x + box_w, box_y + box_h,
                            0.0, temporary_message, hexToBgr("#FFFFFF"), 1, 18, 255});
        }
    }
    (void)render_publication_.publishIfLatest(render_revision, [this, &cmds] {
        overlay_.setCommands(std::move(cmds));
    });
    overlay_.pumpMessages();
}

} // namespace pubg
