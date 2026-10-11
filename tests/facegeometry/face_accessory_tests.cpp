#include "effects/desktopstudiopipeline.h"
#include "providers/face_geometry.h"
#include "effects/gpu/d3d11_context.h"
#include "effects/appearance/appearanceprocessor.h"
#include "effects/overlays/face_accessory_3d.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/dict.h>
}
using namespace PrismForge::Effects;
namespace P=PrismForge::Providers;
namespace A=PrismForge::Effects::AnalysisBridge;
namespace S=PrismForge::Effects::StudioStack;
using Microsoft::WRL::ComPtr;
namespace {
int checks=0,groups=0;
void Require(bool ok,const std::string& reason) {++checks;if(!ok)throw std::runtime_error(reason);}
struct DeleteFrame{void operator()(AVFrame* f)const{av_frame_free(&f);}};
using Frame=std::unique_ptr<AVFrame,DeleteFrame>;
constexpr uint64_t Gen=7,Seq=1;
constexpr int64_t Pts=100000;
Frame MakeFrame(int w=160,int h=120,bool blank=false) {
    Frame frame(av_frame_alloc());Require(bool(frame),"allocate fixture descriptor");
    frame->width=w;frame->height=h;frame->format=AV_PIX_FMT_BGR24;
    Require(av_frame_get_buffer(frame.get(),64)==0,"allocate owned fixture pixels");
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        auto* pixel=frame->data[0]+size_t(y)*frame->linesize[0]+x*3;
        const int v=blank?48:120+((x+y)%2?9:-9);
        pixel[0]=uint8_t(v);pixel[1]=uint8_t(v+3);pixel[2]=uint8_t(v+6);
    }
    frame->pts=Pts;frame->time_base={1,1000000};frame->duration=16667;
    frame->colorspace=AVCOL_SPC_RGB;frame->color_range=AVCOL_RANGE_JPEG;
    frame->color_primaries=AVCOL_PRI_BT709;frame->color_trc=AVCOL_TRC_BT709;
    Require(av_dict_set(&frame->metadata,"touchup-current","original",0)==0,"owned source metadata");
    auto* side=av_frame_new_side_data(frame.get(),AV_FRAME_DATA_AFD,1);
    Require(side!=nullptr,"source side data allocation");side->data[0]=8;
    return frame;
}
std::vector<uint8_t> Bytes(const AVFrame* frame) {
    Require(frame&&frame->format==AV_PIX_FMT_BGR24,"valid BGR output");
    std::vector<uint8_t> out(size_t(frame->width)*frame->height*3);
    for(int y=0;y<frame->height;++y)std::memcpy(out.data()+size_t(y)*frame->width*3,
        frame->data[0]+size_t(y)*frame->linesize[0],size_t(frame->width)*3);
    return out;
}
const uint8_t* Pixel(const AVFrame* f,int x,int y){return f->data[0]+size_t(y)*f->linesize[0]+x*3;}
void Metadata(const AVFrame* out,const AVFrame* in) {
    Require(out->pts==in->pts&&out->time_base.num==in->time_base.num&&out->time_base.den==in->time_base.den&&
        out->duration==in->duration,"original source timing preserved");
    const auto* entry=av_dict_get(out->metadata,"touchup-current",nullptr,0);
    Require(entry&&std::string(entry->value)=="original","source dictionary preserved");
    const auto* side=av_frame_get_side_data(out,AV_FRAME_DATA_AFD);
    Require(side&&side->size==1&&side->data[0]==8,"source side data preserved without duplication");
}

