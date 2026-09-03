#include "pubg_ai_aim/WindowsAdapters.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <onnxruntime_c_api.h>

#include "pubg_ai_aim/Algorithms.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pubg::ai_aim {
namespace {

using AppendDmlFn = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);

std::runtime_error windowsError(const char* operation) {
    return std::runtime_error(
        std::string(operation) + " failed with Win32 error " + std::to_string(GetLastError()));
}

class OrtDmlDetector final : public IObjectDetector {
public:
    explicit OrtDmlDetector(OrtDmlDetectorConfig config) : config_(std::move(config)) {
        applyDecoderConfig(config_.decoder, config_.compatibility);
        try {
            loadRuntime();
            initializeSession();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~OrtDmlDetector() override { cleanup(); }

    bool updateDecoderConfig(
        const DetectorConfig& config,
        CompatibilityMode compatibility) override {
        applyDecoderConfig(config, compatibility);
        return true;
    }

    std::string_view backendName() const noexcept override {
        return dml_enabled_ ? "DirectML" : "CPU";
    }

    std::vector<Detection> infer(const Frame& frame, std::stop_token stop) override {
        if (stop.stop_requested()) {
            throw std::runtime_error("ONNX inference cancelled before Run");
        }
        preprocess(frame);

        const std::array<std::int64_t, 4> dimensions{
            1,
            3,
            config_.decoder.input_height,
            config_.decoder.input_width,
        };
        OrtValue* input = nullptr;
        check(api_->CreateTensorWithDataAsOrtValue(
            memory_info_,
            input_tensor_.data(),
            input_tensor_.size() * sizeof(float),
            dimensions.data(),
            dimensions.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &input));
        ValueGuard input_guard(api_, input);

        const char* input_names[]{config_.input_name.c_str()};
        const char* output_names[]{config_.output_name.c_str()};
        OrtValue* output = nullptr;
        check(api_->Run(
            session_,
            nullptr,
            input_names,
            &input,
            1,
            output_names,
            1,
            &output));
        ValueGuard output_guard(api_, output);

        int is_tensor = 0;
        check(api_->IsTensor(output, &is_tensor));
        if (is_tensor == 0) {
            throw std::runtime_error("ONNX output0 is not a tensor");
        }

        OrtTensorTypeAndShapeInfo* shape = nullptr;
        check(api_->GetTensorTypeAndShape(output, &shape));
        ShapeGuard shape_guard(api_, shape);
        ONNXTensorElementDataType element_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        check(api_->GetTensorElementType(shape, &element_type));
        if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("ONNX output0 must use FLOAT elements");
        }

        std::size_t rank = 0;
        check(api_->GetDimensionsCount(shape, &rank));
        if (rank != 3) {
            throw std::runtime_error("ONNX output0 must have rank 3 with layout [1,C,N]");
        }
        std::array<std::int64_t, 3> output_dimensions{};
        check(api_->GetDimensions(
            shape, output_dimensions.data(), output_dimensions.size()));
        const std::int64_t expected_channels =
            static_cast<std::int64_t>(config_.decoder.class_count) + 4;
        const std::array<std::int64_t, 3> expected_dimensions{
            1,
            expected_channels,
            static_cast<std::int64_t>(config_.decoder.anchor_count),
        };
        if (output_dimensions != expected_dimensions) {
            throw std::runtime_error("ONNX output0 layout does not match configured [1,C,N]");
        }

        std::size_t element_count = 0;
        check(api_->GetTensorShapeElementCount(shape, &element_count));
        const std::size_t required =
            static_cast<std::size_t>(expected_channels) *
            static_cast<std::size_t>(config_.decoder.anchor_count);
        if (element_count != required) {
            throw std::runtime_error("ONNX output0 element count does not match the YOLOv8 layout");
        }

        float* output_data = nullptr;
        check(api_->GetTensorMutableData(output, reinterpret_cast<void**>(&output_data)));
        std::vector<Detection> detections = decodeYoloV8(
            std::span<const float>(output_data, required),
            config_.decoder,
            config_.compatibility);
        const float scale_x = static_cast<float>(frame.width) /
            static_cast<float>(config_.decoder.input_width);
        const float scale_y = static_cast<float>(frame.height) /
            static_cast<float>(config_.decoder.input_height);
        scaleDetectionBoxes(detections, scale_x, scale_y);
        return detections;
    }

private:
    void applyDecoderConfig(
        const DetectorConfig& decoder,
        CompatibilityMode compatibility) {
        AimConfig sanitized;
        sanitized.detector = decoder;
        sanitized.compatibility = compatibility;
        sanitized.sanitize();
        config_.decoder = sanitized.detector;
        config_.compatibility = sanitized.compatibility;
    }

    class ValueGuard {
    public:
        ValueGuard(const OrtApi* api, OrtValue* value) noexcept : api_(api), value_(value) {}
        ~ValueGuard() { if (value_) api_->ReleaseValue(value_); }
        ValueGuard(const ValueGuard&) = delete;
        ValueGuard& operator=(const ValueGuard&) = delete;
    private:
        const OrtApi* api_;
        OrtValue* value_;
    };

    class ShapeGuard {
    public:
        ShapeGuard(const OrtApi* api, OrtTensorTypeAndShapeInfo* value) noexcept
            : api_(api), value_(value) {}
        ~ShapeGuard() { if (value_) api_->ReleaseTensorTypeAndShapeInfo(value_); }
        ShapeGuard(const ShapeGuard&) = delete;
        ShapeGuard& operator=(const ShapeGuard&) = delete;
    private:
        const OrtApi* api_;
        OrtTensorTypeAndShapeInfo* value_;
    };

    void loadRuntime() {
        const std::filesystem::path absolute = std::filesystem::absolute(config_.runtime_library);
        runtime_ = LoadLibraryExW(
            absolute.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!runtime_) {
            throw windowsError("LoadLibraryExW(onnxruntime.dll)");
        }
        const auto get_api_base = reinterpret_cast<decltype(&OrtGetApiBase)>(
            GetProcAddress(runtime_, "OrtGetApiBase"));
        if (!get_api_base) {
            throw std::runtime_error("onnxruntime.dll does not export OrtGetApiBase");
        }
        const OrtApiBase* api_base = get_api_base();
        if (!api_base) {
            throw std::runtime_error("OrtGetApiBase returned null");
        }
        api_ = api_base->GetApi(ORT_API_VERSION);
        if (!api_) {
            throw std::runtime_error("onnxruntime.dll rejected ORT API version 15");
        }
        append_dml_ = reinterpret_cast<AppendDmlFn>(
            GetProcAddress(runtime_, "OrtSessionOptionsAppendExecutionProvider_DML"));
    }

    void initializeSession() {
        check(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "pubg_ai_aim", &env_));
        const std::filesystem::path model = std::filesystem::absolute(config_.model_path);
        configureSessionOptions(config_.prefer_directml);
        OrtStatus* create_status =
            api_->CreateSession(env_, model.c_str(), session_options_, &session_);
        if (create_status && dml_enabled_ && config_.allow_cpu_fallback) {
            api_->ReleaseStatus(create_status);
            if (session_) {
                api_->ReleaseSession(session_);
                session_ = nullptr;
            }
            configureSessionOptions(false);
            check(api_->CreateSession(env_, model.c_str(), session_options_, &session_));
        } else {
            check(create_status);
        }
        check(api_->CreateCpuMemoryInfo(
            OrtArenaAllocator, OrtMemTypeDefault, &memory_info_));
    }

