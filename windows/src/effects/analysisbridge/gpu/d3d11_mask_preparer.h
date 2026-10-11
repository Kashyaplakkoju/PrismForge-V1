#pragma once

#include "../analysis_types.h"
#include "../mask_preparer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>

namespace PrismForge::Effects::AnalysisBridge::Gpu {

enum class GpuMaskFaultSeam {
    None = 0,
    NativeUpload_Map,
    TargetAlloc,
    ShaderCompile,
    ResampleDispatch,
    FeatherDispatch,
    TemporalDispatch,
    DeviceRemoved,
    ResampleConstants_Map,
    FeatherConstants_Map,
    TemporalConstants_Map,
    PartialTargetViewCreation
};

enum class GpuMaskOutcome {
    Success = 0,
    InvalidArgs,
    AllocationFailed,
    ShaderError,
    DispatchFailed,
    DeviceLost,
    InjectedFault
};

/// <summary>
/// Texture-native GPU mask preparation pipeline.
/// Uploads small analysis-resolution masks, executes bilinear resampling,
/// separable box feathering, and temporal stabilization entirely on GPU.
/// </summary>
class D3D11MaskPreparer {
public:
    D3D11MaskPreparer();
    ~D3D11MaskPreparer();

    D3D11MaskPreparer(const D3D11MaskPreparer&) = delete;
    D3D11MaskPreparer& operator=(const D3D11MaskPreparer&) = delete;

    D3D11MaskPreparer(D3D11MaskPreparer&&) noexcept;
    D3D11MaskPreparer& operator=(D3D11MaskPreparer&&) noexcept;

    /// <summary>
    /// Resets temporal stabilization history without releasing GPU resources.
    /// Call on source generation change, geometry change, or PTS discontinuity.
    /// </summary>
    void Reset() noexcept;

    /// <summary>
    /// Releases all owned GPU textures, buffers, and views.
    /// </summary>
    void ReleaseResources() noexcept;

    /// <summary>
    /// Prepares a small native mask into a target-resolution R32_FLOAT GPU texture.
    /// </summary>
    GpuMaskOutcome PrepareMask(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const MaskBuffer& srcMask,
        int targetWidth,
        int targetHeight,
        const MaskPreparationConfig& config,
        GpuMaskFaultSeam faultSeam = GpuMaskFaultSeam::None,
        HRESULT customFaultHr = E_FAIL,
        std::string* outErrorMessage = nullptr
    );

    /// <summary>
    /// Query output shader resource view of the prepared mask texture.
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
