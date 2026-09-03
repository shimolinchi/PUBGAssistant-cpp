#pragma once

#include "AiAimSettings.hpp"

#include <memory>

namespace pubg {

class AiAimCoordinator;
class MouseMotionDispatcher;
class RegionManager;
class ResourcePaths;

// Application-only composition entry point. The coordinator core deliberately
// does not expose Config, RegionManager, DXGI, or ONNX Runtime dependencies.
std::unique_ptr<AiAimCoordinator> makeProductionAiAimCoordinator(
    const ResourcePaths& paths,
    AiAimSettings settings,
    RegionManager& regions,
    std::shared_ptr<MouseMotionDispatcher> dispatcher);

[[nodiscard]] bool applyProductionAiAimSettings(
    AiAimCoordinator& coordinator, const ResourcePaths& paths,
    const AiAimSettings& previous, AiAimSettings settings, RegionManager& regions);

} // namespace pubg
