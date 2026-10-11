#include "providers/analysis_worker.h"
#include "tiny_onnx_fixture.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
using namespace PrismForge::Providers;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Tests;

static std::shared_ptr<const FrameRequest> Frame(uint64_t generation,uint64_t sequence,int64_t pts,int w,int h) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w)*h*3);
    for (size_t i=0;i<pixels.size();++i) pixels[i]=static_cast<uint8_t>((i+sequence*17)%256);
    return FrameRequest::CreateFromPixels(generation,sequence,pts,w,h,w*3,PixelFormat::RGB24,std::move(pixels));
}
static void Require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        int frames=30,w=640,h=480; bool verbose=false;
        std::wstring model,manifestPath;
        ExecutionProviderType provider=ExecutionProviderType::CPU;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--verbose") { verbose=true; continue; }
            if (arg=="--help") { std::cout << "--model PATH --manifest PATH --provider cpu|dml --frames N --width W --height H --verbose\n"; return 0; }
            Require(i+1<argc,"Missing option value"); const std::string value=argv[++i];
            if (arg=="--frames") frames=std::stoi(value);
            else if (arg=="--width") w=std::stoi(value);
            else if (arg=="--height") h=std::stoi(value);
            else if (arg=="--model") model=std::filesystem::u8path(value).wstring();
            else if (arg=="--manifest") manifestPath=std::filesystem::u8path(value).wstring();
            else if (arg=="--provider") {
                Require(value=="cpu" || value=="dml","Unknown provider");
                provider=value=="dml" ? ExecutionProviderType::DirectML : ExecutionProviderType::CPU;
            } else throw std::runtime_error("Unknown argument");
        }
        Require(frames>=4 && frames<=1000 && w>=1 && h>=1 && w<=1920 && h<=1920,"Out of range harness dimensions/count");
        Require(model.empty()==manifestPath.empty(),"Custom model and explicit manifest must be provided together");
        auto manifest=TinyOnnxFixture::CreateManifest();
        if (!manifestPath.empty()) Require(ModelManifest::LoadFromFile(manifestPath,manifest),"Invalid local manifest");
        auto mailbox=std::make_shared<AnalysisMailbox>(); AnalysisWorker worker(mailbox);
        std::mutex mutex; std::condition_variable cv; uint64_t lastGeneration=0,lastSequence=0;
        unsigned callbacks=0;
        worker.SetResultCallback([&](std::shared_ptr<const AnalysisBundle> result) {
            std::lock_guard<std::mutex> lock(mutex);
            lastGeneration=result->sourceGeneration; lastSequence=result->sequenceId; ++callbacks; cv.notify_all();
        });
        worker.ResetSource(1);
        const auto loaded=model.empty() ? worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(),TinyOnnxFixture::kModelByteSize,manifest,provider)
            : worker.LoadModel(model,manifest,provider);
        Require(loaded==RunnerStatusCode::Success,"Model/session contract failed");
        Require(worker.Start(),"Worker start failed");
        uint64_t generation=1; unsigned rejected=0,delivered=0;
        for (int n=1;n<=frames;++n) {
            const uint64_t sequence=static_cast<uint64_t>(n); const int64_t pts=n*33333LL;
            if (n==frames/2+1) {
                generation=2; worker.ResetSource(generation);
                Require(!worker.SubmitFrame(Frame(1,sequence,pts,w,h)),"Old source accepted after reset"); ++rejected;
                AnalysisAlignmentStatus status;
                Require(!mailbox->GetAlignedAnalysis(1,sequence,pts,status) && status==AnalysisAlignmentStatus::SourceGenerationMismatch,
                    "Mailbox retained old source boundary");
            }
            Require(worker.SubmitFrame(Frame(generation,sequence,pts,w,h)),"Current frame rejected");
            {
                std::unique_lock<std::mutex> lock(mutex);
                Require(cv.wait_for(lock,std::chrono::seconds(10),[&]{return lastGeneration==generation && lastSequence==sequence;}),"Exact current-frame inference timed out");
            }
            AnalysisAlignmentStatus status;
            auto aligned=mailbox->GetAlignedAnalysis(generation,sequence,pts,status);
            Require(aligned && status==AnalysisAlignmentStatus::Aligned,"Expected aligned delivery missing");
            Require(aligned->sourceGeneration==generation && aligned->sequenceId==sequence && aligned->timestampPts==pts,"Delivered provenance mismatch");
            Require(aligned->HasPersonMask() && aligned->personMask.width==w && aligned->personMask.height==h,"Source-sized test mask missing");
            ++delivered;
            // Independently test backwards sequence and backwards PTS; neither mutates acceptance state.
            Require(!worker.SubmitFrame(Frame(generation,sequence-1,pts+1,w,h)),"Backwards sequence accepted"); ++rejected;
            Require(!worker.SubmitFrame(Frame(generation,sequence+1,pts-1,w,h)),"New sequence/backwards PTS accepted"); ++rejected;
            if (verbose) std::cout << "Delivered generation="<<generation<<" sequence="<<sequence<<" pts="<<pts<<'\n';
        }
        worker.Stop(); const auto metrics=worker.GetMetrics(); const auto mail=mailbox->GetMetrics();
        Require(delivered==static_cast<unsigned>(frames) && callbacks==delivered,"Exact delivery/callback count mismatch");
        Require(metrics.totalSubmitted==delivered && metrics.totalCompleted==delivered && metrics.totalInferenceErrors==0 && metrics.totalReplacedPending==0,"Worker exact counters mismatch");
        Require(metrics.totalDroppedOutOfOrder==2*delivered && metrics.totalDroppedObsoleteGeneration==1,"Independent reject counts mismatch");
        Require(mail.totalAlignedDelivered==delivered && worker.GetCurrentSourceGeneration()==2,"Mailbox exact transition mismatch");
        Require(!worker.SubmitFrame(Frame(2,frames+1,(frames+1)*33333LL,w,h)),"Stopped submit accepted");
        std::cout << "PASS exact harness deliveries="<<delivered<<" callbacks="<<callbacks<<" rejected="<<rejected
            <<" sourceTransitions=1 requestedProvider="<<(provider==ExecutionProviderType::CPU?"CPU":"DirectML")
            <<" model="<<(model.empty()?"original tiny test graph; not a detector":"explicit local model")<<'\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL harness: "<<error.what()<<'\n'; return 3; }
}