    void createBaseSessionOptions() {
        check(api_->CreateSessionOptions(&session_options_));
        check(api_->SetSessionGraphOptimizationLevel(session_options_, ORT_ENABLE_BASIC));
        check(api_->DisableMemPattern(session_options_));
    }

    void configureSessionOptions(bool request_dml) {
        if (session_options_) {
            api_->ReleaseSessionOptions(session_options_);
            session_options_ = nullptr;
        }
        dml_enabled_ = false;
        createBaseSessionOptions();
        if (!request_dml) {
            return;
        }
        if (!append_dml_) {
            if (!config_.allow_cpu_fallback) {
                throw std::runtime_error("ONNX Runtime does not export the DirectML provider");
            }
            return;
        }
        if (OrtStatus* status = append_dml_(session_options_, config_.dml_device_index)) {
            const char* message = api_->GetErrorMessage(status);
            const std::string copy = message ? message : "DirectML provider initialization failed";
            api_->ReleaseStatus(status);
            if (!config_.allow_cpu_fallback) {
                throw std::runtime_error(copy);
            }
            api_->ReleaseSessionOptions(session_options_);
            session_options_ = nullptr;
            createBaseSessionOptions();
            return;
        }
        dml_enabled_ = true;
    }

    void check(OrtStatus* status) const {
        if (!status) {
            return;
        }
        const char* message = api_->GetErrorMessage(status);
        std::string copy = message ? message : "unknown ONNX Runtime error";
        api_->ReleaseStatus(status);
        throw std::runtime_error(copy);
    }

