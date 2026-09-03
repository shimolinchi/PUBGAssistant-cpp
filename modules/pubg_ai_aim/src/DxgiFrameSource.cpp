#include "pubg_ai_aim/WindowsAdapters.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace pubg::ai_aim {
namespace {

using Microsoft::WRL::ComPtr;

class FrameReleaseGuard {
public:
    explicit FrameReleaseGuard(ComPtr<IDXGIOutputDuplication> duplication) noexcept
        : duplication_(std::move(duplication)) {}
    ~FrameReleaseGuard() {
        if (duplication_) {
            duplication_->ReleaseFrame();
        }
    }
    FrameReleaseGuard(const FrameReleaseGuard&) = delete;
    FrameReleaseGuard& operator=(const FrameReleaseGuard&) = delete;

private:
    ComPtr<IDXGIOutputDuplication> duplication_;
};

class MapGuard {
public:
    MapGuard(ID3D11DeviceContext* context, ID3D11Texture2D* texture) noexcept
        : context_(context), texture_(texture) {}
    ~MapGuard() {
        if (context_ && texture_) {
            context_->Unmap(texture_, 0);
        }
    }
    MapGuard(const MapGuard&) = delete;
    MapGuard& operator=(const MapGuard&) = delete;

private:
    ID3D11DeviceContext* context_;
    ID3D11Texture2D* texture_;
};

class DxgiFrameSource final : public IFrameSource {
public:
    explicit DxgiFrameSource(DxgiFrameSourceConfig config) : config_(config) {
        config_.roi_width = std::max(config_.roi_width, 1);
        config_.roi_height = std::max(config_.roi_height, 1);
        initialize();
    }

    CaptureStatus capture(Frame& frame, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return CaptureStatus::Stopped;
        }
        if (!duplication_ && !initialize()) {
            return CaptureStatus::RecoverableError;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquire = duplication_->AcquireNextFrame(0, &info, &resource);
        if (acquire == DXGI_ERROR_WAIT_TIMEOUT) {
            return CaptureStatus::NoFrame;
        }
        if (acquire == DXGI_ERROR_ACCESS_LOST) {
            resetDevice();
            initialize();
            return CaptureStatus::RecoverableError;
        }
        if (FAILED(acquire)) {
            resetDevice();
            return CaptureStatus::RecoverableError;
        }
        const Clock::time_point acquired_at = Clock::now();
        // Keep the acquired duplication alive until ReleaseFrame, even when an
        // error path resets the source's device members below.
        FrameReleaseGuard release_frame(duplication_);

        ComPtr<ID3D11Texture2D> desktop_texture;
        const HRESULT texture_result = resource.As(&desktop_texture);
        resource.Reset();
        if (FAILED(texture_result)) {
            return CaptureStatus::RecoverableError;
        }

        D3D11_TEXTURE2D_DESC desktop_desc{};
        desktop_texture->GetDesc(&desktop_desc);
        if (!ensureStaging(desktop_desc)) {
            return CaptureStatus::RecoverableError;
        }

        D3D11_BOX source_box{};
        source_box.left = static_cast<UINT>(roi_left_);
        source_box.top = static_cast<UINT>(roi_top_);
        source_box.right = static_cast<UINT>(roi_left_ + roi_width_);
        source_box.bottom = static_cast<UINT>(roi_top_ + roi_height_);
        source_box.front = 0;
        source_box.back = 1;
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop_texture.Get(), 0, &source_box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = context_->Map(
            staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(map_result)) {
            resetDevice();
            return CaptureStatus::RecoverableError;
        }
        MapGuard unmap(context_.Get(), staging_.Get());

        frame.sequence = ++sequence_;
        frame.timestamp = acquired_at;
        frame.width = roi_width_;
        frame.height = roi_height_;
        frame.stride = roi_width_ * 4;
        frame.format = PixelFormat::Bgra8;
        frame.desktop_left = output_desc_.DesktopCoordinates.left + roi_left_;
        frame.desktop_top = output_desc_.DesktopCoordinates.top + roi_top_;
        frame.screen_width = static_cast<int>(desktop_desc.Width);
        frame.screen_height = static_cast<int>(desktop_desc.Height);
        frame.screen_center_x = output_desc_.DesktopCoordinates.left +
            static_cast<int>(desktop_desc.Width / 2);
        frame.screen_center_y = output_desc_.DesktopCoordinates.top +
            static_cast<int>(desktop_desc.Height / 2);
        frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);

        const auto* source = static_cast<const std::byte*>(mapped.pData);
        for (int row = 0; row < frame.height; ++row) {
            std::memcpy(
                frame.pixels.data() + static_cast<std::size_t>(row) * frame.stride,
                source + static_cast<std::size_t>(row) * mapped.RowPitch,
                static_cast<std::size_t>(frame.stride));
        }
        return CaptureStatus::FrameReady;
    }

private:
    bool initialize() {
        resetDevice();

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        if (!selectOutput(factory.Get())) {
            return false;
        }
        if (output_desc_.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
            return false;
        }

        D3D_FEATURE_LEVEL feature_level{};
        constexpr D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        const HRESULT create = D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device_,
            &feature_level,
            &context_);
        if (FAILED(create)) {
            return false;
        }

