#pragma once

#include "windows/src/effects/presets/studio_preset.h"
#include "windows/src/effects/analysisbridge/analysis_mailbox.h"
#include "windows/src/effects/analysisbridge/mask_preparer.h"
#include "windows/src/effects/analysisbridge/normal_preparer.h"
#include "windows/src/effects/lighting/studiolighting.h"
#include "windows/src/effects/appearance/appearanceprocessor.h"
#include "windows/src/effects/overlays/overlay_renderer.h"
#include "windows/src/effects/overlays/face_accessory_3d.h"
#include "windows/src/effects/overlays/tracking_evaluator.h"
#include "windows/src/effects/backgroundcompositor.h"
#include "windows/src/effects/cubelut.h"
#include "windows/src/effects/packs/pack_manager.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>

struct AVFrame;
struct SwsContext;

namespace PrismForge::Effects::StudioStack {
namespace Gpu { struct GpuOverlayTelemetry; }

/// <summary>
/// Status outcome of an individual effects stack stage.
/// </summary>
enum class StageStatus : int {
    Applied = 0,
    Bypassed = 1,
    NotReady = 2,
    Unsupported = 3,
    Failed = 4
};

/// <summary>
/// Policy governing behavior when an intermediate effect stage fails.
/// </summary>
enum class FallbackPolicy : int {
    ContinueWithPreviousStage = 0, // Drop failing stage and proceed with remaining
    FallbackToOriginalFrame = 1,   // Revert immediately to untouched camera frame
    AbortOnError = 2               // Return nullptr on error
};

/// <summary>
/// Execution backend selection for studio stack processing.
/// </summary>
enum class ExecutionBackend : int {
    CpuOnly = 0,
    GpuWithCpuFallback = 1
};

/// <summary>
/// Comprehensive per-frame execution report documenting stage outcomes, counts, and latency.
/// </summary>
struct StackExecutionReport {
    uint64_t frameSequence = 0;
    uint64_t sourceGeneration = 0;
    int64_t timestampPts = 0;

    StageStatus analysisStatus     = StageStatus::Bypassed;
    StageStatus inputLutStatus     = StageStatus::Bypassed; // independent pre-lighting LUT
    StageStatus inputLookStatus    = StageStatus::Bypassed; // Input colour look (before appearance)
    StageStatus appearanceStatus   = StageStatus::Bypassed;
    StageStatus lightingStatus     = StageStatus::Bypassed;
    StageStatus backgroundStatus   = StageStatus::Bypassed;
    StageStatus lutStatus          = StageStatus::Bypassed;
    StageStatus overlayStatus      = StageStatus::Bypassed;
    Overlays::LayerReport overlayLayers;
    Lighting::LightingMode effectiveLightingMode = Lighting::LightingMode::SpatialApproximation;

    bool fallbackToOriginal = false;
    bool gpuFallbackUsed = false;
    int imageUploadCount = 0;
    int imageReadbackCount = 0;
    int maskUploadCount = 0;
    int maskPreparationCount = 0;
    int maskReuseCount = 0;
    int normalUploadCount = 0;
    int normalPreparationCount = 0;
    double maskPrepMs = 0.0;
    double uploadMs = 0.0;
    double effectDispatchMs = 0.0;
    double completionMs = 0.0;
    double gpuDispatchMs = 0.0;
    double totalLatencyMs = 0.0;
    std::string errorDescription;
};

/// <summary>
/// Synchronous fan-out consumer callback.
/// </summary>
using FrameConsumerCallback = std::function<void(const AVFrame* finalFrame, const StackExecutionReport& report)>;

/// <summary>
/// Cached reusable frame scaler to eliminate per-frame SwsContext churn.
/// </summary>
class CachedFrameScaler {
public:
    CachedFrameScaler();
    ~CachedFrameScaler();
    CachedFrameScaler(const CachedFrameScaler&) = delete;
    CachedFrameScaler& operator=(const CachedFrameScaler&) = delete;

