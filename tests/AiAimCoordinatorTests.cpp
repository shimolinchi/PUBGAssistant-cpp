#include "AiAimCoordinator.hpp"
#include "MouseMotionTestSupport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace aim = pubg::ai_aim;
using RecordingBackend = pubg::test::RecordingMouseBackend;
using pubg::test::ScopeExit;
using pubg::test::require;
using pubg::test::waitUntil;

aim::ActivationState allowedActivation() {
    aim::ActivationState activation;
    activation.enabled = true;
    activation.game_foreground = true;
    activation.aim_primary = true;
    return activation;
}

aim::CombatContext combatWithWeapon() {
    aim::CombatContext combat;
    combat.weapon = "M416";
    return combat;
}

aim::AimConfig externalModuleConfig() {
    auto config = aim::AimConfig::correctedDefaults();
    config.prediction.enabled = false;
    config.fire_offset.enabled = false;
    config.controller.fov_enabled = false;
    config.controller.pid_enabled = false;
    config.controller.global_scale_enabled = false;
    config.targeting.class_location[1] = 0.5F;
    return config;
}

pubg::AiAimCoordinator::ModuleBuild makeExternalModuleBuild(
    std::shared_ptr<aim::IMotionSink> motion_sink,
    aim::AimModule** module_out = nullptr,
    std::string startup_warning = {}) {
    auto module = std::make_unique<aim::AimModule>(
        externalModuleConfig(),
        std::move(motion_sink));
    if (module_out) {
        *module_out = module.get();
    }
    return {std::move(module), 5s, std::move(startup_warning)};
}

aim::DetectionFrame detectionFrame(std::uint64_t sequence) {
    aim::DetectionFrame frame;
    frame.sequence = sequence;
    frame.timestamp = aim::Clock::now();
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.screen_width = 320;
    frame.screen_height = 320;
    frame.screen_center_x = 160;
    frame.screen_center_y = 160;
    frame.detections.push_back({
        aim::Box::fromCenter(180.0F, 160.0F, 40.0F, 80.0F),
        0.9F,
        1,
    });
    return frame;
}

class ReadyCoordinatorHarness final {
public:
    ReadyCoordinatorHarness()
        : backend(std::make_shared<RecordingBackend>()),
          dispatcher(pubg::makeMouseMotionDispatcher(backend)) {
        coordinator = std::make_unique<pubg::AiAimCoordinator>(
            dispatcher,
            [this](
                std::shared_ptr<aim::IMotionSink> created_sink,
                std::stop_token) {
                {
                    std::lock_guard lock(sink_mutex_);
                    motion_sink_ = created_sink;
                }
                return makeExternalModuleBuild(std::move(created_sink));
            });
        coordinator->updateCombatContext(combatWithWeapon());
        coordinator->updateActivation(allowedActivation());
        coordinator->setEnabled(true);

        require(waitUntil([&] {
            return coordinator->status().state == pubg::AiAimRuntimeState::Ready;
        }), "coordinator did not reach Ready");
        require(motionSink() != nullptr, "factory did not receive the AI motion sink");
    }

    ~ReadyCoordinatorHarness() {
        backend->setBlocked(false);
        if (coordinator) {
            coordinator->shutdown();
        }
    }

    void submit(double dx, double dy) {
        auto sink = motionSink();
        require(sink != nullptr, "AI motion sink is unavailable");
        aim::MotionCommand command;
        command.dx = dx;
        command.dy = dy;
        command.frame_sequence = next_sequence_.fetch_add(1);
        command.timestamp = aim::Clock::now();
        sink->submitAimMotion(command);
    }

    std::shared_ptr<aim::IMotionSink> motionSink() const {
        std::lock_guard lock(sink_mutex_);
        return motion_sink_;
    }

    std::shared_ptr<RecordingBackend> backend;
    std::shared_ptr<pubg::MouseMotionDispatcher> dispatcher;
    std::unique_ptr<pubg::AiAimCoordinator> coordinator;

private:
    mutable std::mutex sink_mutex_;
    std::shared_ptr<aim::IMotionSink> motion_sink_;
    std::atomic<std::uint64_t> next_sequence_{1};
};

bool waitForMotionGateToReject(ReadyCoordinatorHarness& harness) {
    return waitUntil([&] {
        const auto submitted_before = harness.dispatcher->snapshot().submitted;
        harness.submit(1.0, 1.0);
        return harness.dispatcher->snapshot().submitted == submitted_before;
    });
}

void requireMotionDoesNotReachBackend(
    ReadyCoordinatorHarness& harness,
    const char* message) {
    const auto calls_before = harness.backend->callCount();
    harness.submit(1.0, 1.0);
    std::this_thread::sleep_for(50ms);
    require(harness.backend->callCount() == calls_before, message);
}

void testEmptyWeaponClosesAndValidWeaponReopensMotionGate() {
    ReadyCoordinatorHarness harness;
    harness.submit(3.0, 1.0);
    require(waitUntil([&] { return harness.backend->callCount() == 1; }),
            "accepted AI command did not reach the backend");

    aim::CombatContext empty_combat = combatWithWeapon();
    empty_combat.weapon.clear();
    harness.coordinator->updateCombatContext(empty_combat);

    require(waitForMotionGateToReject(harness),
            "empty weapon accepted a late AI command");
    require(harness.coordinator->status().state == pubg::AiAimRuntimeState::Paused,
            "empty weapon did not publish Paused");

    harness.coordinator->updateCombatContext(combatWithWeapon());
    harness.submit(4.0, 2.0);
    require(waitUntil([&] { return harness.backend->callCount() == 2; }),
            "reopened AI command did not reach the backend");

    harness.coordinator->shutdown();
    requireMotionDoesNotReachBackend(
        harness, "shutdown AI motion gate accepted a late command");
}