    void preprocess(const Frame& frame) {
        if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.pixels.empty()) {
            throw std::invalid_argument("frame is empty");
        }
        int channels = 0;
        switch (frame.format) {
        case PixelFormat::Bgra8:
            channels = 4;
            break;
        case PixelFormat::Bgr8:
        case PixelFormat::Rgb8:
            channels = 3;
            break;
        default:
            throw std::invalid_argument("unsupported pixel format");
        }
        if (frame.stride < frame.width * channels ||
            frame.pixels.size() < static_cast<std::size_t>(frame.stride) * frame.height) {
            throw std::invalid_argument("frame stride or storage is invalid");
        }

        const int output_width = config_.decoder.input_width;
        const int output_height = config_.decoder.input_height;
        const std::size_t plane = static_cast<std::size_t>(output_width) * output_height;
        input_tensor_.resize(plane * 3);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.pixels.data());

        for (int y = 0; y < output_height; ++y) {
            const int source_y = std::min(
                frame.height - 1,
                static_cast<int>((static_cast<std::int64_t>(y) * frame.height) / output_height));
            const auto* row = bytes + static_cast<std::size_t>(source_y) * frame.stride;
            for (int x = 0; x < output_width; ++x) {
                const int source_x = std::min(
                    frame.width - 1,
                    static_cast<int>((static_cast<std::int64_t>(x) * frame.width) / output_width));
                const auto* pixel = row + static_cast<std::size_t>(source_x) * channels;
                std::uint8_t red = 0;
                std::uint8_t green = pixel[1];
                std::uint8_t blue = 0;
                if (frame.format == PixelFormat::Rgb8) {
                    red = pixel[0];
                    blue = pixel[2];
                } else {
                    blue = pixel[0];
                    red = pixel[2];
                }
                const std::size_t index = static_cast<std::size_t>(y) * output_width + x;
                input_tensor_[index] = static_cast<float>(red) / 255.0F;
                input_tensor_[plane + index] = static_cast<float>(green) / 255.0F;
                input_tensor_[2 * plane + index] = static_cast<float>(blue) / 255.0F;
            }
        }
    }

    void cleanup() noexcept {
        if (api_) {
            if (memory_info_) {
                api_->ReleaseMemoryInfo(memory_info_);
                memory_info_ = nullptr;
            }
            if (session_) {
                api_->ReleaseSession(session_);
                session_ = nullptr;
            }
            if (session_options_) {
                api_->ReleaseSessionOptions(session_options_);
                session_options_ = nullptr;
            }
            if (env_) {
                api_->ReleaseEnv(env_);
                env_ = nullptr;
            }
        }
        append_dml_ = nullptr;
        api_ = nullptr;
        if (runtime_) {
            FreeLibrary(runtime_);
            runtime_ = nullptr;
        }
    }

    OrtDmlDetectorConfig config_;
    HMODULE runtime_ = nullptr;
    const OrtApi* api_ = nullptr;
    AppendDmlFn append_dml_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSessionOptions* session_options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_info_ = nullptr;
    bool dml_enabled_ = false;
    std::vector<float> input_tensor_;
};

} // namespace

std::unique_ptr<IObjectDetector> makeOrtDmlDetector(const OrtDmlDetectorConfig& config) {
    return std::make_unique<OrtDmlDetector>(config);
}

} // namespace pubg::ai_aim
