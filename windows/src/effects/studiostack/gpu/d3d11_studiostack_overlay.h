#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "windows/src/effects/overlays/overlay_validation.h"
#include <string>

#include "windows/src/effects/presets/studio_preset.h"
#include "windows/src/effects/overlays/overlay_types.h"
#include "windows/src/effects/overlays/procedural_assets.h"

namespace PrismForge::Effects::StudioStack {
    enum class GpuFaultSeam : uint32_t;
}

namespace PrismForge::Effects::StudioStack::Gpu {

/// <summary>
/// Status outcome for GPU overlay composition operations.
/// </summary>
enum class GpuOverlayOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    DeviceLost = 5,
    ShaderError = 6,
    AllocationFailed = 7
};

/// <summary>
/// Texture-Native D3D11 Overlay and Tracked Accessories Engine for StudioStack.
/// Executes Compute Shaders on caller-provided ID3D11Device and context.
/// Implements affine transform mapping, bilinear sampling, straight/premultiplied alpha blending,
/// behind-subject person mask modulation, and screen-space frame borders.
/// </summary>
struct GpuOverlayTelemetry {
    size_t dispatches=0, assetUploads=0, resourceCreations=0, cachedAssets=0;
};

class D3D11StudioStackOverlay {
public:
    D3D11StudioStackOverlay(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11StudioStackOverlay();

    D3D11StudioStackOverlay(const D3D11StudioStackOverlay&) = delete;
    D3D11StudioStackOverlay& operator=(const D3D11StudioStackOverlay&) = delete;
    D3D11StudioStackOverlay(D3D11StudioStackOverlay&&) noexcept;
    D3D11StudioStackOverlay& operator=(D3D11StudioStackOverlay&&) noexcept;

    /// <summary>
    /// Processes active overlays and procedural accessories in a single GPU dispatch pass.
    /// Order: Hat -> Glasses -> Screen Frame.
    /// </summary>
    GpuOverlayOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* personMaskSRV,
        int width, int height,
        const Presets::StudioPreset& preset,
        const Overlays::TrackedTransform& glassesTransform,
        const Overlays::TrackedTransform& hatTransform,
        const Overlays::AssetAnchorMetrics& glassesMetrics,
        const Overlays::AssetAnchorMetrics& hatMetrics,
        const Overlays::AssetAnchorMetrics& frameMetrics,
        std::string* outError = nullptr
    );

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

    /// <summary>
    /// Telemetry & cache observability.
    /// </summary>
    size_t GetCachedAssetCount() const;
    size_t GetTotalDispatches() const;
    void InvalidateAssetCache();
    bool SetAsset(Overlays::ProceduralAssetId id, const Overlays::RgbaImageBuffer& asset, uint64_t version);
    GpuOverlayTelemetry GetTelemetry() const;
    Overlays::LayerReport GetLayerReport() const;

    /// <summary>
    /// Injects deterministic fault seams for test coverage.
    /// </summary>
    void SetFaultSeam(GpuFaultSeam seam, HRESULT customHr = E_FAIL);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::StudioStack::Gpu