void testPauseResetAndShutdownFenceInFlightMotion() {
    ReadyCoordinatorHarness harness;
    std::future<void> pause;
    harness.backend->setBlocked(true);
    harness.submit(5.0, 1.0);
    require(harness.backend->waitForCalls(1),
            "backend did not enter the pause fence test");

    aim::ActivationState paused = allowedActivation();
    paused.paused = true;
    pause = std::async(std::launch::async, [&] {
        harness.coordinator->updateActivation(paused);
    });
    auto release_pause = ScopeExit([&] { harness.backend->setBlocked(false); });
    require(waitForMotionGateToReject(harness),
            "pause did not close the AI motion gate");
    require(pause.wait_for(30ms) == std::future_status::timeout,
            "pause returned while AI motion was still in flight");

    harness.backend->setBlocked(false);
    require(pause.wait_for(2s) == std::future_status::ready,
            "pause fence did not finish after backend release");
    pause.get();
    release_pause.dismiss();

    harness.coordinator->updateActivation(allowedActivation());
    const auto calls_before_reset = harness.backend->callCount();
    harness.submit(1.0, 0.0);
    require(waitUntil([&] { return harness.backend->callCount() > calls_before_reset; }),
            "allowed activation did not reopen the AI gate");

    std::future<void> reset;
    harness.backend->setBlocked(true);
    const auto reset_call = harness.backend->callCount() + 1;
    harness.submit(6.0, 2.0);
    require(harness.backend->waitForCalls(reset_call),
            "backend did not enter the reset fence test");
    reset = std::async(std::launch::async, [&] {
        harness.coordinator->resetTracking();
    });
    auto release_reset = ScopeExit([&] { harness.backend->setBlocked(false); });
    require(waitForMotionGateToReject(harness),
            "reset did not close the AI motion gate");
    require(reset.wait_for(30ms) == std::future_status::timeout,
            "reset returned while AI motion was still in flight");

    harness.backend->setBlocked(false);
    require(reset.wait_for(2s) == std::future_status::ready,
            "reset fence did not finish after backend release");
    reset.get();
    release_reset.dismiss();
    const auto calls_after_reset = harness.backend->callCount();
    harness.submit(1.0, 0.0);
    require(waitUntil([&] { return harness.backend->callCount() > calls_after_reset; }),
            "reset did not restore an otherwise safe AI motion gate");

    std::future<void> shutdown;
    std::future<void> concurrent_shutdown;
    harness.backend->setBlocked(true);
    const auto shutdown_call = harness.backend->callCount() + 1;
    harness.submit(7.0, 3.0);
    require(harness.backend->waitForCalls(shutdown_call),
            "backend did not enter the shutdown fence test");
    shutdown = std::async(std::launch::async, [&] {
        harness.coordinator->shutdown();
    });
    concurrent_shutdown = std::async(std::launch::async, [&] {
        harness.coordinator->shutdown();
    });
    auto release_shutdown = ScopeExit([&] { harness.backend->setBlocked(false); });
    require(waitForMotionGateToReject(harness),
            "shutdown did not close the AI motion gate");
    require(shutdown.wait_for(30ms) == std::future_status::timeout,
            "shutdown returned while AI motion was still in flight");
    require(concurrent_shutdown.wait_for(30ms) == std::future_status::timeout,
            "concurrent shutdown returned before shutdown completed");

    harness.backend->setBlocked(false);
    require(shutdown.wait_for(2s) == std::future_status::ready,
            "shutdown fence did not finish after backend release");
    require(concurrent_shutdown.wait_for(2s) == std::future_status::ready,
            "concurrent shutdown did not observe completion");
    shutdown.get();
    concurrent_shutdown.get();
    release_shutdown.dismiss();
    requireMotionDoesNotReachBackend(
        harness, "shutdown AI motion gate accepted a late command");
    require(harness.coordinator->status().state == pubg::AiAimRuntimeState::Off,
            "shutdown did not publish Off");
}