P::FaceObservation Observation(int w=160,int h=120) {
    P::FaceObservation o;o.tags={Gen,Seq,Pts};o.imageWidth=w;o.imageHeight=h;o.detected=true;
    o.vertices.assign(478,P::FacePoint3D{.5f,.5f,0});
    o.vertices[200]={.2f,.15f,0};o.vertices[201]={.8f,.15f,0};o.vertices[202]={.8f,.9f,0};o.vertices[203]={.2f,.9f,0};
    o.vertices[33]=o.vertices[133]={.35f,.45f,0};o.vertices[263]=o.vertices[362]={.65f,.45f,0};
    o.vertices[10]={.5f,.2f,0};o.vertices[168]={.5f,.44f,0};return o;
}
std::shared_ptr<A::AnalysisBundle> Geometry(int w=160,int h=120) {
    P::FaceTopology topology;topology.triangles={{{200,201,202}},{{200,202,203}}};
    P::FaceGeometry g;std::string error;
    Require(P::BuildFaceGeometry(Observation(w,h),topology,w,h,g,error),"synthetic actual geometry: "+error);
    auto b=std::make_shared<A::AnalysisBundle>();b->sourceGeneration=Gen;b->sequenceId=Seq;b->timestampPts=Pts;
    b->personMask=std::move(g.coverage);b->normalMap=std::move(g.normals);b->depthMap=std::move(g.depth);
    b->skinMask=std::move(g.touchupSurface);b->protectionMask=std::move(g.featureProtection);b->faceLandmarks=std::move(g.anchors);return b;
}
Presets::StudioPreset Preset(bool glasses=true,bool cap=true) {
    auto p=Presets::StudioPreset::Neutral();p.bypassAll=false;p.overlays.bypass=false;
    p.overlays.glasses.enabled=glasses;p.overlays.hat.enabled=cap;
    for(auto* c:{&p.overlays.glasses,&p.overlays.hat})c->smoothing.enabled=false;
    return p;
}
void Configure(S::StudioStack& stack,const Presets::StudioPreset& p) {stack.SetSourceGeneration(Gen);stack.SetPreset(p);}
void EvidenceAndTransforms() {
    auto b=Geometry();auto& a=b->faceLandmarks;
    Require(a.detected&&!b->HasFaceLandmarks()&&a.overallConfidence==0&&a.leftEye.confidence==0&&a.rightEye.confidence==0,
        "actual geometry retains zero uncalibrated native confidence");
    Require(a.NativeGeometryMatches(160,120,Gen,Seq,Pts),"explicit validated geometry carries exact native source tags");
    const auto p=Preset();
    const auto metrics=Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioAviatorGlasses);
    auto t=Overlays::TrackingEvaluator::ComputeTargetTransform(Overlays::AnchorMode::FaceGlasses,p.overlays.glasses,a,160,120,metrics);
    Require(t.valid&&std::abs(t.posX-80)<.001f&&std::abs(t.posY-54)<.001f&&std::abs(t.scaleX-.2f)<.001f&&std::abs(t.rotationRadians)<.001f,
        "independent eye-center/baseline oracle determines real transform");
    auto h=Overlays::TrackingEvaluator::ComputeTargetTransform(Overlays::AnchorMode::FaceHat,p.overlays.hat,a,160,120,
        Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioCap));
    Require(h.valid&&std::abs(h.posX-80)<.001f&&std::abs(h.posY-24)<.001f,"cap uses actual forehead rather than confidence extrapolation");
    auto legacy=a;legacy.evidence=A::FaceLandmarkEvidence::ConfidenceScores;
    Require(!Overlays::TrackingEvaluator::ComputeTargetTransform(Overlays::AnchorMode::FaceGlasses,p.overlays.glasses,legacy,160,120,metrics).valid,
        "zero-confidence external analysis still fails original confidence gate");
    legacy.overallConfidence=legacy.leftEye.confidence=legacy.rightEye.confidence=.9f;
    Require(Overlays::TrackingEvaluator::ComputeTargetTransform(Overlays::AnchorMode::FaceGlasses,p.overlays.glasses,legacy,160,120,metrics).valid,
        "calibrated external confidence remains supported");
    for(int v=0;v<8;++v) {
        auto bad=a;
        if(v==0)bad.geometryRevision=2;
        if(v==1)bad.geometrySourceWidth=159;
        if(v==2)bad.geometrySourceGeneration=0;
        if(v==3)bad.leftEye.x=std::numeric_limits<float>::quiet_NaN();
        if(v==4)bad.rightEye=bad.leftEye;
        if(v==5)bad.foreheadCenter.y=.9f;
        if(v==6)bad.noseBridge.x=1.f;
        if(v==7)bad.detected=false;
        Require(!bad.HasValidatedNativeGeometry(160,120),"invalid/degenerate/native evidence is unusable");
    }
    // On-screen clamping must not turn partly clipped anchors into evidence.
    auto observation=Observation();observation.vertices[33].x=-.1f;P::FaceGeometry output;
    P::FaceTopology topology;topology.triangles={{{200,201,202}},{{200,202,203}}};std::string error;
    Require(P::BuildFaceGeometry(observation,topology,160,120,output,error)&&
        !output.anchors.HasValidatedNativeGeometry(160,120),"visible lighting geometry can remain usable while clipped accessories are excluded");
    Overlays::TrackingEvaluator tracking;tracking.Update(p.overlays,&a,160,120,Gen,Pts,Seq);
    Require(tracking.GetGlassesTransform().valid,"current native tracking starts");
    tracking.Update(p.overlays,nullptr,160,120,Gen,Pts,Seq);
    Require(!tracking.GetGlassesTransform().valid&&!tracking.GetHatTransform().valid,"missing same-frame native evidence removes stale transforms immediately");
    tracking.Update(p.overlays,&a,160,120,Gen,Pts+1,Seq+1);
    Require(!tracking.GetGlassesTransform().valid,"late nested native tags never track current frame");
}
void CpuPixelsAndOwnership() {
    auto input=MakeFrame();auto geometry=Geometry();auto original=Bytes(input.get());S::StudioStack stack;Configure(stack,Preset());
    S::StackExecutionReport r;auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,geometry);
    Require(out&&r.overlayStatus==S::StageStatus::Applied&&r.overlayLayers.glasses==0&&r.overlayLayers.hat==0,"both original accessories render on CPU");
    auto pixels=Bytes(out);size_t changed=0;for(size_t i=0;i<pixels.size();++i)if(pixels[i]!=original[i])++changed;
    Require(changed>600&&changed<pixels.size()/2,"substantial accessory rasterization affects only its bounded area");
    Require(std::memcmp(Pixel(out,0,119),Pixel(input.get(),0,119),3)==0&&
        std::memcmp(Pixel(out,159,119),Pixel(input.get(),159,119),3)==0,"independent distant corners remain byte exact");
    // At the left lens center asset coordinates are exactly (200,160). This is
    // an independent straight-alpha pixel oracle, not a second renderer call.
    auto glass=Overlays::ProceduralAssetGenerator::GetAsset(Overlays::ProceduralAssetId::StudioAviatorGlasses);
    const auto* texel=glass.data.data()+160*glass.strideBytes+200*4;
    S::StudioStack only;Configure(only,Preset(true,false));auto* eye=only.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,geometry);
    for(int c=0;c<3;++c) {
        const float alpha=texel[3]/255.f;const int expected=int(texel[2-c]*alpha+Pixel(input.get(),56,54)[c]*(1-alpha)+.5f);
        Require(std::abs(int(Pixel(eye,56,54)[c])-expected)<=1,"independent lens-center alpha blend matches");
    }
    Require(Bytes(input.get())==original,"caller pixels never mutated");Metadata(out,input.get());
    Frame retained(av_frame_clone(out));Require(bool(retained),"retain processed fanout output");auto saved=Bytes(retained.get());
    auto next=std::make_shared<A::AnalysisBundle>(*geometry);next->sequenceId=2;next->timestampPts=Pts+16667;
    next->faceLandmarks.geometrySequenceId=2;next->faceLandmarks.geometryTimestampPts=Pts+16667;
    input->pts+=16667;stack.ProcessFrame(input.get(),2,input->pts,&r,Gen,nullptr,nullptr,next);
    stack.SetSourceGeneration(Gen+1);stack.ResetTrackedAccessories();Require(Bytes(retained.get())==saved,"retained frame survives next frame and source reset");
}

