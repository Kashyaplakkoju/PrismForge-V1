#include "effects/desktopstudiopipeline.h"
#include "providers/face_geometry.h"
#include "effects/gpu/d3d11_context.h"
#include "effects/appearance/appearanceprocessor.h"
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
template<size_t N>void Feature(P::FaceObservation& o,const std::array<size_t,N>& points,
    double cx,double cy,double rx,double ry) {
    for(size_t i=0;i<N;++i) {const double theta=2*3.141592653589793*double(i)/N;
        o.vertices[points[i]]={float(cx+rx*std::cos(theta)),float(cy+ry*std::sin(theta)),0};}
}
P::FaceObservation Observation(int w=160,int h=120) {
    P::FaceObservation o;o.tags={Gen,Seq,Pts};o.imageWidth=w;o.imageHeight=h;o.detected=true;
    o.vertices.assign(478,P::FacePoint3D{.5f,.5f,0});
    o.vertices[200]={.2f,.15f,0};o.vertices[201]={.8f,.15f,0};o.vertices[202]={.8f,.9f,0};o.vertices[203]={.2f,.9f,0};
    Feature(o,std::array<size_t,16>{33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246},.35,.4,.07,.03);
    Feature(o,std::array<size_t,16>{263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466},.65,.4,.07,.03);
    Feature(o,std::array<size_t,20>{61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185},.5,.7,.11,.045);
    Feature(o,std::array<size_t,10>{46,53,52,65,55,70,63,105,66,107},.35,.3,.08,.02);
    Feature(o,std::array<size_t,10>{276,283,282,295,285,300,293,334,296,336},.65,.3,.08,.02);
    return o;
}
std::shared_ptr<A::AnalysisBundle> Geometry(int w=160,int h=120) {
    P::FaceTopology topology;topology.triangles={{{200,201,202}},{{200,202,203}}};
    P::FaceGeometry g;std::string error;
    Require(P::BuildFaceGeometry(Observation(w,h),topology,w,h,g,error),"synthetic actual geometry: "+error);
    auto bundle=std::make_shared<A::AnalysisBundle>();bundle->sourceGeneration=Gen;bundle->sequenceId=Seq;bundle->timestampPts=Pts;
    bundle->personMask=std::move(g.coverage);bundle->normalMap=std::move(g.normals);bundle->depthMap=std::move(g.depth);
    bundle->skinMask=std::move(g.touchupSurface);bundle->protectionMask=std::move(g.featureProtection);
    bundle->faceLandmarks=std::move(g.anchors);return bundle;
}
Presets::StudioPreset Preset() {
    auto preset=Presets::StudioPreset::Neutral();preset.bypassAll=false;preset.appearance.bypass=false;
    preset.appearance.smoothingStrength=.65f;preset.appearance.detailPreservation=.5f;preset.appearance.sharpenStrength=0;
    return preset;
}
void Configure(S::StudioStack& stack,const Presets::StudioPreset& preset) {stack.SetSourceGeneration(Gen);stack.SetPreset(preset);}
void ExactProtected(const AVFrame* out,const AVFrame* in,const A::AnalysisBundle& g) {
    Require(out->width==in->width&&out->height==in->height,"same source shape");
    size_t changed=0,protectedCount=0,outsideCount=0;
    for(int y=0;y<in->height;++y)for(int x=0;x<in->width;++x) {
        const size_t at=size_t(y)*g.skinMask.strideFloats+x;
        const bool different=std::memcmp(Pixel(out,x,y),Pixel(in,x,y),3)!=0;
        if(g.skinMask.data[at]==0||g.protectionMask.data[at]==1) {
            Require(!different,"every ineligible/protected synthetic source pixel remains exact");
            if(g.protectionMask.data[at]==1)++protectedCount;
            if(g.personMask.data[at]==0)++outsideCount;
        }else if(different)++changed;
    }
    Require(changed>300&&protectedCount>300&&outsideCount>3000,"real smoothing and independent feature/background preservation");
    // Independent texture metric: smoothing must reduce known alternating luminance.
    double originalContrast=0,processedContrast=0;size_t edges=0;
    for(int y=0;y<in->height;++y)for(int x=1;x<in->width;++x) {
        const auto at=size_t(y)*g.skinMask.strideFloats+x;
        if(g.skinMask.data[at]<1||g.skinMask.data[at-1]<1)continue;
        originalContrast+=std::abs(int(Pixel(in,x,y)[0])-Pixel(in,x-1,y)[0]);
        processedContrast+=std::abs(int(Pixel(out,x,y)[0])-Pixel(out,x-1,y)[0]);++edges;
    }
    Require(edges>1000&&processedContrast<originalContrast*.99&&processedContrast>originalContrast*.6,
        "independent texture contrast shows restrained smoothing rather than a flat face");
}
void GeometryProtection() {
    auto g=Geometry();
    const std::array<std::array<int,2>,5> centers={{{56,48},{104,48},{80,84},{56,36},{104,36}}};
    for(const auto& point:centers) {
        const auto at=size_t(point[1])*g->protectionMask.strideFloats+point[0];
        Require(g->protectionMask.data[at]==1&&g->skinMask.data[at]==0,"independent eye/lip/brow center excluded");
    }
    Require(g->skinMask.data[64*160+64]>.9f,"cheek remains eligible for smoothing");
    Require(g->personMask.data[20*160+32]==1&&g->skinMask.data[20*160+32]==0,
        "first covered silhouette column is exactly excluded");
    Require(g->skinMask.data[20*160+33]==.5f&&g->skinMask.data[20*160+34]==1,
        "second covered column has the independent half-strength margin then full interior support");
    Require(g->faceLandmarks.detected&&!g->HasFaceLandmarks()&&g->faceLandmarks.overallConfidence==0,
        "native geometric support does not fabricate accessory confidence");
    for(size_t i=0;i<g->skinMask.data.size();++i)
        Require(std::isfinite(g->skinMask.data[i])&&g->skinMask.data[i]>=0&&g->skinMask.data[i]<=g->personMask.data[i],
            "all eligibility is bounded by actual surface");
    auto invalid=Observation();invalid.vertices[33].x=std::numeric_limits<float>::quiet_NaN();
    P::FaceGeometry output;P::FaceTopology topology;topology.triangles={{{200,201,202}},{{200,202,203}}};std::string error;
    Require(!P::BuildFaceGeometry(invalid,topology,160,120,output,error)&&!output.featureProtection.IsValid(),
        "malformed feature coordinates clear all output");
}
void CpuPixelsAndOwnership() {
    auto input=MakeFrame();auto geometry=Geometry();const auto original=Bytes(input.get());
    S::StudioStack stack;Configure(stack,Preset());S::StackExecutionReport report;
    auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,geometry);
    Require(report.appearanceStatus==S::StageStatus::Applied&&report.lightingStatus==S::StageStatus::Bypassed,"actual CPU appearance");
    ExactProtected(out,input.get(),*geometry);Metadata(out,input.get());Require(Bytes(input.get())==original,"caller input never modified");
    Frame retained(av_frame_clone(out));Require(bool(retained),"retained output lease");const auto saved=Bytes(retained.get());
    input->pts+=16667;auto next=std::make_shared<A::AnalysisBundle>(*geometry);next->sequenceId=2;next->timestampPts=input->pts;
    stack.ProcessFrame(input.get(),2,input->pts,&report,Gen,nullptr,next);
    Require(Bytes(retained.get())==saved,"retained fanout output survives later reuse");
    Require(report.imageUploadCount==0&&report.imageReadbackCount==0,"CPU route has no GPU transfers");
}
void ActualGpu(bool hardware) {
    Gpu::D3D11Context d3d;Require(d3d.Initialize(hardware)&&d3d.IsHardwareAdapter()==hardware,"actual hardware/WARP adapter");
    std::cout<<"ADAPTER "<<d3d.GetAdapterName()<<std::endl;
    auto input=MakeFrame();auto g=Geometry();auto preset=Preset();S::StackExecutionReport report,cpuReport;
    S::StudioStack cpu;Configure(cpu,preset);const auto expected=Bytes(cpu.ProcessFrame(input.get(),Seq,Pts,&cpuReport,Gen,nullptr,g));
    S::StudioStack gpu;Configure(gpu,preset);gpu.SetExecutionBackend(S::ExecutionBackend::GpuWithCpuFallback);
    Require(gpu.InitializeGpu(d3d.GetDevice(),d3d.GetContext()),"GPU stack resources initialized");
    auto* output=gpu.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,g);
    Require(report.appearanceStatus==S::StageStatus::Applied&&!report.gpuFallbackUsed&&report.imageUploadCount==1&&report.imageReadbackCount==1,
        "actual GPU protected appearance and completed readback");
    ExactProtected(output,input.get(),*g);Metadata(output,input.get());
    auto actual=Bytes(output);int maxDifference=0;
    for(size_t i=0;i<actual.size();++i)maxDifference=std::max(maxDifference,std::abs(int(actual[i])-expected[i]));
    std::cout<<"GPU_SAME_GEOMETRY_MAX_BYTE_ERROR "<<maxDifference<<std::endl;
    Require(maxDifference<=1,"same-geometry GPU matches CPU within one byte");
    for(auto fault:{S::GpuFailureTarget::AppearanceStage,S::GpuFailureTarget::Readback}) {
        gpu.SetGpuFailureTarget(fault);output=gpu.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,g);
        Require(report.gpuFallbackUsed&&report.appearanceStatus==S::StageStatus::Applied&&Bytes(output)==expected,"actual fault restarts exact current CPU appearance");
    }
    gpu.SetGpuFailureTarget(S::GpuFailureTarget::None);
}
std::shared_ptr<A::AnalysisBundle> BodySkin(const A::AnalysisBundle& face) {
    auto body=std::make_shared<A::AnalysisBundle>(face);
    std::fill(body->skinMask.data.begin(),body->skinMask.data.end(),1.f);
    std::fill(body->protectionMask.data.begin(),body->protectionMask.data.end(),0.f);
    for(int y=0;y<body->personMask.height;++y)for(int x=0;x<body->personMask.width;++x)
        body->personMask.data[size_t(y)*body->personMask.strideFloats+x]=(x>=15&&x<145)?1.f:0.f;
    return body;
}
void InvalidAndIndependent() {
    auto input=MakeFrame();const auto original=Bytes(input.get());auto g=Geometry();
    for(int variant=0;variant<9;++variant) {
        auto absent=std::make_shared<A::AnalysisBundle>(*g);
        if(variant==0)absent=std::make_shared<A::AnalysisBundle>();
        else if(variant==1)--absent->sourceGeneration;
        else if(variant==2)--absent->sequenceId;
        else if(variant==3)--absent->timestampPts;
        else if(variant==4)absent->protectionMask={};
        else if(variant==5)absent->skinMask.width--;
        else if(variant==6)absent->skinMask.data[64*160+64]=std::numeric_limits<float>::quiet_NaN();
        else if(variant==7)absent->personMask={};
        else absent->skinMask.data[0]=1;
        S::StudioStack stack;Configure(stack,Preset());auto mailbox=std::make_shared<A::AnalysisMailbox>();
        stack.AttachAnalysisMailbox(mailbox);Require(mailbox->PostAnalysis(BodySkin(*g)),"fresh generic mailbox skin deliberately available");
        S::StackExecutionReport report;auto* output=stack.ProcessFrame(input.get(),Seq,Pts,&report,Gen,nullptr,absent);
        Require(output&&report.appearanceStatus==S::StageStatus::NotReady&&Bytes(output)==original,
            "explicit missing/stale/malformed face cannot borrow generic skin");
    }
    // A lost exact face cannot suppress independently valid colour/background/LUT.
    auto full=Preset();full.inputLook=int(Look::Warm);
    full.backgroundMode=Presets::StudioBackgroundMode::Solid;full.background.solidB=11;full.background.solidG=22;full.background.solidR=33;
    std::string error;
    auto lut=CubeLut::Parse("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n",&error);Require(bool(lut),"identity LUT fixture");
    full.inputLutIntensity=.8f;
    S::StudioStack actual,expected;Configure(actual,full);auto neutral=full;neutral.appearance.bypass=true;Configure(expected,neutral);
    auto sharedLut=std::shared_ptr<const CubeLut>(std::move(lut));
    actual.SetInputLut(sharedLut);expected.SetInputLut(sharedLut);
    auto aBox=std::make_shared<A::AnalysisMailbox>(),eBox=std::make_shared<A::AnalysisMailbox>();actual.AttachAnalysisMailbox(aBox);expected.AttachAnalysisMailbox(eBox);
    Require(aBox->PostAnalysis(BodySkin(*g))&&eBox->PostAnalysis(BodySkin(*g)),"independent body analysis");
    S::StackExecutionReport ar,er;auto* oracle=expected.ProcessFrame(input.get(),Seq,Pts,&er,Gen);
    auto* output=actual.ProcessFrame(input.get(),Seq,Pts,&ar,Gen,nullptr,std::make_shared<A::AnalysisBundle>());
    Require(ar.appearanceStatus==S::StageStatus::NotReady&&ar.inputLookStatus==S::StageStatus::Applied&&
        ar.inputLutStatus==S::StageStatus::Applied&&ar.backgroundStatus==S::StageStatus::Applied&&Bytes(output)==Bytes(oracle),
        "look/LUT/body background continue unchanged when exact face is missing");
    Require(Pixel(output,1,60)[0]==11&&Pixel(output,1,60)[1]==22&&Pixel(output,1,60)[2]==33,"independent room replacement exact");
}
void MalformedDesktopInputs() {
    auto pixels=MakeFrame(64,48);
    DesktopStudioPipeline pipeline(false);DesktopColorSettings color;color.look=Look::Warm;pipeline.Configure(color);
    AVFrame raw{};raw.width=64;raw.height=48;raw.format=AV_PIX_FMT_BGR24;
    raw.pts=Pts;raw.time_base={1,1000000};raw.duration=16667;
    raw.colorspace=AVCOL_SPC_RGB;raw.color_range=AVCOL_RANGE_JPEG;raw.color_trc=AVCOL_TRC_BT709;
    raw.linesize[0]=pixels->linesize[0];
    Require(raw.extended_data==nullptr,"malformed raw fixtures exercise absent extended_data");
    auto rejected=[&](const std::string& reason) {
        Require(pipeline.Process(&raw,7)==&raw&&pipeline.GetLastReport().errorDescription=="Invalid software frame planes"&&
            pipeline.GetLastReport().imageUploadCount==0,reason);
    };
    rejected("raw null plane is rejected without dereferencing auxiliary plane array");
    raw.data[0]=pixels->data[0];raw.linesize[0]=1;rejected("raw short stride rejected before any read");
    raw.linesize[0]=pixels->linesize[0];
    AVBufferRef shortOwner=*pixels->buf[0];shortOwner.size=1;raw.buf[3]=&shortOwner;
    rejected("raw descriptor with short standard owner rejected");
    shortOwner=*pixels->buf[0];
    raw.data[0]=reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(shortOwner.data)+shortOwner.size+1);
    rejected("raw data outside owned capacity is rejected before reading");
    raw.data[0]=pixels->data[0];shortOwner.size=SIZE_MAX;
    rejected("reported owner extent overflow is rejected");
    shortOwner=*pixels->buf[0];shortOwner.data=reinterpret_cast<uint8_t*>(UINTPTR_MAX-15);shortOwner.size=32;
    rejected("reported owner end pointer overflow is rejected");
    raw.buf[3]=nullptr;raw.data[0]=reinterpret_cast<uint8_t*>(UINTPTR_MAX-4);
    rejected("borrowed input address-span overflow is rejected");
    // A genuine synchronous borrowed frame remains supported. It has actual
    // live pixel storage, no reference owners, and no auxiliary data array.
    raw.data[0]=pixels->data[0];auto* borrowed=pipeline.Process(&raw,7);
    Require(borrowed&&borrowed!=&raw&&pipeline.GetLastReport().inputLookStatus==S::StageStatus::Applied,
        "legitimate synchronous borrowed camera pixels retain working colour processing");
    Require(Bytes(borrowed)!=Bytes(pixels.get()),"borrowed input is actually processed rather than status-only success");
    shortOwner=*pixels->buf[0];raw.buf[3]=&shortOwner;
    auto* owned=pipeline.Process(&raw,7);
    Require(owned&&owned!=&raw&&pipeline.GetLastReport().inputLookStatus==S::StageStatus::Applied,
        "raw descriptor with valid owner in nonzero slot works despite null extended_data");
    const auto original=Bytes(pixels.get());
    raw.buf[3]=nullptr;raw.buf[0]=&shortOwner;
    owned=pipeline.Process(&raw,7);
    Require(owned&&owned!=&raw&&pipeline.GetLastReport().inputLookStatus==S::StageStatus::Applied,
        "raw descriptor with valid primary owner works despite null extended_data");
    Require(Bytes(owned)!=original&&Bytes(pixels.get())==original&&raw.extended_data==nullptr,
        "normalizing only the private video ref view preserves source descriptor and caller pixels");
}
void NeutralFastPath() {
    auto input=MakeFrame();DesktopStudioPipeline pipeline(false);
    DesktopStudioSettings s;s.touchupStrength=1;s.touchupDetail=1;pipeline.ConfigureStudio(s);
    Require(pipeline.Process(input.get(),7)==input.get()&&pipeline.GetFaceAnalysisState().phase==P::FaceAnalysisPhase::Disabled,"all-detail neutral preserves pointer without inference");
    s.touchupStrength=0;s.touchupDetail=.5f;pipeline.ConfigureStudio(s);
    Require(pipeline.Process(input.get(),7)==input.get()&&pipeline.GetFaceAnalysisState().submittedFrames==0,"zero touch-up preserves pointer without inference");
    s.touchupStrength=1;s.touchupDetail=.5f;pipeline.ConfigureStudio(s);DesktopColorSettings c;c.bypassed=true;pipeline.Configure(c);
    Require(pipeline.Process(input.get(),7)==input.get()&&pipeline.GetFaceAnalysisState().submittedFrames==0,"master bypass prevents native work");
    Require(pipeline.GetStudioSettings().touchupStrength==1&&pipeline.GetStudioSettings().touchupDetail==.5f,"bypass retains exact face settings");
    s.touchupStrength=std::numeric_limits<float>::quiet_NaN();s.touchupDetail=std::numeric_limits<float>::infinity();pipeline.ConfigureStudio(s);
    Require(pipeline.GetStudioSettings().touchupStrength==0&&pipeline.GetStudioSettings().touchupDetail==.75f,"nonfinite settings sanitized");
    s.touchupStrength=7;s.touchupDetail=-5;pipeline.ConfigureStudio(s);
    Require(pipeline.GetStudioSettings().touchupStrength==1&&pipeline.GetStudioSettings().touchupDetail==0,"finite settings clamped");
    MalformedDesktopInputs();
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
AVFrame* WaitApplied(DesktopStudioPipeline& pipeline,AVFrame* input,uint64_t generation,bool hardware,bool fallback=false) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    do {
        input->pts+=16667;auto* output=pipeline.Process(input,generation);
        const auto report=pipeline.GetLastReport();const auto state=pipeline.GetFaceAnalysisState();
        if(state.phase==P::FaceAnalysisPhase::Failed)throw std::runtime_error("native face service failed: "+state.lastError);
        if(report.appearanceStatus==S::StageStatus::Applied) {
            Require(output&&output!=input&&state.enabled&&state.lastDetected&&state.ready,"actual native touch-up applied");
            Require(report.sourceGeneration==generation&&state.sourceGeneration==generation&&state.lastCompletedSequence==report.frameSequence&&
                state.lastCompletedPts==input->pts&&report.timestampPts==input->pts,"exact current native face report tags");
            Require(report.gpuFallbackUsed==fallback&&report.imageUploadCount==(hardware?1:0),"truthful actual route/fallback");
            Metadata(output,input);return output;
        }
        Require(output==input&&report.appearanceStatus==S::StageStatus::NotReady,"loading/late original-pointer fallback");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<end);
    throw std::runtime_error("actual touch-up did not apply before fixture deadline");
}
void BoostPresetCompatibility() {
    auto preset=Preset();preset.appearance.touchupBoost=.73f;
    Presets::StudioPreset decoded;std::string error;
    auto json=Presets::PresetStore::Serialize(preset);
    Require(Presets::PresetStore::Deserialize(json,decoded,error)&&decoded.appearance.touchupBoost==.73f,
        "optional strong touch-up field survives actual preset roundtrip");
    const auto begin=json.find("    \"touchupBoost\":");Require(begin!=std::string::npos,"serialized boost field present");
    auto oldJson=json;oldJson.erase(begin,oldJson.find('\n',begin)-begin+1);
    Require(Presets::PresetStore::Deserialize(oldJson,decoded,error)&&decoded.appearance.touchupBoost==0,
        "older preset with no boost retains the legacy renderer exactly");
    preset.appearance.touchupBoost=8;
    Require(Presets::PresetStore::Deserialize(Presets::PresetStore::Serialize(preset),decoded,error)&&decoded.appearance.touchupBoost==1,
        "deserialized boost is bounded to one");
    preset.appearance.touchupBoost=-8;
    Require(Presets::PresetStore::Deserialize(Presets::PresetStore::Serialize(preset),decoded,error)&&decoded.appearance.touchupBoost==0,
        "deserialized negative boost is bounded to zero");
    preset.appearance.touchupBoost=std::numeric_limits<float>::infinity();
    Require(!preset.Validate(error)&&error.find("appearance.touchupBoost")!=std::string::npos,
        "non-finite boost is rejected by actual preset validation");
}
void StrongNativeProduction(const wchar_t* fixture) {
    auto input=Decode(fixture);const auto original=Bytes(input.get());
    DesktopStudioSettings settings;settings.touchupStrength=.5f;settings.touchupDetail=0;
    DesktopStudioPipeline cpu(false);cpu.ConfigureStudio(settings);
    const auto subtle=Bytes(WaitApplied(cpu,input.get(),30,false));
    settings.touchupStrength=1;cpu.ConfigureStudio(settings);
    const auto strong=Bytes(WaitApplied(cpu,input.get(),30,false));
    uint64_t subtleDelta=0,strongDelta=0;
    for(size_t i=0;i<original.size();++i) {
        subtleDelta+=std::abs(int(original[i])-int(subtle[i]));
        strongDelta+=std::abs(int(original[i])-int(strong[i]));
    }
    Require(subtleDelta>100&&strongDelta>subtleDelta*1.5,
        "actual desktop/native upper slider removes substantially more facial texture than the subtle half");
    std::cout<<"NATIVE_STRENGTH byte delta: subtle="<<subtleDelta<<" strong="<<strongDelta<<std::endl;
    Require(Bytes(input.get())==original,"strong native route leaves input unchanged");
    settings.touchupDetail=1;cpu.ConfigureStudio(settings);
    Require(cpu.Process(input.get(),30)==input.get(),"full detail remains identity with maximum slider boost");
}
void NativeProduction(const wchar_t* fixture) {
    auto input=Decode(fixture);const auto original=Bytes(input.get());
    DesktopStudioSettings settings;settings.touchupStrength=1;settings.touchupDetail=.5f;
    DesktopStudioPipeline cpu(false),gpu(true);cpu.ConfigureStudio(settings);gpu.ConfigureStudio(settings);
    Frame retainedCpu(av_frame_clone(WaitApplied(cpu,input.get(),20,false)));Require(bool(retainedCpu),"retain current actual touch-up output");
    const auto saved=Bytes(retainedCpu.get());auto* output=WaitApplied(gpu,input.get(),20,true);
    auto bytes=Bytes(output);size_t changed=0;for(size_t i=0;i<bytes.size();++i)if(bytes[i]!=original[i])++changed;
    Require(changed>50&&changed<bytes.size()/5,"real native appearance affects a restrained face region");
    for(int y=0;y<32;++y)Require(std::memcmp(output->data[0]+size_t(y)*output->linesize[0],input->data[0]+size_t(y)*input->linesize[0],96)==0,
        "known room corner is byte-exact in real native GPU output");
    Require(gpu.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"face touch-up alone never invokes body model");
    Require(Bytes(input.get())==original,"actual native touch-up never mutates source pixels");
    gpu.InjectGpuReadbackFailureForTesting(true);WaitApplied(gpu,input.get(),21,true,true);gpu.InjectGpuReadbackFailureForTesting(false);
    Require(Bytes(retainedCpu.get())==saved,"retained native processed frame survives later source/fallback");
    // Both effects share one submitted analysis job and one exact result.
    settings.light=DesktopLightMode::Custom;settings.strength=.5f;settings.faceLighting=true;
    gpu.ConfigureStudio(settings);WaitApplied(gpu,input.get(),22,true);
    const auto before=gpu.GetFaceAnalysisState().submittedFrames;input->pts+=16667;
    gpu.Process(input.get(),22);const auto after=gpu.GetFaceAnalysisState();const auto report=gpu.GetLastReport();
    Require(after.submittedFrames==before+1,"touch-up and facial lighting submit only one native inference per frame");
    if(report.appearanceStatus==S::StageStatus::Applied)
        Require(report.lightingStatus==S::StageStatus::Applied&&after.lastCompletedSequence==report.frameSequence,"both stages use same exact completed bundle");
    // Different source and resolution carry no retained masks.
    auto blank=MakeFrame(96,64,true);blank->pts=2000000;bool noFace=false;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    do {
        blank->pts+=16667;output=gpu.Process(blank.get(),23);const auto r=gpu.GetLastReport();const auto state=gpu.GetFaceAnalysisState();
        Require(output==blank.get()&&r.appearanceStatus==S::StageStatus::NotReady&&r.lightingStatus==S::StageStatus::NotReady&&
            r.imageUploadCount==0&&r.normalPreparationCount==0,"no face after source/resolution reset preserves current frame without transfers");
        if(state.ready&&state.lastCompletedPts==blank->pts&&!state.lastDetected){noFace=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<end);
    Require(noFace,"actual no-face inference completed on changed source and resolution");
    settings.light=DesktopLightMode::Off;gpu.ConfigureStudio(settings);WaitApplied(gpu,input.get(),24,true);
    DesktopColorSettings bypass;bypass.bypassed=true;gpu.Configure(bypass);const auto submitted=gpu.GetFaceAnalysisState().submittedFrames;
    Require(gpu.Process(input.get(),24)==input.get()&&gpu.GetFaceAnalysisState().submittedFrames==submitted&&
        gpu.GetFaceAnalysisState().phase==P::FaceAnalysisPhase::Disabled,"bypass stops shared face worker and keeps source pointer");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2){std::cerr<<"Usage: face_touchup_tests fixture.png\n";return 2;}
    const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComGuard{bool owns;~ComGuard(){if(owns)CoUninitialize();}} guard{SUCCEEDED(hr)};
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return 2;
    try {
#define GROUP(label,body) ++groups;std::cout<<"GROUP " label<<std::endl;{body;}std::cout<<"PASS " label<<std::endl;
        GROUP("geometric feature masks / no fabricated confidence",GeometryProtection());
        GROUP("CPU protected pixels / independent texture metric / metadata / retained ownership",CpuPixelsAndOwnership());
        GROUP("hardware GPU protected pixels / actual failure restart / CPU agreement",ActualGpu(true));
        // Internal WARP repetition is excluded from distinct case-group total.
        ActualGpu(false);std::cout<<"PASS WARP repetition of GPU route"<<std::endl;
        GROUP("exact stale / malformed / missing face / independent body and colour stages",InvalidAndIndependent());
        GROUP("neutral / zero / bypass / finite setting guards",NeutralFastPath());
        GROUP("actual native CPU and GPU / shared inference / source and resolution / no face",NativeProduction(argv[1]));
        GROUP("strong touch-up preset compatibility / actual native upper slider",BoostPresetCompatibility();StrongNativeProduction(argv[1]));
#undef GROUP
        std::cout<<"FACE_TOUCHUP_PASS groups="<<groups<<" assertions="<<checks<<"; offline native fixture only; no live FPS/quality claim\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FACE_TOUCHUP_FAIL groups="<<groups<<" assertions="<<checks<<" reason="<<e.what()<<std::endl;return 1;}
}