void testStaleInitializationFailureDoesNotOverwriteNewGeneration() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);

    std::mutex factory_mutex;
    std::condition_variable factory_cv;
    int factory_calls = 0;
    bool first_factory_entered = false;
    bool release_first_factory = false;

    std::mutex status_mutex;
    std::vector<pubg::AiAimRuntimeStatus> statuses;

    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            bool stale_failure = false;
            {
                std::unique_lock lock(factory_mutex);
                ++factory_calls;
                if (factory_calls == 1) {
                    first_factory_entered = true;
                    factory_cv.notify_all();
                    factory_cv.wait(lock, [&] { return release_first_factory; });
                    stale_failure = true;
                }
            }
            if (stale_failure) {
                throw std::runtime_error("stale initialization failure");
            }
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    auto release_factory = ScopeExit([&] {
        {
            std::lock_guard lock(factory_mutex);
            release_first_factory = true;
        }
        factory_cv.notify_all();
    });
    coordinator.setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        std::lock_guard lock(status_mutex);
        statuses.push_back(status);
    });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);

    {
        std::unique_lock lock(factory_mutex);
        require(factory_cv.wait_for(lock, 2s, [&] { return first_factory_entered; }),
                "first AI module factory did not start");
    }

    coordinator.setEnabled(false);
    coordinator.setEnabled(true);
    {
        std::lock_guard lock(factory_mutex);
        release_first_factory = true;
    }
    factory_cv.notify_all();
    release_factory.dismiss();

    require(waitUntil([&] {
        std::lock_guard lock(factory_mutex);
        return factory_calls >= 2 &&
               coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "replacement AI generation did not reach Ready");
    require(coordinator.desiredEnabled(),
            "stale initialization failure disabled the replacement generation");

    {
        std::lock_guard lock(status_mutex);
        for (const auto& status : statuses) {
            require(status.state != pubg::AiAimRuntimeState::Error,
                    "stale initialization failure published Error");
        }
    }
    coordinator.shutdown();
}

void testDisableCancelsInFlightInitialization() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::mutex factory_mutex;
    std::condition_variable_any factory_cv;
    bool factory_entered = false;
    bool cancellation_observed = false;

    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink>, const std::stop_token stop) ->
            pubg::AiAimCoordinator::ModuleBuild {
            std::unique_lock lock(factory_mutex);
            factory_entered = true;
            factory_cv.notify_all();
            factory_cv.wait(lock, stop, [] { return false; });
            cancellation_observed = stop.stop_requested();
            factory_cv.notify_all();
            throw std::runtime_error("cancelled stale initialization");
        });

    coordinator.setEnabled(true);
    {
        std::unique_lock lock(factory_mutex);
        require(factory_cv.wait_for(lock, 2s, [&] { return factory_entered; }),
                "AI initialization factory did not start");
    }

    coordinator.setEnabled(false);
    {
        std::unique_lock lock(factory_mutex);
        require(factory_cv.wait_for(lock, 2s, [&] { return cancellation_observed; }),
                "disable did not cancel the in-flight AI initialization");
    }
    require(coordinator.status().state == pubg::AiAimRuntimeState::Off,
            "cancelled stale initialization overwrote Off");
    require(!coordinator.desiredEnabled(),
            "cancelled initialization left AI aim enabled");
    coordinator.shutdown();
}

void testReplacementGenerationGetsFreshInitializationToken() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::mutex factory_mutex;
    std::condition_variable_any factory_cv;
    int factory_calls = 0;
    bool first_factory_entered = false;
    bool first_factory_cancelled = false;
    bool second_factory_started_stopped = false;

    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink,
            const std::stop_token stop) {
            int call = 0;
            {
                std::lock_guard lock(factory_mutex);
                call = ++factory_calls;
                if (call == 1) {
                    first_factory_entered = true;
                    factory_cv.notify_all();
                } else {
                    second_factory_started_stopped = stop.stop_requested();
                    factory_cv.notify_all();
                }
            }
            if (call == 1) {
                std::unique_lock lock(factory_mutex);
                factory_cv.wait(lock, stop, [] { return false; });
                first_factory_cancelled = stop.stop_requested();
                factory_cv.notify_all();
                throw std::runtime_error("cancelled first generation");
            }
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    {
        std::unique_lock lock(factory_mutex);
        require(factory_cv.wait_for(lock, 2s, [&] { return first_factory_entered; }),
                "first AI initialization generation did not start");
    }

    coordinator.setEnabled(false);
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        std::lock_guard lock(factory_mutex);
        return first_factory_cancelled && factory_calls == 2 &&
               coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "replacement AI initialization generation did not reach Ready");
    require(!second_factory_started_stopped,
            "replacement AI generation inherited the cancelled stop token");
    coordinator.shutdown();
}

void testThrowingStatusCallbackCannotBreakLifecycle() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    std::atomic<int> loading_callbacks = 0;
    coordinator.setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        if (status.state == pubg::AiAimRuntimeState::Loading) {
            loading_callbacks.fetch_add(1);
            throw std::runtime_error("observer failure");
        }
    });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());

    bool enable_threw = false;
    try {
        coordinator.setEnabled(true);
    } catch (...) {
        enable_threw = true;
    }
    require(!enable_threw, "status callback exception escaped setEnabled");
    require(waitUntil([&] {
        return coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "throwing status callback prevented Ready");
    require(loading_callbacks.load() >= 1,
            "throwing Loading callback was not exercised");

    coordinator.shutdown();
}

void testControlWorkerStatusCallbackCanRequestShutdown() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::unique_ptr<pubg::AiAimCoordinator> coordinator;
    coordinator = std::make_unique<pubg::AiAimCoordinator>(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    std::atomic<bool> shutdown_requested = false;
    std::atomic<bool> off_observed = false;
    coordinator->setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        if (status.state == pubg::AiAimRuntimeState::Ready &&
            !shutdown_requested.exchange(true)) {
            coordinator->shutdown();
        }
        if (status.state == pubg::AiAimRuntimeState::Off && shutdown_requested.load()) {
            off_observed.store(true);
        }
    });
    coordinator->updateCombatContext(combatWithWeapon());
    coordinator->updateActivation(allowedActivation());
    coordinator->setEnabled(true);

    require(waitUntil([&] { return off_observed.load(); }),
            "control-worker callback shutdown did not complete");
    coordinator->shutdown();
    require(!coordinator->desiredEnabled(),
            "callback-requested shutdown left AI aim enabled");
}

