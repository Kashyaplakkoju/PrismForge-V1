#include "analysis_worker.h"
#include <system_error>

namespace PrismForge::Providers {
namespace {
thread_local const void* currentWorkerState = nullptr;

void HighWater(std::atomic<size_t>& high, size_t value) noexcept {
    size_t old = high.load(std::memory_order_relaxed);
    while (old < value && !high.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}
size_t RetainedBytes(const std::shared_ptr<const FrameRequest>& frame) noexcept {
    return frame && frame->pixelData ? frame->pixelData->capacity() : 0;
}
}

struct AnalysisWorker::WorkerState {
    // External Start/Stop serialize on lifecycleMutex, and may join while holding it.
    // Worker callbacks MUST NOT acquire that mutex: self Stop only requests exit.
    // Publication order is publicationMutex -> workerMutex -> mailbox internals.
    // No inference, callback, or join executes with workerMutex held.
    mutable std::mutex lifecycleMutex;
    mutable std::recursive_mutex publicationMutex;
    mutable std::mutex workerMutex;
    std::condition_variable cv;
    std::thread workerThread;
    bool running = false;
    bool stopping = true;
    bool failThreadStart = false;
    std::shared_ptr<const FrameRequest> pending;
    std::shared_ptr<std::atomic<bool>> activeCancellation;
    bool haveAccepted = false;
    uint64_t lastAcceptedSequence = 0;
    int64_t lastAcceptedPts = 0;
    bool haveDelivered = false;
    uint64_t lastDeliveredSequence = 0;
    int64_t lastDeliveredPts = 0;
    uint64_t sourceGeneration = 0;
    uint64_t sourceEpoch = 0; // Also invalidates ResetSource(same generation).
    uint64_t modelEpoch = 0;
    uint64_t runnerEpoch = 0;
    OnnxRunner runner;
    ModelManifest manifest;
    MockInferenceFn mockInference;
    std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox;
    ResultCallback callback;

    std::atomic<uint64_t> submitted{0}, completed{0}, replaced{0}, obsolete{0};
    std::atomic<uint64_t> unordered{0}, errors{0}, reloads{0}, callbackErrors{0};
    std::atomic<size_t> pendingBytes{0}, activeBytes{0}, inputCapacity{0}, outputCapacity{0};
    std::atomic<size_t> peakPendingBytes{0}, peakActiveBytes{0}, peakInputCapacity{0}, peakOutputCapacity{0};
    std::atomic<unsigned> pendingCount{0}, activeCount{0}, liveThreads{0}, peakThreads{0};

