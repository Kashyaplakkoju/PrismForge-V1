#include "providers/segmentation_service.h"
#include "providers/frame_request.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace PrismForge::Providers;
using namespace PrismForge::Effects::AnalysisBridge;
int checks=0, groups=0;
void Require(bool condition,const char* reason) { ++checks; if(!condition) throw std::runtime_error(reason); }
void Group(const char* name) { ++groups; std::cout << "GROUP " << name << '\n'; }
using OwnedFrame = std::unique_ptr<AVFrame,void(*)(AVFrame*)>;
OwnedFrame Frame(int w,int h,int seed) {
    AVFrame* raw=av_frame_alloc(); Require(raw!=nullptr,"frame allocation");
    OwnedFrame frame(raw,[](AVFrame* p){av_frame_free(&p);});
    frame->format=AV_PIX_FMT_RGB24; frame->width=w; frame->height=h;
    frame->color_range=AVCOL_RANGE_JPEG; frame->colorspace=AVCOL_SPC_RGB;
    Require(av_frame_get_buffer(frame.get(),32)>=0,"owned planes");
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        uint8_t* p=frame->data[0]+y*frame->linesize[0]+x*3;
        p[0]=static_cast<uint8_t>((x*255/std::max(1,w-1)+seed*31)%256);
        p[1]=static_cast<uint8_t>((y*255/std::max(1,h-1)+seed*59)%256);
        p[2]=static_cast<uint8_t>((x+y+seed*83)%256);
    }
    return frame;
}
bool WaitReady(SegmentationService& service) {
    auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<limit) {
        auto state=service.GetState();
        if(state.phase==SegmentationPhase::Ready) return true;
        if(state.phase==SegmentationPhase::Failed) { std::cerr << state.message << '\n'; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
void CurrentCompleted(SegmentationService& service, uint64_t expected) {
    auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(service.GetState().currentCompletedFrames<expected && std::chrono::steady_clock::now()<limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(service.GetState().currentCompletedFrames==expected,"current-source completion count");
    Require(service.GetState().consecutiveInferenceErrors==0,"successful inference clears consecutive errors");
}
auto Exact(SegmentationService& service,uint64_t gen,uint64_t seq,int64_t pts) {
    std::shared_ptr<const AnalysisBundle> result;
    auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<limit) {
        AnalysisAlignmentStatus status;
        auto candidate=service.GetMailbox()->GetAlignedAnalysis(gen,seq,pts,status);
        if(candidate && candidate->sequenceId==seq && candidate->timestampPts==pts) { result=candidate; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(result!=nullptr,"real exact frame completion"); return result;
}
void ScaledFrame() {
    Group("bounded1080p RGB resize preserves independent source tags and color range");
    auto frame=Frame(1920,1080,0); auto request=FrameRequest::CreateFromAVFrameScaled(17,23,91000,frame.get(),256);
    Require(request && request->IsValid(),"scaled request valid");
    Require(request->width==256 && request->height==144,"aspect preserved and bounded");
    Require(request->sourceGeneration==17 && request->sequenceId==23 && request->timestampPts==91000,"exact tags");
    Require(request->pixelData->size()==256*144*3 && request->pixelData->capacity()<=256*144*3,"no fullframe retained");
    Require(request->colorRange==ColorRange::Full && request->colorMatrix==ColorMatrix::Identity,"explicit RGB conversion");
    frame->color_range=AVCOL_RANGE_MPEG;
    std::memset(frame->data[0],16,frame->linesize[0]*frame->height);
    request=FrameRequest::CreateFromAVFrameScaled(17,24,92000,frame.get(),192);
    Require(request && request->pixelData->front()==0,"limited RGB black expands");
    Require(!FrameRequest::CreateFromAVFrameScaled(17,25,93000,frame.get(),-1),"invalid dimension rejected");
    auto tiny=Frame(1,1,0); request=FrameRequest::CreateFromAVFrameScaled(17,26,94000,tiny.get(),256);
    Require(request && request->width==1 && request->height==1,"no enlargement");
}
void RunService(const std::wstring& model,const std::wstring& runtime,ExecutionProviderType provider) {
    Group(provider==ExecutionProviderType::CPU?"realPPHumanSeg CPU smallmask, dynamic input and reset":"realPPHumanSeg DirectML smallmask, dynamic input and reset");
    SegmentationOptions options;options.modelDirectory=model;options.runtimeDllPath=runtime;options.requestedProvider=provider;
    SegmentationService service(options);service.ResetSource(7);
    auto first=Frame(1920,1080,0);Require(!service.Submit(first.get(),7,1,100000),"disabled submit rejects");
    const auto start=std::chrono::steady_clock::now();service.Configure(true,true);
    Require(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100),"Configure does not load synchronously");
    Require(WaitReady(service),"model ready");
    auto state=service.GetState();
    Require(state.activeProvider==provider,"actual requested provider selected on this machine");
    Require(!state.fallbackOccurred,"no unreported CPU fallback");
    Require(service.Submit(first.get(),7,1,100000),"first accepted");auto a=Exact(service,7,1,100000);
    Require(a->personMask.width==256 && a->personMask.height==144,"sourceaspect small mask");
    Require(a->personMask.data.size()==256*144,"bounded mask storage");
    Require(!a->HasSkinMask() && !a->HasFaceLandmarks() && !a->HasNormals(),"truthful person-only capability");
    auto second=Frame(1920,1080,3);
    Require(!service.Submit(second.get(),7,2,116666),"capture60hz does not queue segmentation60hz");
    Require(service.Submit(second.get(),7,3,166667),"changing input accepted");auto b=Exact(service,7,3,166667);
    double difference=0;for(size_t i=0;i<a->personMask.data.size();++i) {
        float p=b->personMask.data[i];Require(std::isfinite(p)&&p>=0&&p<=1,"finite softperson probability");
        difference+=std::abs(p-a->personMask.data[i]);
    }
    Require(difference>0.01,"real model reacts to changing input");
    auto pstart=std::chrono::steady_clock::now();service.ResetSource(8);
    Require(std::chrono::steady_clock::now()-pstart<std::chrono::milliseconds(100),"source reset prompt");
    AnalysisAlignmentStatus status;
    Require(!service.GetMailbox()->GetAlignedAnalysis(7,3,166667,status) && status==AnalysisAlignmentStatus::SourceGenerationMismatch,"oldsource erased");
    Require(!service.Submit(second.get(),7,4,233334),"oldsource rejects");
    auto portrait=Frame(1080,1920,5);Require(service.Submit(portrait.get(),8,1,1000000),"portrait newsource");
    auto c=Exact(service,8,1,1000000);Require(c->personMask.width==144 && c->personMask.height==256,"portrait aspect mask");
    auto square=Frame(400,400,6);Require(service.Submit(square.get(),8,2,1066667),"samegen source geometry reset");
    auto d=Exact(service,8,2,1066667);Require(d->personMask.width==256&&d->personMask.height==256,"oldgeometry cleared");
    service.Configure(false);Require(!service.Submit(square.get(),8,3,1133334),"disabled immediately stops intake");
    service.Configure(true,false);Require(WaitReady(service),"reenable afterdisable");
    Require(service.Submit(square.get(),8,4,1200000),"balanced accepts");
    auto e=Exact(service,8,4,1200000);Require(e->personMask.width==192&&e->personMask.height==192,"balanced bounded192");
    CurrentCompleted(service,1);
    service.ResetSource(8);Require(service.GetState().currentCompletedFrames==0,"same-generation reset clears current readiness");
    Require(service.Submit(square.get(),8,1,2000000),"same-generation epoch can restart sequence");
    auto f=Exact(service,8,1,2000000);CurrentCompleted(service,1);
    Require(service.Submit(square.get(),8,2,1000),"newer sequence accepts backward PTS after timeline reset");
    auto g=Exact(service,8,2,1000);CurrentCompleted(service,1);
    Require(g->timestampPts==1000,"rollback result preserves actual microsecond timeline");
    Require(!service.Submit(square.get(),8,3,1000),"duplicate PTS rejected without reset");
    Require(!service.Submit(square.get(),8,2,-1000),"old sequence cannot reset current timeline");
    Require(service.Submit(square.get(),8,4,67666),"postrollback throttle resumes");
    auto h=Exact(service,8,4,67666);CurrentCompleted(service,2);
    // Configure temporarily clears Ready. Source reset during that transition must
    // reset the live worker too, rather than only its mailbox.
    auto small=Frame(64,48,4);
    for(uint64_t turn=0;turn<12;++turn) {
        service.Configure(true,(turn%2)==0);service.ResetSource(20+turn);
        Require(service.GetState().currentCompletedFrames==0,"transition source clears readiness");
        Require(WaitReady(service),"quality transition ready");
        Require(service.Submit(small.get(),20+turn,1,100000),"transition source reaches worker");
        auto fresh=Exact(service,20+turn,1,100000);CurrentCompleted(service,1);
        Require(fresh->sourceGeneration==20+turn,"no old generation after configuration reset");
    }
    service.Configure(false);Require(service.GetState().phase==SegmentationPhase::Disabled,"disabled status updates synchronously");
    service.Stop();Require(service.GetState().phase==SegmentationPhase::Stopped,"stopped state");
    Require(!service.Submit(square.get(),8,5,1266667),"stopped submit rejects");
    std::cout<<"actualProvider="<<(provider==ExecutionProviderType::CPU?"CPU":"DirectML")<<" changedProbabilityL1="<<difference<<" completed="<<service.GetState().completedFrames<<'\n';
}
template<class Predicate>
void Until(Predicate predicate,const char* reason,int milliseconds=20000) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(predicate(),reason);
}
void ErrorFrame(SegmentationService& service,const AVFrame* frame,uint64_t generation,uint64_t& sequence,int64_t& pts) {
    const auto errors=service.GetState().inferenceErrors;
    ++sequence;pts+=100000;
    const auto start=std::chrono::steady_clock::now();
    Require(service.Submit(frame,generation,sequence,pts),"error-injection frame accepted");
    Require(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100),"Submit never waits for error recovery/inference");
    Until([&]{return service.GetState().inferenceErrors==errors+1;},"worker reports an actual inference error");
}
void EmptyCurrent(SegmentationService& service,uint64_t generation,uint64_t sequence,int64_t pts) {
    AnalysisAlignmentStatus status;
    Require(!service.GetMailbox()->GetAlignedAnalysis(generation,sequence,pts,status),"recovery clears old mailbox mask");
    Require(service.GetState().currentCompletedFrames==0,"recovery clears current readiness count");
}
void Recovery(const std::wstring& model,const std::wstring& runtime,ExecutionProviderType requested) {
    Group(requested==ExecutionProviderType::CPU?"real CPU inference transient, bounded recovery, terminal error and explicit retry":"real DirectML recovery selects actual CPU after repeated inference failure");
    SegmentationOptions options;options.modelDirectory=model;options.runtimeDllPath=runtime;options.requestedProvider=requested;
    SegmentationService service(options);service.ResetSource(100);service.Configure(true);
    Require(WaitReady(service),"recovery test initial real session ready");
    Require(service.GetState().activeProvider==requested,"initial recovery session uses requested native provider");
    auto frame=Frame(400,400,2);uint64_t generation=100,sequence=1;int64_t pts=100000;
    Require(service.Submit(frame.get(),generation,sequence,pts),"real frame before injected failure");
    auto before=Exact(service,generation,sequence,pts);CurrentCompleted(service,1);
    service.SetInferenceErrorInjectionForTesting(true);ErrorFrame(service,frame.get(),generation,sequence,pts);
    Require(service.GetState().phase==SegmentationPhase::Ready&&service.GetState().recoveryAttempts==0,"single error does not reload a healthy provider");
    service.SetInferenceErrorInjectionForTesting(false);++sequence;pts+=100000;
    Require(service.Submit(frame.get(),generation,sequence,pts),"native inference restored after transient error");
    auto transient=Exact(service,generation,sequence,pts);CurrentCompleted(service,2);
    Require(transient->personMask.data==before->personMask.data,"successful post-error output comes from the same real model");
    service.SetInferenceErrorInjectionForTesting(true);
    ErrorFrame(service,frame.get(),generation,sequence,pts);ErrorFrame(service,frame.get(),generation,sequence,pts);
    // A quality change clears stale masks but must retain actual error history/streak.
    const auto sourceErrors=service.GetState().currentInferenceErrors;
    service.Configure(true,false);Require(WaitReady(service),"quality revision while error streak retained");
    Require(service.GetState().currentInferenceErrors==sourceErrors&&service.GetState().consecutiveInferenceErrors==2,"quality change cannot erase repeated inference errors");
    ErrorFrame(service,frame.get(),generation,sequence,pts);
    Until([&]{auto s=service.GetState();return s.phase==SegmentationPhase::Recovering&&s.recoveryAttempts==1;},"three errors trigger first recovery");
    EmptyCurrent(service,generation,sequence,pts);
    Require(!service.Submit(frame.get(),generation,sequence+1,pts+100000),"recovery intake is paused promptly");
    const auto completedBeforeRecovery=service.GetState().completedFrames;
    service.SetInferenceErrorInjectionForTesting(false);
    // Recovery delay is interrupted by source and quality revisions; only their latest
    // values can commit, and source resets cannot clear the attempted-recovery budget.
    service.ResetSource(101);service.Configure(true,true);service.ResetSource(102);service.Configure(true,false);
    generation=102;sequence=0;pts=1000000;
    Require(WaitReady(service),"latest source/quality resumes after interrupted recovery");
    Require(service.GetState().recoveryAttempts==1&&service.GetState().completedFrames==completedBeforeRecovery,"reload is not counted as successful inference or a fresh retry budget");
    Require(!service.Submit(frame.get(),100,99,9900000),"old source cannot reach recovered worker");
    ++sequence;pts+=100000;Require(service.Submit(frame.get(),generation,sequence,pts),"real latest-source post-recovery frame");
    auto first=Exact(service,generation,sequence,pts);CurrentCompleted(service,1);
    Require(first->sourceGeneration==102&&first->personMask.width==192,"latest balanced quality and source delivered");
    for(uint32_t attempt=2;attempt<=3;++attempt) {
        service.SetInferenceErrorInjectionForTesting(true);
        for(int i=0;i<3;++i)ErrorFrame(service,frame.get(),generation,sequence,pts);
        Until([&]{auto s=service.GetState();return s.phase==SegmentationPhase::Recovering&&s.recoveryAttempts==attempt;},"bounded retry attempt is observable");
        EmptyCurrent(service,generation,sequence,pts);
        service.SetInferenceErrorInjectionForTesting(false);
        Require(WaitReady(service),"actual recovered model session ready");
        auto state=service.GetState();
        Require(state.recoveryAttempts==attempt&&state.maxRecoveryAttempts==3,"successful model load preserves retry accounting");
        Require(state.activeProvider==ExecutionProviderType::CPU,"persistent DirectML inference errors load actual CPU session");
        Require(state.fallbackOccurred==(requested==ExecutionProviderType::DirectML),"explicit recovery CPU fallback is reported honestly");
        ++sequence;pts+=100000;Require(service.Submit(frame.get(),generation,sequence,pts),"native frame after repeated provider errors");
        auto actual=Exact(service,generation,sequence,pts);CurrentCompleted(service,1);
        Require(actual->personMask.width==192&&actual->personMask.data.size()==192*192,"real CPU fallback mask retains bounded current-source geometry");
        double difference=0;for(size_t i=0;i<first->personMask.data.size();++i)difference+=std::abs(first->personMask.data[i]-actual->personMask.data[i]);
        Require(difference<1.0,"recovered native session reproduces actual model probabilities");
    }
    service.SetInferenceErrorInjectionForTesting(true);
    for(int i=0;i<3;++i)ErrorFrame(service,frame.get(),generation,sequence,pts);
    Until([&]{return service.GetState().phase==SegmentationPhase::Failed;},"persistent CPU inference errors exhaust recovery cap");
    Require(service.GetState().recoveryAttempts==3,"no fourth automatic retry");EmptyCurrent(service,generation,sequence,pts);
    Require(!service.Submit(frame.get(),generation,sequence+1,pts+100000),"terminal failure stops intake");
    for(uint64_t turn=0;turn<6;++turn){service.Configure(true,(turn%2)==0);service.ResetSource(200+turn);}
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    Require(service.GetState().phase==SegmentationPhase::Failed&&service.GetState().recoveryAttempts==3,"quality/source churn cannot replenish exhausted retries");
    service.SetInferenceErrorInjectionForTesting(false);service.Configure(false);service.Configure(true);
    Require(WaitReady(service),"immediate explicit off/on retries even when controller events coalesce");
    Require(service.GetState().recoveryAttempts==0&&service.GetState().activeProvider==requested,"explicit new enable session resets retries and requests original provider");
    Require(service.Submit(frame.get(),205,1,100000),"real inference after explicit retry");auto resumed=Exact(service,205,1,100000);CurrentCompleted(service,1);
    Require(resumed->HasPersonMask(),"explicit retry uses real inference rather than synthetic success");
    std::cout<<"recovery requested="<<(requested==ExecutionProviderType::CPU?"CPU":"DirectML")<<" nativeCompleted="<<service.GetState().completedFrames<<" actualErrors="<<service.GetState().inferenceErrors<<" cap=3 finalProvider="<<(service.GetState().activeProvider==ExecutionProviderType::CPU?"CPU":"DirectML")<<'\n';
    service.Stop();
}
void RecoveryCancellation(const std::wstring& model,const std::wstring& runtime) {
    Group("recovery backoff and active inference cancel without delayed disabled/source delivery");
    SegmentationOptions options;options.modelDirectory=model;options.runtimeDllPath=runtime;options.requestedProvider=ExecutionProviderType::CPU;
    SegmentationService service(options);service.ResetSource(300);service.Configure(true);
    Require(WaitReady(service),"cancellation native CPU ready");auto frame=Frame(320,240,7);uint64_t sequence=1;int64_t pts=100000;
    Require(service.Submit(frame.get(),300,sequence,pts),"native inference before cancellation checks");auto before=Exact(service,300,sequence,pts);CurrentCompleted(service,1);
    service.SetInferenceErrorInjectionForTesting(true);for(int i=0;i<3;++i)ErrorFrame(service,frame.get(),300,sequence,pts);
    Until([&]{return service.GetState().phase==SegmentationPhase::Recovering;},"cancellable backoff entered");
    const auto complete=service.GetState().completedFrames;auto start=std::chrono::steady_clock::now();service.Configure(false);
    Require(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100),"disable during retry is prompt");
    Until([&]{return service.GetState().phase==SegmentationPhase::Disabled&&service.GetState().recoveryAttempts==0;},"controller cancels retry budget after disable");
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    Require(service.GetState().phase==SegmentationPhase::Disabled&&service.GetState().completedFrames==complete,"disabled service never restarts delayed inference");
    EmptyCurrent(service,300,sequence,pts);Require(!service.Submit(frame.get(),300,99,9900000),"disabled recovery intake rejects");
    service.SetInferenceErrorInjectionForTesting(false);service.Configure(true);Require(WaitReady(service),"reenable after cancelled backoff");
    Require(service.Submit(frame.get(),300,1,1000000),"real inference after backoff cancellation");auto after=Exact(service,300,1,1000000);CurrentCompleted(service,1);
    Require(before->personMask.data==after->personMask.data,"cancellation followed by actual native model result");
    service.SetInferenceErrorInjectionForTesting(true,2000);auto injected=service.GetInjectedInferenceStartsForTesting();
    Require(service.Submit(frame.get(),300,2,1100000),"cancellable in-flight error request accepted");
    Until([&]{return service.GetInjectedInferenceStartsForTesting()>injected;},"in-flight cancellable error started");
    auto errors=service.GetState().inferenceErrors;start=std::chrono::steady_clock::now();service.ResetSource(301);
    Require(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100),"source reset cancels active inference promptly");
    service.SetInferenceErrorInjectionForTesting(false);Require(service.Submit(frame.get(),301,1,2000000),"new source accepted during old cancellation");
    auto current=Exact(service,301,1,2000000);CurrentCompleted(service,1);
    Require(current->sourceGeneration==301&&service.GetState().inferenceErrors==errors,"cancelled obsolete work cannot publish a mask or count as inference failure");
    service.SetInferenceErrorInjectionForTesting(true,2000);injected=service.GetInjectedInferenceStartsForTesting();
    Require(service.Submit(frame.get(),301,2,2100000),"second cancellable active request accepted");
    Until([&]{return service.GetInjectedInferenceStartsForTesting()>injected;},"second in-flight error started");
    start=std::chrono::steady_clock::now();service.Stop();
    Require(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(250),"Stop interrupts active cancellation instead of waiting two-second delay");
    Require(service.GetState().phase==SegmentationPhase::Stopped,"cancelled service reaches Stopped");
    service.Configure(true);Require(!service.Submit(frame.get(),301,3,2200000),"stopped controller never restarts");
    EmptyCurrent(service,301,2,2100000);
}
void Missing(const std::wstring& runtime) {
    Group("missing model is explicit and capture intake never blocks");
    SegmentationOptions options;options.runtimeDllPath=runtime;options.modelDirectory=(std::filesystem::path(runtime).parent_path()/L"absent-model").wstring();
    SegmentationService service(options);service.Configure(true);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(service.GetState().phase!=SegmentationPhase::Failed &&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Require(service.GetState().phase==SegmentationPhase::Failed,"explicit missing-model status");
    auto frame=Frame(64,32,1);Require(!service.Submit(frame.get(),0,1,100000),"missing provider never substitutes synthetic mask");
    service.Stop();
}
int wmain(int argc,wchar_t**argv) {
    try { Require(argc==3,"model dir and runtime arguments");ScaledFrame();RunService(argv[1],argv[2],ExecutionProviderType::CPU);RunService(argv[1],argv[2],ExecutionProviderType::DirectML);Recovery(argv[1],argv[2],ExecutionProviderType::CPU);Recovery(argv[1],argv[2],ExecutionProviderType::DirectML);RecoveryCancellation(argv[1],argv[2]);Missing(argv[2]);
        std::cout<<"PASS segmentation groups="<<groups<<" assertions="<<checks<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL segmentation: "<<error.what()<<" groups="<<groups<<" assertions="<<checks<<'\n';return 1;}
}
