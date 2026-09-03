#pragma once

#include "MouseMotionDispatcher.hpp"
#include "pubg_ai_aim/AimModule.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace pubg {

namespace detail {
class AiAimMotionSinkPermit;
}

enum class AiAimRuntimeState {
    Off,
    Loading,
    Ready,
    Paused,
    Error,
};

struct AiAimRuntimeStatus {
    AiAimRuntimeState state = AiAimRuntimeState::Off;
    std::string detail = "关闭";
    std::string warning;
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t revision = 0;
};

// 主程序与独立 pubg_ai_aim 库之间的宿主适配层。
// 负责首次开启时异步加载模型、把 App 状态推送给 AimModule，并在任何门控失效时
// 先 fence 鼠标通道，确保关闭/失焦后不会补发旧位移。
class AiAimCoordinator final {
public:
    using StatusCallback = std::function<void(const AiAimRuntimeStatus&)>;
    struct ModuleBuild {
        std::unique_ptr<pubg::ai_aim::AimModule> module;
        std::chrono::milliseconds motion_max_age{80};
        std::string startup_warning;
    };
    using ModuleFactory = std::function<ModuleBuild(
        std::shared_ptr<pubg::ai_aim::IMotionSink>,
        std::stop_token)>;

    AiAimCoordinator(
        std::shared_ptr<MouseMotionDispatcher> dispatcher,
        ModuleFactory module_factory);
    ~AiAimCoordinator();

    AiAimCoordinator(const AiAimCoordinator&) = delete;
    AiAimCoordinator& operator=(const AiAimCoordinator&) = delete;

    // The observer may request lifecycle changes or replace itself. As with
    // AimModule sink callbacks, it must not destroy this coordinator or release
    // its final owner until the callback has returned.
    void setStatusCallback(StatusCallback callback);
    void setEnabled(bool enabled);
    [[nodiscard]] bool desiredEnabled() const;

    // source_revision is optional for standalone users. A non-zero revision is
    // monotonic per host and prevents an older recognition thread from
    // overwriting a newer context after it finishes late.
    void updateCombatContext(
        pubg::ai_aim::CombatContext context,
        std::uint64_t source_revision = 0);
    void updateActivation(pubg::ai_aim::ActivationState activation);
    void resetTracking();
    // Apply tunable parameters without rebuilding the loaded model. A new
    // factory snapshot is retained for the next enable/loading attempt.
    // Device changes may request an asynchronous runtime replacement instead.
    [[nodiscard]] bool updateConfiguration(
        ModuleFactory module_factory, pubg::ai_aim::AimConfig config,
        std::chrono::milliseconds motion_max_age, bool recreate_runtime = false);
    void shutdown() noexcept;

    [[nodiscard]] AiAimRuntimeStatus status() const;
    [[nodiscard]] pubg::ai_aim::AimSnapshot snapshot() const;

private:
    struct BuildAttempt final {
        std::stop_source stop_source;
    };

    void controlLoop(std::stop_token stop) noexcept;
    void finishBuildAttempt(const std::shared_ptr<BuildAttempt>& attempt) noexcept;
    void closeMotionGateLocked();
    void reconcileMotionGateLocked();
    [[nodiscard]] std::uint64_t updateStatusLocked(
        AiAimRuntimeState state,
        std::string detail,
        std::string warning = {});
    void publishStatus(AiAimRuntimeState state, std::string detail);
    void publishTransitionStatus(
        std::uint64_t generation,
        bool expected_enabled,
        AiAimRuntimeState state,
        std::string detail);
    void publishModuleStatus(
        std::uint64_t generation,
        std::uint64_t combat_revision,
        const std::shared_ptr<pubg::ai_aim::AimModule>& expected_module,
        AiAimRuntimeState state,
        std::string detail);
    void drainStatusCallbacksLocked(
        std::unique_lock<std::recursive_mutex>& callback_lock) noexcept;
    void dispatchStatus(std::uint64_t revision) noexcept;

    std::shared_ptr<MouseMotionDispatcher> dispatcher_;
    std::shared_ptr<MouseMotionChannel> motion_channel_;
    std::shared_ptr<detail::AiAimMotionSinkPermit> module_motion_permit_;
    ModuleFactory module_factory_;

    // Lock order is always motion_gate_mutex_ -> mutex_. No caller may acquire
    // the motion gate while already holding mutex_.
    std::mutex motion_gate_mutex_;
    // Serializes observer delivery. Lifecycle publishers never wait for this
    // mutex: its current owner drains the newest revision before releasing it.
    // Recursive ownership remains necessary because an observer may replace
    // itself while handling a callback.
    std::recursive_mutex status_callback_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::jthread control_worker_;
    std::thread::id control_worker_id_{};
    std::shared_ptr<pubg::ai_aim::AimModule> module_;
    std::string module_warning_;
    // Each asynchronous factory attempt owns a distinct stop state. Any
    // generation change requests it without holding coordinator locks.
    std::shared_ptr<BuildAttempt> active_build_attempt_;
    pubg::ai_aim::CombatContext combat_{};
    pubg::ai_aim::ActivationState activation_{};
    AiAimRuntimeStatus status_{};
    StatusCallback status_callback_;
    std::uint64_t status_revision_ = 0;
    std::uint64_t status_callback_epoch_ = 0;
    std::uint64_t delivered_callback_epoch_ = 0;
    std::uint64_t delivered_status_revision_ = 0;
    bool callback_snapshot_delivered_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t combat_source_revision_ = 0;
    bool module_ready_ = false;
    bool module_restart_requested_ = false;
    bool desired_enabled_ = false;
    bool shutdown_ = false;
    bool shutdown_complete_ = false;
};

} // namespace pubg
