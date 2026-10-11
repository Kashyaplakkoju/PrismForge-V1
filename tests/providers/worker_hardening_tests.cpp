#include "providers/analysis_worker.h"
#include "effects/analysisbridge/analysis_mailbox.h"
#include "tiny_onnx_fixture.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace PrismForge::Providers;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Tests;
using namespace std::chrono_literals;
namespace {
int assertions = 0, failures = 0, groups = 0;
void Check(bool condition, const char* text, int line) {
    ++assertions;
    if (!condition) { ++failures; std::cerr << "[WORKER FAIL] line=" << line << " " << text << '\n'; }
}
#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __LINE__)

template<class Predicate> bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::mutex mutex; std::condition_variable cv;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return predicate();
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, 1ms); // Wait for an asserted state, never infer success from elapsed time.
    }
    return true;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false, timedOut = false;
    void Enter() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        if (!cv.wait_for(lock, 3000ms, [&] { return released; })) timedOut = true;
    }
    bool WaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 3000ms, [&] { return entered; });
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct Results {
    std::mutex mutex; std::condition_variable cv;
    std::vector<std::shared_ptr<const AnalysisBundle>> values;
    void Add(std::shared_ptr<const AnalysisBundle> value) {
        std::lock_guard<std::mutex> lock(mutex); values.push_back(std::move(value)); cv.notify_all();
    }
    bool Wait(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 3000ms, [&] { return values.size() >= count; });
    }
    std::vector<std::shared_ptr<const AnalysisBundle>> Copy() {
        std::lock_guard<std::mutex> lock(mutex); return values;
    }
};
std::shared_ptr<const FrameRequest> Frame(uint64_t generation, uint64_t sequence, int64_t pts,
    int width = 8, int height = 6, uint8_t red = 90) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < pixels.size(); i += 3) {
        pixels[i] = red; pixels[i+1] = 70; pixels[i+2] = 40;
    }
    return FrameRequest::CreateFromPixels(generation, sequence, pts, width, height, width*3,
        PixelFormat::RGB24, std::move(pixels));
}
AnalysisWorker::MockInferenceFn ProbabilityMock() {
    return [](const std::vector<float>&, std::vector<float>& out, const std::atomic<bool>* cancelled) {
        if (cancelled && cancelled->load()) return RunnerStatusCode::Cancelled;
        out.assign(16*16, 0.6f); return RunnerStatusCode::Success;
    };
}
void Group(const char* name, const std::function<void()>& run) {
    const int previous = failures; ++groups;
    try { run(); }
    catch (const std::exception& ex) { ++failures; std::cerr << "[WORKER EXCEPTION] " << name << ' ' << ex.what() << '\n'; }
    catch (...) { ++failures; std::cerr << "[WORKER EXCEPTION] " << name << " unknown\n"; }
    std::cout << "[WORKER GROUP] " << name << ' ' << (previous == failures ? "PASS" : "FAIL") << '\n';
}

