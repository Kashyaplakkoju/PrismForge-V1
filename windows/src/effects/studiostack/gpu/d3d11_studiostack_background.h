#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "windows/src/effects/backgroundcompositor.h"

namespace PrismForge::Effects::StudioStack::Gpu {

/// <summary>
/// Status outcome for GPU background composition operations.
/// </summary>
enum class GpuBackgroundOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    DeviceLost = 5,
    ShaderError = 6
};

/// <summary>
/// Texture-Native D3D11 Background Compositing Engine for StudioStack.
/// Executes Compute Shaders on caller-provided ID3D11Device and context.
/// Implements masked background replacement, separable box blur, and solid color fill
/// preserving foreground and detail semantics with bit-exact / threshold parity.
/// </summary>
class D3D11StudioStackBackground {
public:
    D3D11StudioStackBackground(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11StudioStackBackground();

    D3D11StudioStackBackground(const D3D11StudioStackBackground&) = delete;
    D3D11StudioStackBackground& operator=(const D3D11StudioStackBackground&) = delete;
    D3D11StudioStackBackground(D3D11StudioStackBackground&&) noexcept;
    D3D11StudioStackBackground& operator=(D3D11StudioStackBackground&&) noexcept;

    GpuBackgroundOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* maskSRV,
        int width, int height,
        const BackgroundSettings& settings,
        ID3D11ShaderResourceView* bgImageSRV = nullptr,
        int bgImageW = 0,
        int bgImageH = 0,
        int fitMode = 0,
        bool isImageMode = false
    );

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::StudioStack::Gpu