        ComPtr<IDXGIDevice1> dxgi_device;
        if (SUCCEEDED(device_.As(&dxgi_device))) {
            dxgi_device->SetMaximumFrameLatency(1);
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output_.As(&output1))) {
            return false;
        }
        return SUCCEEDED(output1->DuplicateOutput(device_.Get(), &duplication_));
    }

    bool selectOutput(IDXGIFactory1* factory) {
        if (!factory) {
            return false;
        }
        if (config_.preferred_output.has_value() &&
            trySelectPreferredOutput(factory, *config_.preferred_output)) {
            return true;
        }
        if (FAILED(factory->EnumAdapters1(config_.adapter_index, &adapter_))) {
            return false;
        }
        if (FAILED(adapter_->EnumOutputs(config_.output_index, &output_))) {
            return false;
        }
        return SUCCEEDED(output_->GetDesc(&output_desc_));
    }

    bool trySelectPreferredOutput(IDXGIFactory1* factory, const DesktopRect& preferred) {
        if (preferred.width <= 0 || preferred.height <= 0) {
            return false;
        }

        const std::int64_t preferred_right =
            static_cast<std::int64_t>(preferred.left) + preferred.width;
        const std::int64_t preferred_bottom =
            static_cast<std::int64_t>(preferred.top) + preferred.height;
        const std::int64_t center_x =
            static_cast<std::int64_t>(preferred.left) + preferred.width / 2;
        const std::int64_t center_y =
            static_cast<std::int64_t>(preferred.top) + preferred.height / 2;

        ComPtr<IDXGIAdapter1> center_adapter;
        ComPtr<IDXGIOutput> center_output;
        DXGI_OUTPUT_DESC center_desc{};
        for (UINT adapter_index = 0;; ++adapter_index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapter_result = factory->EnumAdapters1(adapter_index, &adapter);
            if (adapter_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(adapter_result)) {
                continue;
            }
            for (UINT output_index = 0;; ++output_index) {
                ComPtr<IDXGIOutput> output;
                const HRESULT output_result = adapter->EnumOutputs(output_index, &output);
                if (output_result == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(output_result)) {
                    continue;
                }
                DXGI_OUTPUT_DESC desc{};
                if (FAILED(output->GetDesc(&desc))) {
                    continue;
                }
                const RECT& rect = desc.DesktopCoordinates;
                const bool exact =
                    rect.left == preferred.left && rect.top == preferred.top &&
                    static_cast<std::int64_t>(rect.right) == preferred_right &&
                    static_cast<std::int64_t>(rect.bottom) == preferred_bottom;
                if (exact) {
                    adapter_ = std::move(adapter);
                    output_ = std::move(output);
                    output_desc_ = desc;
                    return true;
                }
                const bool contains_center =
                    center_x >= rect.left && center_x < rect.right &&
                    center_y >= rect.top && center_y < rect.bottom;
                if (contains_center && !center_output) {
                    center_adapter = adapter;
                    center_output = output;
                    center_desc = desc;
                }
            }
        }
        if (!center_output) {
            return false;
        }
        adapter_ = std::move(center_adapter);
        output_ = std::move(center_output);
        output_desc_ = center_desc;
        return true;
    }

    bool ensureStaging(const D3D11_TEXTURE2D_DESC& desktop_desc) {
        const int width = static_cast<int>(desktop_desc.Width);
        const int height = static_cast<int>(desktop_desc.Height);
        const int wanted_width = std::min(config_.roi_width, width);
        const int wanted_height = std::min(config_.roi_height, height);
        const int wanted_left = std::max((width - wanted_width) / 2, 0);
        const int wanted_top = std::max((height - wanted_height) / 2, 0);
        if (staging_ && wanted_width == roi_width_ && wanted_height == roi_height_ &&
            wanted_left == roi_left_ && wanted_top == roi_top_ &&
            desktop_desc.Format == staging_format_) {
            return true;
        }

        staging_.Reset();
        D3D11_TEXTURE2D_DESC staging_desc{};
        staging_desc.Width = static_cast<UINT>(wanted_width);
        staging_desc.Height = static_cast<UINT>(wanted_height);
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.Format = desktop_desc.Format;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr, &staging_))) {
            return false;
        }
        roi_width_ = wanted_width;
        roi_height_ = wanted_height;
        roi_left_ = wanted_left;
        roi_top_ = wanted_top;
        staging_format_ = desktop_desc.Format;
        return true;
    }

    void resetDevice() noexcept {
        staging_.Reset();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        output_.Reset();
        adapter_.Reset();
        roi_width_ = 0;
        roi_height_ = 0;
        roi_left_ = 0;
        roi_top_ = 0;
        staging_format_ = DXGI_FORMAT_UNKNOWN;
    }

    DxgiFrameSourceConfig config_;
    DXGI_OUTPUT_DESC output_desc_{};
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    int roi_width_ = 0;
    int roi_height_ = 0;
    int roi_left_ = 0;
    int roi_top_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
    std::uint64_t sequence_ = 0;
};

} // namespace

std::unique_ptr<IFrameSource> makeDxgiFrameSource(const DxgiFrameSourceConfig& config) {
    return std::make_unique<DxgiFrameSource>(config);
}

} // namespace pubg::ai_aim