    AVFrame* Scale(const AVFrame* src, int targetW, int targetH);
    void Reset();
    size_t GetCacheCount() const { return m_cache.size(); }

private:
    struct Entry {
        int srcW = 0, srcH = 0, srcFmt = 0;
        int dstW = 0, dstH = 0, dstFmt = 0;
        SwsContext* sws = nullptr;
    };
    std::vector<Entry> m_cache;
    AVFrame* m_reusableDst = nullptr;
};

class OutputPoolCoordinator;

/// <summary>
/// Internal atomic core for shared RAII output surface leases.
/// </summary>
struct OutputLeaseCore {
    std::atomic<bool> isReleased{ false };
    std::shared_ptr<OutputPoolCoordinator> coordinator;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint64_t leaseId = 0;
    uint64_t epochId = 0;
    size_t slotIndex = 0;

    OutputLeaseCore() noexcept = default;
    ~OutputLeaseCore() noexcept;
    void Release() noexcept;
};

/// <summary>
/// Targeted fault injection seams at actual allocation, view, map, and device boundaries.
/// </summary>
enum class GpuFaultSeam : uint32_t {
    None = 0,
    TexA_Creation = 1,
    SrvA_Creation = 2,
    UavA_Creation = 3,
    TexB_Creation = 4,
    SrvB_Creation = 5,
    UavB_Creation = 6,
    UploadStaging_Creation = 7,
    ReadbackStaging_Creation = 8,
    SkinMaskTex_Creation = 9,
    SkinMaskSrv_Creation = 10,
    SkinMask_Map = 11,
    ProtMaskTex_Creation = 12,
    ProtMaskSrv_Creation = 13,
    ProtMask_Map = 14,
    PersonMaskTex_Creation = 15,
    PersonMaskSrv_Creation = 16,
    PersonMask_Map = 17,
    OutputTex_Creation = 18,
    OutputSrv_Creation = 19,
    ImageUpload_Creation = 20,
    ImageUpload_View = 21,
    Readback_Map = 22,
    DeviceRemoved_Seam = 23,
    OverlayCB_Creation = 24,
    OverlayAsset_Upload = 25,
    OverlayAsset_View = 26,
    OverlayShader_Compile = 27,
    Overlay_Map = 28,
    Overlay_Dispatch = 29,
    NormalTex_Creation = 30,
    NormalSrv_Creation = 31,
    Normal_Map = 32,
    Normal_Resample = 33,
    DepthTex_Creation = 34,
    Depth_Map = 35,
    InputLook_ShaderCompile=36, InputLook_ShaderCreation=37, InputLook_BufferCreation=38,
    InputLook_Map=39, InputLook_Dispatch=40,
    InputLut_ShaderCompile=41, InputLut_ShaderCreation=42, InputLut_BufferCreation=43,
    InputLut_Texture=44, InputLut_View=45, InputLut_Map=46, InputLut_Dispatch=47
};

/// <summary>
/// Texture surface descriptor for direct GPU-native surface consumers.
/// Holds strong COM references (textureRef, srvRef) and a shared lease handle.
/// Guarantees safe retention across successive frame processing, dynamic resolution changes,
/// and stack GPU release/destruction.
/// Managed through an explicit, bounded lease contract with capacity 4.
/// Move/copy/assignment behaves with shared lease semantics; releasing or destroying an alias
/// does not free the slot while another alias exists. When the last alias drops, the slot
/// releases automatically.
/// </summary>
struct GpuTextureSurface {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textureRef;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvRef;
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool isValid = false;
    uint64_t leaseId = 0;
    std::shared_ptr<OutputLeaseCore> leaseCore;