void testDestructorDoesNotNotifyExternalObserver() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::atomic<int> callback_count = 0;
    int count_before_destruction = 0;
    {
        auto coordinator = std::make_unique<pubg::AiAimCoordinator>(
            dispatcher,
            [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
                return makeExternalModuleBuild(std::move(motion_sink));
            });
        coordinator->updateCombatContext(combatWithWeapon());
        coordinator->updateActivation(allowedActivation());
        coordinator->setEnabled(true);
        require(waitUntil([&] {
            return coordinator->status().state == pubg::AiAimRuntimeState::Ready;
        }), "coordinator did not reach Ready before destructor test");
        coordinator->setStatusCallback([&](const pubg::AiAimRuntimeStatus&) {
            callback_count.fetch_add(1);
        });
        // setStatusCallback delivers the current snapshot synchronously, so
        // returning here proves the callback has left its ownership-sensitive
        // stack before the final owner is released.
        require(callback_count.load() == 1,
                "current Ready snapshot was not delivered exactly once");
        count_before_destruction = callback_count.load();
    }
    require(callback_count.load() == count_before_destruction,
            "coordinator destructor invoked an external status observer");
}

void testCallbackReplacementDrainsOnlyTheCurrentEpoch() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    std::mutex observations_mutex;
    std::vector<pubg::AiAimRuntimeStatus> callback_b_statuses;
    std::atomic<bool> callback_replaced = false;
    std::atomic<bool> callback_b_disabled = false;
    std::atomic<bool> callback_a_received_off_after_replacement = false;

    pubg::AiAimCoordinator::StatusCallback callback_b;
    callback_b = [&](const pubg::AiAimRuntimeStatus& status) {
        {
            std::lock_guard lock(observations_mutex);
            callback_b_statuses.push_back(status);
        }
        if (status.state == pubg::AiAimRuntimeState::Loading &&
            !callback_b_disabled.exchange(true)) {
            coordinator.setEnabled(false);
        }
    };
    coordinator.setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        if (status.state == pubg::AiAimRuntimeState::Off &&
            callback_replaced.load()) {
            callback_a_received_off_after_replacement.store(true);
        }
        if (status.state == pubg::AiAimRuntimeState::Loading &&
            !callback_replaced.exchange(true)) {
            coordinator.setStatusCallback(callback_b);
        }
    });

    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        std::lock_guard lock(observations_mutex);
        return coordinator.status().state == pubg::AiAimRuntimeState::Off &&
               !callback_b_statuses.empty() &&
               callback_b_statuses.back().state == pubg::AiAimRuntimeState::Off;
    }), "replacement callback did not drain the final Off status");

    require(callback_replaced.load() && callback_b_disabled.load(),
            "recursive callback replacement path was not exercised");
    require(!callback_a_received_off_after_replacement.load(),
            "replaced callback received a status from the new callback epoch");
    {
        std::lock_guard lock(observations_mutex);
        int loading_count = 0;
        int off_count = 0;
        for (const auto& status : callback_b_statuses) {
            loading_count += status.state == pubg::AiAimRuntimeState::Loading ? 1 : 0;
            off_count += status.state == pubg::AiAimRuntimeState::Off ? 1 : 0;
        }
        require(loading_count == 1 && off_count == 1,
                "replacement callback received duplicate lifecycle statuses");
        const auto final_status = coordinator.status();
        require(callback_b_statuses.back().lifecycle_generation ==
                    final_status.lifecycle_generation &&
                callback_b_statuses.back().revision == final_status.revision,
                "replacement callback did not receive the current status identity");
    }
    require(!coordinator.desiredEnabled(),
            "replacement callback disable left AI aim enabled");
    coordinator.shutdown();
}

void testLoadingCallbackDisableLeavesFinalStatusOff() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::unique_ptr<pubg::AiAimCoordinator> coordinator;
    coordinator = std::make_unique<pubg::AiAimCoordinator>(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    std::mutex status_mutex;
    std::vector<pubg::AiAimRuntimeState> states;
    std::atomic<bool> disabled_from_loading = false;
    coordinator->setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        {
            std::lock_guard lock(status_mutex);
            states.push_back(status.state);
        }
        if (status.state == pubg::AiAimRuntimeState::Loading &&
            !disabled_from_loading.exchange(true)) {
            coordinator->setEnabled(false);
        }
    });
    coordinator->updateCombatContext(combatWithWeapon());
    coordinator->updateActivation(allowedActivation());
    coordinator->setEnabled(true);

    require(waitUntil([&] {
        return disabled_from_loading.load() &&
               coordinator->status().state == pubg::AiAimRuntimeState::Off;
    }), "Loading callback disable did not leave the coordinator Off");
    std::this_thread::sleep_for(50ms);
    require(!coordinator->desiredEnabled(),
            "Loading callback disable left AI aim enabled");
    {
        std::lock_guard lock(status_mutex);
        require(!states.empty() && states.back() == pubg::AiAimRuntimeState::Off,
                "a stale status overwrote Off after Loading callback disable");
    }
    coordinator->shutdown();
}

