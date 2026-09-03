#include "pubg_ai_aim/WindowsAdapters.hpp"

#include <stdexcept>

namespace pubg::ai_aim {

std::unique_ptr<IFrameSource> makeDxgiFrameSource(const DxgiFrameSourceConfig&) {
    throw std::runtime_error("DXGI frame capture is available only on Windows");
}

std::unique_ptr<IObjectDetector> makeOrtDmlDetector(const OrtDmlDetectorConfig&) {
    throw std::runtime_error("the bundled ORT/DirectML adapter is available only on Windows");
}

} // namespace pubg::ai_aim
