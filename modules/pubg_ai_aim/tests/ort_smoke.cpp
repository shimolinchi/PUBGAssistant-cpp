#include "pubg_ai_aim/pubg_ai_aim.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace aim = pubg::ai_aim;

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (argc != 3 && argc != 4) {
        std::cout << "SKIP: pass onnxruntime.dll, PUBGV8.onnx, and optional --cpu\n";
        return 0;
    }

    try {
        aim::OrtDmlDetectorConfig config;
        config.runtime_library = std::filesystem::path(argv[1]);
        config.model_path = std::filesystem::path(argv[2]);
        const bool force_cpu = argc == 4 && std::string_view(argv[3]) == "--cpu";
        config.prefer_directml = !force_cpu;
        auto detector = aim::makeOrtDmlDetector(config);
        if (force_cpu && detector->backendName() != "CPU") {
            throw std::runtime_error("--cpu did not select the CPU provider");
        }

        aim::Frame frame;
        frame.width = 320;
        frame.height = 320;
        frame.stride = 320 * 4;
        frame.format = aim::PixelFormat::Bgra8;
        frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);
        const auto detections = detector->infer(frame);
        std::cout << "ORT smoke passed; backend=" << detector->backendName()
                  << "; zero-frame detections=" << detections.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ORT smoke failed: " << error.what() << '\n';
        return 1;
    }
}
