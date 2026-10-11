#include "providers/analysis_worker.h"
#include "tiny_onnx_fixture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
using namespace PrismForge::Providers;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Tests;
using Clock=std::chrono::steady_clock;
static void Require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
static double Us(Clock::time_point begin,Clock::time_point end) { return std::chrono::duration<double,std::micro>(end-begin).count(); }
static void Stats(const char* name,std::vector<double> samples) {
    Require(samples.size()>=60,"Insufficient completed measured samples"); std::sort(samples.begin(),samples.end());
    std::cout<<"  "<<name<<" completed="<<samples.size()<<" p50_us="<<samples[samples.size()/2]
        <<" p95_us="<<samples[(samples.size()*95)/100]<<" mean_us="<<std::accumulate(samples.begin(),samples.end(),0.0)/samples.size()<<'\n';
}
static std::shared_ptr<const FrameRequest> Frame(int seq,int w,int h,float& expected) {
    const uint8_t r=static_cast<uint8_t>(20+seq%90),g=static_cast<uint8_t>(80+seq%70),b=static_cast<uint8_t>(150-seq%90);
    expected=TinyOnnxFixture::ComputeExpectedLuma(r/255.0f,g/255.0f,b/255.0f);
    std::vector<uint8_t> pixels(static_cast<size_t>(w)*h*3);
    for (size_t i=0;i<pixels.size();i+=3) { pixels[i]=r; pixels[i+1]=g; pixels[i+2]=b; }
    return FrameRequest::CreateFromPixels(1,seq,seq*33333LL,w,h,w*3,PixelFormat::RGB24,std::move(pixels));
}
static std::vector<uint8_t> Graph(int side) {
    auto bytes=TinyOnnxFixture::GetModelByteVector();
    if (side==16) return bytes;
    Require(side==64,"Unsupported original fixture side"); int count=0;
    // Four protobuf spatial dimensions; 64 still has a one-byte varint, leaving message lengths unchanged.
    for (size_t i=0;i+3<bytes.size();++i) if (bytes[i]==0x0a && bytes[i+1]==0x02 && bytes[i+2]==0x08 && bytes[i+3]==0x10) { bytes[i+3]=64; ++count; }
    Require(count==4,"Fixture dimensions were not exactly patched"); return bytes;
}
static void Suite(ExecutionProviderType requested) {
    for (int side : {16,64}) {
        OnnxRunner runner; auto graph=Graph(side);
        Require(runner.LoadModelFromMemory(graph.data(),graph.size(),requested)==RunnerStatusCode::Success,"Benchmark load failed");
        const auto info=runner.GetSessionInfo();
        Require(info.inputShape==std::vector<int64_t>({1,3,side,side}) && info.outputShape==std::vector<int64_t>({1,1,side,side}),"Larger graph actual dimensions mismatch");
        std::cout<<"PROVIDER requested="<<(requested==ExecutionProviderType::CPU?"CPU":"DirectML")<<" actual="
            <<(info.activeProvider==ExecutionProviderType::CPU?"CPU":"DirectML")<<" device="<<info.deviceId
            <<" fallback="<<info.fallbackOccurred<<" reason="<<info.fallbackReason<<" fixture="<<side<<'\n';
        auto manifest=TinyOnnxFixture::CreateManifest(); manifest.inputDims={1,3,side,side}; manifest.outputDims={1,1,side,side}; manifest.aspectMode=AspectMode::Stretch;
        for (const auto size : {std::pair<int,int>{640,480},{1280,720},{1920,1080}}) {
            FramePreprocessor prep; ResultAdapter adapter; std::vector<float> in,out;
            std::vector<double> times[4]; float previous=-1.0f;
            for (int i=1;i<=70;++i) {
                float expected; auto frame=Frame(i,size.first,size.second,expected); Require(frame!=nullptr,"Benchmark source allocation failed");
                GeometryTransform transform; std::shared_ptr<AnalysisBundle> result;
                const auto a=Clock::now(); Require(prep.Preprocess(*frame,side,side,manifest.inputLayout,manifest.normConfig,manifest.aspectMode,in,transform),"Benchmark preprocessing failed");
                const auto b=Clock::now(); Require(runner.RunInference(in,out)==RunnerStatusCode::Success,"Benchmark actual inference failed");
                const auto c=Clock::now(); Require(adapter.AdaptResult(out,manifest,transform,*frame,result),"Benchmark adaptation failed");
                const auto d=Clock::now();
                Require(out.size()==static_cast<size_t>(side)*side && result && result->HasPersonMask(),"Benchmark output missing");
                Require(std::abs(out[0]-expected)<1e-4f && std::abs(out.back()-expected)<1e-4f,"Completed inference disagrees with independent luma oracle");
                Require(std::abs(result->personMask.data[(size.second/2)*size.first+size.first/2]-expected)<1e-4f,"Projected source result disagrees with oracle");
                Require(i==1 || out[0]!=previous,"Benchmark input/output did not change"); previous=out[0];
                if (i>10) { times[0].push_back(Us(a,b)); times[1].push_back(Us(b,c)); times[2].push_back(Us(c,d)); times[3].push_back(Us(a,d)); }
            }
            std::cout<<"SCENARIO source="<<size.first<<'x'<<size.second<<" fixture="<<side<<" warmup=10 measured=60\n";
            Stats("preprocess",times[0]); Stats("completed_inference",times[1]); Stats("postprocess_source_projection",times[2]); Stats("total_completed_stages",times[3]);
        }
    }
    // Measure Submit separately from inference; every accepted frame is proven completed before the next.
    AnalysisWorker worker; worker.ResetSource(1); auto manifest=TinyOnnxFixture::CreateManifest();
    Require(worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(),TinyOnnxFixture::kModelByteSize,manifest,requested)==RunnerStatusCode::Success,"Submit benchmark model failed");
    std::mutex mutex; std::condition_variable cv; uint64_t completed=0;
    worker.SetResultCallback([&](std::shared_ptr<const AnalysisBundle> result) { std::lock_guard<std::mutex> lock(mutex); completed=result->sequenceId; cv.notify_all(); });
    Require(worker.Start(),"Submit benchmark worker failed"); std::vector<double> submits;
    for (int i=1;i<=70;++i) {
        float ignored; auto frame=Frame(i,640,480,ignored); const auto a=Clock::now(); Require(worker.SubmitFrame(frame),"Submit benchmark rejected current frame"); const auto b=Clock::now();
        std::unique_lock<std::mutex> lock(mutex); Require(cv.wait_for(lock,std::chrono::seconds(10),[&]{return completed==static_cast<uint64_t>(i);}),"Submit benchmark actual completion timed out");
        if (i>10) submits.push_back(Us(a,b));
    }
    worker.Stop(); Require(worker.GetMetrics().totalCompleted==70 && worker.GetMetrics().totalInferenceErrors==0,"Submit benchmark completion counters mismatch"); Stats("bounded_submit_640x480",submits);
}
int main() {
    try {
        std::cout<<std::fixed<<std::setprecision(3)<<"Original grayscale graphs only; fixture overhead, not real detector speed or live FPS.\n"
            <<"Changing owned RGB source allocation excluded. Preprocess includes resize/normalize; postprocess includes source-resolution mask allocation/mapping.\n"
            <<"Actual synchronous CPU/DML completion; no renderer, camera, virtual output or transport. Nonisolated machine/background activity.\n";
        Suite(ExecutionProviderType::CPU); Suite(ExecutionProviderType::DirectML); return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL benchmark: "<<error.what()<<'\n'; return 1; }
}
