#pragma once

#include "frame_request.h"
#include "onnx_runner.h"
#include "model_manifest.h"
#include "frame_preprocessor.h"
#include "result_adapter.h"
#include "effects/analysisbridge/analysis_types.h"
#include "effects/analysisbridge/analysis_mailbox.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>
#include <string>

namespace PrismForge::Providers {

/// <summary>
/// Operational metrics for AnalysisWorker.
/// </summary>
struct WorkerMetrics {
    uint64_t totalSubmitted = 0;
    uint64_t totalCompleted = 0;
    uint64_t totalReplacedPending = 0;
    uint64_t totalDroppedObsoleteGeneration = 0;
    uint64_t totalDroppedOutOfOrder = 0;
    uint64_t totalInferenceErrors = 0;
    uint64_t totalModelReloads = 0;
    uint64_t totalCallbackErrors = 0;
};

/// <summary>
/// Bounded Asynchronous Analysis Worker.
/// Coordinates frame submission, background tensor preprocessing, native ONNX inference,
/// result adaptation, and delivery to the frozen AnalysisMailbox.
/// 
/// Production hardening features:
/// - Shared owned worker state (no raw this / self-detach lifetime hazards)
/// - Safe callback-initiated self-stop, destruction, and external restart
/// - Strict sequenceId and timestampPts monotonic ordering
/// - Thread-safe lock-free operational metrics
/// - Full exception boundaries around preprocessing, inference, adaptation, and callbacks
/// - Native cancellation integration on source reset and stop
/// - Exactly ONE in-flight inference + at most ONE latest pending frame
/// </summary>
// Retained vector capacities, not a claim about process-wide heap/driver memory.
// Metrics are individual atomic observations rather than a globally coherent snapshot.
struct WorkerResourceUsage {
    size_t pendingSourceBytes = 0, inFlightSourceBytes = 0;
    size_t inputTensorCapacityBytes = 0, outputTensorCapacityBytes = 0;
    size_t peakPendingSourceBytes = 0, peakInFlightSourceBytes = 0;
    size_t peakInputTensorCapacityBytes = 0, peakOutputTensorCapacityBytes = 0;
    unsigned pendingFrames = 0, inFlightFrames = 0;
    unsigned activeWorkerThreads = 0, peakWorkerThreads = 0;
};
class AnalysisWorker {
public:
    using ResultCallback = std::function<void(std::shared_ptr<const Effects::AnalysisBridge::AnalysisBundle>)>;

    AnalysisWorker();
    explicit AnalysisWorker(std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox);
    ~AnalysisWorker();

    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;

    /// <summary>
    /// Starts the background analysis worker thread.
    /// Handles thread creation failure gracefully and supports restart after stop.
    /// </summary>
    bool Start();

    /// <summary>
    /// Gracefully stops the worker thread and signals in-flight cancellation.
    /// Safe against self-join if invoked from result callback.
    /// </summary>
    void Stop();

    /// <summary>
    /// Resets the camera source generation.
    /// Immediately drops any pending frame, marks cancellation on in-flight inference,
    /// and resets delivery sequence trackers.
    /// </summary>
    void ResetSource(uint64_t newSourceGeneration);

    /// <summary>
    /// Submits a newly captured camera frame for background analysis.
    /// Strictly non-blocking: never waits on inference or render pipeline.
    /// If an older pending frame is waiting, it is immediately replaced and released.
    /// Rejects submissions promptly when stopped or stopping.
    /// </summary>
    /// <returns>True if frame was accepted as pending</returns>
    bool SubmitFrame(std::shared_ptr<const FrameRequest> frame);

    /// <summary>
    /// Configures the active model and manifest.
    /// Transactionally reloads the model on the runner and bumps the model epoch.
    /// </summary>
    RunnerStatusCode LoadModel(
        const std::wstring& modelPath,
        const ModelManifest& manifest,
        ExecutionProviderType provider = ExecutionProviderType::CPU,
        int deviceId = 0
    );

    /// <summary>
    /// Loads a model directly from memory buffer.
    /// </summary>
    RunnerStatusCode LoadModelFromMemory(
        const void* modelData,
        size_t modelDataSize,
        const ModelManifest& manifest,
        ExecutionProviderType provider = ExecutionProviderType::CPU,
        int deviceId = 0,
        const std::string& identifier = "memory_buffer"
    );

    /// <summary>
    /// Connects or replaces the destination mailbox safely.
    /// </summary>
    void SetMailbox(std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox);

    /// <summary>
    /// Sets an optional auxiliary result callback (e.g. for metrics/tests).
    /// Callback receives an immutable shared_ptr owning the bundle.
    /// </summary>
    void SetResultCallback(ResultCallback callback);

    /// <summary>
    /// Injects a custom mock inference function for lifecycle and fault-injection testing.
    /// If set, the worker calls this instead of the native OnnxRunner.
    /// </summary>
    using MockInferenceFn = std::function<RunnerStatusCode(
        const std::vector<float>& inputTensor,
        std::vector<float>& outputTensor,
        const std::atomic<bool>* cancelSignal
    )>;
    void SetMockInferenceFunction(MockInferenceFn mockFn);
    // Explicit scheduling-test contract. Never an implicit missing-model fallback.
    bool ConfigureMockForTesting(const ModelManifest& manifest, MockInferenceFn mockFn);

    /// <summary>
    /// Returns current worker operational metrics thread-safely.
    /// </summary>
    WorkerMetrics GetMetrics() const;
    WorkerResourceUsage GetResourceUsage() const;

    // Explicit failure injection; default false. Never substitutes model inference.
    void SetThreadStartFailureForTesting(bool fail);

    /// <summary>
    /// Returns current active model manifest.
    /// </summary>
    ModelManifest GetCurrentManifest() const;

    /// <summary>
    /// Returns active source generation.
    /// </summary>
    uint64_t GetCurrentSourceGeneration() const;

    /// <summary>
    /// Returns true if worker thread is actively running.
    /// </summary>
    bool IsRunning() const;
    bool InitializeRuntime(const std::wstring& absoluteDllPath);
    RunnerSessionInfo GetSessionInfo() const;

    struct WorkerState;
private:
    static void RunWorkerLoop(std::shared_ptr<WorkerState> state);
    std::shared_ptr<WorkerState> m_state;
};

} // namespace PrismForge::Providers