    GpuTextureSurface() noexcept;
    ~GpuTextureSurface() noexcept;
    GpuTextureSurface(const GpuTextureSurface& other) noexcept;
    GpuTextureSurface& operator=(const GpuTextureSurface& other) noexcept;
    GpuTextureSurface(GpuTextureSurface&& other) noexcept;
    GpuTextureSurface& operator=(GpuTextureSurface&& other) noexcept;

    void Release() noexcept;
};

/// <summary>
/// Targeted failure injection points for GPU pipeline fault testing.
/// </summary>
enum class GpuFailureTarget : uint32_t {
    None = 0,
    ResourceCreation = 1,
    AppearanceStage = 2,
    LightingStage = 3,
    BackgroundStage = 4,
    LutStage = 5,
    Readback = 6,
    AnyGpu = 7,
    OverlayStage = 8,
    InputLookStage = 9,
    InputLutStage = 10
};

/// <summary>
/// Standalone Studio Effects Stack Coordinator.
/// Synchronously processes captured frames through analysis alignment, appearance,
/// two-light studio engine, background compositing, 3D LUT, and tracked overlays,
/// followed by synchronous fan-out to registered consumers outside configuration locks.
/// </summary>
class StudioStack {
public:
    explicit StudioStack(FallbackPolicy policy = FallbackPolicy::FallbackToOriginalFrame);
    ~StudioStack();

    StudioStack(const StudioStack&) = delete;
    StudioStack& operator=(const StudioStack&) = delete;

    // Session management
    void SetSourceGeneration(uint64_t generation);
    uint64_t GetSourceGeneration() const;
    void ResetSource();

    // Fallback policy
    void SetFallbackPolicy(FallbackPolicy policy);
    FallbackPolicy GetFallbackPolicy() const;

    // Execution backend selection & GPU setup
    void SetExecutionBackend(ExecutionBackend backend);
    ExecutionBackend GetExecutionBackend() const;
    bool InitializeGpu(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr);
    void ReleaseGpu();

    // Preset & LUT configuration
    void SetPreset(const Presets::StudioPreset& preset);
    Presets::StudioPreset GetPreset() const;
    bool ApplyPreset(
        const Presets::StudioPreset& preset,
        const std::string& packLibraryRoot,
        std::string& outError);
    bool LoadLutFromBuffer(const std::string& cubeLutText, std::string& outError);
    bool LoadLutFromFile(const std::string& cubeLutFilePath, std::string& outError);

    bool LoadInputLutFromBuffer(const std::string& text, std::string& outError);
    bool LoadInputLutFromFile(const std::string& path, std::string& outError);
    // Installs an immutable owned LUT snapshot; each frame snapshots it under the config lock.
    // GPU content-aware cache refreshes on the next execution; nullptr clears input grading.
    void SetInputLut(std::shared_ptr<const CubeLut> lut);
    void ClearInputLut();

    // Analysis bridge
    void AttachAnalysisMailbox(std::shared_ptr<AnalysisBridge::AnalysisMailbox> mailbox);

    // Consumer registration (thread-safe, deadlock-free callbacks outside lock)
    void RegisterConsumer(const std::string& name, FrameConsumerCallback callback);
    void UnregisterConsumer(const std::string& name);

    // Main frame processing entry point
    AVFrame* ProcessFrame(
        AVFrame* inputFrame,
        uint64_t sequenceId,
        int64_t timestampPts = 0,
        StackExecutionReport* outReport = nullptr,
        uint64_t frameSourceGeneration = 0,
        std::shared_ptr<const AnalysisBridge::AnalysisBundle> lightingGeometry = nullptr,
        std::shared_ptr<const AnalysisBridge::AnalysisBundle> appearanceGeometry = nullptr,
        std::shared_ptr<const AnalysisBridge::AnalysisBundle> accessoryGeometry = nullptr);