void testMainThreadStatusCallbackCanRequestShutdown() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::unique_ptr<pubg::AiAimCoordinator> coordinator;
    coordinator = std::make_unique<pubg::AiAimCoordinator>(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });

    std::atomic<bool> shutdown_requested = false;
    std::atomic<bool> off_observed = false;
    coordinator->setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        if (status.state == pubg::AiAimRuntimeState::Loading &&
            !shutdown_requested.exchange(true)) {
            coordinator->shutdown();
        }
        if (status.state == pubg::AiAimRuntimeState::Off && shutdown_requested.load()) {
            off_observed.store(true);
        }
    });
    coordinator->updateCombatContext(combatWithWeapon());
    coordinator->updateActivation(allowedActivation());
    coordinator->setEnabled(true);

    require(waitUntil([&] { return off_observed.load(); }),
            "main-thread status callback shutdown did not complete");
    coordinator->shutdown();
}

void testOffCallbackReenableRestoresReadyStatus() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::atomic<int> factory_calls = 0;
    std::unique_ptr<pubg::AiAimCoordinator> coordinator;
    coordinator = std::make_unique<pubg::AiAimCoordinator>(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            factory_calls.fetch_add(1);
            return makeExternalModuleBuild(std::move(motion_sink));
        });
    coordinator->updateCombatContext(combatWithWeapon());
    coordinator->updateActivation(allowedActivation());
    coordinator->setEnabled(true);
    require(waitUntil([&] {
        return coordinator->status().state == pubg::AiAimRuntimeState::Ready;
    }), "coordinator did not reach Ready before callback re-enable");

    std::atomic<bool> reenabled = false;
    coordinator->setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        if (status.state == pubg::AiAimRuntimeState::Off &&
            !reenabled.exchange(true)) {
            coordinator->setEnabled(true);
        }
    });
    coordinator->setEnabled(false);

    require(waitUntil([&] {
        return reenabled.load() && coordinator->desiredEnabled() &&
               coordinator->status().state == pubg::AiAimRuntimeState::Ready;
    }), "Off callback re-enable left a live module stuck in Loading");
    require(factory_calls.load() == 1,
            "callback re-enable unnecessarily rebuilt the live AI module");
    coordinator->shutdown();
}

void testCurrentFactoryFailurePublishesErrorAndDisables() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink>, std::stop_token) ->
            pubg::AiAimCoordinator::ModuleBuild {
            throw std::runtime_error("expected factory failure");
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);

    require(waitUntil([&] {
        return coordinator.status().state == pubg::AiAimRuntimeState::Error;
    }), "current factory failure did not publish Error");
    require(coordinator.status().detail == "expected factory failure",
            "factory failure detail was not preserved");
    require(!coordinator.desiredEnabled(),
            "current factory failure did not disable AI aim");
    coordinator.shutdown();
}

void testFactoryWarningUsesExistingRuntimeStatusChannel() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(
                std::move(motion_sink), nullptr, "DirectML unavailable; using CPU fallback");
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);

    require(waitUntil([&] {
        const auto status = coordinator.status();
        return status.state == pubg::AiAimRuntimeState::Ready &&
               status.warning == "DirectML unavailable; using CPU fallback";
    }), "factory startup warning was not published with Ready");

    auto no_weapon = combatWithWeapon();
    no_weapon.weapon.clear();
    coordinator.updateCombatContext(no_weapon);
    const auto paused = coordinator.status();
    require(paused.state == pubg::AiAimRuntimeState::Paused &&
            paused.warning == "DirectML unavailable; using CPU fallback",
            "module warning was lost on a runtime status transition");

    coordinator.setEnabled(false);
    require(coordinator.status().warning.empty(),
            "Off status retained a stale module warning");
    coordinator.shutdown();
}

