#include "desktopstudiopipeline.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <filesystem>
#include <windows.h>
extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/mathematics.h>
}

namespace PrismForge::Effects {
namespace {
Providers::FaceAnalysisOptions FaceOptions() {
    std::wstring path(32768,L'\0');
    const DWORD size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    Providers::FaceAnalysisOptions options;
    if(!size||size>=path.size())return options;
    path.resize(size);const auto directory=std::filesystem::path(path).parent_path();
    options.runtimeDllPath=(directory/L"libmediapipe.dll").wstring();
    options.modelPath=(directory/L"models"/L"face-landmarker"/L"face_landmarker.task").wstring();
    options.topologyPath=(directory/L"models"/L"face-landmarker"/L"canonical_face_model.obj").wstring();
    return options;
}
float FiniteBound(float value,float minimum,float maximum,float fallback) {
    return std::isfinite(value)?std::clamp(value,minimum,maximum):fallback;
}
Lighting::StudioLight SanitizedLight(Lighting::StudioLight light,const Lighting::StudioLight& fallback) {
    light.posX=FiniteBound(light.posX,-1.f,1.f,fallback.posX);
    light.posY=FiniteBound(light.posY,-1.f,1.f,fallback.posY);
    light.posZ=FiniteBound(light.posZ,.1f,2.f,fallback.posZ);
    light.intensity=FiniteBound(light.intensity,0.f,2.f,fallback.intensity);
    light.colorR=FiniteBound(light.colorR,0.f,1.f,fallback.colorR);
    light.colorG=FiniteBound(light.colorG,0.f,1.f,fallback.colorG);
    light.colorB=FiniteBound(light.colorB,0.f,1.f,fallback.colorB);
    light.colorTemperature=FiniteBound(light.colorTemperature,1500.f,15000.f,fallback.colorTemperature);
    light.tint=FiniteBound(light.tint,-1.f,1.f,0.f);
    return light;
}
bool LightingActive(const DesktopStudioSettings& settings) {
    return settings.light!=DesktopLightMode::Off&&settings.strength>0&&
        (settings.light!=DesktopLightMode::Custom||
         (settings.keyLight.enabled&&settings.keyLight.intensity>0)||
         (settings.fillLight.enabled&&settings.fillLight.intensity>0));
}
Presets::StudioPreset Preset(const DesktopStudioSettings& settings,const DesktopColorSettings& color) {
    auto p=Presets::StudioPreset::Neutral();p.bypassAll=false;
    if(LightingActive(settings)) {
        auto& c=p.lighting;c=Lighting::LightingConfig::DefaultTwoLight();
        c.subjectOnlyMode=settings.lightingSubjectOnly;c.globalStrength=settings.strength;c.ambientIntensity=1.f-.15f*settings.strength;c.fillIntensity=.1f*settings.strength;
        if(settings.light==DesktopLightMode::Custom) {
            c.light1=settings.keyLight;c.light2=settings.fillLight;
        } else {
        float x=settings.direction==DesktopLightDirection::Left?-.55f:settings.direction==DesktopLightDirection::Right?.55f:0.f;
        c.light1.posX=x;c.light2.posX=-x;c.light1.posY=c.light2.posY=-.25f;
        c.light1.intensity=1;c.light2.intensity=.55f;
        if(settings.light!=DesktopLightMode::Balanced) {
            c.light2.enabled=false;
            if(settings.light==DesktopLightMode::Cool) {c.light1.colorR=.6f;c.light1.colorG=.8f;c.light1.colorB=1;}
        }
        }
    }
    if(LightingActive(settings)&&settings.faceLighting) {
        auto& light=p.lighting;
        light.subjectOnlyMode=true;light.lightingMode=Lighting::LightingMode::NormalGuided;
        light.ambientIntensity=1.f-.4f*settings.strength;light.fillIntensity=.05f*settings.strength;
        // A nearer key and higher fill expose the inferred facial geometry.
        // Stored UI positions/Kelvin/intensities stay the user's values.
        light.light1.posZ=.1f;light.light2.posZ=.35f;
        light.shadowConfig.enabled=true;light.shadowConfig.maxSteps=16;
        light.shadowConfig.maxDistance=.35f;light.shadowConfig.rayElevation=.01f;
        light.shadowConfig.shadowIntensity=.65f;light.shadowConfig.shadowSoftness=.2f;
    }
    if(settings.touchupStrength>0.f&&settings.touchupDetail<1.f) {
        p.appearance.bypass=false;
        p.appearance.smoothingStrength=.65f*settings.touchupStrength;
        // Preserve subtle settings; progressively broaden smoothing above 50%.
        p.appearance.touchupBoost=std::max(0.f,(settings.touchupStrength-.5f)*2.f);
        p.appearance.detailPreservation=settings.touchupDetail;
        p.appearance.sharpenStrength=0.f;
    }
    p.inputLook=static_cast<int>(color.look);p.inputLookStrength=1.f;
    p.inputLutIntensity=color.lut?color.intensity:0.f;
    if(settings.background!=DesktopBackgroundMode::Off) {
        p.backgroundMode=settings.background==DesktopBackgroundMode::Blur?Presets::StudioBackgroundMode::Blur:
            settings.backgroundImage?Presets::StudioBackgroundMode::Image:Presets::StudioBackgroundMode::Solid;
        p.background.blurRadius=1+settings.blurStrength*31/100;
        p.background.solidB=settings.backgroundB;p.background.solidG=settings.backgroundG;p.background.solidR=settings.backgroundR;
    }
    if(settings.faceFilterStyle==0&&(settings.glasses||settings.cap)) {
        p.realisticAccessories=true;p.realisticCapStyle=settings.capStyle;
        p.realisticGlassesStyle=settings.glassesStyle;p.realisticCapScale=settings.capScale;p.realisticCapVerticalOffset=settings.capVerticalOffset;
        p.overlays.bypass=false;p.overlays.glasses.enabled=settings.glasses;p.overlays.hat.enabled=settings.cap;
        p.overlays.glasses.scaleMultiplier=p.overlays.hat.scaleMultiplier=settings.accessoryScale;
        // Current pose and depth remain authoritative without a frame queue.
        for(auto* accessory:{&p.overlays.glasses,&p.overlays.hat}) {
            accessory->smoothing.positionAlpha=.65f;accessory->smoothing.scaleAlpha=.65f;
            accessory->smoothing.rotationAlpha=.6f;accessory->smoothing.fadeInSpeed=1.f;
        }
    }
    p.faceFilterStyle=settings.faceFilterStyle;p.faceFilterScale=settings.faceFilterScale;
    p.faceFilterVerticalOffset=settings.faceFilterVerticalOffset;p.faceFilterRenderEdge=settings.faceFilterRenderEdge;
    p.faceFilterCustomModel=settings.faceFilterCustomModel;p.faceFilterCustomPath=settings.faceFilterCustomPath;
    p.faceFilterCustomName=settings.faceFilterCustomName;p.faceFilterCustomFit=settings.faceFilterCustomFit;
    if(settings.faceFilterStyle>0){p.realisticAccessories=true;p.overlays.bypass=false;}
    if(settings.studioFrame) {p.overlays.bypass=false;p.overlays.studioFrame.enabled=true;p.overlays.studioFrame.opacity=1;}
    return p;
}
bool ValidPlanes(const AVFrame* frame) {
    if(!frame || frame->width<=0 || frame->height<=0 || frame->width>16384 || frame->height>16384 ||
        int64_t(frame->width)*frame->height>64LL*1024*1024) return false;
    auto format=static_cast<AVPixelFormat>(frame->format);auto* desc=av_pix_fmt_desc_get(format);
    if(!desc || (desc->flags&(AV_PIX_FMT_FLAG_HWACCEL|AV_PIX_FMT_FLAG_BITSTREAM|AV_PIX_FMT_FLAG_PAL)))return false;
    int lines[4]{};if(av_image_fill_linesizes(lines,format,frame->width)<0)return false;
    int planes=av_pix_fmt_count_planes(format);if(planes<=0||planes>4)return false;
    for(int p=0;p<planes;++p) {
        int height=frame->height;
        if(!(desc->flags&AV_PIX_FMT_FLAG_RGB) && (p==1||p==2))height=AV_CEIL_RSHIFT(height,desc->log2_chroma_h);
        if(!frame->data[p] || lines[p]<=0 || frame->linesize[p]<lines[p])return false;
        uint64_t span=uint64_t(frame->linesize[p])*(height-1)+lines[p];
        if(span>1ULL<<30)return false;
        // Raw software descriptors can omit extended_data. Inspect standard
        // owners directly, without calling helpers that dereference that array.
        // Truly borrowed planes remain valid for this synchronous coordinator;
        // an owned descriptor must have an owner spanning every accessed row.
        const uintptr_t start=reinterpret_cast<uintptr_t>(frame->data[p]);
        if(span>UINTPTR_MAX-start)return false;
        bool hasOwner=false,covered=false;
        for(const AVBufferRef* owner:frame->buf) {
            if(!owner)continue;
            hasOwner=true;
            if(!owner->data||!owner->size)continue;
            const uintptr_t base=reinterpret_cast<uintptr_t>(owner->data);
            if(owner->size>UINTPTR_MAX-base||start<base||start>base+owner->size)continue;
            if(span<=base+owner->size-start){covered=true;break;}
        }
        if(hasOwner&&!covered)return false;
    }return true;
}
int64_t FramePts(const AVFrame* frame) {
    if(frame->pts!=AV_NOPTS_VALUE && frame->time_base.num>0 && frame->time_base.den>0)
        return av_rescale_q(frame->pts,frame->time_base,AVRational{1,1000000});
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
struct DesktopStudioPipeline::Impl {
    DesktopColorPipeline color;
    StudioStack::StudioStack stack;
    AVFrame* owned=nullptr;
    std::shared_ptr<Providers::SegmentationService> segmentation;
    std::shared_ptr<Providers::FaceAnalysisService> faceAnalysis;
    std::shared_ptr<AnalysisBridge::AnalysisMailbox> testMailbox;
    std::shared_ptr<const CubeLut> loadedLut;
    std::shared_ptr<const AVFrame> loadedImage;
    bool gpuReady=false;
    unsigned consecutiveGpuFailures=0;
    std::chrono::steady_clock::time_point retryGpuAt{};
    int sourceWidth=0,sourceHeight=0;
    uint64_t generation=0,sequence=0;
    bool allowGpu=true,gpuAttempted=false;
    explicit Impl(bool gpu):allowGpu(gpu){}
    ~Impl(){av_frame_free(&owned);}
    AVFrame* Own(const AVFrame* result, const AVFrame* currentInput) {
        if(!owned||owned->width!=result->width||owned->height!=result->height||owned->format!=result->format) {
            AVFrame* next=av_frame_alloc();if(!next)return nullptr;
            next->width=result->width;next->height=result->height;next->format=result->format;
            if(av_frame_get_buffer(next,64)<0){av_frame_free(&next);return nullptr;}
            av_frame_free(&owned);owned=next;
        }
        if(av_frame_make_writable(owned)<0||av_frame_copy(owned,result)<0)return nullptr;
        // Clear properties rather than accumulating side data across reused outputs.
        while(owned->nb_side_data>0)av_frame_remove_side_data(owned,owned->side_data[0]->type);
        av_dict_free(&owned->metadata);
        if(av_frame_copy_props(owned,currentInput)<0)return nullptr;
        owned->color_range=AVCOL_RANGE_JPEG;owned->colorspace=AVCOL_SPC_RGB;
        return owned;
    }
};
DesktopStudioPipeline::DesktopStudioPipeline(bool allowGpu):m_impl(std::make_unique<Impl>(allowGpu)){}
DesktopStudioPipeline::~DesktopStudioPipeline()=default;
void DesktopStudioPipeline::Configure(DesktopColorSettings settings) {
    if(settings.look<Look::Original||settings.look>Look::Mono)settings.look=Look::Original;
    settings.intensity=std::isfinite(settings.intensity)?std::clamp(settings.intensity,0.f,1.f):0;
    std::lock_guard<std::mutex> lock(m_mutex);m_color=std::move(settings);
}
DesktopColorSettings DesktopStudioPipeline::GetSettings() const {std::lock_guard<std::mutex> lock(m_mutex);return m_color;}
Look DesktopStudioPipeline::GetLook() const {auto c=GetSettings();return c.bypassed?Look::Original:c.look;}
void DesktopStudioPipeline::ConfigureStudio(DesktopStudioSettings settings) {
    if(settings.light<DesktopLightMode::Off||settings.light>DesktopLightMode::Custom)settings.light=DesktopLightMode::Off;
    if(settings.direction<DesktopLightDirection::Left||settings.direction>DesktopLightDirection::Right)settings.direction=DesktopLightDirection::Left;
    settings.strength=std::isfinite(settings.strength)?std::clamp(settings.strength,0.f,1.f):0;
    settings.touchupStrength=FiniteBound(settings.touchupStrength,0.f,1.f,0.f);
    settings.touchupDetail=FiniteBound(settings.touchupDetail,0.f,1.f,.75f);
    settings.accessoryScale=FiniteBound(settings.accessoryScale,.5f,1.5f,1.f);
    settings.faceFilterStyle=settings.faceFilterStyle>=0&&settings.faceFilterStyle<=5?settings.faceFilterStyle:0;
    settings.faceFilterRenderEdge=std::clamp(settings.faceFilterRenderEdge,256u,Overlays::Realistic::HardModelRenderEdge);
    settings.faceFilterScale=FiniteBound(settings.faceFilterScale,.75f,1.5f,1.f);
    settings.faceFilterVerticalOffset=FiniteBound(settings.faceFilterVerticalOffset,-3.f,3.f,0.f);
    auto& fit=settings.faceFilterCustomFit;
    fit.xCm=FiniteBound(fit.xCm,-20.f,20.f,0.f);fit.yCm=FiniteBound(fit.yCm,-20.f,20.f,0.f);fit.zCm=FiniteBound(fit.zCm,-20.f,20.f,0.f);
    fit.tiltDegrees=FiniteBound(fit.tiltDegrees,-180.f,180.f,0.f);fit.turnDegrees=FiniteBound(fit.turnDegrees,-180.f,180.f,0.f);
    fit.rollDegrees=FiniteBound(fit.rollDegrees,-180.f,180.f,0.f);
    if(settings.faceFilterCustomPath.size()>4096)settings.faceFilterCustomPath.clear();
    if(settings.faceFilterCustomName.size()>160)settings.faceFilterCustomName.clear();
    settings.glassesStyle=settings.glassesStyle>=0&&settings.glassesStyle<=3?settings.glassesStyle:0;
    settings.capScale=FiniteBound(settings.capScale,.75f,1.5f,1.f);
    settings.capVerticalOffset=FiniteBound(settings.capVerticalOffset,-2.f,3.f,0.f);
    if(settings.capStyle!=2){settings.cap=false;settings.capStyle=2;}
    const DesktopStudioSettings defaults;
    settings.keyLight=SanitizedLight(settings.keyLight,defaults.keyLight);
    settings.fillLight=SanitizedLight(settings.fillLight,defaults.fillLight);
    if(settings.background<DesktopBackgroundMode::Off||settings.background>DesktopBackgroundMode::Replace)settings.background=DesktopBackgroundMode::Off;
    settings.blurStrength=std::clamp(settings.blurStrength,0,100);
    if(settings.backgroundImage && (!ValidPlanes(settings.backgroundImage.get()) || settings.backgroundImage->format!=AV_PIX_FMT_BGR24))settings.backgroundImage.reset();
    std::lock_guard<std::mutex> lock(m_mutex);m_studio=std::move(settings);
}
DesktopStudioSettings DesktopStudioPipeline::GetStudioSettings() const {std::lock_guard<std::mutex> lock(m_mutex);return m_studio;}
StudioStack::StackExecutionReport DesktopStudioPipeline::GetLastReport() const {std::lock_guard<std::mutex> lock(m_mutex);return m_lastReport;}
void DesktopStudioPipeline::InjectGpuReadbackFailureForTesting(bool enabled) {std::lock_guard<std::mutex> lock(m_mutex);m_injectReadbackFailure=enabled;}
Providers::SegmentationState DesktopStudioPipeline::GetSegmentationState() const {
    std::shared_ptr<Providers::SegmentationService> service;
    {std::lock_guard<std::mutex> lock(m_mutex);service=m_impl->segmentation;}
    return service?service->GetState():Providers::SegmentationState{};
}
Providers::FaceAnalysisState DesktopStudioPipeline::GetFaceAnalysisState() const {
    std::shared_ptr<Providers::FaceAnalysisService> service;
    {std::lock_guard<std::mutex> lock(m_mutex);service=m_impl->faceAnalysis;}
    return service?service->GetState():Providers::FaceAnalysisState{};
}
void DesktopStudioPipeline::AttachAnalysisMailboxForTesting(std::shared_ptr<AnalysisBridge::AnalysisMailbox> mailbox) {
    m_impl->testMailbox=std::move(mailbox);m_impl->stack.AttachAnalysisMailbox(m_impl->testMailbox);
}
AVFrame* DesktopStudioPipeline::Process(AVFrame* input,uint64_t sourceGeneration) {
    DesktopColorSettings c;DesktopStudioSettings s;bool inject;
    {std::lock_guard<std::mutex> lock(m_mutex);c=m_color;s=m_studio;inject=m_injectReadbackFailure;}
    auto& impl=*m_impl;StudioStack::StackExecutionReport r;r.sourceGeneration=sourceGeneration?sourceGeneration:1;
    const bool sourceChanged=impl.generation!=r.sourceGeneration;
    const bool sizeChanged=input&&(impl.sourceWidth!=input->width||impl.sourceHeight!=input->height);
    if(sourceChanged||sizeChanged){impl.generation=r.sourceGeneration;if(sourceChanged)impl.sequence=0;
        impl.stack.SetSourceGeneration(impl.generation);if(impl.segmentation)impl.segmentation->ResetSource(impl.generation);
        if(impl.faceAnalysis)impl.faceAnalysis->ResetSource(impl.generation);
        impl.sourceWidth=input?input->width:0;impl.sourceHeight=input?input->height:0;}
    r.frameSequence=input?++impl.sequence:impl.sequence;r.timestampPts=input?FramePts(input):0;
    AVFrame* result=input;
    try {
        impl.color.Configure({}); // Conversion only; all active looks/LUTs run inside the shared stack.
        if(input && !ValidPlanes(input)) {r.errorDescription="Invalid software frame planes";r.lightingStatus=StudioStack::StageStatus::Failed;}
        else {
            const bool backgroundActive=!c.bypassed&&s.background!=DesktopBackgroundMode::Off;
            const bool lightingActive=!c.bypassed&&LightingActive(s);
            const bool faceLightingActive=lightingActive&&s.faceLighting;
            const bool touchupActive=!c.bypassed&&s.touchupStrength>0.f&&s.touchupDetail<1.f;
            const bool faceFilterActive=!c.bypassed&&s.faceFilterStyle>0;
            const bool accessoriesActive=!c.bypassed&&(faceFilterActive||s.glasses||s.cap);
            const bool facialAnalysisActive=faceLightingActive||touchupActive||accessoriesActive;
            const bool subjectLightingActive=lightingActive&&s.lightingSubjectOnly&&!s.faceLighting;
            const bool analysisActive=backgroundActive||subjectLightingActive;
            if(analysisActive&&!impl.testMailbox&&!impl.segmentation) {
                Providers::SegmentationOptions options;
                if(!impl.allowGpu)options.requestedProvider=Providers::ExecutionProviderType::CPU;
                auto service=std::make_shared<Providers::SegmentationService>(options);
                service->ResetSource(impl.generation);impl.stack.AttachAnalysisMailbox(service->GetMailbox());
                {std::lock_guard<std::mutex> lock(m_mutex);impl.segmentation=std::move(service);}
            }
            if(impl.segmentation)impl.segmentation->Configure(analysisActive,s.highQualityMask);
            if(facialAnalysisActive&&!impl.faceAnalysis) {
                auto service=std::make_shared<Providers::FaceAnalysisService>(FaceOptions());
                service->ResetSource(impl.generation);
                {std::lock_guard<std::mutex> lock(m_mutex);impl.faceAnalysis=std::move(service);}
            }
            if(impl.faceAnalysis)impl.faceAnalysis->Configure(facialAnalysisActive);
            if(!accessoriesActive)impl.stack.ResetTrackedAccessories();
            const bool active=!c.bypassed&&(s.studioFrame||backgroundActive||lightingActive||touchupActive||accessoriesActive||c.look!=Look::Original||(c.lut&&c.intensity>0));
            if(input&&active) {
                const auto* descriptor=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
                const bool limitedRgb=input->color_range==AVCOL_RANGE_MPEG&&descriptor&&(descriptor->flags&AV_PIX_FMT_FLAG_RGB);
                auto* bgr=limitedRgb?nullptr:impl.color.ToBgr(input);
                if(!bgr){r.errorDescription=limitedRgb?"Limited-range RGB studio input is unsupported":"Studio effects require a supported SDR frame";r.lightingStatus=StudioStack::StageStatus::Unsupported;r.overlayStatus=StudioStack::StageStatus::Unsupported;r.backgroundStatus=StudioStack::StageStatus::Unsupported;}
                else {
                    if(analysisActive&&impl.segmentation)impl.segmentation->Submit(bgr,impl.generation,r.frameSequence,r.timestampPts);
                    const bool onlyMaskedEffects=analysisActive&&!facialAnalysisActive&&!s.studioFrame&&(!lightingActive||subjectLightingActive)&&c.look==Look::Original&&(!c.lut||c.intensity<=0);
                    if(onlyMaskedEffects) {
                        auto mailbox=impl.testMailbox?impl.testMailbox:impl.segmentation->GetMailbox();
                        AnalysisBridge::AnalysisAlignmentStatus alignment;
                        const auto analysis=mailbox->GetAlignedAnalysis(impl.generation,r.frameSequence,r.timestampPts,alignment);
                        if(!analysis||!analysis->HasPersonMask()) {
                            r.backgroundStatus=backgroundActive?StudioStack::StageStatus::NotReady:StudioStack::StageStatus::Bypassed;
                            r.lightingStatus=subjectLightingActive?StudioStack::StageStatus::NotReady:StudioStack::StageStatus::Bypassed;
                            r.analysisStatus=StudioStack::StageStatus::NotReady;
                            std::lock_guard<std::mutex> lock(m_mutex);m_lastReport=std::move(r);return input;
                        }
                    }
                    std::shared_ptr<const AnalysisBridge::AnalysisBundle> faceGeometry;
                    if(facialAnalysisActive&&impl.faceAnalysis)
                        faceGeometry=impl.faceAnalysis->Analyse(bgr,impl.generation,r.frameSequence,r.timestampPts,std::chrono::milliseconds(16));
                    const bool haveFaceGeometry=static_cast<bool>(faceGeometry);
                    const bool haveAccessoryGeometry=faceGeometry&&faceGeometry->faceLandmarks.NativeGeometryMatches(
                        bgr->width,bgr->height,impl.generation,r.frameSequence,r.timestampPts)&&
                        faceGeometry->faceLandmarks.accessoryMesh&&faceGeometry->faceLandmarks.accessoryMesh->Matches(
                            bgr->width,bgr->height,impl.generation,r.frameSequence,r.timestampPts);
                    const bool haveNeededFaceGeometry=(faceLightingActive||touchupActive)?haveFaceGeometry:haveAccessoryGeometry;
                    const bool onlyFaceEffects=facialAnalysisActive&&!backgroundActive&&!s.studioFrame&&
                        (!lightingActive||faceLightingActive)&&c.look==Look::Original&&(!c.lut||c.intensity<=0);
                    if(onlyFaceEffects&&!haveNeededFaceGeometry) {
                        // No exact face means no lighting work or GPU transfers.
                        // Keep the original source object and timestamp unchanged.
                        r.lightingStatus=faceLightingActive?StudioStack::StageStatus::NotReady:StudioStack::StageStatus::Bypassed;
                        r.appearanceStatus=touchupActive?StudioStack::StageStatus::NotReady:StudioStack::StageStatus::Bypassed;
                        r.analysisStatus=StudioStack::StageStatus::NotReady;
                        if(accessoriesActive) {impl.stack.ResetTrackedAccessories();r.overlayStatus=StudioStack::StageStatus::NotReady;
                            r.overlayLayers.glasses=!faceFilterActive&&s.glasses?2:1;
                            r.overlayLayers.hat=!faceFilterActive&&s.cap?2:1;r.overlayLayers.faceFilter=faceFilterActive?2:1;}
                        const auto state=impl.faceAnalysis->GetState();
                        r.errorDescription=state.lastError;
                        std::lock_guard<std::mutex> lock(m_mutex);m_lastReport=std::move(r);return input;
                    }
                    const auto now=std::chrono::steady_clock::now();
                    if(impl.allowGpu&&(!impl.gpuAttempted||(!impl.gpuReady&&now>=impl.retryGpuAt))) {
                        impl.gpuAttempted=true;impl.stack.ReleaseGpu();impl.loadedImage.reset();impl.gpuReady=impl.stack.InitializeGpu();
                        impl.consecutiveGpuFailures=0;impl.retryGpuAt=now+std::chrono::seconds(5);
                    }
                    if(impl.loadedLut!=c.lut){impl.stack.SetInputLut(c.lut);impl.loadedLut=c.lut;}
                    if(impl.loadedImage!=s.backgroundImage) {
                        std::string error;
                        if(!s.backgroundImage){impl.stack.ClearBackgroundImage();impl.loadedImage.reset();}
                        else if(impl.stack.SetBackgroundImage(nullptr,s.backgroundImage.get(),Presets::BackgroundFitMode::CropFill,&error))impl.loadedImage=s.backgroundImage;
                        else {impl.stack.ClearBackgroundImage();impl.loadedImage.reset();r.errorDescription=error;r.backgroundStatus=StudioStack::StageStatus::Failed;r.fallbackToOriginal=true;std::lock_guard<std::mutex> lock(m_mutex);m_lastReport=std::move(r);return input;}
                    }
                    impl.stack.SetGpuFailureTarget(inject?StudioStack::GpuFailureTarget::Readback:StudioStack::GpuFailureTarget::None);
                    if(facialAnalysisActive&&!haveFaceGeometry) {
                        // Explicit absent geometry is a tagged, empty lighting-only
                        // bundle; it cannot borrow an older/general mailbox face.
                        auto absent=std::make_shared<AnalysisBridge::AnalysisBundle>();
                        absent->sourceGeneration=impl.generation;absent->sequenceId=r.frameSequence;absent->timestampPts=r.timestampPts;
                        faceGeometry=std::move(absent);
                    }
                    impl.stack.SetPreset(Preset(s,c));
                    auto* processed=impl.stack.ProcessFrame(bgr,r.frameSequence,r.timestampPts,&r,impl.generation,
                        faceLightingActive?faceGeometry:nullptr,touchupActive?faceGeometry:nullptr,accessoriesActive?faceGeometry:nullptr);
                    if(facialAnalysisActive&&!haveFaceGeometry) {
                        const auto state=impl.faceAnalysis->GetState();
                        if(!state.lastError.empty()&&r.errorDescription.empty())r.errorDescription=state.lastError;
                    }
                    if(impl.gpuReady&&!inject) {
                        impl.consecutiveGpuFailures=r.gpuFallbackUsed?impl.consecutiveGpuFailures+1:0;
                        if(impl.consecutiveGpuFailures>=3){impl.gpuReady=false;impl.retryGpuAt=now+std::chrono::seconds(5);impl.stack.SetExecutionBackend(StudioStack::ExecutionBackend::CpuOnly);}
                    }
                    if(processed&&processed!=bgr){auto* stable=impl.Own(processed,input);if(stable)result=stable;else {r.fallbackToOriginal=true;r.errorDescription="Could not retain studio output";}}
                    // Missing/expired analysis leaves the original source frame unchanged.
                }
            }
        }
    }catch(const std::exception& e){r.errorDescription=e.what();r.lightingStatus=StudioStack::StageStatus::Failed;r.overlayStatus=StudioStack::StageStatus::Failed;r.backgroundStatus=StudioStack::StageStatus::Failed;}
    {std::lock_guard<std::mutex> lock(m_mutex);m_lastReport=std::move(r);}
    return result;
}
}