    // lightingGeometry is an exact-frame face-only override for normal-guided lighting.
    // Its personMask is face coverage in this argument only. Mailbox person, skin, and
    // background analysis remain separate. Invalid or stale geometry is NotReady.
    // appearanceGeometry is a separate exact-frame facial surface/protection override;
    // non-null empty or stale bundles skip smoothing rather than borrow generic skin.
    // accessoryGeometry is a separate exact-frame foreground-anchor override.
    // Non-null empty/stale geometry never borrows mailbox face/body analysis.
    // The same immutable bundle can drive all three stages with one inference.
    void ResetTrackedAccessories();

    // Texture-native GPU entry point for shared-surface pipelines
    GpuTextureSurface ProcessTextureNative(
        ID3D11ShaderResourceView* inputSRV,
        int width, int height,
        uint64_t sequenceId,
        int64_t timestampPts = 0,
        StackExecutionReport* outReport = nullptr,
        uint64_t frameSourceGeneration = 0);

    // Bounded surface lease management
    void ReleaseOutputSurface(GpuTextureSurface& surface);
    size_t GetTotalTextureAllocations() const;
    size_t GetActiveLeaseCount() const;
    size_t GetLiveOutputAllocations() const;
    size_t GetRetiredEpochCount() const;
    size_t GetMaxLeaseCapacity() const; // Hard limit: 4

    // Background image replacement configuration
    bool SetBackgroundImage(
        ID3D11ShaderResourceView* gpuSRV,
        const AVFrame* cpuFrame = nullptr,
        Presets::BackgroundFitMode fitMode = Presets::BackgroundFitMode::CropFill,
        std::string* outError = nullptr);
    void ClearBackgroundImage();
    bool HasBackgroundImage() const;
    void GetBackgroundImageDimensions(int& outW, int& outH) const;

    // Testing and fault injection API
    void SetGpuFailureInjection(bool inject);
    bool IsGpuFailureInjected() const;
    void SetGpuFailureTarget(GpuFailureTarget target);
    GpuFailureTarget GetGpuFailureTarget() const;
    void SetGpuFaultSeam(GpuFaultSeam seam, HRESULT customHr = E_FAIL);
    GpuFaultSeam GetGpuFaultSeam() const;
    HRESULT GetGpuFaultHResult() const;
    void SetMaskPreparationFault(bool fail);
    bool IsMaskPreparationFault() const;

    // Tracking & overlay telemetry
    Overlays::TrackedTransform GetTrackedTransform(Overlays::AnchorMode mode) const;
    size_t GetOverlayDispatchCount() const;
    size_t GetOverlayCachedAssetCount() const;
    Gpu::GpuOverlayTelemetry GetOverlayTelemetry() const;
    void SetStageProfilingEnabled(bool enabled);
    bool IsStageProfilingEnabled() const;

    // Fan-out utility: creates a scaled BGR24 frame (for preview / snapshot)
    static AVFrame* CreateScaledFrame(const AVFrame* sourceFrame, int targetWidth, int targetHeight);

private:
    struct GpuResources;

    // CPU fallback for image background composition
    AVFrame* ComposeImageBackgroundCpu(
        AVFrame* currentFrame,
        const AlphaMask* mask,
        Presets::BackgroundFitMode fitMode);

    struct GpuPipelineResult {
        bool success = false;
        ID3D11Texture2D* outputTexture = nullptr;
        ID3D11ShaderResourceView* outputSRV = nullptr;
        std::string error;
        bool isDeviceLost = false;
    };

    GpuPipelineResult ExecuteGpuStages(
        ID3D11ShaderResourceView* initialInputSRV,
        ID3D11Texture2D* initialInputTex,
        int width, int height,
        const Presets::StudioPreset& preset,
        std::shared_ptr<const CubeLut> activeLut,
        std::shared_ptr<const CubeLut> inputLut,
        const AnalysisBridge::AnalysisBundle* analysisBundle,
        StackExecutionReport& report,
        const AnalysisBridge::AnalysisBundle* lightingGeometry = nullptr,
        const AnalysisBridge::AnalysisBundle* appearanceGeometry = nullptr,
        const AnalysisBridge::AnalysisBundle* accessoryGeometry = nullptr);