void StartFailureAndIdempotence() {
    AnalysisWorker worker;
    worker.SetMockInferenceFunction(ProbabilityMock());
    worker.SetThreadStartFailureForTesting(true);
    CHECK(!worker.Start()); CHECK(!worker.IsRunning()); CHECK(!worker.SubmitFrame(Frame(0,1,1)));
    worker.SetThreadStartFailureForTesting(false);
    CHECK(worker.Start()); CHECK(worker.Start());
    worker.Stop(); worker.Stop();
    CHECK(!worker.IsRunning()); CHECK(!worker.SubmitFrame(Frame(0,1,1)));
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,0,-5)));
    CHECK(WaitUntil([&] { return worker.GetMetrics().totalCompleted == 1; }));
    worker.Stop(); CHECK(worker.GetResourceUsage().peakWorkerThreads == 1);
}
void BoundedPendingAndIndependentOrder() {
    Gate gate; std::atomic<unsigned> entered{0}; Results results;
    AnalysisWorker worker;
    worker.SetMockInferenceFunction([&](const std::vector<float>&, std::vector<float>& out, const std::atomic<bool>*) {
        if (entered.fetch_add(1) == 0) gate.Enter();
        out.assign(256,0.6f); return RunnerStatusCode::Success;
    });
    worker.SetResultCallback([&](auto result) { results.Add(std::move(result)); });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,0,-10)));
    CHECK(gate.WaitEntered());
    CHECK(!worker.SubmitFrame(Frame(0,1,-11))); // Higher sequence alone cannot admit backwards PTS.
    CHECK(!worker.SubmitFrame(Frame(0,0,1))); // Higher PTS alone cannot admit repeated sequence.
    auto replaced = Frame(0,1,0,20,10); std::weak_ptr<const FrameRequest> oldPending = replaced;
    CHECK(worker.SubmitFrame(replaced)); replaced.reset();
    CHECK(worker.SubmitFrame(Frame(0,2,1,10,20)));
    CHECK(oldPending.expired());
    auto usage = worker.GetResourceUsage();
    CHECK(usage.inFlightFrames == 1 && usage.pendingFrames == 1);
    CHECK(usage.inFlightSourceBytes == 8*6*3 && usage.pendingSourceBytes == 10*20*3);
    gate.Release(); CHECK(results.Wait(2)); worker.Stop();
    const auto delivered = results.Copy();
    CHECK(delivered.size() == 2);
    if (delivered.size() == 2) CHECK(delivered[0]->sequenceId == 0 && delivered[1]->sequenceId == 2);
    CHECK(worker.GetMetrics().totalReplacedPending == 1);
    CHECK(worker.GetMetrics().totalDroppedOutOfOrder == 2);
    CHECK(!gate.timedOut);
}
void CallbackStopAndRestart() {
    AnalysisWorker worker; Results results; std::atomic<unsigned> callbacks{0}; std::atomic<bool> selfStartRejected{false};
    worker.SetMockInferenceFunction(ProbabilityMock());
    worker.SetResultCallback([&](auto result) {
        results.Add(std::move(result));
        if (callbacks.fetch_add(1) == 0) { worker.Stop(); selfStartRejected.store(!worker.Start()); }
    });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1)));
    CHECK(results.Wait(1)); CHECK(WaitUntil([&] { return !worker.IsRunning(); }));
    CHECK(WaitUntil([&] { return selfStartRejected.load(); })); CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1)));
    CHECK(results.Wait(2)); worker.Stop(); CHECK(worker.GetResourceUsage().peakWorkerThreads == 1);
}
void ExternalStopWhileCallbackSelfStops() {
    AnalysisWorker worker; Gate callbackGate; std::promise<void> stopDone;
    worker.SetMockInferenceFunction(ProbabilityMock());
    worker.SetResultCallback([&](auto) { callbackGate.Enter(); worker.Stop(); });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1))); CHECK(callbackGate.WaitEntered());
    auto future = stopDone.get_future();
    std::thread stopper([&] { worker.Stop(); stopDone.set_value(); });
    CHECK(WaitUntil([&] { return !worker.IsRunning(); }));
    callbackGate.Release(); CHECK(future.wait_for(3000ms) == std::future_status::ready); stopper.join();
    CHECK(!callbackGate.timedOut); CHECK(worker.Start()); worker.Stop();
}
void CallbackDestroysLastOwner() {
    for (int iteration = 0; iteration < 12; ++iteration) {
        std::unique_ptr<AnalysisWorker> owner = std::make_unique<AnalysisWorker>();
        std::promise<void> destroyed; auto done = destroyed.get_future();
        owner->SetMockInferenceFunction(ProbabilityMock());
        owner->SetResultCallback([&](auto) { owner.reset(); destroyed.set_value(); });
        CHECK(owner->Start());
        auto frame = Frame(0,1,1); std::weak_ptr<const FrameRequest> released = frame;
        CHECK(owner->SubmitFrame(frame)); frame.reset();
        CHECK(done.wait_for(3000ms) == std::future_status::ready);
        CHECK(WaitUntil([&] { return released.expired(); }));
        CHECK(!owner);
    }
}
void ThrowsRecover() {
    AnalysisWorker worker; Results results; std::atomic<unsigned> runs{0}, callbackCount{0};
    worker.SetMockInferenceFunction([&](const std::vector<float>&, std::vector<float>& out, const std::atomic<bool>*) {
        if (runs.fetch_add(1) == 0) throw std::bad_alloc(); // Labelled allocation-fault injection.
        out.assign(256,0.6f); return RunnerStatusCode::Success;
    });
    worker.SetResultCallback([&](auto result) {
        if (callbackCount.fetch_add(1) == 0) throw std::runtime_error("injected callback failure");
        results.Add(std::move(result));
    });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1)));
    CHECK(WaitUntil([&] { return worker.GetMetrics().totalInferenceErrors == 1; }));
    CHECK(worker.SubmitFrame(Frame(0,2,2)));
    CHECK(WaitUntil([&] { return worker.GetMetrics().totalCallbackErrors == 1; }));
    CHECK(worker.SubmitFrame(Frame(0,3,3))); CHECK(results.Wait(1)); worker.Stop();
    CHECK(worker.GetMetrics().totalCompleted == 2);
    CHECK(worker.GetMetrics().totalInferenceErrors == 1);
    const auto delivered = results.Copy(); CHECK(delivered.size() == 1 && delivered.front()->sequenceId == 3);
}
struct ThrowingCopyMock {
    std::shared_ptr<std::atomic<bool>> armed;
    explicit ThrowingCopyMock(std::shared_ptr<std::atomic<bool>> flag) : armed(std::move(flag)) {}
    ThrowingCopyMock(const ThrowingCopyMock& other) : armed(other.armed) {
        if (armed->exchange(false)) throw std::bad_alloc();
    }
    RunnerStatusCode operator()(const std::vector<float>&, std::vector<float>& output, const std::atomic<bool>*) const {
        output.assign(256,0.6f); return RunnerStatusCode::Success;
    }
};
void SnapshotAllocationFaultRecovers() {
    AnalysisWorker worker; Results results; auto armed = std::make_shared<std::atomic<bool>>(false);
    worker.SetMockInferenceFunction(ThrowingCopyMock(armed));
    worker.SetResultCallback([&](auto result) { results.Add(std::move(result)); });
    CHECK(worker.Start()); armed->store(true); CHECK(worker.SubmitFrame(Frame(0,1,1)));
    CHECK(WaitUntil([&] { return worker.GetMetrics().totalInferenceErrors == 1; }));
    CHECK(worker.GetResourceUsage().pendingFrames == 0 && worker.GetResourceUsage().inFlightFrames == 0);
    CHECK(worker.SubmitFrame(Frame(0,2,2))); CHECK(results.Wait(1)); worker.Stop();
    const auto delivered = results.Copy(); CHECK(delivered.size() == 1 && delivered.front()->sequenceId == 2);
}
void ResetStaleCompletion(bool sameGeneration) {
    AnalysisWorker worker; Gate gate; Results results; std::atomic<unsigned> runs{0};
    worker.SetMockInferenceFunction([&](const std::vector<float>&, std::vector<float>& out, const std::atomic<bool>*) {
        if (runs.fetch_add(1) == 0) gate.Enter(); // Deliberately ignores cancellation to prove epoch gate.
        out.assign(256,0.6f); return RunnerStatusCode::Success;
    });
    worker.SetResultCallback([&](auto result) { results.Add(std::move(result)); });
    worker.ResetSource(11); CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(11,9,9)));
    CHECK(gate.WaitEntered()); auto discarded = Frame(11,10,10); std::weak_ptr<const FrameRequest> released = discarded;
    CHECK(worker.SubmitFrame(discarded)); discarded.reset();
    const uint64_t next = sameGeneration ? 11 : 12;
    worker.ResetSource(next); CHECK(released.expired());
    CHECK(worker.SubmitFrame(Frame(next,1,1))); gate.Release(); CHECK(results.Wait(1)); worker.Stop();
    const auto delivered = results.Copy(); CHECK(delivered.size() == 1);
    if (delivered.size() == 1) CHECK(delivered.front()->sequenceId == 1 && delivered.front()->sourceGeneration == next);
    CHECK(!gate.timedOut);
}
void MailboxReplacementAndResetBoundary() {
    auto first = std::make_shared<AnalysisMailbox>(); auto second = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(first); Gate callbackGate; Results results; std::atomic<unsigned> calls{0};
    worker.SetMockInferenceFunction(ProbabilityMock()); worker.ResetSource(71);
    worker.SetResultCallback([&](auto result) {
        results.Add(std::move(result)); if (calls.fetch_add(1) == 0) callbackGate.Enter();
    });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(71,1,1))); CHECK(callbackGate.WaitEntered());
    std::promise<void> switched, attempted; auto done = switched.get_future(); auto begun = attempted.get_future();
    std::thread setter([&] { attempted.set_value(); worker.SetMailbox(second); worker.ResetSource(72); switched.set_value(); });
    CHECK(begun.wait_for(3000ms) == std::future_status::ready);
    CHECK(done.wait_for(25ms) == std::future_status::timeout); // A proven live callback holds the publication boundary.
    callbackGate.Release(); CHECK(done.wait_for(3000ms) == std::future_status::ready); setter.join();
    CHECK(first->GetMetrics().totalPosted == 1); CHECK(second->GetMetrics().currentQueueDepth == 0);
    CHECK(worker.SubmitFrame(Frame(72,1,1))); CHECK(results.Wait(2)); worker.Stop();
    CHECK(second->GetMetrics().totalPosted == 1 && second->GetMetrics().totalRejectedGeneration == 0);
    AnalysisAlignmentStatus alignment;
    auto result = second->GetAlignedAnalysis(72,1,1,alignment);
    CHECK(result && result->sourceGeneration == 72);
    CHECK(!callbackGate.timedOut);
}
void CallbackReentrantResetMailbox() {
    auto mailbox = std::make_shared<AnalysisMailbox>(); AnalysisWorker worker; Results results;
    std::atomic<unsigned> calls{0};
    worker.SetMockInferenceFunction(ProbabilityMock());
    worker.SetResultCallback([&](auto result) {
        results.Add(std::move(result));
        if (calls.fetch_add(1) == 0) { worker.ResetSource(8); worker.SetMailbox(mailbox); }
    });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1))); CHECK(results.Wait(1));
    CHECK(WaitUntil([&] { return worker.GetCurrentSourceGeneration() == 8; }));
    CHECK(worker.SubmitFrame(Frame(8,1,1))); CHECK(results.Wait(2)); worker.Stop();
    CHECK(mailbox->GetMetrics().totalPosted == 1);
}
void ConcurrentTransitionsAndMetrics() {
    AnalysisWorker worker; worker.SetMockInferenceFunction(ProbabilityMock()); worker.ResetSource(1); CHECK(worker.Start());
    std::atomic<bool> finished{false}; std::atomic<bool> bounded{true};
    std::thread submitter([&] {
        for (uint64_t n = 1; n <= 250; ++n) {
            const auto generation = worker.GetCurrentSourceGeneration();
            worker.SubmitFrame(Frame(generation,n,static_cast<int64_t>(n)));
        }
        finished.store(true);
    });
    std::thread observer([&] {
        while (!finished.load()) {
            const auto metrics = worker.GetMetrics(); const auto usage = worker.GetResourceUsage();
            if (usage.inFlightFrames > 1 || usage.pendingFrames > 1 || usage.peakWorkerThreads > 1)
                bounded.store(false);
            (void)metrics;
        }
    });
    std::thread resetter([&] { for (uint64_t n = 2; n < 32; ++n) worker.ResetSource(n); });
    std::thread reloader([&] { for (unsigned n = 0; n < 16; ++n) worker.SetMockInferenceFunction(ProbabilityMock()); });
    submitter.join(); resetter.join(); reloader.join(); observer.join();
    std::thread stop1([&] { worker.Stop(); }); std::thread stop2([&] { worker.Stop(); });
    stop1.join(); stop2.join(); CHECK(bounded.load());
    const auto usage = worker.GetResourceUsage(); CHECK(usage.pendingFrames == 0 && usage.inFlightFrames == 0);
    CHECK(usage.activeWorkerThreads == 0 && usage.peakWorkerThreads == 1);
    CHECK(!worker.SubmitFrame(Frame(worker.GetCurrentSourceGeneration(),999,999)));
    CHECK(worker.Start()); const auto before = worker.GetMetrics().totalCompleted;
    CHECK(worker.SubmitFrame(Frame(worker.GetCurrentSourceGeneration(),1,1)));
    CHECK(WaitUntil([&] { return worker.GetMetrics().totalCompleted > before; })); worker.Stop();
}
void ActualRetainedCapacityPlateau() {
    AnalysisMailboxConfig config; config.maxQueueDepth = 1;
    auto mailbox = std::make_shared<AnalysisMailbox>(config);
    AnalysisWorker worker(mailbox); worker.SetMockInferenceFunction(ProbabilityMock()); CHECK(worker.Start());
    const int sizes[][2] = {{8,6},{80,50},{16,12},{40,30},{8,6},{80,50}};
    size_t firstInput = 0, firstOutput = 0, firstFramePeak = 0;
    uint64_t sequence = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const auto& size : sizes) {
            ++sequence; CHECK(worker.SubmitFrame(Frame(0,sequence,static_cast<int64_t>(sequence),size[0],size[1])));
            CHECK(WaitUntil([&] { return worker.GetMetrics().totalCompleted == sequence && worker.GetResourceUsage().inFlightFrames == 0; }));
            const auto usage = worker.GetResourceUsage();
            CHECK(usage.pendingSourceBytes == 0 && usage.inFlightSourceBytes == 0);
            CHECK(usage.inputTensorCapacityBytes >= 768*sizeof(float) && usage.outputTensorCapacityBytes >= 256*sizeof(float));
            CHECK(usage.pendingFrames <= 1 && usage.inFlightFrames <= 1 && usage.peakWorkerThreads == 1);
        }
        const auto usage = worker.GetResourceUsage();
        if (cycle == 0) {
            firstInput = usage.peakInputTensorCapacityBytes; firstOutput = usage.peakOutputTensorCapacityBytes;
            firstFramePeak = usage.peakInFlightSourceBytes;
        } else {
            CHECK(usage.peakInputTensorCapacityBytes == firstInput && usage.peakOutputTensorCapacityBytes == firstOutput);
            CHECK(usage.peakInFlightSourceBytes == firstFramePeak);
        }
        CHECK(usage.peakInFlightSourceBytes == 80*50*3);
        CHECK(mailbox->GetMetrics().currentQueueDepth == 1);
    }
    worker.Stop(); const auto finalUsage = worker.GetResourceUsage();
    CHECK(finalUsage.inputTensorCapacityBytes == 0 && finalUsage.outputTensorCapacityBytes == 0);
    CHECK(finalUsage.inFlightSourceBytes == 0 && finalUsage.pendingSourceBytes == 0);
}
void NHWCWorkerGeometry() {
    AnalysisWorker worker; Results results; std::atomic<bool> tensorCorrect{false};
    ModelManifest manifest; manifest.inputLayout = TensorLayout::NHWC; manifest.inputDims = {1,3,5,3};
    manifest.outputDims = {1,1,3,5}; manifest.aspectMode = AspectMode::Stretch;
    CHECK(worker.ConfigureMockForTesting(manifest,[&](const std::vector<float>& input, std::vector<float>& out, const std::atomic<bool>*) {
        tensorCorrect.store(input.size() == 45 && std::abs(input[0]-90.0f/255.0f) < 1e-6f &&
            std::abs(input[1]-70.0f/255.0f) < 1e-6f && std::abs(input[2]-40.0f/255.0f) < 1e-6f);
        out.assign(15,0.6f); return RunnerStatusCode::Success;
    }));
    worker.SetResultCallback([&](auto value) { results.Add(std::move(value)); });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1,5,3))); CHECK(results.Wait(1)); worker.Stop();
    CHECK(tensorCorrect.load()); const auto values = results.Copy();
    CHECK(values.size() == 1 && values.front()->personMask.width == 5 && values.front()->personMask.height == 3);
}
void TransactionalWorkerContract() {
    // Real ORT CPU graph, separate from all scheduler-injected mocks above.
    AnalysisWorker worker; Results results; const auto manifest = TinyOnnxFixture::CreateManifest();
    CHECK(worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest) == RunnerStatusCode::Success);
    auto wrong = manifest; wrong.inputTensorName = "incorrect_input_name";
    CHECK(worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, wrong) == RunnerStatusCode::InvalidConfiguration);
    CHECK(worker.GetCurrentManifest().inputTensorName == manifest.inputTensorName);
    worker.SetResultCallback([&](auto value) { results.Add(std::move(value)); });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1))); CHECK(results.Wait(1)); worker.Stop();
    CHECK(worker.GetMetrics().totalModelReloads == 1 && worker.GetMetrics().totalCompleted == 1);
}
void RealReloadSuppressesBlockedMockAndRecoversNative() {
    AnalysisWorker worker; Results results; Gate gate;
    const auto manifest=TinyOnnxFixture::CreateManifest();
    CHECK(worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(),TinyOnnxFixture::kModelByteSize,manifest)==RunnerStatusCode::Success);
    worker.SetMockInferenceFunction([&](const std::vector<float>&,std::vector<float>& output,const std::atomic<bool>*) {
        gate.Enter(); output.assign(256,0.9f); return RunnerStatusCode::Success;
    });
    worker.SetResultCallback([&](auto value) { results.Add(std::move(value)); });
    CHECK(worker.Start()); CHECK(worker.SubmitFrame(Frame(0,1,1000))); CHECK(gate.WaitEntered());
    CHECK(worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(),TinyOnnxFixture::kModelByteSize,manifest)==RunnerStatusCode::Success);
    worker.SetMockInferenceFunction({}); gate.Release();
    CHECK(WaitUntil([&]{return worker.GetResourceUsage().inFlightFrames==0;}));
    CHECK(results.Copy().empty());
    CHECK(worker.SubmitFrame(Frame(0,2,2000))); CHECK(results.Wait(1)); worker.Stop();
    const auto delivered=results.Copy(); CHECK(delivered.size()==1 && delivered.front()->sequenceId==2);
    const float expected=TinyOnnxFixture::ComputeExpectedLuma(90.0f/255,70.0f/255,40.0f/255);
    CHECK(!delivered.empty() && std::abs(delivered.front()->personMask.data[3*8+4]-expected)<1e-4f);
    CHECK(worker.GetMetrics().totalCompleted==1 && worker.GetMetrics().totalModelReloads==2 && worker.GetMetrics().totalInferenceErrors==0);
    CHECK(!gate.timedOut);
}

}
int main() {
    Group("thread failure, idempotent start, restart", StartFailureAndIdempotence);
    Group("single latest pending, independent sequence and PTS", BoundedPendingAndIndependentOrder);
    Group("callback self stop and external restart", CallbackStopAndRestart);
    Group("external join concurrent with callback self stop", ExternalStopWhileCallbackSelfStops);
    Group("callback destroys last wrapper owner", CallbackDestroysLastOwner);
    Group("injected allocation/inference and callback exceptions recover", ThrowsRecover);
    Group("snapshot allocation exception releases pending and recovers", SnapshotAllocationFaultRecovers);
    Group("reset rejects proven in-flight stale completion", [] { ResetStaleCompletion(false); });
    Group("same generation reset has independent epoch", [] { ResetStaleCompletion(true); });
    Group("mailbox replacement and reset publication boundary", MailboxReplacementAndResetBoundary);
    Group("callback reentrant reset and mailbox setter", CallbackReentrantResetMailbox);
    Group("concurrent submit reset reload stop and metrics", ConcurrentTransitionsAndMetrics);
    Group("measured retained tensor/frame capacity plateau", ActualRetainedCapacityPlateau);
    Group("NHWC worker uses correct spatial dimensions", NHWCWorkerGeometry);
    Group("real CPU failed worker manifest transaction preserves model", TransactionalWorkerContract);
    Group("real model reload invalidates blocked mock then native inference recovers", RealReloadSuppressesBlockedMockAndRecoversNative);
    std::cout << "[WORKER SUMMARY] groups=" << groups << " assertions=" << assertions << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}