    explicit WorkerState(std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mb)
        : mailbox(std::move(mb)) {
        if (mailbox) mailbox->ResetSource(sourceGeneration);
    }
    ~WorkerState() {
        if (!workerThread.joinable()) return;
        if (workerThread.get_id() == std::this_thread::get_id()) {
            // Only the thread's final shared-state owner can reach this branch.
            // RunWorkerLoop has returned and its last state use is over. Detach
            // releases the completed native handle; no wrapper or state is orphaned.
            workerThread.detach();
        } else {
            workerThread.join();
        }
    }
    void DropPending() noexcept {
        pending.reset();
        pendingBytes.store(0, std::memory_order_relaxed);
        pendingCount.store(0, std::memory_order_relaxed);
    }
    void CancelActive() noexcept {
        if (activeCancellation) activeCancellation->store(true, std::memory_order_release);
        // The runner cancellation path is separate from its synchronous run mutex.
        runner.CancelActiveRun();
    }
    void RequestStop() noexcept {
        std::lock_guard<std::mutex> lock(workerMutex);
        stopping = true;
        DropPending();
        CancelActive();
        cv.notify_all();
    }
};

void AnalysisWorker::RunWorkerLoop(std::shared_ptr<WorkerState> state) {
    currentWorkerState = state.get();
    const unsigned threads = state->liveThreads.fetch_add(1) + 1;
    unsigned high = state->peakThreads.load();
    while (high < threads && !state->peakThreads.compare_exchange_weak(high, threads)) {}
    // These buffers belong only to this invocation. Their measured capacities are
    // published for bounded-storage tests; they are released on every stop/restart.
    std::vector<float> input, output;
    FramePreprocessor preprocessor;
    ResultAdapter adapter;
    for (;;) {
        std::shared_ptr<const FrameRequest> frame;
        std::shared_ptr<std::atomic<bool>> cancellation;
        ModelManifest manifest;
        MockInferenceFn mock;
        uint64_t sourceEpoch = 0, modelEpoch = 0, runnerEpoch = 0;
        bool active = false;
        try {
            {
                std::unique_lock<std::mutex> lock(state->workerMutex);
                state->cv.wait(lock, [&] { return state->stopping || state->pending != nullptr; });
                if (state->stopping) break;
                // Snapshot allocations may throw; leave/drop the pending slot in
                // the catch boundary instead of terminating the worker process.
                manifest = state->manifest;
                mock = state->mockInference;
                cancellation = std::make_shared<std::atomic<bool>>(false);
                frame = std::move(state->pending);
                state->DropPending();
                sourceEpoch = state->sourceEpoch;
                modelEpoch = state->modelEpoch;
                runnerEpoch = state->runnerEpoch;
                state->activeCancellation = cancellation;
                state->activeBytes.store(RetainedBytes(frame), std::memory_order_relaxed);
                HighWater(state->peakActiveBytes, RetainedBytes(frame));
                state->activeCount.store(1, std::memory_order_relaxed);
                active = true;
            }
            if (!frame || !manifest.Validate() || (!mock && runnerEpoch == 0)) {
                state->errors.fetch_add(1, std::memory_order_relaxed);
            } else {
                int width = 0, height = 0;
                if (!manifest.InputGeometry(width, height)) {
                    state->errors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    GeometryTransform transform;
                    const bool prepared = preprocessor.Preprocess(*frame, width, height,
                        manifest.inputLayout, manifest.normConfig, manifest.aspectMode, input, transform);
                    state->inputCapacity.store(input.capacity() * sizeof(float), std::memory_order_relaxed);
                    HighWater(state->peakInputCapacity, input.capacity() * sizeof(float));
                    if (!prepared) {
                        state->errors.fetch_add(1, std::memory_order_relaxed);
                    } else if (!cancellation->load(std::memory_order_acquire)) {
                        // No worker lock is held; Submit remains a latest-slot write.
                        output.clear();
                        const RunnerStatusCode status = mock ? mock(input, output, cancellation.get()) :
                            state->runner.RunInference(input, output, cancellation.get(), runnerEpoch);
                        state->outputCapacity.store(output.capacity() * sizeof(float), std::memory_order_relaxed);
                        HighWater(state->peakOutputCapacity, output.capacity() * sizeof(float));
                        if (status != RunnerStatusCode::Success) {
                            if (status != RunnerStatusCode::Cancelled && !cancellation->load())
                                state->errors.fetch_add(1, std::memory_order_relaxed);
                        } else if (!cancellation->load(std::memory_order_acquire)) {
                            std::shared_ptr<Effects::AnalysisBridge::AnalysisBundle> bundle;
                            if (!adapter.AdaptResult(output, manifest, transform, *frame, bundle) ||
                                !bundle || !bundle->HasPersonMask()) {
                                state->errors.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                // Reset/replace/model commit are serialized with the
                                // entire publication, including the callback. This
                                // prevents a callback beginning after reset returns.
                                // Recursive permits callback Reset/Stop/SetMailbox.
                                std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
                                std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox;
                                ResultCallback callback;
                                bool publish = false;
                                {
                                    std::lock_guard<std::mutex> lock(state->workerMutex);
                                    if (state->stopping || cancellation->load() ||
                                        sourceEpoch != state->sourceEpoch || modelEpoch != state->modelEpoch ||
                                        bundle->sourceGeneration != state->sourceGeneration) {
                                        state->obsolete.fetch_add(1, std::memory_order_relaxed);
                                    } else if (state->haveDelivered &&
                                        (bundle->sequenceId <= state->lastDeliveredSequence ||
                                         bundle->timestampPts <= state->lastDeliveredPts)) {
                                        state->unordered.fetch_add(1, std::memory_order_relaxed);
                                    } else {
                                        mailbox = state->mailbox;
                                        callback = state->callback;
                                        state->haveDelivered = true;
                                        state->lastDeliveredSequence = bundle->sequenceId;
                                        state->lastDeliveredPts = bundle->timestampPts;
                                        publish = true;
                                    }
                                }
                                if (publish) {
                                    if (mailbox) mailbox->PostAnalysis(bundle);
                                    state->completed.fetch_add(1, std::memory_order_relaxed);
                                    if (callback) {
                                        try { callback(bundle); }
                                        catch (...) { state->callbackErrors.fetch_add(1, std::memory_order_relaxed); }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
            state->errors.fetch_add(1, std::memory_order_relaxed);
            if (!active) {
                std::lock_guard<std::mutex> lock(state->workerMutex);
                state->DropPending();
            }
        }
        if (active) {
            // Keep the cancellation pointer valid through adaptation/publication,
            // including a callback that destroys the last AnalysisWorker wrapper.
            std::lock_guard<std::mutex> lock(state->workerMutex);
            if (state->activeCancellation == cancellation) state->activeCancellation.reset();
            state->activeBytes.store(0, std::memory_order_relaxed);
            state->activeCount.store(0, std::memory_order_relaxed);
        }
    }
    // Vector destruction releases capacity after the exit bookkeeping; no potentially throwing shrink_to_fit on the thread tail.
    state->inputCapacity.store(0, std::memory_order_relaxed);
    state->outputCapacity.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state->workerMutex);
        state->running = false;
        state->DropPending();
        state->activeCancellation.reset();
    }
    state->liveThreads.fetch_sub(1);
    currentWorkerState = nullptr;
}

AnalysisWorker::AnalysisWorker() : m_state(std::make_shared<WorkerState>(nullptr)) {}
AnalysisWorker::AnalysisWorker(std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox)
    : m_state(std::make_shared<WorkerState>(std::move(mailbox))) {}
AnalysisWorker::~AnalysisWorker() { Stop(); }

bool AnalysisWorker::Start() {
    const auto state = m_state;
    if (!state || currentWorkerState == state.get()) return false;
    std::lock_guard<std::mutex> lifecycle(state->lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(state->workerMutex);
        if (state->running && !state->stopping) return true;
    }
    if (state->workerThread.joinable()) state->workerThread.join();
    try {
        std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
        std::lock_guard<std::mutex> lock(state->workerMutex);
        if (state->failThreadStart) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        state->stopping = false;
        state->running = true;
        state->DropPending();
        state->haveAccepted = state->haveDelivered = false;
        ++state->sourceEpoch;
        if (state->mailbox) state->mailbox->ResetSource(state->sourceGeneration);
        state->workerThread = std::thread([state] { RunWorkerLoop(state); });
        return true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(state->workerMutex);
        state->running = false;
        state->stopping = true;
        state->DropPending();
        state->errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void AnalysisWorker::Stop() {
    const auto state = m_state;
    if (!state) return;
    if (currentWorkerState == state.get()) {
        state->RequestStop(); // Never wait on the external join mutex from a callback.
        return;
    }
    std::lock_guard<std::mutex> lifecycle(state->lifecycleMutex);
    state->RequestStop();
    if (state->workerThread.joinable()) state->workerThread.join();
}

void AnalysisWorker::ResetSource(uint64_t generation) {
    const auto state = m_state;
    if (!state) return;
    std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
    std::lock_guard<std::mutex> lock(state->workerMutex);
    ++state->sourceEpoch;
    state->sourceGeneration = generation;
    state->CancelActive();
    state->DropPending();
    state->haveAccepted = state->haveDelivered = false;
    if (state->mailbox) state->mailbox->ResetSource(generation);
}

bool AnalysisWorker::SubmitFrame(std::shared_ptr<const FrameRequest> frame) {
    const auto state = m_state;
    if (!state || !frame || !frame->IsValid()) return false;
    std::lock_guard<std::mutex> lock(state->workerMutex);
    if (!state->running || state->stopping) return false;
    if (frame->sourceGeneration != state->sourceGeneration) {
        state->obsolete.fetch_add(1, std::memory_order_relaxed); return false;
    }
    if (state->haveAccepted && (frame->sequenceId <= state->lastAcceptedSequence || frame->timestampPts <= state->lastAcceptedPts)) {
        state->unordered.fetch_add(1, std::memory_order_relaxed); return false;
    }
    state->haveAccepted = true;
    state->lastAcceptedSequence = frame->sequenceId;
    state->lastAcceptedPts = frame->timestampPts;
    if (state->pending) state->replaced.fetch_add(1, std::memory_order_relaxed);
    state->pending = std::move(frame);
    const size_t bytes = RetainedBytes(state->pending);
    state->pendingBytes.store(bytes, std::memory_order_relaxed);
    HighWater(state->peakPendingBytes, bytes);
    state->pendingCount.store(1, std::memory_order_relaxed);
    state->submitted.fetch_add(1, std::memory_order_relaxed);
    state->cv.notify_one();
    return true;
}

bool AnalysisWorker::InitializeRuntime(const std::wstring& path) { return m_state && m_state->runner.Initialize(path); }
RunnerSessionInfo AnalysisWorker::GetSessionInfo() const { return m_state ? m_state->runner.GetSessionInfo() : RunnerSessionInfo{}; }

RunnerStatusCode AnalysisWorker::LoadModel(const std::wstring& path, const ModelManifest& manifest,
    ExecutionProviderType provider, int deviceId) {
    const auto state = m_state;
    if (!state) return RunnerStatusCode::MissingRuntime;
    try {
        ModelManifest candidate = manifest;
        if (!candidate.Validate()) return RunnerStatusCode::InvalidConfiguration;
        std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
        { std::lock_guard<std::mutex> lock(state->workerMutex); state->CancelActive(); }
        const auto status = state->runner.LoadModel(path, provider, deviceId, &candidate);
        if (status != RunnerStatusCode::Success) return status;
        std::lock_guard<std::mutex> lock(state->workerMutex);
        state->manifest = std::move(candidate);
        state->runnerEpoch = state->runner.GetModelEpoch();
        ++state->modelEpoch;
        state->CancelActive(); state->DropPending();
        state->reloads.fetch_add(1, std::memory_order_relaxed);
        return status;
    } catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
      catch (...) { return RunnerStatusCode::InvalidConfiguration; }
}
RunnerStatusCode AnalysisWorker::LoadModelFromMemory(const void* data, size_t size, const ModelManifest& manifest,
    ExecutionProviderType provider, int deviceId, const std::string& identifier) {
    const auto state = m_state;
    if (!state) return RunnerStatusCode::MissingRuntime;
    try {
        ModelManifest candidate = manifest;
        if (!candidate.Validate()) return RunnerStatusCode::InvalidConfiguration;
        std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
        { std::lock_guard<std::mutex> lock(state->workerMutex); state->CancelActive(); }
        const auto status = state->runner.LoadModelFromMemory(data, size, provider, deviceId, identifier, &candidate);
        if (status != RunnerStatusCode::Success) return status;
        std::lock_guard<std::mutex> lock(state->workerMutex);
        state->manifest = std::move(candidate);
        state->runnerEpoch = state->runner.GetModelEpoch();
        ++state->modelEpoch;
        state->CancelActive(); state->DropPending();
        state->reloads.fetch_add(1, std::memory_order_relaxed);
        return status;
    } catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
      catch (...) { return RunnerStatusCode::InvalidConfiguration; }
}
void AnalysisWorker::SetMailbox(std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox) {
    const auto state = m_state;
    std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
    std::lock_guard<std::mutex> lock(state->workerMutex);
    if (mailbox) mailbox->ResetSource(state->sourceGeneration);
    state->mailbox = std::move(mailbox);
}
void AnalysisWorker::SetResultCallback(ResultCallback callback) {
    const auto state = m_state;
    std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
    std::lock_guard<std::mutex> lock(state->workerMutex);
    state->callback = std::move(callback);
}
void AnalysisWorker::SetMockInferenceFunction(MockInferenceFn mock) {
    const auto state = m_state;
    std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
    std::lock_guard<std::mutex> lock(state->workerMutex);
    state->CancelActive(); state->DropPending();
    state->mockInference = std::move(mock);
    ++state->modelEpoch;
}
bool AnalysisWorker::ConfigureMockForTesting(const ModelManifest& manifest, MockInferenceFn mock) {
    try {
        if (!mock || !manifest.Validate()) return false;
        ModelManifest candidate = manifest;
        const auto state = m_state;
        std::lock_guard<std::recursive_mutex> publication(state->publicationMutex);
        std::lock_guard<std::mutex> lock(state->workerMutex);
        state->CancelActive(); state->DropPending();
        state->manifest = std::move(candidate);
        state->mockInference = std::move(mock);
        ++state->modelEpoch;
        return true;
    } catch (...) { return false; }
}
WorkerMetrics AnalysisWorker::GetMetrics() const {
    const auto state = m_state;
    WorkerMetrics metrics;
    metrics.totalSubmitted = state->submitted.load(std::memory_order_relaxed);
    metrics.totalCompleted = state->completed.load(std::memory_order_relaxed);
    metrics.totalReplacedPending = state->replaced.load(std::memory_order_relaxed);
    metrics.totalDroppedObsoleteGeneration = state->obsolete.load(std::memory_order_relaxed);
    metrics.totalDroppedOutOfOrder = state->unordered.load(std::memory_order_relaxed);
    metrics.totalInferenceErrors = state->errors.load(std::memory_order_relaxed);
    metrics.totalModelReloads = state->reloads.load(std::memory_order_relaxed);
    metrics.totalCallbackErrors = state->callbackErrors.load(std::memory_order_relaxed);
    return metrics;
}
WorkerResourceUsage AnalysisWorker::GetResourceUsage() const {
    const auto state = m_state;
    WorkerResourceUsage usage;
    usage.pendingSourceBytes = state->pendingBytes.load(); usage.inFlightSourceBytes = state->activeBytes.load();
    usage.inputTensorCapacityBytes = state->inputCapacity.load(); usage.outputTensorCapacityBytes = state->outputCapacity.load();
    usage.peakPendingSourceBytes = state->peakPendingBytes.load(); usage.peakInFlightSourceBytes = state->peakActiveBytes.load();
    usage.peakInputTensorCapacityBytes = state->peakInputCapacity.load(); usage.peakOutputTensorCapacityBytes = state->peakOutputCapacity.load();
    usage.pendingFrames = state->pendingCount.load(); usage.inFlightFrames = state->activeCount.load();
    usage.activeWorkerThreads = state->liveThreads.load(); usage.peakWorkerThreads = state->peakThreads.load();
    return usage;
}
void AnalysisWorker::SetThreadStartFailureForTesting(bool fail) {
    const auto state = m_state;
    std::lock_guard<std::mutex> lock(state->workerMutex);
    state->failThreadStart = fail;
}
ModelManifest AnalysisWorker::GetCurrentManifest() const {
    const auto state = m_state; std::lock_guard<std::mutex> lock(state->workerMutex); return state->manifest;
}
uint64_t AnalysisWorker::GetCurrentSourceGeneration() const {
    const auto state = m_state; std::lock_guard<std::mutex> lock(state->workerMutex); return state->sourceGeneration;
}
bool AnalysisWorker::IsRunning() const {
    const auto state = m_state; std::lock_guard<std::mutex> lock(state->workerMutex); return state->running && !state->stopping;
}
} // namespace PrismForge::Providers