    FallbackPolicy m_fallbackPolicy;
    ExecutionBackend m_backend = ExecutionBackend::CpuOnly;
    uint64_t m_sourceGeneration = 1;
    mutable std::mutex m_configMutex;
    mutable std::mutex m_executionMutex;
    std::atomic<bool> m_isProcessing{ false };
    std::atomic<bool> m_gpuFailureInjected{ false };
    std::atomic<GpuFailureTarget> m_gpuFailureTarget{ GpuFailureTarget::None };
    std::atomic<GpuFaultSeam> m_gpuFaultSeam{ GpuFaultSeam::None };
    std::atomic<HRESULT> m_gpuFaultHResult{ E_FAIL };
    std::atomic<bool> m_maskPrepFaultInjected{ false };
    std::atomic<bool> m_stageProfilingEnabled{ false };
    std::shared_ptr<OutputPoolCoordinator> m_poolCoordinator;

    Presets::StudioPreset m_currentPreset;
    std::shared_ptr<const CubeLut> m_activeLut;
    std::shared_ptr<const CubeLut> m_inputLut;

    // Stage processors (CPU)
    std::shared_ptr<AnalysisBridge::AnalysisMailbox> m_analysisMailbox;
    AnalysisBridge::MaskPreparer m_maskPreparerPerson;
    AnalysisBridge::MaskPreparer m_maskPreparerFace; // No person-mask history.
    AnalysisBridge::MaskPreparer m_maskPreparerSkin;
    AnalysisBridge::MaskPreparer m_maskPreparerProt;
    int m_lastFrameW = 0;
    int m_lastFrameH = 0;
    int64_t m_lastTimestampPts = -1;
    Lighting::StudioLighting m_lightingEngine;
    Appearance::AppearanceProcessor m_appearanceProcessor;
    Overlays::OverlayRenderer m_overlayRenderer;
    Overlays::Realistic::FaceAccessory3DRenderer m_faceAccessoryRenderer;
    Overlays::TrackingEvaluator m_trackingEvaluator;
    BackgroundCompositor m_backgroundCompositor;
    CubeLutProcessor m_cubeLutProcessor;
    CubeLutProcessor m_inputLutProcessor;

    // GPU implementation
    std::unique_ptr<GpuResources> m_gpu;

    // Reusable scratch mask buffers for persistent geometry allocation
    AnalysisBridge::MaskBuffer m_scratchSkin;
    AnalysisBridge::MaskBuffer m_scratchProt;
    AnalysisBridge::MaskBuffer m_scratchPerson;
    AnalysisBridge::MaskBuffer m_scratchFace;
    AnalysisBridge::NormalPreparer m_normalPreparer;
    AnalysisBridge::NormalBuffer m_scratchNormal;
    AnalysisBridge::MaskBuffer m_scratchShadowDepth; // Fully initialized current relative depth; no temporal state.

    // Frame scaler
    CachedFrameScaler m_scaler;

    // Consumers
    std::unordered_map<std::string, FrameConsumerCallback> m_consumers;
    AVFrame* m_readbackFrame = nullptr;

    // Background image storage
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bgTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_bgSRV;
    int m_bgWidth = 0;
    int m_bgHeight = 0;
    Presets::BackgroundFitMode m_bgFitMode = Presets::BackgroundFitMode::CropFill;
    AVFrame* m_cpuBgFrame = nullptr;
    AVFrame* m_cpuBgOutputFrame = nullptr;
    AVFrame* m_cpuOutputFrame = nullptr; // Final owned CPU output; clones survive subsequent frames.
    AVFrame* m_cpuLookScratchFrame = nullptr; // Owned scratch for CPU input colour look stage
};

} // namespace PrismForge::Effects::StudioStack
