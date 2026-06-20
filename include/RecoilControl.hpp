#pragma once

#include "Config.hpp"
#include "InputController.hpp"
#include "RegionManager.hpp"
#include "ScopeMotionTracker.hpp"

namespace pubg {

// 辅助压枪模块，对应 Python 版 modules/recoil_control.py 的主要压枪曲线逻辑。
// 它读取 recoil_settings，根据当前武器、配件、姿势计算移动量，并用 SendInput 移动鼠标。
class RecoilControl {
public:
    // 构造时读取 recoil_settings，并启动后台 workerLoop。
    explicit RecoilControl(Config& config, RegionManager* regions = nullptr);

    // 停止后台线程并释放 fire_key，避免退出时卡键。
    ~RecoilControl();

    // 开关压枪位移。关闭时会停止 firing 状态并释放 fire_key。
    void setEnabled(bool enabled);
    void setFeatureEnabled(const std::string& key, bool enabled);

    // 更新当前手持武器。
    // 会从 config.json 中取该武器的 recoil_curve、type、auto_fire 等配置。
    void updateCurrentWeapon(const std::string& weapon);

    // 更新当前武器的配件信息。
    // 配件倍率来自 scope/grip/muzzle/stock_multipliers。
    void updateAttachments(const WeaponSlotInfo& info);

    // 更新当前姿势 stand/squat/lie，用于 stance_multipliers。
    void updateStance(const std::string& stance);

    // 重新读取 recoil_settings。后续做调试窗口时可热重载参数。
    void reloadConfig();

    // 玩家物理按下 Q/E 时更新 SG 闪身喷方向。0=无，1=左，2=右。
    void setSgPeekDirection(int direction);
    [[nodiscard]] int sgPeekDirection() const;
    [[nodiscard]] bool shouldShowSgPeekDirection() const;
    void hideSgPeekDirection();

    // 当前是否处于压枪开火状态。App 用它避免开火中武器识别抖动打断压枪。
    [[nodiscard]] bool isFiring() const;

private:
    // 从 Config 中读取 fire_key、曲线步长、武器曲线、姿势倍率和配件倍率。
    void loadConfig();

    // 把 JSON 中的数字或数组统一转成 double 曲线。
    // Python 版既支持单值也支持列表，这里保持兼容。
    static std::vector<double> normalizeCurve(const Json& value, double fallback);

    // 按 elapsed 和 step 对曲线做线性采样。
    // 例如 recoil_curve_step=0.4 表示每 0.4 秒走到下一个曲线点。
    static double sampleCurve(const std::vector<double>& curve, double elapsed, double step);

    // 根据 key 读取某类倍率曲线并采样；key 为空或不存在时返回 1.0。
    double multiplier(const std::unordered_map<std::string, std::vector<double>>& map, const std::string& key, double elapsed) const;

    // 计算当前时刻总压枪强度：
    // 武器曲线 * 倍镜倍率 * 握把倍率 * 枪口倍率 * 枪托倍率 * 姿势倍率。
    double calculateStrength(double elapsed);

    // 后台控制循环：
    // 轮询左键状态，同步 fire_key，并在压枪开启时按曲线移动鼠标。
    void workerLoop();
    std::string scopeEdgeRegionName() const;
    void startSrBreathTrackingLocked();
    ScopeMotionTracker* ensureSrTrackerLocked();
    void stopSrBreathTrackingLocked();
    void applySrBreathControl(ScreenCapture& capture);
    void triggerSgQuickPeekShot(int direction);

    // 外部依赖：读取 recoil_settings 和 hotkeys.fire_key。
    Config& config_;
    RegionManager* regions_ = nullptr;

    // 后台 worker 生命周期和总开关。
    std::atomic_bool running_{true};
    std::atomic_bool enabled_{false};
    std::thread worker_;

    // 运行中共享状态保护。
    mutable std::mutex mutex_;

    // 基础压枪配置：开火键、每次移动间隔、曲线采样间隔。
    int fire_vk_ = VK_END;
    int hip_aim_vk_ = VK_RBUTTON;
    double recoil_delay_ = 0.02;
    double recoil_curve_step_ = 0.4;

    // 从 config.json 加载的武器曲线、姿势倍率和配件倍率曲线。
    std::unordered_map<std::string, Json> weapon_configs_;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> stance_multipliers_;
    std::unordered_map<std::string, std::vector<double>> scope_curves_;
    std::unordered_map<std::string, std::vector<double>> grip_curves_;
    std::unordered_map<std::string, std::vector<double>> muzzle_curves_;
    std::unordered_map<std::string, std::vector<double>> stock_curves_;

    // 当前武器/配件/姿势状态。由 WeaponDetector、EquipmentDetector、GestureIdentifier 回调更新。
    std::string current_weapon_;
    std::string weapon_type_ = "ar";
    std::vector<double> recoil_curve_;
    bool auto_fire_ = false;
    std::string scope_ = "hip";
    std::string grip_;
    std::string muzzle_;
    std::string stock_;
    std::string stance_ = "stand";

    // 当前是否正在左键开火，以及本轮开火开始时间。
    bool firing_ = false;
    double fire_start_ = 0.0;
    int sg_peek_direction_ = 0;
    bool sg_peek_visible_ = false;
    bool auto_recoil_enabled_ = true;
    bool dmr_tap_enabled_ = true;
    bool sr_breath_control_enabled_ = true;
    bool sg_quick_peek_enabled_ = true;
    std::atomic_bool sg_action_running_{false};
    std::thread sg_action_thread_;
    bool sr_breath_enabled_ = false;
    double sr_scope_ready_time_ = 0.0;
    std::unique_ptr<ScopeMotionTracker> sr_tracker_;
    double sr_scope_delay_ = 0.6;
    double sr_track_interval_ = 1.0 / 60.0;
    double sr_move_scale_ = 1.0;
    int sr_max_step_ = 12;
    double sr_max_edge_delta_ = 5.0;
    int sr_miss_limit_ = 3;
    double sr_min_confidence_ = 0.25;
    bool sr_invert_y_ = true;
    Json sr_tracker_config_ = Json::object();
    double sr_probe_seconds_ = 2.0;
    double sr_scope_lost_seconds_ = 2.0;
    int sr_scope_confirm_frames_ = 3;
    int sr_miss_count_ = 0;
    int sr_edge_hit_streak_ = 0;
    bool sr_scope_active_ = false;
    bool sr_resume_after_movement_ = false;
    double sr_probe_until_ = 0.0;
    double sr_last_confirmed_edge_time_ = 0.0;
    double sr_last_track_time_ = 0.0;
    std::string sr_tracker_region_name_;
};

} // namespace pubg