void RawBgrOwners() {
    auto input=MakeFrame();auto g=Geometry();auto original=Bytes(input.get());
    AVFrame raw=*input.get();raw.extended_data=nullptr;for(auto& owner:raw.buf)owner=nullptr;
    raw.buf[3]=input->buf[0];Require(raw.extended_data==nullptr&&raw.buf[0]==nullptr,"raw fixture owns pixels in nonzero standard slot without auxiliary planes");
    S::StudioStack cpu;Configure(cpu,Preset());S::StackExecutionReport cr;
    auto* out=cpu.ProcessFrame(&raw,Seq,Pts,&cr,Gen,nullptr,nullptr,g);
    Require(out&&cr.overlayStatus==S::StageStatus::Applied&&Bytes(out)!=original,"actual CPU overlay accepts valid owned raw BGR without crashing");
    Metadata(out,input.get());const auto expected=Bytes(out);
    Gpu::D3D11Context d3d;Require(d3d.Initialize(false),"actual WARP for raw fallback");S::StudioStack gpu;Configure(gpu,Preset());
    gpu.SetExecutionBackend(S::ExecutionBackend::GpuWithCpuFallback);Require(gpu.InitializeGpu(d3d.GetDevice(),d3d.GetContext()),"raw GPU resources");
    gpu.SetGpuFailureTarget(S::GpuFailureTarget::Readback);S::StackExecutionReport gr;
    out=gpu.ProcessFrame(&raw,Seq,Pts,&gr,Gen,nullptr,nullptr,g);
    Require(out&&gr.gpuFallbackUsed&&gr.overlayStatus==S::StageStatus::Applied&&Bytes(out)==expected,
        "actual GPU readback failure restarts valid raw BGR overlay on CPU with exact pixels");
    Require(Bytes(input.get())==original&&raw.extended_data==nullptr&&raw.buf[3]==input->buf[0],"borrowed raw descriptor and caller owners unchanged");
    Overlays::OverlayRenderer renderer;Overlays::OverlayOutcome outcome;
    AVBufferRef shortOwner=*input->buf[0];shortOwner.size=1;raw.buf[3]=&shortOwner;
    Require(!renderer.ProcessFrame(&raw,Preset().overlays,&g->faceLandmarks,nullptr,&outcome,nullptr,Gen,Pts,Seq)&&
        outcome==Overlays::OverlayOutcome::InvalidInput,"raw owned short capacity rejected before reads");
    shortOwner=*input->buf[0];shortOwner.data=reinterpret_cast<uint8_t*>(UINTPTR_MAX-4);shortOwner.size=10;
    Require(!renderer.ProcessFrame(&raw,Preset().overlays,&g->faceLandmarks,nullptr,&outcome,nullptr,Gen,Pts,Seq)&&
        outcome==Overlays::OverlayOutcome::InvalidInput,"owned address extent overflow rejected");
    raw.buf[3]=nullptr;raw.data[0]=reinterpret_cast<uint8_t*>(UINTPTR_MAX-4);
    Require(!renderer.ProcessFrame(&raw,Preset().overlays,&g->faceLandmarks,nullptr,&outcome,nullptr,Gen,Pts,Seq)&&
        outcome==Overlays::OverlayOutcome::InvalidInput,"raw source address/span overflow rejected");
    raw.data[0]=input->data[0];out=renderer.ProcessFrame(&raw,Preset().overlays,&g->faceLandmarks,nullptr,&outcome,nullptr,Gen,Pts,Seq);
    Require(out&&outcome==Overlays::OverlayOutcome::Success&&Bytes(out)==expected,"legitimate synchronous unowned raw BGR remains supported");
}
void ActualGpu(bool hardware) {
    Gpu::D3D11Context d3d;Require(d3d.Initialize(hardware)&&d3d.IsHardwareAdapter()==hardware,"actual hardware/WARP adapter");
    std::cout<<"ADAPTER "<<d3d.GetAdapterName()<<std::endl;
    auto input=MakeFrame();auto g=Geometry();auto p=Preset();S::StackExecutionReport r,cr;
    S::StudioStack cpu,gpu;Configure(cpu,p);Configure(gpu,p);gpu.SetExecutionBackend(S::ExecutionBackend::GpuWithCpuFallback);
    Require(gpu.InitializeGpu(d3d.GetDevice(),d3d.GetContext()),"actual GPU stack initialization");
    const auto expected=Bytes(cpu.ProcessFrame(input.get(),Seq,Pts,&cr,Gen,nullptr,nullptr,g));
    auto* output=gpu.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,g);
    Require(r.overlayStatus==S::StageStatus::Applied&&r.overlayLayers.glasses==0&&r.overlayLayers.hat==0&&!r.gpuFallbackUsed&&
        r.imageUploadCount==1&&r.imageReadbackCount==1,"actual combined GPU overlay and completed readback");
    auto actual=Bytes(output);int maximum=0;for(size_t i=0;i<actual.size();++i)maximum=std::max(maximum,std::abs(int(actual[i])-expected[i]));
    std::cout<<"ACCESSORY_GPU_MAX_BYTE_ERROR "<<maximum<<std::endl;Require(maximum<=1,"GPU overlay agrees with actual CPU oracle within one byte");
    Metadata(output,input.get());
    for(auto fault:{S::GpuFailureTarget::OverlayStage,S::GpuFailureTarget::Readback}) {
        gpu.SetGpuFailureTarget(fault);output=gpu.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,g);
        Require(r.gpuFallbackUsed&&r.overlayStatus==S::StageStatus::Applied&&Bytes(output)==expected,"actual GPU fault restarts same exact accessory geometry on CPU");
    }
}
std::shared_ptr<A::AnalysisBundle> Body(const A::AnalysisBundle& face) {
    auto body=std::make_shared<A::AnalysisBundle>(face);auto& a=body->faceLandmarks;a.evidence=A::FaceLandmarkEvidence::ConfidenceScores;
    a.overallConfidence=a.leftEye.confidence=a.rightEye.confidence=a.foreheadCenter.confidence=.9f;
    for(int y=0;y<body->personMask.height;++y)for(int x=0;x<body->personMask.width;++x)
        body->personMask.data[size_t(y)*body->personMask.strideFloats+x]=(x>=15&&x<145)?1.f:0.f;
    return body;
}
void ExactMissingAndIndependent() {
    auto input=MakeFrame();auto g=Geometry();const auto original=Bytes(input.get());
    for(int variant=0;variant<9;++variant) {
        auto bad=std::make_shared<A::AnalysisBundle>(*g);
        if(variant==0)bad=std::make_shared<A::AnalysisBundle>();
        if(variant==1)--bad->sourceGeneration;
        if(variant==2)--bad->sequenceId;
        if(variant==3)--bad->timestampPts;
        if(variant==4)--bad->faceLandmarks.geometrySourceGeneration;
        if(variant==5)--bad->faceLandmarks.geometrySequenceId;
        if(variant==6)--bad->faceLandmarks.geometryTimestampPts;
        if(variant==7)--bad->faceLandmarks.geometrySourceWidth;
        if(variant==8)bad->faceLandmarks.leftEye.x=std::numeric_limits<float>::infinity();
        S::StudioStack stack;Configure(stack,Preset());auto box=std::make_shared<A::AnalysisMailbox>();stack.AttachAnalysisMailbox(box);
        Require(box->PostAnalysis(Body(*g)),"calibrated generic mailbox face deliberately exists");S::StackExecutionReport r;
        stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,g);
        auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,bad);
        Require(out&&r.overlayStatus==S::StageStatus::NotReady&&r.overlayLayers.glasses==2&&r.overlayLayers.hat==2&&Bytes(out)==original,
            "explicit missing/stale/invalid native anchors remove both overlays without generic mailbox fallback");
    }
    auto full=Preset();full.inputLook=int(Look::Warm);full.backgroundMode=Presets::StudioBackgroundMode::Solid;
    full.background.solidB=11;full.background.solidG=22;full.background.solidR=33;full.inputLutIntensity=.8f;
    std::string error;auto lut=CubeLut::Parse("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n",&error);
    Require(bool(lut),"owned identity LUT");auto shared=std::shared_ptr<const CubeLut>(std::move(lut));S::StudioStack actual,expected;
    Configure(actual,full);auto plain=full;plain.overlays=Overlays::OverlayStackConfig::Neutral();Configure(expected,plain);
    actual.SetInputLut(shared);expected.SetInputLut(shared);auto ab=std::make_shared<A::AnalysisMailbox>(),eb=std::make_shared<A::AnalysisMailbox>();
    actual.AttachAnalysisMailbox(ab);expected.AttachAnalysisMailbox(eb);Require(ab->PostAnalysis(Body(*g))&&eb->PostAnalysis(Body(*g)),"independent person mailbox");
    S::StackExecutionReport ar,er;const auto oracle=Bytes(expected.ProcessFrame(input.get(),Seq,Pts,&er,Gen));
    auto* out=actual.ProcessFrame(input.get(),Seq,Pts,&ar,Gen,nullptr,nullptr,std::make_shared<A::AnalysisBundle>());
    Require(ar.overlayStatus==S::StageStatus::NotReady&&ar.inputLookStatus==S::StageStatus::Applied&&ar.inputLutStatus==S::StageStatus::Applied&&
        ar.backgroundStatus==S::StageStatus::Applied&&Bytes(out)==oracle,"lost face preserves independent current look/LUT/person background");
    Require(Pixel(out,1,60)[0]==11&&Pixel(out,1,60)[1]==22&&Pixel(out,1,60)[2]==33,"independent replacement corner oracle");
}
void NeutralSettings() {
    auto input=MakeFrame();DesktopStudioPipeline pipeline(false);Require(pipeline.Process(input.get(),7)==input.get()&&
        pipeline.GetFaceAnalysisState().submittedFrames==0,"accessories default off without analysis");
    DesktopStudioSettings s;s.glasses=true;s.cap=true;s.accessoryScale=std::numeric_limits<float>::quiet_NaN();pipeline.ConfigureStudio(s);
    Require(pipeline.GetStudioSettings().accessoryScale==1,"nonfinite accessory scale uses neutral size");
    s.accessoryScale=4;pipeline.ConfigureStudio(s);Require(pipeline.GetStudioSettings().accessoryScale==1.5f,"oversized accessory settings bounded");
    s.accessoryScale=-4;pipeline.ConfigureStudio(s);Require(pipeline.GetStudioSettings().accessoryScale==.5f,"undersized accessory settings bounded");
    DesktopColorSettings color;color.bypassed=true;pipeline.Configure(color);
    Require(pipeline.Process(input.get(),7)==input.get()&&pipeline.GetFaceAnalysisState().submittedFrames==0&&pipeline.GetStudioSettings().glasses&&
        pipeline.GetStudioSettings().cap,"master bypass preserves choices without inference");
}
Frame Decode(const wchar_t* path) {
    auto checked=[](HRESULT hr){if(FAILED(hr))throw std::runtime_error("WIC fixture decode failed");};
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> converter;ComPtr<IWICBitmapClipper> clip;
    checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    checked(factory->CreateDecoderFromFilename(path,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder));
    checked(decoder->GetFrame(0,&decoded));UINT w=0,h=0;checked(decoded->GetSize(&w,&h));
    Require(w&&h&&w<=8192&&h<=8192&&uint64_t(w)*h*3<=32*1024*1024,"bounded fixture before copying");
    ComPtr<IWICBitmapSource> source=decoded;
    if((w==1376||w==1377)&&(h==915||h==917)) {
        checked(factory->CreateBitmapClipper(&clip));WICRect area{126,287,792,443};checked(clip->Initialize(decoded.Get(),&area));
        source=clip;w=792;h=443; // User-supplied camera crop, held in memory only.
    }
    checked(factory->CreateFormatConverter(&converter));checked(converter->Initialize(source.Get(),GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    auto frame=MakeFrame(int(w),int(h));
    checked(converter->CopyPixels(nullptr,UINT(frame->linesize[0]),UINT(size_t(frame->linesize[0])*h),frame->data[0]));
    return frame;
}

AVFrame* WaitApplied(DesktopStudioPipeline& pipeline,AVFrame* input,uint64_t generation,bool gpu,bool fallback=false) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    do {
        input->pts+=16667;auto* output=pipeline.Process(input,generation);const auto r=pipeline.GetLastReport();const auto state=pipeline.GetFaceAnalysisState();
        if(state.phase==P::FaceAnalysisPhase::Failed)throw std::runtime_error("native service failure: "+state.lastError);
        const auto requested=pipeline.GetStudioSettings();
        const int glassesStatus=requested.glasses?0:1,capStatus=requested.cap?0:1;
        if(r.overlayLayers.glasses==glassesStatus&&r.overlayLayers.hat==capStatus) {
            Require(output&&output!=input&&state.enabled&&state.ready&&state.lastDetected,"all selected accessories use actual native current observation");
            Require(state.sourceGeneration==generation&&state.lastCompletedSequence==r.frameSequence&&state.lastCompletedPts==input->pts&&r.timestampPts==input->pts,
                "native accessory tags match this original source frame");
            Require(r.gpuFallbackUsed==fallback&&r.imageUploadCount==(gpu?1:0),"truthful current production GPU/fallback route");
            Metadata(output,input);return output;
        }
        Require(output==input&&r.overlayStatus==S::StageStatus::NotReady&&r.imageUploadCount==0,"pending native accessory-only frame remains original without transfers");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("native accessories did not apply by offline fixture deadline");
}

namespace R=Overlays::Realistic;
std::shared_ptr<A::AnalysisBundle> RealGeometry(float yaw=0) {
    auto o=Observation();o.hasTransform=true;o.transform={};
    o.transform[0]=o.transform[10]=std::cos(yaw);o.transform[2]=std::sin(yaw);o.transform[8]=-std::sin(yaw);
    o.transform[5]=o.transform[15]=1;
    o.vertices[468]={.35f,.45f,0};o.vertices[473]={.65f,.45f,0};
    // The independent cheek surface is behind the nasal bridge.
    for(int i:{200,201,202,203})o.vertices[i].z=.08f;
    P::FaceTopology t;t.triangles={{{200,201,202}},{{200,202,203}}};P::FaceGeometry g;std::string error;
    Require(P::BuildFaceGeometry(o,t,160,120,g,error),"actual synthetic 3D evidence "+error);
    auto b=std::make_shared<A::AnalysisBundle>();b->sourceGeneration=Gen;b->sequenceId=Seq;b->timestampPts=Pts;b->faceLandmarks=g.anchors;
    return b;
}
Presets::StudioPreset RealPreset(bool glasses=true,bool cap=true) {auto p=Preset(glasses,cap);p.realisticAccessories=true;return p;}
void MeshAndPerspective() {
    for(const auto* mesh:{&R::GlassesMesh(),&R::CapMesh()}){
        Require(mesh->vertices.size()>1000&&mesh->vertices.size()<12000&&mesh->triangles.size()>1000,"bounded genuine mesh geometry");
        float minZ=10,maxZ=-10;
        for(const auto& v:mesh->vertices){float n=v.nx*v.nx+v.ny*v.ny+v.nz*v.nz;Require(std::isfinite(v.x+v.y+v.z+n)&&std::abs(n-1)<.001f,"finite unit vertex normals");minZ=std::min(minZ,v.z);maxZ=std::max(maxZ,v.z);}
        Require(maxZ-minZ>.8f,"depth extends into curved temples / crown and visor");
        for(const auto& t:mesh->triangles)Require(t.a<mesh->vertices.size()&&t.b<mesh->vertices.size()&&t.c<mesh->vertices.size(),"triangle references stay bounded");
    }
    const auto& reference=R::GlassesMesh();
    size_t hinges=0;float lowestLens=10,highestLens=-10;float minLensX=10,maxLensX=-10;
    for(const auto& t:reference.triangles)for(uint32_t i:{t.a,t.b,t.c}) {
        const auto& v=reference.vertices[i];
        if(t.material==R::Material::Lens){lowestLens=std::min(lowestLens,v.y);highestLens=std::max(highestLens,v.y);minLensX=std::min(minLensX,v.x);maxLensX=std::max(maxLensX,v.x);}
        if(t.material==R::Material::Frame&&std::abs(std::abs(v.x)-.48f)<.023f&&v.y>-.036f&&v.y<.006f&&v.z>-.079f&&v.z<-.041f)++hinges;
    }
    Require(hinges>=36&&std::abs(minLensX+.46f)<.0001f&&std::abs(maxLensX-.46f)<.0001f&&
            std::abs(lowestLens+.255f)<.0001f&&std::abs(highestLens-.055f)<.0001f,
            "reference Wayfarer full-size lenses and solid hinges survive native import");
    bool squareCorner=false;for(const auto& v:reference.vertices)
        if(v.x>.415f&&v.y>.020f&&v.z>-.060f&&v.z<-.020f)squareCorner=true;
    Require(squareCorner,"reference superellipse has squarer upper outer corners than a circular frame");
    auto g=RealGeometry();Require(g->faceLandmarks.accessoryMesh&&g->faceLandmarks.accessoryMesh->Matches(160,120,Gen,Seq,Pts),"owned 478-vertex mesh has exact current tags");
    R::ProjectedScene scene;std::string error;Require(R::BuildProjectedScene(&g->faceLandmarks,160,120,true,true,1,scene,error),error);
    float lensTop=120,lensBottom=0;
    for(const auto& v:scene.lenses)if(v.a>.79f){lensTop=std::min(lensTop,v.y);lensBottom=std::max(lensBottom,v.y);}
    Require(std::abs((lensTop+lensBottom)*.5f-54.f)<2.5f,
            "imported lens centers fit the independent current pupil row, instead of hanging below the eyes");
    const float focal=120/(2.f*std::tan(3.14159265358979323846f/8.f));
    Require(std::abs(scene.focalPixels-focal)<.0001f&&std::abs(scene.faceDistance-focal*6.3f/48)<.001f,"independent 45-degree-camera and 63mm pupil baseline oracle");
    Require(scene.rasterWidth<=640&&scene.rasterHeight<=640&&scene.rasterWidth*scene.rasterHeight<=262144&&scene.faces.size()==6&&!scene.lenses.empty(),"bounded depth-only face and translucent lens lists");
    auto yaw=RealGeometry(.5f);R::ProjectedScene turned;Require(R::BuildProjectedScene(&yaw->faceLandmarks,160,120,true,true,1,turned,error),error);
    Require(std::abs(turned.opaque.front().x-scene.opaque.front().x)>2&&std::abs(turned.opaque.front().distance-scene.opaque.front().distance)>.2f,"row-major rigid yaw changes perspective and true depth");
    auto p=RealPreset();std::string json=Presets::PresetStore::Serialize(p);Presets::StudioPreset restored;
    Require(Presets::PresetStore::Deserialize(json,restored,error)&&restored.realisticAccessories,"3D preset round trip preserves renderer selection");
    const auto key=json.find("\"realisticAccessories\"");Require(key!=std::string::npos,"renderer selection serialized explicitly");
    auto legacy=Presets::StudioPreset::Neutral();Require(!legacy.realisticAccessories,"legacy presets retain their existing renderer by default");
}
void CapPresetRouting() {
    auto input=MakeFrame();auto geometry=RealGeometry();const auto original=Bytes(input.get());
    for(int style:{1,2}) {
        auto p=RealPreset(true,true);p.realisticCapStyle=style;std::string error;
        Presets::StudioPreset restored;
        Require(Presets::PresetStore::Deserialize(Presets::PresetStore::Serialize(p),restored,error)&&restored.realisticCapStyle==style,
            "downloaded cap selection survives preset round trip");
        R::FaceAccessory3DRenderer renderer;Overlays::OverlayOutcome outcome;Overlays::LayerReport layers;
        const auto expected=Bytes(renderer.ProcessFrame(input.get(),&geometry->faceLandmarks,true,true,1,&outcome,&layers,style));
        Require(outcome==Overlays::OverlayOutcome::Success&&expected!=original,"selected textured cap direct CPU output applies");
        S::StudioStack stack;Configure(stack,restored);S::StackExecutionReport report;
        Require(Bytes(stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry))==expected&&report.overlayLayers.hat==0,
            "serialized cap style reaches common stack rather than old procedural cap");
        Gpu::D3D11Context context;Require(context.Initialize(true),"downloaded cap stack hardware context");
        Require(stack.InitializeGpu(context.GetDevice(),context.GetContext()),"downloaded cap GPU graph initialization");
        auto* gpu=stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry);
        Require(gpu&&report.overlayStatus==S::StageStatus::Applied&&!report.gpuFallbackUsed&&report.overlayLayers.hat==0,
            "selected textured cap reaches actual GPU shared output");
        stack.SetGpuFailureTarget(S::GpuFailureTarget::OverlayStage);
        Require(Bytes(stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry))==expected&&report.gpuFallbackUsed,
            "actual fault restarts selected downloaded cap on same-frame CPU");
        Require(Bytes(input.get())==original,"cap style does not mutate camera input");
    }
    auto invalid=RealPreset();std::string error;invalid.realisticCapStyle=3;
    Require(!invalid.Validate(error),"out-of-range cap preset rejected");
    auto json=Presets::PresetStore::Serialize(RealPreset());const auto at=json.find("\"realisticCapStyle\": 0");
    Require(at!=std::string::npos,"cap style optional field serialized");
    for(const auto* value:{"1.5","-1","3","1e20","\"2\"","1garbage","01"}) {
        auto invalidJson=json;invalidJson.replace(at,std::strlen("\"realisticCapStyle\": 0"),std::string("\"realisticCapStyle\": ")+value);
        Presets::StudioPreset restored;Require(!Presets::PresetStore::Deserialize(invalidJson,restored,error),"malformed cap style cannot select arbitrary mesh");
    }
    DesktopStudioPipeline pipeline(false);DesktopStudioSettings settings;settings.capStyle=123;pipeline.ConfigureStudio(settings);
    Require(pipeline.GetStudioSettings().capStyle==2&&!pipeline.GetStudioSettings().cap,"invalid desktop cap style leaves effect off without selecting unreliable visor");
}
void GlassesCollectionRouting() {
    auto input=MakeFrame();auto geometry=RealGeometry();const auto original=Bytes(input.get());std::vector<std::vector<uint8_t>> outputs;
    for(int style=0;style<=3;++style){
        auto preset=RealPreset(true,true);preset.realisticGlassesStyle=style;preset.realisticCapStyle=2;
        preset.realisticCapScale=1.18f;preset.realisticCapVerticalOffset=.65f;
        Presets::StudioPreset restored;std::string error;
        Require(Presets::PresetStore::Deserialize(Presets::PresetStore::Serialize(preset),restored,error)&&
            restored.realisticGlassesStyle==style&&std::abs(restored.realisticCapScale-1.18f)<.00001f&&std::abs(restored.realisticCapVerticalOffset-.65f)<.00001f,
            "selected source glasses and independent cap fitting survive strict preset round trip");
        R::FaceAccessory3DRenderer renderer;Overlays::OverlayOutcome outcome;Overlays::LayerReport layers;
        const auto expected=Bytes(renderer.ProcessFrame(input.get(),&geometry->faceLandmarks,true,true,1,&outcome,&layers,2,style,1.18f,.65f));
        Require(outcome==Overlays::OverlayOutcome::Success&&expected!=original,"actual selected reference mesh/materials produce pixels");
        for(const auto& prior:outputs)Require(expected!=prior,"each authentic glasses selection has distinct actual pixels");outputs.push_back(expected);
        S::StudioStack stack;Configure(stack,restored);S::StackExecutionReport report;
        Require(Bytes(stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry))==expected,"current CPU stack preserves selected glasses and cap fit");
        Gpu::D3D11Context d3d;Require(d3d.Initialize(true)&&stack.InitializeGpu(d3d.GetDevice(),d3d.GetContext()),"actual collection GPU graph initialized");
        auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry);
        Require(out&&report.overlayStatus==S::StageStatus::Applied&&!report.gpuFallbackUsed,"all reference styles render on actual shared GPU graph");
        stack.SetGpuFailureTarget(S::GpuFailureTarget::OverlayStage);
        Require(Bytes(stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,nullptr,geometry))==expected&&report.gpuFallbackUsed,
            "selected glasses and independent cap fit restart the same frame on CPU after real GPU stage failure");
        Require(Bytes(input.get())==original,"collection leaves camera source exact");
    }
    const auto json=Presets::PresetStore::Serialize(RealPreset());
    for(const auto& field:std::vector<std::string>{"realisticGlassesStyle","realisticCapScale","realisticCapVerticalOffset"}){
        const auto key="\""+field+"\": ";const auto at=json.find(key);Require(at!=std::string::npos,"fit field serialized");
        const auto end=json.find(',',at);Require(end!=std::string::npos,"serialized field terminator");
        for(const auto& value:std::vector<std::string>{"\"1\"","NaN","1garbage","01","1e99"}){
            auto malformed=json;malformed.replace(at,end-at,key+value);Presets::StudioPreset restored;std::string error;
            Require(!Presets::PresetStore::Deserialize(malformed,restored,error),"malformed numeric selection/fit is rejected");
        }
    }
    for(int style:{-1,4}){auto p=RealPreset();p.realisticGlassesStyle=style;std::string error;Require(!p.Validate(error),"invalid mesh selector rejected");}
    for(float value:{-.1f,.5f,1.6f,std::nanf("")}){auto p=RealPreset();p.realisticCapScale=value;std::string error;Require(!p.Validate(error),"invalid independent cap size rejected");}
    DesktopStudioPipeline desktop(false);DesktopStudioSettings settings;settings.cap=true;settings.capStyle=1;settings.glassesStyle=99;settings.capScale=std::nanf("");settings.capVerticalOffset=std::nanf("");desktop.ConfigureStudio(settings);
    const auto clean=desktop.GetStudioSettings();Require(!clean.cap&&clean.capStyle==2&&clean.glassesStyle==0&&clean.capScale==1&&clean.capVerticalOffset==0,
        "unreliable visor is disabled and invalid persisted fit/style has bounded neutral defaults");
}
void RealCpuAndOcclusion() {
    auto input=MakeFrame();auto g=RealGeometry();auto original=Bytes(input.get());R::FaceAccessory3DRenderer renderer;Overlays::OverlayOutcome outcome;Overlays::LayerReport layers;
    auto* out=renderer.ProcessFrame(input.get(),&g->faceLandmarks,true,false,1,&outcome,&layers);
    Require(out&&outcome==Overlays::OverlayOutcome::Success&&layers.glasses==0&&Bytes(out)!=original,"perspective glasses actually render");Metadata(out,input.get());
    Require(std::memcmp(Pixel(out,52,65),Pixel(input.get(),52,65),3)!=0,"transparent near lens is visible in front of independently placed cheek");
    auto hidden=std::make_shared<A::FaceAccessoryGeometry>(*g->faceLandmarks.accessoryMesh);
    for(int i:{200,201,202,203})hidden->vertices[i].z=-.08f;
    auto lm=g->faceLandmarks;lm.accessoryMesh=hidden;
    out=renderer.ProcessFrame(input.get(),&lm,true,false,1,&outcome,&layers);
    Require(out&&std::memcmp(Pixel(out,52,65),Pixel(input.get(),52,65),3)==0,"independent nearer face plane fully occludes lens, not a painted face sticker");
    out=renderer.ProcessFrame(input.get(),&g->faceLandmarks,true,true,1,&outcome,&layers);Frame held(av_frame_clone(out));const auto saved=Bytes(held.get());
    renderer.Reset();Require(Bytes(held.get())==saved&&Bytes(input.get())==original,"owned output survives reset and caller pixels remain exact");
    AVFrame raw=*input.get();raw.extended_data=nullptr;for(auto& owner:raw.buf)owner=nullptr;raw.buf[3]=input->buf[0];
    Require(renderer.ProcessFrame(&raw,&g->faceLandmarks,true,true,1,&outcome,&layers)&&outcome==Overlays::OverlayOutcome::Success,"raw BGR accepts nonzero owner without auxiliary pointer");
    AVBufferRef shortOwner=*input->buf[0];shortOwner.size=1;raw.buf[3]=&shortOwner;
    Require(!renderer.ProcessFrame(&raw,&g->faceLandmarks,true,false,1,&outcome,&layers)&&outcome==Overlays::OverlayOutcome::InvalidInput,"short owned capacity rejected before pixel reads");
    Require(renderer.ProcessFrame(input.get(),nullptr,true,true,1,&outcome,&layers)==input.get()&&outcome==Overlays::OverlayOutcome::NotReady&&layers.glasses==2,"no-face path immediately returns current input");
}
void RealGuardsAndFrame() {
    auto input=MakeFrame();auto g=RealGeometry();const auto original=Bytes(input.get());
    for(int v=0;v<7;++v){auto bad=std::make_shared<A::AnalysisBundle>(*g);auto mesh=std::make_shared<A::FaceAccessoryGeometry>(*g->faceLandmarks.accessoryMesh);bad->faceLandmarks.accessoryMesh=mesh;
        if(v==0)--bad->sourceGeneration;if(v==1)--mesh->sequenceId;if(v==2)--mesh->timestampPts;if(v==3)--mesh->sourceWidth;
        if(v==4)mesh->transform[0]=0;if(v==5)mesh->vertices[468].x=std::nanf("");if(v==6)mesh->triangles[0][0]=478;
        S::StudioStack stack;Configure(stack,RealPreset());S::StackExecutionReport r;
        auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,bad);
        Require(out&&r.overlayStatus==S::StageStatus::NotReady&&r.overlayLayers.glasses==2&&Bytes(out)==original,"3D invalid/stale current mesh never paints previous accessories");
    }
    auto p=RealPreset();p.overlays.studioFrame.enabled=true;S::StudioStack stack;Configure(stack,p);S::StackExecutionReport r;
    auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,g);
    Require(out&&r.overlayLayers.glasses==0&&r.overlayLayers.hat==0&&r.overlayLayers.frame==0,"3D accessories compose with independent screen frame");
    out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,std::make_shared<A::AnalysisBundle>());
    Require(out&&r.overlayLayers.glasses==2&&r.overlayLayers.hat==2&&r.overlayLayers.frame==0&&Bytes(out)!=original,"missing face leaves independent screen frame enabled");
}
void RealGpu(bool hardware) {
    Gpu::D3D11Context context;Require(context.Initialize(hardware),hardware?"actual hardware 3D context":"actual WARP 3D context");
    auto input=MakeFrame();auto g=RealGeometry(.15f);const auto original=Bytes(input.get());S::StudioStack cpu,gpu;auto p=RealPreset();p.overlays.studioFrame.enabled=true;Configure(cpu,p);Configure(gpu,p);
    Require(gpu.InitializeGpu(context.GetDevice(),context.GetContext()),"3D stack GPU initialize");S::StackExecutionReport cr,gr;
    auto expected=Bytes(cpu.ProcessFrame(input.get(),Seq,Pts,&cr,Gen,nullptr,nullptr,g));auto* out=gpu.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,g);auto actual=Bytes(out);
    Require(gr.overlayStatus==S::StageStatus::Applied&&gr.overlayLayers.glasses==0&&gr.overlayLayers.hat==0&&gr.overlayLayers.frame==0&&!gr.gpuFallbackUsed&&gr.imageUploadCount==1&&gr.imageReadbackCount==1,"actual raster GPU and frame share one upload/readback without hidden fallback");
    double sum=0;size_t wide=0;int maximum=0;for(size_t i=0;i<actual.size();++i){int e=std::abs(int(actual[i])-int(expected[i]));sum+=e;maximum=std::max(maximum,e);if(e>4)++wide;}
    std::cout<<"3D "<<(hardware?"hardware":"WARP")<<" raster parity mean="<<sum/actual.size()<<" >4-byte="<<wide<<" maximum="<<maximum<<std::endl;
    Require(maximum<=2&&wide==0,"perspective raster backends agree within two UNORM codes for the independent synthetic fixture");
    Frame held(av_frame_clone(out));auto saved=Bytes(held.get());
    for(auto target:{S::GpuFailureTarget::OverlayStage,S::GpuFailureTarget::Readback}){
        gpu.SetGpuFailureTarget(target);out=gpu.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,g);
        Require(out&&gr.gpuFallbackUsed&&gr.overlayLayers.glasses==0&&Bytes(out)==expected,"actual overlay/readback fault restarts same frame through 3D CPU oracle");
    }
    gpu.SetGpuFailureTarget(S::GpuFailureTarget::None);out=gpu.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,g);
    Require(out&&!gr.gpuFallbackUsed&&Bytes(held.get())==saved&&Bytes(input.get())==original,"GPU recovery leaves retained output and source untouched");
}
void SavePng(const AVFrame* f,const wchar_t* name) {
    wchar_t exe[32768]{};Require(GetModuleFileNameW(nullptr,exe,32768)>0,"visual fixture directory");const auto path=std::filesystem::path(exe).parent_path()/name;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    auto check=[](HRESULT hr){Require(SUCCEEDED(hr),"encode real fixture PNG");};check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    check(factory->CreateStream(&stream));check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    check(encoder->CreateNewFrame(&frame,nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(f->width,f->height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;check(frame->SetPixelFormat(&format));
    Require(format==GUID_WICPixelFormat24bppBGR,"PNG source pixels unchanged");check(frame->WritePixels(f->height,f->linesize[0],UINT(size_t(f->linesize[0])*f->height),f->data[0]));check(frame->Commit());check(encoder->Commit());
    std::wcout<<L"REALISTIC_VISUAL "<<path.wstring()<<std::endl;
}
void NativeProduction(const wchar_t* fixture) {
    auto input=Decode(fixture);const auto original=Bytes(input.get());DesktopStudioSettings s;s.glasses=true;s.cap=true;
    DesktopStudioPipeline cpu(false),gpu(true);cpu.ConfigureStudio(s);gpu.ConfigureStudio(s);
    Frame retained(av_frame_clone(WaitApplied(cpu,input.get(),20,false)));Require(bool(retained),"retain exact processed fanout pixels");auto saved=Bytes(retained.get());SavePng(input.get(),L"realistic-source.png");SavePng(retained.get(),L"realistic-native-cpu.png");
    auto* out=WaitApplied(gpu,input.get(),20,true);SavePng(out,L"realistic-native-gpu.png");auto bytes=Bytes(out);size_t changed=0;for(size_t i=0;i<bytes.size();++i)if(bytes[i]!=original[i])++changed;
    Require(changed>200&&changed<bytes.size()/3,"actual native accessory rasterized and confined");
    Require(Bytes(input.get())==original&&gpu.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"accessory-only route does not mutate input or invoke person model");
    gpu.InjectGpuReadbackFailureForTesting(true);WaitApplied(gpu,input.get(),21,true,true);gpu.InjectGpuReadbackFailureForTesting(false);
    Require(Bytes(retained.get())==saved,"retained native fanout survives source change and actual fallback");
    s.touchupStrength=.5f;s.touchupDetail=.25f;s.light=DesktopLightMode::Custom;s.strength=.5f;s.faceLighting=true;
    gpu.ConfigureStudio(s);WaitApplied(gpu,input.get(),22,true);
    Require(gpu.GetLastReport().appearanceStatus==S::StageStatus::Applied&&gpu.GetLastReport().lightingStatus==S::StageStatus::Applied,
        "current native geometry supports touch-up, facial lighting and both accessories together");
    const auto before=gpu.GetFaceAnalysisState().submittedFrames;input->pts+=16667;gpu.Process(input.get(),22);const auto after=gpu.GetFaceAnalysisState();
    Require(after.submittedFrames==before+1,"all facial effects submit exactly one native analysis per input");
    auto blank=MakeFrame(96,64,true);blank->pts=2000000;bool noFace=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    do {
        blank->pts+=16667;out=gpu.Process(blank.get(),23);const auto r=gpu.GetLastReport();const auto state=gpu.GetFaceAnalysisState();
        Require(out==blank.get()&&r.overlayLayers.glasses==2&&r.overlayLayers.hat==2&&r.imageUploadCount==0&&r.imageReadbackCount==0,
            "source/resolution reset with no face removes accessories and retains current zero-transfer source");
        if(state.ready&&state.lastCompletedPts==blank->pts&&!state.lastDetected){noFace=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    Require(noFace,"actual no-face native observation completed after reset");
    s.capStyle=2;gpu.ConfigureStudio(s);out=WaitApplied(gpu,input.get(),24,true);
    SavePng(out,L"pokemon-native-gpu.png");
    Require(gpu.GetStudioSettings().capStyle==2&&gpu.GetLastReport().overlayLayers.hat==0,
        "actual native face route applies selected Pokemon cap alongside facial effects");
    std::vector<std::vector<uint8_t>> rendered;
    const wchar_t* names[]={L"wayfarer-native-gpu.png",L"aviator-native-gpu.png",L"tortoise-native-gpu.png",L"cateye-native-gpu.png"};
    s.cap=false;s.touchupStrength=0;s.faceLighting=false;s.light=DesktopLightMode::Off;
    for(int style=0;style<=3;++style){s.glassesStyle=style;gpu.ConfigureStudio(s);out=WaitApplied(gpu,input.get(),25+style,true);SavePng(out,names[style]);
        auto pixels=Bytes(out);Require(pixels!=original&&gpu.GetLastReport().overlayLayers.glasses==0,"native camera fixture renders selected authentic glasses");
        for(const auto& prior:rendered)Require(pixels!=prior,"native rendered styles remain visually distinct");rendered.push_back(std::move(pixels));}
    DesktopColorSettings bypass;bypass.bypassed=true;gpu.Configure(bypass);
    const auto submitted=gpu.GetFaceAnalysisState().submittedFrames;
    Require(gpu.Process(input.get(),24)==input.get()&&gpu.GetFaceAnalysisState().submittedFrames==submitted&&
        gpu.GetFaceAnalysisState().phase==P::FaceAnalysisPhase::Disabled,"bypass disables shared native worker and retains source");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2){std::cerr<<"Usage: face_accessory_tests fixture.png\n";return 2;}
    const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);struct ComGuard{bool owns;~ComGuard(){if(owns)CoUninitialize();}} guard{SUCCEEDED(hr)};
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return 2;
    try {
#define GROUP(label,body) ++groups;std::cout<<"GROUP " label<<std::endl;{body;}std::cout<<"PASS " label<<std::endl;
        GROUP("explicit native geometric evidence / no fabricated confidence / independent transforms / no stale tracking",EvidenceAndTransforms());
        GROUP("actual CPU original accessories / independent pixel oracle / source metadata / retained fanout",CpuPixelsAndOwnership());
        GROUP("raw owned/borrowed BGR without auxiliary planes / checked bounds / actual GPU-to-CPU restart",RawBgrOwners());
        GROUP("actual hardware GPU accessories / CPU agreement / same-frame fault restart",ActualGpu(true));
        ActualGpu(false);std::cout<<"PASS WARP repetition of GPU route"<<std::endl;
        GROUP("exact source/sequence/PTS/dimensions guards / no generic face borrowing / independent background and LUT",ExactMissingAndIndependent());
        GROUP("default off / bounded settings / bypass without inference",NeutralSettings());
        GROUP("genuine 3D meshes / independent perspective camera / row-major pose / preset compatibility",MeshAndPerspective());
        GROUP("downloaded cap preset styles / actual shared CPU-GPU graph / same-frame fallback",CapPresetRouting());
        GROUP("authentic reference glasses / independent cap fit / strict presets / actual CPU-GPU-fault routing",GlassesCollectionRouting());
        GROUP("3D CPU depth-only face occlusion / clear lens / raw ownership / no-face",RealCpuAndOcclusion());
        GROUP("3D exact mesh tags / invalid pose / independent screen frame",RealGuardsAndFrame());
        GROUP("actual hardware 3D raster / bounded CPU agreement / fault restart / recovery",RealGpu(true));
        RealGpu(false);std::cout<<"PASS WARP repetition of 3D raster"<<std::endl;
        GROUP("actual native CPU/GPU / all facial effects one analysis / source-resolution reset / no face",NativeProduction(argv[1]));
#undef GROUP
        std::cout<<"FACE_ACCESSORY_PASS groups="<<groups<<" assertions="<<checks<<"; offline native fixture only; no live FPS/quality claim\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FACE_ACCESSORY_FAIL groups="<<groups<<" assertions="<<checks<<" reason="<<e.what()<<std::endl;return 1;}
}