void testLifecycleSwitchDoesNotWaitForBlockedStatusObserver() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::mutex factory_mutex;
    std::condition_variable factory_cv;
    bool factory_entered = false;
    bool release_factory = false;

    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            std::unique_lock lock(factory_mutex);
            factory_entered = true;
            factory_cv.notify_all();
            factory_cv.wait(lock, [&] { return release_factory; });
            return makeExternalModuleBuild(
                std::move(motion_sink), nullptr, "stale Ready warning");
        });

    auto unblock_factory = ScopeExit([&] {
        {
            std::lock_guard lock(factory_mutex);
            release_factory = true;
        }
        factory_cv.notify_all();
    });

    // Models App's lifecycle mutex: the switch thread owns it while the
    // asynchronous Ready observer is already waiting for it.
    std::recursive_timed_mutex app_lifecycle_mutex;
    std::mutex accepted_mutex;
    std::vector<pubg::AiAimRuntimeStatus> accepted;
    std::atomic<bool> ready_observer_entered = false;
    std::atomic<bool> observer_timed_out = false;
    coordinator.setStatusCallback([&](const pubg::AiAimRuntimeStatus& status) {
        std::unique_lock app_lock(app_lifecycle_mutex, std::defer_lock);
        if (status.state == pubg::AiAimRuntimeState::Ready) {
            ready_observer_entered.store(true);
            if (!app_lock.try_lock_for(1500ms)) {
                observer_timed_out.store(true);
                return;
            }
        } else {
            app_lock.lock();
        }

        const auto latest = coordinator.status();
        if (latest.lifecycle_generation == status.lifecycle_generation &&
            latest.revision == status.revision) {
            std::lock_guard lock(accepted_mutex);
            accepted.push_back(status);
        }
    });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);

    {
        std::unique_lock lock(factory_mutex);
        require(factory_cv.wait_for(lock, 2s, [&] { return factory_entered; }),
                "AI factory did not enter the observer lock-order test");
    }

    std::unique_lock app_lock(app_lifecycle_mutex);
    {
        std::lock_guard lock(factory_mutex);
        release_factory = true;
    }
    factory_cv.notify_all();
    unblock_factory.dismiss();
    require(waitUntil([&] { return ready_observer_entered.load(); }),
            "Ready observer did not enter the simulated App lifecycle lock");

    const auto disable_started = std::chrono::steady_clock::now();
    coordinator.setEnabled(false);
    const auto disable_elapsed =
        std::chrono::steady_clock::now() - disable_started;
    require(disable_elapsed < 500ms,
            "lifecycle switch waited for a blocked status observer");
    app_lock.unlock();

    require(waitUntil([&] {
        std::lock_guard lock(accepted_mutex);
        return !accepted.empty() &&
               accepted.back().state == pubg::AiAimRuntimeState::Off;
    }), "callback owner did not drain the final Off status");
    require(!observer_timed_out.load(),
            "Ready observer remained blocked after disable returned");
    {
        std::lock_guard lock(accepted_mutex);
        for (const auto& status : accepted) {
            require(status.warning != "stale Ready warning",
                    "stale Ready warning passed the final status identity check");
        }
    }
    const auto final_status = coordinator.status();
    require(final_status.state == pubg::AiAimRuntimeState::Off,
            "observer contention did not leave the coordinator Off");
    coordinator.shutdown();
}

void testRuntimeStatusCarriesLifecycleGenerationAndRevision() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            return makeExternalModuleBuild(std::move(motion_sink));
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        return coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "first lifecycle did not reach Ready");
    const auto first_ready = coordinator.status();
    require(first_ready.lifecycle_generation > 0 && first_ready.revision > 0,
            "Ready status did not expose its lifecycle identity");

    coordinator.setEnabled(false);
    const auto disabled = coordinator.status();
    require(disabled.state == pubg::AiAimRuntimeState::Off &&
            disabled.lifecycle_generation > first_ready.lifecycle_generation &&
            disabled.revision > first_ready.revision,
            "disable did not advance the runtime status identity");

    coordinator.setEnabled(true);
    require(waitUntil([&] {
        return coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "second lifecycle did not reach Ready");
    const auto second_ready = coordinator.status();
    require(second_ready.lifecycle_generation > disabled.lifecycle_generation &&
            second_ready.revision > disabled.revision,
            "re-enable reused an earlier runtime status identity");
    coordinator.shutdown();
}

void testStaleHostCombatRevisionCannotOverwriteNewerContext() {
    ReadyCoordinatorHarness harness;

    auto no_weapon = combatWithWeapon();
    no_weapon.weapon.clear();
    harness.coordinator->updateCombatContext(no_weapon, 2);
    require(waitUntil([&] {
        return harness.coordinator->status().state ==
               pubg::AiAimRuntimeState::Paused;
    }), "newer empty combat context did not pause the coordinator");

    harness.coordinator->updateCombatContext(combatWithWeapon(), 1);
    require(harness.coordinator->status().state ==
                pubg::AiAimRuntimeState::Paused,
            "stale host combat context overwrote a newer revision");
    require(waitForMotionGateToReject(harness),
            "stale host combat context reopened the motion gate");

    harness.coordinator->updateCombatContext(combatWithWeapon(), 3);
    require(waitUntil([&] {
        return harness.coordinator->status().state ==
               pubg::AiAimRuntimeState::Ready;
    }), "newest host combat context was not accepted");
}

void testExternalDetectionTraversesCoordinatorMotionGate() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::atomic<aim::AimModule*> module = nullptr;
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            aim::AimModule* created = nullptr;
            auto build = makeExternalModuleBuild(std::move(motion_sink), &created);
            module.store(created);
            return build;
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        return coordinator.status().state == pubg::AiAimRuntimeState::Ready &&
               module.load() != nullptr;
    }), "external-mode coordinator did not reach Ready");

    require(module.load()->submitDetections(detectionFrame(1)),
            "external detection frame was rejected");
    require(backend->waitForCalls(1),
            "external detection did not reach the shared mouse dispatcher");

    auto paused = allowedActivation();
    paused.paused = true;
    coordinator.updateActivation(paused);
    require(!module.load()->submitDetections(detectionFrame(2)),
            "paused coordinator accepted an external detection frame");
    std::this_thread::sleep_for(50ms);
    require(backend->callCount() == 1,
            "paused external detection produced mouse motion");
    coordinator.shutdown();
}

