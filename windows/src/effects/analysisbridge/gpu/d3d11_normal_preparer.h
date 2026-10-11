#pragma once

#include "../analysis_types.h"
#include "../normal_preparer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>

namespace PrismForge::Effects::AnalysisBridge::Gpu {

enum class GpuNormalFaultSeam {
    None = 0,
    NativeUpload_Map,
    TargetAlloc,
    ShaderCompile,
    ResampleDispatch,
    DeviceRemoved
};

enum class GpuNormalOutcome {
    Success = 0,
    InvalidArgs,
    AllocationFailed,
    ShaderError,
    DispatchFailed,
    DeviceLost,
    InjectedFault
};

/// <summary>
/// Texture-native GPU normal preparation pipeline.
/// Uploads small analysis-resolution normal maps to dynamic R32G32B32A32_FLOAT texture,
/// and executes bilinear resampling and unit vector normalization in HLSL compute.
/// Guarantees transactional execution: resets output SRV/texture on any failure,
/// preventing stale normal field leakage.
/// </summary>
class D3D11NormalPreparer {
public:
    D3D11NormalPreparer();
    ~D3D11NormalPreparer();

    D3D11NormalPreparer(const D3D11NormalPreparer&) = delete;
    D3D11NormalPreparer& operator=(const D3D11NormalPreparer&) = delete;

    D3D11NormalPreparer(D3D11NormalPreparer&&) noexcept;
    D3D11NormalPreparer& operator=(D3D11NormalPreparer&&) noexcept;

    /// <summary>
    /// Resets internal scratch/state without releasing reusable GPU textures.
    /// </summary>
    void Reset() noexcept;

    /// <summary>
    /// Releases all owned GPU textures, buffers, and views.
    /// </summary>
    void ReleaseResources() noexcept;

    /// <summary>
    /// Prepares small native normals into a target-resolution R32G32B32A32_FLOAT GPU texture.
    /// </summary>
    GpuNormalOutcome PrepareNormals(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const NormalBuffer& srcNormals,
        int targetWidth,
        int targetHeight,
        const NormalPreparationConfig& config,
        GpuNormalFaultSeam faultSeam = GpuNormalFaultSeam::None,
        HRESULT customFaultHr = E_FAIL,
        std::string* outErrorMessage = nullptr
    );

    /// <summary>
    /// Query output shader resource view of the prepared normals texture.
    /// </summary>
    ID3D11ShaderResourceView* GetOutputSRV() const noexcept;

    /// <summary>
    /// Query output texture resource.
    /// </summary>
    ID3D11Texture2D* GetOutputTexture() const noexcept;

    int GetWidth() const noexcept;
    int GetHeight() const noexcept;
    bool HasOutput() const noexcept;

    /// <summary>
    /// Diagnostic helper: reads back GPU output pixels into a CPU vector for verification.
    /// Reads 4 floats per pixel: (nx, ny, nz, 1.0f).
    /// </summary>
    bool ReadbackPixels(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        std::vector<float>& outPixels,
        int& outW,
        int& outH
    );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::AnalysisBridge::Gpu
