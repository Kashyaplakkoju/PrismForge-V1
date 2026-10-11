#pragma once
#include "desktopcolorpipeline.h"
#include "studiostack/studio_stack.h"
#include "providers/segmentation_service.h"
#include "providers/face_analysis_service.h"

namespace PrismForge::Effects {
enum class DesktopLightMode : int { Off=0, Warm=1, Cool=2, Balanced=3, Custom=4 };
enum class DesktopLightDirection : int { Left=0, Front=1, Right=2 };
enum class DesktopBackgroundMode : int { Off=0, Blur=1, Replace=2 };
struct DesktopStudioSettings {
    DesktopLightMode light=DesktopLightMode::Off;
    DesktopLightDirection direction=DesktopLightDirection::Left;
    float strength=.5f;
    bool studioFrame=false;
    // Local spatial light placement, without inferred face geometry or depth.
    Lighting::StudioLight keyLight=[] {auto light=Lighting::LightingConfig::DefaultTwoLight().light1;light.colorTemperature=3200.f;return light;}();
    Lighting::StudioLight fillLight=[] {auto light=Lighting::LightingConfig::DefaultTwoLight().light2;light.colorTemperature=6500.f;light.intensity=.5f;return light;}();
    bool lightingSubjectOnly=false;
    // Opt-in exact-frame face mesh lighting. Background segmentation stays separate.
    bool faceLighting=false;
    // Opt-in restrained facial smoothing, sharing the exact-frame native face service.
    float touchupStrength=0.f;
    float touchupDetail=.75f;
    // Original procedural accessories, sharing the same exact-frame face analysis.
    bool glasses=false;
    bool cap=false;
    int glassesStyle=0; // Exact reference frames: Wayfarer / Aviator / Tortoise / Cat-eye.
    float capScale=1.f; // .75..1.5, independent of glasses size.
    float capVerticalOffset=0.f; // -2..3cm along head-local +Y.
    int capStyle=2; // The public cap choice is Pokemon; the unreliable visor is hidden.
    float accessoryScale=1.f; // .5..1.5 relative to the detected eye baseline.
    int faceFilterStyle=0; // 1 fox, 2 owl, 3 explorer, 4 local, 5 optional King's crown; default Off.
    uint32_t faceFilterRenderEdge=640; // Bounded raster quality; geometry keeps its authored proportions.
    float faceFilterScale=1.f;
    float faceFilterVerticalOffset=0.f;
    std::shared_ptr<const Overlays::Realistic::NativeFaceFilter> faceFilterCustomModel; // Immutable, transient snapshot.
    std::string faceFilterCustomPath,faceFilterCustomName; // Owned library PFM, UTF-8; never the source GLB.
    Overlays::Realistic::CustomFaceFilterFit faceFilterCustomFit;
    DesktopBackgroundMode background=DesktopBackgroundMode::Off;
    int blurStrength=35; // 0..100, mapped to a bounded pixel radius.
    uint8_t backgroundB=75,backgroundG=48,backgroundR=36;
    std::shared_ptr<const AVFrame> backgroundImage;
    bool highQualityMask=true;
};
// Both camera types use this coordinator before preview/virtual-camera/snapshot fan-out.
// Configure/Get methods are GUI-thread safe. Process has one serialized caller, the
// application's SessionGate. Returned frames are borrowed; av_frame_clone retains pixels.
// Camera PTS is converted to microseconds for the asynchronous analysis bridge.
// Real model analysis is requested for backgrounds or subject-only lighting; masks are bounded,
// immutable, source-tagged and shared by all processed output consumers.
class DesktopStudioPipeline {
public:
    explicit DesktopStudioPipeline(bool allowGpu=true);
    ~DesktopStudioPipeline();
    DesktopStudioPipeline(const DesktopStudioPipeline&)=delete;
    DesktopStudioPipeline& operator=(const DesktopStudioPipeline&)=delete;
    void Configure(DesktopColorSettings settings);
    DesktopColorSettings GetSettings() const;
    Look GetLook() const;
    void ConfigureStudio(DesktopStudioSettings settings);
    DesktopStudioSettings GetStudioSettings() const;
    StudioStack::StackExecutionReport GetLastReport() const;
    Providers::SegmentationState GetSegmentationState() const;
    Providers::FaceAnalysisState GetFaceAnalysisState() const;
    // Synthetic oracle seam only; serialized test caller, never the production UI.
    void AttachAnalysisMailboxForTesting(std::shared_ptr<AnalysisBridge::AnalysisMailbox> mailbox);
    AVFrame* Process(AVFrame* input,uint64_t sourceGeneration=1);
    // Test-only configuration; triggers the engine's real readback failure boundary.
    void InjectGpuReadbackFailureForTesting(bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    DesktopColorSettings m_color;
    DesktopStudioSettings m_studio;
    StudioStack::StackExecutionReport m_lastReport;
    bool m_injectReadbackFailure=false;
};
}
