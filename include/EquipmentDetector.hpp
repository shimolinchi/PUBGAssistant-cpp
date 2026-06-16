#pragma once

#include "RegionManager.hpp"
#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

namespace pubg {

// 装备栏识别模块，对应 Python 版 modules/equipment_detector.py。
// 它识别 Tab 装备栏中的 1/2 号武器名和配件，用于限定手持武器候选和计算压枪倍率。
class EquipmentDetector {
public:
    // 扫描完成后的回调。map key 为武器槽位 1/2。
    using Callback = std::function<void(const std::map<int, WeaponSlotInfo>&)>;

    // 预留状态回调，后续可用于通知装备栏 open/closed/idle。
    using StatusCallback = std::function<void(const std::string&)>;

    // fps 控制后台扫描频率；idle_timeout 用于装备栏长时间不可见后清空结果。
    EquipmentDetector(Config& config, RegionManager& regions, int fps = 30, double idle_timeout = 10.0);

    // 停止后台线程。
    ~EquipmentDetector();

    // 开关装备栏后台扫描。cb 非空时更新扫描结果回调。
    void setEnabled(bool enabled, Callback cb = {});

    // 设置装备栏打开/关闭/确认状态回调，对应 Python on_status_change。
    void setStatusCallback(StatusCallback cb);

    // Tab 按键触发的立即扫描入口。
    // 对应 Python 版 on_tab_press，用于打开装备栏时马上识别一次。
    void onTabPress();

    // 请求 worker 线程在下一帧强制扫描一次（鼠标左键松开等外部事件触发）。
    void requestScan();

    // 获取最近一次识别到的两个武器槽位信息。
    [[nodiscard]] std::map<int, WeaponSlotInfo> currentWeapons() const;

private:
    // 判断装备栏中是否能看到任意武器编号，用作“装备栏是否打开”的轻量开关。
    bool detectAnyNumber(ScreenCapture& capture);

    // 检测指定槽位的编号区域是否匹配 1/2 号编号模板。
    bool detectWeaponNumber(ScreenCapture& capture, int slot);

    // 完整识别某个槽位：武器名、倍镜、握把、枪口、枪托。
    WeaponSlotInfo detectWeapon(ScreenCapture& capture, int slot);

    // 识别当前装备栏两个槽位，只用识别到武器名的结果覆盖 current_（空结果不覆盖），
    // 有更新时回调通知。供 onTabPress 和后台上升沿复用。
    void scanCurrentEquipment(ScreenCapture& capture);

    // 后台循环：装备栏可见时持续识别，不可见超过 idle_timeout 后清空。
    void run();

    // 根据 region_scaling_settings 返回武器名称 ROI 的缩放目标尺寸。
    std::pair<int, int> nameTargetSize(const std::string& region_key) const;

    // 外部依赖：配置提供模板路径和缩放尺寸；RegionManager 提供装备栏各截图区域。
    Config& config_;
    RegionManager& regions_;

    // 后台扫描节奏和装备栏不可见后的清空时间。
    int fps_ = 15;
    double idle_timeout_ = 10.0;

    // 各类别模板匹配阈值。名称阈值和配件阈值分开，便于后续调参。
    static double thresholdFor(const std::string& key);

    // 装备栏模板缓存：武器名走灰度模板，配件走带 mask 的彩色模板，编号用于判断装备栏打开。
    std::unordered_map<std::string, std::vector<cv::Mat>> name_templates_;
    std::unordered_map<std::string, std::vector<MaskedTemplate>> scope_templates_;
    std::unordered_map<std::string, std::vector<MaskedTemplate>> grip_templates_;
    std::unordered_map<std::string, std::vector<MaskedTemplate>> muzzle_templates_;
    std::unordered_map<std::string, std::vector<MaskedTemplate>> stock_templates_;
    std::unordered_map<int, cv::Mat> number_templates_;

    // 后台线程、回调和最近一次稳定识别结果。
    mutable std::mutex mutex_;
    std::atomic_bool enabled_{false};
    std::atomic_bool stop_{false};
    std::atomic_bool active_{false};
    // 外部请求强制扫描一次（Tab 打开、鼠标左键松开）。worker 线程消费后清零。
    std::atomic_bool scan_requested_{false};
    std::thread worker_;
    Callback callback_;
    StatusCallback status_callback_;
    std::map<int, WeaponSlotInfo> current_;
    std::string last_status_ = "closed";

    // 最近一次检测到装备栏编号的时间，用于 idle_timeout 清空逻辑。
    double last_detected_time_ = 0.0;
    double confirming_until_time_ = 0.0;
    int consecutive_no_numbers_ = 0;

    // 线程安全地复制当前回调，避免后台线程和 UI 线程同时访问 std::function。
    Callback callbackCopy() const;

    // 状态变化去重后通知 UI。
    void emitStatus(const std::string& status);
};

} // namespace pubg
