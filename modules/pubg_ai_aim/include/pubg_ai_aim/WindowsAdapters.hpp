#pragma once

#include "pubg_ai_aim/AimConfig.hpp"
#include "pubg_ai_aim/Interfaces.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace pubg::ai_aim {

struct DesktopRect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct DxgiFrameSourceConfig {
    unsigned adapter_index = 0;
    unsigned output_index = 0;
    int roi_width = 320;
    int roi_height = 320;
    // When present, enumerate all DXGI outputs and prefer the one matching
    // this physical-desktop rectangle. adapter/output indices remain fallback.
    std::optional<DesktopRect> preferred_output;
};

struct OrtDmlDetectorConfig {
    std::filesystem::path runtime_library = L"onnxruntime.dll";
    std::filesystem::path model_path = L"PUBGV8.onnx";
    std::string input_name = "images";
    std::string output_name = "output0";
    bool prefer_directml = true;
    bool allow_cpu_fallback = true;
    int dml_device_index = 0;
    DetectorConfig decoder{};
    CompatibilityMode compatibility = CompatibilityMode::Corrected;
};

[[nodiscard]] std::unique_ptr<IFrameSource> makeDxgiFrameSource(
    const DxgiFrameSourceConfig& config);

[[nodiscard]] std::unique_ptr<IObjectDetector> makeOrtDmlDetector(
    const OrtDmlDetectorConfig& config);

} // namespace pubg::ai_aim
