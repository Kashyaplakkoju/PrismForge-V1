#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "../approximate_shadows.h"
#include "effects/lighting/studiolighting.h"

namespace PrismForge::Effects::Relighting::Gpu {

/// <summary>
/// Status outcome for GPU approximate geometry shadow operations.
/// </summary>
enum class GpuShadowOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    DeviceLost = 4,
    ShaderError = 5,
    ResourceCreationFailed = 6
};

/// <summary>
/// Injectable fault seams for testing robust failure and transactional reset behavior.
/// </summary>
enum class GpuShadowFaultSeam : int {
    None = 0,
    DepthTex_Creation = 1,
    OutputUav_Creation = 2,
    Cb_Creation = 3,
    Shader_Compile = 4,
    Dispatch = 5,
    Device_Removed = 6
};

/// <summary>
/// Texture-Native D3D11 Approximate Geometry Shadows Engine.
/// 
/// Evaluates screen-space ray-marched self-occlusion visibility for the two studio lights
/// in a Compute Shader, writing two float visibilities (V1, V2) into a DXGI_FORMAT_R32G32_FLOAT texture.
/// 
/// Features:
/// - Exact mathematical parity with CPU ApproximateShadows.
/// - Transactional safety: active SRV and texture are cleared on any failure, guaranteeing zero stale leakage.
/// - Unbinds all resources (SRVs, UAVs, CBs, CS) after dispatch to prevent bind hazards.
/// - Device loss detection (DXGI_ERROR_DEVICE_REMOVED / RESET) with explicit recovery.
/// </summary>
class D3D11ApproximateShadows {
public:
    D3D11ApproximateShadows(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11ApproximateShadows();

    D3D11ApproximateShadows(const D3D11ApproximateShadows&) = delete;
    D3D11ApproximateShadows& operator=(const D3D11ApproximateShadows&) = delete;
    D3D11ApproximateShadows(D3D11ApproximateShadows&&) noexcept;
    D3D11ApproximateShadows& operator=(D3D11ApproximateShadows&&) noexcept;

    /// <summary>
    /// Processes texture inputs directly into caller's UAV.
    /// </summary>
    GpuShadowOutcome Process(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalsSRV,
        ID3D11ShaderResourceView* maskSRV,
        ID3D11UnorderedAccessView* outputUAV,
        int width, int height,
        const PrismForge::Effects::Lighting::LightingConfig& config
    );

    /// <summary>
    /// Computes shadows into an internally managed R32G32_FLOAT texture, accessible via GetOutputSRV().
    /// </summary>
    GpuShadowOutcome ExecuteShadowPass(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalsSRV,
        ID3D11ShaderResourceView* maskSRV,
        int width, int height,
        const PrismForge::Effects::Lighting::LightingConfig& config
    );

    ID3D11ShaderResourceView* GetOutputSRV() const;
    ID3D11Texture2D* GetOutputTexture() const;
    bool HasOutput() const;

    void ReleaseResources();
    void SetFaultSeam(GpuShadowFaultSeam seam);

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

    /// <summary>
    /// Standalone test helper: uploads CPU buffers, runs compute shader, and reads back GPU results.
    /// </summary>
    bool UploadDepthAndCompute(
        const DepthBuffer& depth,
        const NormalBuffer* normals,
        const PrismForge::Effects::Lighting::AlphaMask* mask,
        const PrismForge::Effects::Lighting::LightingConfig& config,
        ShadowMapBuffer& outShadows
    );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::Relighting::Gpu
