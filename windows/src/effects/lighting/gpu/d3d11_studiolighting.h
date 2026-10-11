#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "../studiolighting.h"

namespace PrismForge::Effects::Lighting::Gpu {

/// <summary>
/// Status outcome for GPU lighting operations.
/// </summary>
enum class GpuLightingOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    DeviceLost = 5,
    ShaderError = 6,
    ResourceCreationFailed = 7
};

/// <summary>
/// Texture-Native D3D11 Studio Lighting Engine.
/// 
/// Executes a Compute Shader on caller-provided ID3D11Device and ID3D11DeviceContext.
/// Operates on SDR RGBA/BGRA textures in linear space, with optional mask and normal SRVs.
/// 
/// Features:
/// - Exact mathematical parity with CPU StudioLighting.
/// - Unbinds all resources (SRVs, UAVs, CBs, CS) after dispatch to prevent bind hazards.
/// - Device loss detection (DXGI_ERROR_DEVICE_REMOVED / RESET) with explicit recovery.
/// </summary>
class D3D11StudioLighting {
public:
    D3D11StudioLighting(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11StudioLighting();

    D3D11StudioLighting(const D3D11StudioLighting&) = delete;
    D3D11StudioLighting& operator=(const D3D11StudioLighting&) = delete;
    D3D11StudioLighting(D3D11StudioLighting&&) noexcept;
    D3D11StudioLighting& operator=(D3D11StudioLighting&&) noexcept;

    /// <summary>
    /// Processes texture inputs using the GPU compute shader.
    /// </summary>
    /// <param name="inputSRV">Source SDR color texture (RGBA/BGRA).</param>
    /// <param name="outputUAV">Destination writable texture (UAV).</param>
    /// <param name="maskSRV">Optional single-channel or alpha mask SRV.</param>
    /// <param name="normalsSRV">Optional 3D surface normal SRV.</param>
    /// <param name="width">Pixel width.</param>
    /// <param name="height">Pixel height.</param>
    /// <param name="config">Lighting configuration snapshot.</param>
    /// <returns>GpuLightingOutcome code.</returns>
    GpuLightingOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* maskSRV,
        ID3D11ShaderResourceView* normalsSRV,
        ID3D11ShaderResourceView* depthSRV,
        int width, int height,
        const LightingConfig& config
    );

    GpuLightingOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* maskSRV,
        ID3D11ShaderResourceView* normalsSRV,
        int width, int height,
        const LightingConfig& config
    ) {
        return Process(inputSRV, outputUAV, maskSRV, normalsSRV, nullptr, width, height, config);
    }

    /// <summary>
    /// Resets device and context after device loss or recreation.
    /// </summary>
    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);

    /// <summary>
    /// Returns true if the underlying device has been removed or reset.
    /// </summary>
    bool IsDeviceLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// <summary>
/// Standalone Test Adapter & CPU Fallback Wrapper.
/// 
/// Accepts software AVFrames (BGR24) and optional AlphaMask / NormalField descriptors.
/// Uploads data to D3D11 textures, executes the GPU operator, and reads back the result.
/// 
/// If any GPU error occurs (unsupported format, compilation failure, or device loss),
/// it automatically falls back to CPU StudioLighting, ensuring deterministic continuity.
/// Measures staging upload, GPU dispatch, and staging readback independently.
/// </summary>
class D3D11StudioLightingAdapter {
public:
    struct TimingStats {
        double uploadMs = 0.0;
        double dispatchGpuMs = 0.0;
        double readbackMs = 0.0;
        double totalRoundtripMs = 0.0;
        bool fallbackUsed = false;
    };

    D3D11StudioLightingAdapter(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11StudioLightingAdapter();

    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const AlphaMask* mask,
        const NormalField* normals,
        const DepthField* depth,
        const LightingConfig& config,
        LightingOutcome& outcome,
        TimingStats* outTiming = nullptr
    );

    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const AlphaMask* mask,
        const NormalField* normals,
        const LightingConfig& config,
        LightingOutcome& outcome,
        TimingStats* outTiming = nullptr
    ) {
        return ProcessFrame(inputFrame, mask, normals, nullptr, config, outcome, outTiming);
    }

    D3D11StudioLighting* GetGpuOperator();
    StudioLighting* GetCpuFallback();

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::Lighting::Gpu