void testRetiredGenerationSinkCannotRegainMotionAuthority() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::mutex sinks_mutex;
    std::vector<std::shared_ptr<aim::IMotionSink>> sinks;
    pubg::AiAimCoordinator coordinator(
        dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> motion_sink, std::stop_token) {
            {
                std::lock_guard lock(sinks_mutex);
                sinks.push_back(motion_sink);
            }
            return makeExternalModuleBuild(std::move(motion_sink));
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        std::lock_guard lock(sinks_mutex);
        return sinks.size() == 1 &&
               coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "first AI module generation did not reach Ready");

    std::shared_ptr<aim::IMotionSink> retired_sink;
    {
        std::lock_guard lock(sinks_mutex);
        retired_sink = sinks[0];
    }
    std::atomic<std::uint64_t> sequence = 1;
    std::jthread retired_writer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            aim::MotionCommand command;
            command.dx = 5.0;
            command.dy = 1.0;
            command.frame_sequence = sequence.fetch_add(1);
            command.timestamp = aim::Clock::now();
            retired_sink->submitAimMotion(command);
            std::this_thread::yield();
        }
    });

    coordinator.setEnabled(false);
    require(waitUntil([&] { return !coordinator.snapshot().running; }),
            "first AI module generation was not unloaded");
    coordinator.setEnabled(true);
    require(waitUntil([&] {
        std::lock_guard lock(sinks_mutex);
        return sinks.size() == 2 &&
               coordinator.status().state == pubg::AiAimRuntimeState::Ready;
    }), "second AI module generation did not reach Ready");

    std::shared_ptr<aim::IMotionSink> current_sink;
    {
        std::lock_guard lock(sinks_mutex);
        current_sink = sinks[1];
    }
    const auto calls_after_replacement = backend->callCount();
    std::this_thread::sleep_for(50ms);
    require(backend->callCount() == calls_after_replacement,
            "retired AI module sink regained motion authority");
    retired_writer.request_stop();
    retired_writer.join();

    aim::MotionCommand command;
    command.dx = 5.0;
    command.dy = 1.0;
    command.frame_sequence = sequence.fetch_add(1);
    command.timestamp = aim::Clock::now();
    current_sink->submitAimMotion(command);
    require(backend->waitForCalls(calls_after_replacement + 1),
            "current AI module sink did not retain motion authority");
    coordinator.shutdown();
}

void testLiveConfigurationUpdatesReadyModuleAndFutureFactory() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::atomic<aim::AimModule*> current{nullptr};
    std::atomic<int> builds{0};
    std::shared_ptr<aim::IMotionSink> sink;
    const auto factoryFor = [&](aim::AimConfig config, std::chrono::milliseconds age) {
        return pubg::AiAimCoordinator::ModuleFactory{
            [&, config, age](std::shared_ptr<aim::IMotionSink> motion, std::stop_token) {
                sink = motion;
                auto module = std::make_unique<aim::AimModule>(config, std::move(motion));
                current.store(module.get());
                ++builds;
                return pubg::AiAimCoordinator::ModuleBuild{std::move(module), age, {}};
            }};
    };
    auto config = externalModuleConfig();
    pubg::AiAimCoordinator coordinator(dispatcher, factoryFor(config, 5s));
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] { return coordinator.status().state == pubg::AiAimRuntimeState::Ready; }),
            "initial runtime not ready");
    require(current.load()->submitDetections(detectionFrame(1)), "initial frame rejected");
    require(backend->waitForCalls(1), "initial frame did not produce motion");
    require(backend->calls().back().first > 3, "initial motion must exceed the new limit");
    config.controller.max_pixels = 3;
    config.tracking.max_frame_age_ms = 50;
    require(coordinator.updateConfiguration(factoryFor(config, 50ms), config, 50ms), "live update rejected");
    require(coordinator.desiredEnabled() && builds.load() == 1,
            "live update changed switch or rebuilt model unnecessarily");
    require(!coordinator.snapshot().detection_frame, "live update retained old target boxes");
    std::uint64_t sequence = 2;
    require(waitUntil([&] {
        (void)current.load()->submitDetections(detectionFrame(sequence++));
        return backend->callCount() >= 2;
    }), "updated module did not process fresh frames");
    require(backend->calls().back().first > 0 && backend->calls().back().first <= 3,
            "new motion limit was not applied live");
    coordinator.updateActivation({});
    // A save while paused must not reopen the output gate.
    require(coordinator.updateConfiguration(factoryFor(config, 50ms), config, 50ms), "paused update failed");
    require(!current.load()->submitDetections(detectionFrame(sequence++)), "save resumed paused AI");
    coordinator.updateActivation(allowedActivation());
    const auto stale_before = dispatcher->snapshot().dropped_stale;
    aim::MotionCommand old_motion;
    old_motion.dx = 1;
    old_motion.frame_sequence = sequence++;
    old_motion.timestamp = aim::Clock::now() - 100ms;
    sink->submitAimMotion(old_motion);
    require(waitUntil([&] { return dispatcher->snapshot().dropped_stale > stale_before; }),
            "motion channel kept old frame lifetime");

    coordinator.setEnabled(false);
    require(waitUntil([&] { return !coordinator.snapshot().running; }), "disabled runtime not released");
    config.controller.max_pixels = 2;
    require(coordinator.updateConfiguration(factoryFor(config, 50ms), config, 50ms), "disabled save failed");
    require(!coordinator.desiredEnabled() && builds.load() == 1, "disabled save started AI");
    coordinator.setEnabled(true);
    coordinator.updateActivation(allowedActivation());
    require(waitUntil([&] { return coordinator.status().state == pubg::AiAimRuntimeState::Ready; }), "updated factory not ready");
    const auto before = backend->callCount();
    sequence = 1;
    require(waitUntil([&] {
        (void)current.load()->submitDetections(detectionFrame(sequence++));
        return backend->callCount() > before;
    }), "reenabled module did not produce motion");
    require(builds.load() == 2 && backend->calls().back().first > 0 && backend->calls().back().first <= 2,
            "reenable used stale captured configuration");
    const auto retired_sink = sink;
    require(coordinator.updateConfiguration(factoryFor(config, 50ms), config, 50ms, true),
            "device replacement rejected");
    require(waitUntil([&] { return builds.load() == 3 && coordinator.status().state == pubg::AiAimRuntimeState::Ready; }),
            "requested runtime replacement did not finish");
    old_motion.timestamp = aim::Clock::now();
    const auto after_rebuild = dispatcher->snapshot().submitted;
    retired_sink->submitAimMotion(old_motion);
    require(dispatcher->snapshot().submitted == after_rebuild, "retired sink reopened after replacement");
    coordinator.shutdown();
    require(!coordinator.updateConfiguration(factoryFor(config, 50ms), config, 50ms), "save revived shutdown coordinator");
}

