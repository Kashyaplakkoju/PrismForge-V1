#include "effects/desktopstudiopipeline.h"
#include "effects/analysisbridge/mask_preparer.h"
#include "video/guipreviewscaler.h"
#include "video/directshowscaler.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace PrismForge::Effects;
namespace A=PrismForge::Effects::AnalysisBridge;
namespace S=PrismForge::Effects::StudioStack;
namespace P=PrismForge::Providers;
static int groups=0, assertions=0;
static void Require(bool condition,const char* description) {
    ++assertions;
    if(!condition) throw std::runtime_error(description);
}
struct Frame {
    AVFrame* f=av_frame_alloc();
    Frame(int w=160,int h=96) {
        Require(f!=nullptr,"frame allocation"); f->width=w;f->height=h;f->format=AV_PIX_FMT_BGR24;
        Require(av_frame_get_buffer(f,64)>=0,"owned frame planes");
        f->pts=100000;f->time_base={1,1000000};f->duration=16666;
        f->color_range=AVCOL_RANGE_JPEG;f->colorspace=AVCOL_SPC_RGB;
        f->color_primaries=AVCOL_PRI_BT709;f->color_trc=AVCOL_TRC_BT709;
        Fill(61,102,155);
    }
    ~Frame(){av_frame_free(&f);}
    void Fill(int b,int g,int r) {
        Require(av_frame_make_writable(f)>=0,"writable input frame");
        for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x) {
            auto* p=f->data[0]+y*f->linesize[0]+x*3;
            p[0]=static_cast<uint8_t>(b);p[1]=static_cast<uint8_t>(g);p[2]=static_cast<uint8_t>(r);
        }
    }
    void Pattern(int seed) {
        Require(av_frame_make_writable(f)>=0,"writable changing source");
        for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x) {
            auto* p=f->data[0]+y*f->linesize[0]+x*3;
            p[0]=static_cast<uint8_t>((x*255/std::max(1,f->width-1)+seed*31)%256);
            p[1]=static_cast<uint8_t>((y*255/std::max(1,f->height-1)+seed*59)%256);
            p[2]=static_cast<uint8_t>((x+y+seed*83)%256);
        }
    }
};
static std::array<int,3> Pixel(const AVFrame* f,int x,int y) {
    Require(f&&f->format==AV_PIX_FMT_BGR24,"pixel oracle receives BGR24");
    const auto* p=f->data[0]+y*f->linesize[0]+x*3;return {p[0],p[1],p[2]};
}
static std::vector<uint8_t> Bytes(const AVFrame* f) {
    Require(f&&f->format==AV_PIX_FMT_BGR24,"owned output bytes");std::vector<uint8_t> bytes;
    for(int y=0;y<f->height;++y)bytes.insert(bytes.end(),f->data[0]+y*f->linesize[0],f->data[0]+y*f->linesize[0]+f->width*3);
    return bytes;
}
static int Difference(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b) {
    Require(a.size()==b.size(),"equal comparison geometry");int maxDiff=0;
    for(size_t i=0;i<a.size();++i)maxDiff=std::max(maxDiff,std::abs(int(a[i])-int(b[i])));
    return maxDiff;
}
static auto Bundle(uint64_t generation,uint64_t sequence,int64_t pts,bool central=true) {
    auto bundle=std::make_shared<A::AnalysisBundle>();
    bundle->sourceGeneration=generation;bundle->sequenceId=sequence;bundle->timestampPts=pts;
    auto& mask=bundle->personMask;mask.width=20;mask.height=12;mask.strideFloats=23;
    mask.data.assign(size_t(mask.strideFloats)*mask.height,0.f);
    if(central)for(int y=3;y<=8;++y)for(int x=6;x<=13;++x)mask.data[y*mask.strideFloats+x]=1.f;
    return bundle;
}
static DesktopStudioSettings Solid() {
    DesktopStudioSettings settings;settings.background=DesktopBackgroundMode::Replace;
    settings.backgroundB=17;settings.backgroundG=29;settings.backgroundR=43;return settings;
}
static auto Prepare(DesktopStudioPipeline& pipeline,Frame& frame,uint64_t generation=7) {
    auto mailbox=std::make_shared<A::AnalysisMailbox>();pipeline.AttachAnalysisMailboxForTesting(mailbox);
    pipeline.ConfigureStudio(Solid());
    Require(Bytes(pipeline.Process(frame.f,generation))==Bytes(frame.f),"initial background pending passthrough");
    Require(pipeline.GetLastReport().backgroundStatus==S::StageStatus::NotReady,"initial explicit background NotReady");
    Require(pipeline.GetLastReport().imageUploadCount==0&&pipeline.GetLastReport().imageReadbackCount==0,"pending background performs no camera GPU transfers");
    return mailbox;
}
static void Applied(const DesktopStudioPipeline& pipeline,bool gpu) {
    auto r=pipeline.GetLastReport();Require(r.backgroundStatus==S::StageStatus::Applied,"background stage applied");
    Require(r.analysisStatus==S::StageStatus::Applied,"actual mask analysis aligned");
    if(gpu)Require(r.imageUploadCount==1&&r.imageReadbackCount==1&&!r.gpuFallbackUsed,"GPU uses one camera upload/readback without fallback");
    else Require(r.imageUploadCount==0&&r.imageReadbackCount==0,"CPU has no GPU camera transfers");
}
static void PendingAndAlignment() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);
        auto original=Bytes(frame.f);auto invalid=std::make_shared<A::AnalysisBundle>();invalid->sourceGeneration=7;invalid->sequenceId=1;invalid->timestampPts=frame.f->pts;
        Require(mailbox->PostAnalysis(invalid),"empty real bundle accepted by mailbox");
        Require(Bytes(pipeline.Process(frame.f,7))==original,"missing person mask never invents segmentation");
        Require(pipeline.GetLastReport().backgroundStatus==S::StageStatus::NotReady,"invalid person mask NotReady");
        mailbox->ResetSource(7);Require(mailbox->PostAnalysis(Bundle(7,50,frame.f->pts+500000)),"future oracle retained");
        Require(Bytes(pipeline.Process(frame.f,7))==original,"future mask leaves current source untouched");
        Require(pipeline.GetLastReport().backgroundStatus==S::StageStatus::NotReady,"future mask NotReady");
        mailbox->ResetSource(7);Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts-200000)),"old mask posted");
        Require(Bytes(pipeline.Process(frame.f,7))==original,"expired mask cannot affect source");
        Require(pipeline.GetLastReport().backgroundStatus==S::StageStatus::NotReady,"expired mask NotReady");
    }
}
static void LowResolutionSolid() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);
        Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"padded low resolution mask posted");
        auto* output=pipeline.Process(frame.f,7);Applied(pipeline,gpu);
        Require(Pixel(output,80,48)==Pixel(frame.f,80,48),"normalized subject center preserves exact source pixels");
        Require(Pixel(output,8,8)==std::array<int,3>{17,29,43},"background corner replaced with selected BGR color");
        Require(Pixel(output,151,87)==std::array<int,3>{17,29,43},"opposite corner mapped across entire source");
        Require(Bytes(frame.f)!=Bytes(output),"input is distinct from composed output");
        Require(Pixel(frame.f,8,8)==std::array<int,3>{61,102,155},"compositor never mutates camera source");
    }
}
static void ImageAssetLifetime() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);Frame image(42,27);image.Fill(193,71,33);
        auto owner=std::shared_ptr<const AVFrame>(av_frame_clone(image.f),[](const AVFrame* p){auto* raw=const_cast<AVFrame*>(p);av_frame_free(&raw);});
        Require(owner!=nullptr,"retained selected image");auto settings=Solid();settings.backgroundImage=owner;pipeline.ConfigureStudio(settings);
        Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"image mask posted");auto* output=pipeline.Process(frame.f,7);Applied(pipeline,gpu);
        Require(Pixel(output,8,8)==std::array<int,3>{193,71,33},"selected image replaces background");
        Require(Pixel(output,80,48)==Pixel(frame.f,80,48),"selected image preserves subject");
        auto retained=av_frame_clone(output);Require(retained!=nullptr,"retained image output");auto saved=Bytes(retained);
        image.Fill(2,3,4);owner.reset();settings.backgroundImage.reset();pipeline.ConfigureStudio(settings);frame.f->pts+=16666;
        output=pipeline.Process(frame.f,7);Require(Pixel(output,8,8)==std::array<int,3>{17,29,43},"clearing image restores selected solid color");
        Require(Bytes(retained)==saved,"retained image output survives asset clear");av_frame_free(&retained);
    }
}
static std::array<int,3> BoxOracle(const AVFrame* frame,int x,int y,int radius) {
    std::array<int,3> value{};const int count=radius*2+1;
    for(int c=0;c<3;++c) {int vertical=0;for(int dy=-radius;dy<=radius;++dy) {
        int horizontal=0;int py=std::clamp(y+dy,0,frame->height-1);
        for(int dx=-radius;dx<=radius;++dx) {int px=std::clamp(x+dx,0,frame->width-1);horizontal+=frame->data[0][py*frame->linesize[0]+px*3+c];}
        vertical+=(horizontal+radius)/count;
    }value[c]=(vertical+radius)/count;}return value;
}
static void BackgroundBlur() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);
        for(int y=0;y<frame.f->height;++y)for(int x=0;x<frame.f->width;++x) {
            auto* pixel=frame.f->data[0]+y*frame.f->linesize[0]+x*3;int checker=(x+y)%2?240:10;
            if(x>=48&&x<=112&&y>=24&&y<=72){pixel[0]=61;pixel[1]=102;pixel[2]=155;}else pixel[0]=pixel[1]=pixel[2]=static_cast<uint8_t>(checker);
        }
        auto settings=Solid();settings.background=DesktopBackgroundMode::Blur;settings.blurStrength=25;pipeline.ConfigureStudio(settings);
        Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"blur mask posted");auto* output=pipeline.Process(frame.f,7);Applied(pipeline,gpu);
        Require(Pixel(output,80,48)==Pixel(frame.f,80,48),"background blur leaves subject center sharp");
        auto reference=BoxOracle(frame.f,10,10,1+settings.blurStrength*31/100);auto got=Pixel(output,10,10);
        for(int c=0;c<3;++c)Require(std::abs(got[c]-reference[c])<=1,"blur matches independent separable-box pixel oracle");
        Require(std::abs(Pixel(output,10,10)[0]-Pixel(output,11,10)[0])<10,"background high-frequency checker suppressed");
        Require(std::abs(Pixel(frame.f,10,10)[0]-Pixel(frame.f,11,10)[0])==230,"original sharp checker still intact");
    }
}
static void MasterBypassRestore() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);
        Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"master bypass oracle mask");auto first=Bytes(pipeline.Process(frame.f,7));
        DesktopColorSettings color;color.bypassed=true;pipeline.Configure(color);frame.f->pts+=16666;
        Require(pipeline.Process(frame.f,7)==frame.f,"master bypass returns untouched camera pointer");
        Require(pipeline.GetLastReport().imageUploadCount==0,"bypass does not upload camera");
        auto settings=pipeline.GetStudioSettings();Require(settings.background==DesktopBackgroundMode::Replace&&settings.backgroundB==17&&settings.highQualityMask,"bypass retains selected effect configuration");
        color.bypassed=false;pipeline.Configure(color);frame.f->pts+=16666;
        Require(Bytes(pipeline.Process(frame.f,7))==first,"unbypass restores identical processed output");Applied(pipeline,gpu);
        settings.background=DesktopBackgroundMode::Off;pipeline.ConfigureStudio(settings);Require(pipeline.Process(frame.f,7)==frame.f,"background off uses unchanged source");
    }
}
static void SourceAndResolutionFlush() {
    for(bool gpu:{false,true}) {
        DesktopStudioPipeline pipeline(gpu);Frame frame;auto mailbox=Prepare(pipeline,frame);
        Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"old source mask");Require(pipeline.Process(frame.f,7)!=frame.f,"old source composition available");
        frame.Fill(120,140,160);frame.f->pts=300000;
        Require(Bytes(pipeline.Process(frame.f,8))==Bytes(frame.f),"new source cannot reuse old mask");auto report=pipeline.GetLastReport();
        Require(report.sourceGeneration==8&&report.frameSequence==1&&report.backgroundStatus==S::StageStatus::NotReady,"generation resets sequence and analysis");
        Require(!mailbox->PostAnalysis(Bundle(7,20,frame.f->pts)),"prior source result rejected");
        Require(mailbox->PostAnalysis(Bundle(8,1,frame.f->pts)),"new source result accepted");Require(pipeline.Process(frame.f,8)!=frame.f,"new source compositions resume");
        Frame portrait(96,160);portrait.Fill(31,52,73);portrait.f->pts=316666;
        Require(Bytes(pipeline.Process(portrait.f,8))==Bytes(portrait.f),"same-generation resize clears old geometry masks");
        Require(pipeline.GetLastReport().backgroundStatus==S::StageStatus::NotReady,"resize pending truthful");
        Require(mailbox->PostAnalysis(Bundle(8,pipeline.GetLastReport().frameSequence,portrait.f->pts)),"new geometry mask posted");
        auto* output=pipeline.Process(portrait.f,8);Applied(pipeline,gpu);
        Require(output->width==96&&output->height==160,"portrait dimensions preserved");Require(Pixel(output,48,80)==Pixel(portrait.f,48,80),"portrait normalized subject mapped correctly");
    }
}
static void FailureRecoveryAndRetention() {
    DesktopStudioPipeline cpu(false),gpu(true);Frame frame;auto cpuBox=Prepare(cpu,frame),gpuBox=Prepare(gpu,frame);
    Require(cpuBox->PostAnalysis(Bundle(7,1,frame.f->pts))&&gpuBox->PostAnalysis(Bundle(7,1,frame.f->pts)),"matching CPU and GPU masks");
    auto expected=Bytes(cpu.Process(frame.f,7));auto* output=gpu.Process(frame.f,7);Applied(gpu,true);Require(Difference(Bytes(output),expected)<=2,"normal GPU background parity");
    auto retained=av_frame_clone(output);Require(retained!=nullptr,"retained processed frame");auto saved=Bytes(retained);
    frame.Fill(93,124,187);frame.f->pts+=16666;expected=Bytes(cpu.Process(frame.f,7));gpu.InjectGpuReadbackFailureForTesting(true);output=gpu.Process(frame.f,7);
    Require(gpu.GetLastReport().gpuFallbackUsed&&gpu.GetLastReport().backgroundStatus==S::StageStatus::Applied,"actual readback failure takes CPU fallback");
    Require(Bytes(output)==expected&&Pixel(output,80,48)==std::array<int,3>{93,124,187},"fallback uses current frame, not a cached old result");
    Require(Bytes(retained)==saved,"retained fanout survives current-frame fallback");gpu.InjectGpuReadbackFailureForTesting(false);frame.f->pts+=16666;
    output=gpu.Process(frame.f,7);Applied(gpu,true);Require(Difference(Bytes(output),expected)<=2,"GPU recovered after fault removed");
    Frame other(320,180);other.Fill(10,20,30);gpu.Process(other.f,9);Require(Bytes(retained)==saved,"owned output survives source and resolution change");av_frame_free(&retained);
}
static void ConsumerFanout() {
    DesktopStudioPipeline pipeline(true);Frame frame;auto mailbox=Prepare(pipeline,frame);Require(mailbox->PostAnalysis(Bundle(7,1,frame.f->pts)),"consumer mask posted");
    auto* output=pipeline.Process(frame.f,7);Applied(pipeline,true);auto expected=Bytes(output);int w=0,h=0;
    Video::GuiPreviewScaler preview(frame.f->width,frame.f->height);Video::DirectShowScaler camera(frame.f->width,frame.f->height);
    auto* previewPixels=preview.Process(output,w,h);Require(previewPixels&&w==frame.f->width&&h==frame.f->height,"preview processed geometry");
    Require(std::memcmp(previewPixels,expected.data(),expected.size())==0,"preview uses same clean composed pixels");
    auto* cameraPixels=camera.Process(output,w,h);Require(cameraPixels&&std::memcmp(cameraPixels,expected.data(),expected.size())==0,"virtual output uses same clean composed pixels");
    auto* snapshot=av_frame_clone(output);Require(snapshot&&Bytes(snapshot)==expected,"snapshot retains same clean composed pixels");av_frame_free(&snapshot);
}
static bool WaitReady(P::SegmentationService& service) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<deadline) {auto state=service.GetState();if(state.phase==P::SegmentationPhase::Ready)return true;
        if(state.phase==P::SegmentationPhase::Failed){std::cerr<<state.message<<'\n';return false;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}return false;
}
static auto Exact(P::SegmentationService& service,uint64_t generation,uint64_t sequence,int64_t pts) {
    std::shared_ptr<const A::AnalysisBundle> result;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<deadline){A::AnalysisAlignmentStatus status;auto current=service.GetMailbox()->GetAlignedAnalysis(generation,sequence,pts,status);
        if(current&&current->sequenceId==sequence&&current->timestampPts==pts){result=current;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    Require(result!=nullptr,"real pinned model completed exact source sequence and microsecond PTS");return result;
}
static std::vector<uint8_t> RealMaskOracle(const Frame& frame,const A::MaskBuffer& mask,const DesktopStudioSettings& settings) {
    A::MaskPreparer preparer;A::MaskBuffer prepared;A::MaskPreparationConfig config;config.featherRadius=3;config.enableTemporalStabilization=true;
    Require(preparer.PrepareMask(mask,frame.f->width,frame.f->height,config,prepared),"real-model mask resized and feathered");
    std::vector<uint8_t> expected(size_t(frame.f->width)*frame.f->height*3);const int color[3]={settings.backgroundB,settings.backgroundG,settings.backgroundR};
    for(int y=0;y<frame.f->height;++y)for(int x=0;x<frame.f->width;++x)for(int c=0;c<3;++c){float alpha=prepared.data[y*prepared.strideFloats+x];
        const int source=frame.f->data[0][y*frame.f->linesize[0]+x*3+c];expected[(size_t(y)*frame.f->width+x)*3+c]=static_cast<uint8_t>(std::clamp(std::round(source*alpha+color[c]*(1-alpha)),0.f,255.f));}
    return expected;
}
static void RealServiceWorkflow(const std::wstring& model,const std::wstring& runtime,P::ExecutionProviderType provider) {
    P::SegmentationOptions options;options.modelDirectory=model;options.runtimeDllPath=runtime;options.requestedProvider=provider;
    P::SegmentationService service(options);service.Configure(true);Require(WaitReady(service),"real person model ready for workflow");
    auto state=service.GetState();Require(state.activeProvider==provider&&!state.fallbackOccurred,"real requested inference backend active");
    DesktopStudioPipeline pipeline(provider==P::ExecutionProviderType::DirectML);pipeline.AttachAnalysisMailboxForTesting(service.GetMailbox());pipeline.ConfigureStudio(Solid());
    std::vector<float> previousMask;std::vector<uint8_t> previousOutput;
    for(int seed:{0,3}) {Frame frame(320,180);frame.Pattern(seed);uint64_t generation=100+seed;
        Require(Bytes(pipeline.Process(frame.f,generation))==Bytes(frame.f),"real workflow initially waits for source analysis");service.ResetSource(generation);
        std::shared_ptr<const A::AnalysisBundle> appliedBundle;std::vector<uint8_t> bytes;
        int64_t capturedPts=100000;uint64_t sequence=1;
        for(int attempt=0;attempt<3;++attempt) {
            frame.f->pts=capturedPts;
            Require(service.Submit(frame.f,generation,sequence,capturedPts),"real small-frame request accepted");auto bundle=Exact(service,generation,sequence,capturedPts);
            Require(bundle->HasPersonMask()&&!bundle->HasSkinMask()&&!bundle->HasNormals(),"actual real inference is person-only");
            Require(bundle->personMask.width==256&&bundle->personMask.height==144,"actual person mask remains bounded and normalized");
            auto expected=RealMaskOracle(frame,bundle->personMask,Solid());frame.f->pts=capturedPts+16666;
            auto* output=pipeline.Process(frame.f,generation);auto report=pipeline.GetLastReport();
            if(report.backgroundStatus==S::StageStatus::NotReady) {
                const auto nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const bool wallExpired=bundle->timestampCompleted>0&&nowMs>bundle->timestampCompleted&&nowMs-bundle->timestampCompleted>150;
                const bool ptsExpired=frame.f->pts>bundle->timestampPts&&frame.f->pts-bundle->timestampPts>150000;
                Require(provider==P::ExecutionProviderType::DirectML&&seed==0&&attempt==0,"only initial cold GPU startup may discard its mask");
                Require(wallExpired||ptsExpired,"NotReady requires actual immutable result freshness expiry");
                Require(Bytes(output)==Bytes(frame.f),"expired startup result leaves current source unchanged");
                std::cout<<"COLD_GPU_EXPIRED ageMs="<<nowMs-bundle->timestampCompleted<<" sequence="<<sequence<<'\n';
                // Refresh through actual inference on the next sequence; never change
                // completion timestamps or bypass the production freshness bound.
                ++sequence;capturedPts+=40000;continue;
            }
            Applied(pipeline,provider==P::ExecutionProviderType::DirectML);bytes=Bytes(output);
            Require(Difference(bytes,expected)<=(provider==P::ExecutionProviderType::DirectML?2:0),"actual inference mask reaches wrapper and clean background composition");
            Require(bytes!=Bytes(frame.f),"real-model composition changes appropriate source pixels");appliedBundle=std::move(bundle);break;
        }
        Require(appliedBundle!=nullptr,"real background applied within three bounded inference attempts");
        if(!previousMask.empty()) {double changed=0;for(size_t i=0;i<previousMask.size();++i)changed+=std::abs(previousMask[i]-appliedBundle->personMask.data[i]);Require(changed>0.01,"actual model responds to changing input");Require(bytes!=previousOutput,"real processed output changes with source");}
        previousMask=appliedBundle->personMask.data;previousOutput=std::move(bytes);
    }
    service.Stop();Require(service.GetState().phase==P::SegmentationPhase::Stopped,"real service releases inference worker");
    std::cout<<"REAL_PROVIDER "<<(provider==P::ExecutionProviderType::CPU?"CPU":"DirectML")<<" completed="<<service.GetState().completedFrames<<'\n';
}
int wmain(int argc,wchar_t** argv) {
    try {Require(argc==3,"pinned model directory and runtime DLL required");
#define RUN(name,call) ++groups;std::cout<<"GROUP " name <<std::endl;call;std::cout<<"PASS " name <<std::endl;
        RUN("pending missing future expired analysis leaves source untouched",PendingAndAlignment());
        RUN("low-resolution padded normalized solid masks CPU/GPU",LowResolutionSolid());
        RUN("selected image clear and retained asset/output lifetime CPU/GPU",ImageAssetLifetime());
        RUN("background blur independent box oracle and subject protection CPU/GPU",BackgroundBlur());
        RUN("master bypass preserves and restores background selections CPU/GPU",MasterBypassRestore());
        RUN("source generation and same-source resolution mask flush CPU/GPU",SourceAndResolutionFlush());
        RUN("real GPU readback failure current-frame CPU fallback and recovery",FailureRecoveryAndRetention());
        RUN("common clean preview virtual-output snapshot fanout",ConsumerFanout());
        RUN("real pinned PP-HumanSeg CPU service to desktop composition",RealServiceWorkflow(argv[1],argv[2],P::ExecutionProviderType::CPU));
        RUN("real pinned PP-HumanSeg DirectML service to desktop composition",RealServiceWorkflow(argv[1],argv[2],P::ExecutionProviderType::DirectML));
        std::cout<<"SUMMARY "<<groups<<"/10 PASS assertions="<<assertions<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL background workflow: "<<error.what()<<" groups="<<groups<<" assertions="<<assertions<<'\n';return 1;}
}
