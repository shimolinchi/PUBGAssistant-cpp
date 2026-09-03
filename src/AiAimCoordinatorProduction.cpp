#include "AiAimCoordinatorProduction.hpp"

#include "AiAimCoordinator.hpp"
#include "AiAimRuntimePreflight.hpp"
#include "RegionManager.hpp"
#include "ResourcePaths.hpp"
#include "pubg_ai_aim/WindowsAdapters.hpp"

#include <stdexcept>
#include <utility>

namespace pubg {
namespace {

using namespace pubg::ai_aim;

void throwIfInitializationStopped(const std::stop_token stop) {
    if (stop.stop_requested()) {
        throw std::runtime_error("AI initialization was cancelled");
    }
}

AiAimDirectMlPolicy directMlPolicy(const AiAimRuntimeSettings& runtime) noexcept {
    if (!runtime.prefer_directml) {
        return AiAimDirectMlPolicy::Disabled;
    }
    return runtime.allow_cpu_fallback
        ? AiAimDirectMlPolicy::Optional
        : AiAimDirectMlPolicy::Required;
}

AiAimCoordinator::ModuleFactory makeProductionModuleFactory(
    const ResourcePaths& paths,
    AiAimSettings settings,
    RegionManager& regions) {
    const DesktopRect preferred_output{
        regions.screenLeft(),
        regions.screenTop(),
        regions.screenWidth(),
        regions.screenHeight(),
    };
    const auto runtime_files = locateAiAimRuntimeFiles(paths.aiAimRuntimeDir());

    return [settings = std::move(settings),
            runtime_files,
            preferred_output](
               std::shared_ptr<IMotionSink> motion_sink,
               const std::stop_token stop) {
        throwIfInitializationStopped(stop);
        if (!settings.valid()) {
            throw std::runtime_error(
                "Invalid AI aim settings: " + settings.validation_error);
        }
        const auto preflight = validateAiAimRuntime(
            runtime_files, directMlPolicy(settings.runtime), stop);
        if (preflight.cancelled) {
            throw std::runtime_error("AI initialization was cancelled");
        }
        if (!preflight.ready) {
            throw std::runtime_error(preflight.error);
        }
        throwIfInitializationStopped(stop);

        const AimConfig aim_config = settings.algorithm;

        DxgiFrameSourceConfig capture_config;
        capture_config.adapter_index = settings.runtime.capture_adapter_index;
        capture_config.output_index = settings.runtime.capture_output_index;
        capture_config.roi_width = aim_config.detector.input_width;
        capture_config.roi_height = aim_config.detector.input_height;
        capture_config.preferred_output = preferred_output;

        OrtDmlDetectorConfig detector_config;
        detector_config.runtime_library = runtime_files.onnx_runtime;
        detector_config.model_path = runtime_files.model;
        detector_config.prefer_directml =
            settings.runtime.prefer_directml && preflight.directml_available;
        detector_config.allow_cpu_fallback = settings.runtime.allow_cpu_fallback;
        detector_config.dml_device_index = settings.runtime.dml_device_index;
        detector_config.decoder = aim_config.detector;
        detector_config.compatibility = aim_config.compatibility;

        auto frame_source = makeDxgiFrameSource(capture_config);
        throwIfInitializationStopped(stop);
        auto detector = makeOrtDmlDetector(detector_config);
        throwIfInitializationStopped(stop);
        return AiAimCoordinator::ModuleBuild{
            std::make_unique<AimModule>(
                aim_config,
                std::move(frame_source),
                std::move(detector),
                std::move(motion_sink)),
            settings.motionMaxAge(),
            preflight.warning,
        };
    };
}

} // namespace

std::unique_ptr<AiAimCoordinator> makeProductionAiAimCoordinator(
    const ResourcePaths& paths,
    AiAimSettings settings,
    RegionManager& regions,
    std::shared_ptr<MouseMotionDispatcher> dispatcher) {
    return std::make_unique<AiAimCoordinator>(
        std::move(dispatcher),
        makeProductionModuleFactory(paths, std::move(settings), regions));
}

bool applyProductionAiAimSettings(
    AiAimCoordinator& coordinator, const ResourcePaths& paths,
    const AiAimSettings& previous, AiAimSettings settings, RegionManager& regions) {
    if (!settings.valid()) throw std::invalid_argument(settings.validation_error);
    const bool recreate = previous.runtime != settings.runtime;
    return coordinator.updateConfiguration(
        makeProductionModuleFactory(paths, settings, regions),
        settings.algorithm, settings.motionMaxAge(), recreate);
}

} // namespace pubg