void testSaveDuringInitializationCancelsOldFactoryAndUsesLatest() {
    auto backend = std::make_shared<RecordingBackend>();
    auto dispatcher = pubg::makeMouseMotionDispatcher(backend);
    std::atomic<bool> entered{false}, cancelled{false};
    std::atomic<int> latest_builds{0};
    pubg::AiAimCoordinator coordinator(dispatcher,
        [&](std::shared_ptr<aim::IMotionSink> sink, std::stop_token stop) {
            entered = true;
            while (!stop.stop_requested()) std::this_thread::yield();
            cancelled = true;
            return makeExternalModuleBuild(std::move(sink));
        });
    coordinator.updateCombatContext(combatWithWeapon());
    coordinator.updateActivation(allowedActivation());
    coordinator.setEnabled(true);
    require(waitUntil([&] { return entered.load(); }), "blocking factory not entered");
    require(coordinator.updateConfiguration(
        [&](std::shared_ptr<aim::IMotionSink> sink, std::stop_token stop) {
            require(!stop.stop_requested(), "new factory inherited cancelled token");
            ++latest_builds;
            return makeExternalModuleBuild(std::move(sink));
        }, externalModuleConfig(), 5s), "save during loading rejected");
    require(waitUntil([&] { return coordinator.status().state == pubg::AiAimRuntimeState::Ready; }),
            "latest configuration did not finish initializing");
    require(cancelled.load() && latest_builds.load() == 1 && coordinator.desiredEnabled(),
            "stale loading attempt interfered with saved configuration");
    coordinator.shutdown();
}

void testConfigurationUpdateFencesInflightMotion() {
    ReadyCoordinatorHarness harness;
    std::future<bool> update;
    harness.backend->setBlocked(true);
    auto release = ScopeExit([&] { harness.backend->setBlocked(false); });
    harness.submit(4, 2);
    require(harness.backend->waitForCalls(1), "backend did not block");
    update = std::async(std::launch::async, [&] {
        return harness.coordinator->updateConfiguration(
            [](std::shared_ptr<aim::IMotionSink> sink, std::stop_token) {
                return makeExternalModuleBuild(std::move(sink));
            }, externalModuleConfig(), 5s);
    });
    require(waitForMotionGateToReject(harness), "config save did not close motion channel");
    require(update.wait_for(30ms) == std::future_status::timeout, "config update skipped inflight fence");
    harness.backend->setBlocked(false);
    require(update.wait_for(2s) == std::future_status::ready && update.get(), "config update did not finish");
}

} // namespace

int main() {
    try {
        testLiveConfigurationUpdatesReadyModuleAndFutureFactory();
        testSaveDuringInitializationCancelsOldFactoryAndUsesLatest();
        testConfigurationUpdateFencesInflightMotion();
        testEmptyWeaponClosesAndValidWeaponReopensMotionGate();
        testPauseResetAndShutdownFenceInFlightMotion();
        testStaleInitializationFailureDoesNotOverwriteNewGeneration();
        testDisableCancelsInFlightInitialization();
        testReplacementGenerationGetsFreshInitializationToken();
        testThrowingStatusCallbackCannotBreakLifecycle();
        testControlWorkerStatusCallbackCanRequestShutdown();
        testDestructorDoesNotNotifyExternalObserver();
        testCallbackReplacementDrainsOnlyTheCurrentEpoch();
        testLoadingCallbackDisableLeavesFinalStatusOff();
        testMainThreadStatusCallbackCanRequestShutdown();
        testOffCallbackReenableRestoresReadyStatus();
        testCurrentFactoryFailurePublishesErrorAndDisables();
        testFactoryWarningUsesExistingRuntimeStatusChannel();
        testLifecycleSwitchDoesNotWaitForBlockedStatusObserver();
        testRuntimeStatusCarriesLifecycleGenerationAndRevision();
        testStaleHostCombatRevisionCannotOverwriteNewerContext();
        testExternalDetectionTraversesCoordinatorMotionGate();
        testRetiredGenerationSinkCannotRegainMotionAuthority();
        std::cout << "AiAimCoordinator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AiAimCoordinator tests failed: " << error.what() << '\n';
        return 1;
    }
}
