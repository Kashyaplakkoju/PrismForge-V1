#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "windows/src/effects/cubelut.h"
#include "d3d11_desktop_looks.h"

namespace PrismForge::Effects::StudioStack::Gpu {

/// <summary>
/// Status outcome for GPU LUT operations.
/// </summary>
enum class GpuLutOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    DeviceLost = 5,
    ShaderError = 6
};

/// <summary>
/// Texture-Native D3D11 3D LUT Engine for StudioStack.
/// Executes Compute Shader on caller-provided ID3D11Device and context.
/// Implements .cube LUT domain handling, trilinear interpolation, and intensity blending
/// with exact parity to the CPU CubeLut reference.
/// </summary>
class D3D11StudioStackLut {
public:
    D3D11StudioStackLut(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11StudioStackLut();

    D3D11StudioStackLut(const D3D11StudioStackLut&) = delete;
    D3D11StudioStackLut& operator=(const D3D11StudioStackLut&) = delete;
    D3D11StudioStackLut(D3D11StudioStackLut&&) noexcept;
    D3D11StudioStackLut& operator=(D3D11StudioStackLut&&) noexcept;

    GpuLutOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        int width, int height,
        const CubeLut* lut,
        float intensity
    );

    // Owning immutable snapshot fast path. Cache retains one snapshot and one 3D texture;
    // content-equivalent replacement snapshots reuse the texture, changed same-size data refreshes it.
    GpuLutOutcome Process(ID3D11ShaderResourceView* inputSRV, ID3D11UnorderedAccessView* outputUAV,
        int width, int height, std::shared_ptr<const CubeLut> lut, float intensity);
    void SetFault(DesktopColourFault fault);
    size_t GetUploadCount() const;
    size_t GetCachedTextureCount() const;

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::StudioStack::Gpu
