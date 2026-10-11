#include "segmentation_service.h"
#include <windows.h>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
extern "C" {
#include <libavutil/frame.h>
}

namespace PrismForge::Providers {
namespace {
constexpr uint32_t kRecoveryLimit = 3;
constexpr uint64_t kErrorThreshold = 3;
std::filesystem::path ExeDirectory() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size); return std::filesystem::path(path).parent_path();
}
}
struct SegmentationService::Impl {
    SegmentationOptions options;
    std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> mailbox;
    AnalysisWorker worker;
    mutable std::mutex controlMutex, stateMutex, submitMutex, stopMutex;
    std::condition_variable changed;
    std::thread controller;
    bool terminate = false, enabled = false, highQuality = true;
    uint64_t revision = 0, sourceRevision = 0, enableEpoch = 0;
    std::atomic<bool> ready{false}, accepting{false}, workerAvailable{false};
    std::atomic<uint64_t> currentCompleted{0}, sourceErrorBaseline{0}, lastSuccessErrors{0};
    std::atomic<uint64_t> recoveryErrorBaseline{0}, injectedStarts{0};
    std::atomic<bool> quality{true};
    uint64_t sourceGeneration = 0;
    int sourceWidth = 0, sourceHeight = 0;
    bool haveSubmitted = false;
    int64_t lastSubmittedPts = 0;
    uint64_t lastSubmittedSequence = 0;
    SegmentationState state;

    explicit Impl(SegmentationOptions opts)
        : options(std::move(opts)), mailbox(std::make_shared<Effects::AnalysisBridge::AnalysisMailbox>()), worker(mailbox) {
        auto root = ExeDirectory();
        if (options.modelDirectory.empty()) options.modelDirectory = (root / L"models/pphumanseg").wstring();
        if (options.runtimeDllPath.empty()) options.runtimeDllPath = (root / L"onnxruntime.dll").wstring();
        worker.SetResultCallback([this](std::shared_ptr<const Effects::AnalysisBridge::AnalysisBundle>) {
            currentCompleted.fetch_add(1, std::memory_order_relaxed);
            lastSuccessErrors.store(worker.GetMetrics().totalInferenceErrors, std::memory_order_release);
            changed.notify_one();
        });
        controller = std::thread([this] { ControlLoop(); });
    }
    // submitMutex serializes intake/reset. While a model is loading, reset only the
    // mailbox: LoadModel owns the worker publication gate until native load finishes.
    void ResetCurrentSource(bool resetMetrics = true) {
        if (workerAvailable.load(std::memory_order_acquire)) worker.ResetSource(sourceGeneration);
        else mailbox->ResetSource(sourceGeneration);
        currentCompleted.store(0, std::memory_order_release);
        if (resetMetrics) {
            const auto errors = worker.GetMetrics().totalInferenceErrors;
            sourceErrorBaseline.store(errors, std::memory_order_release);
            lastSuccessErrors.store(errors, std::memory_order_release);
            recoveryErrorBaseline.store(errors, std::memory_order_release);
        }
        haveSubmitted = false;
    }
    void SetState(SegmentationPhase phase, std::string message, const RunnerSessionInfo& info,
        uint32_t attempts, bool recoveryFallback) {
        std::lock_guard<std::mutex> lock(stateMutex);
        state.phase = phase; state.message = std::move(message);
        state.highQuality = quality.load(); state.activeProvider = info.activeProvider;
        state.fallbackOccurred = info.fallbackOccurred || recoveryFallback;
        state.recoveryAttempts = attempts; state.maxRecoveryAttempts = kRecoveryLimit;
    }
    void ControlLoop() noexcept {
        uint64_t handled = 0, handledSource = 0, handledEnable = 0;
        bool runtimeLoaded = false, modelLoaded = false, recoveryPending = false, startPending = false;
        bool exhausted = false, recoveryFallback = false;
        uint32_t attempts = 0;
        RunnerSessionInfo info;
        auto provider = options.requestedProvider;
        auto beginRecovery = [&](const std::string& reason) {
            ready.store(false, std::memory_order_release);
            // Invalidate publication before joining, including retained same-source masks.
            { std::lock_guard<std::mutex> source(submitMutex); ResetCurrentSource(false); }
            recoveryErrorBaseline.store(worker.GetMetrics().totalInferenceErrors, std::memory_order_release);
            if (attempts >= kRecoveryLimit) {
                exhausted = true; recoveryPending = false;
                worker.Stop();
                SetState(SegmentationPhase::Failed, "Person segmentation failed after 3 recovery attempts; turn backgrounds off and on to retry", info, attempts, recoveryFallback);
                return;
            }
            ++attempts; recoveryPending = true; modelLoaded = false;
            if (info.activeProvider == ExecutionProviderType::DirectML && attempts >= 2) {
                provider = ExecutionProviderType::CPU; recoveryFallback = true;
            } else if (!recoveryFallback) provider = info.activeProvider;
            SetState(SegmentationPhase::Recovering,
                reason + " (attempt " + std::to_string(attempts) + "/3" +
                (recoveryFallback ? ", retrying on CPU)" : ")"), info, attempts, recoveryFallback);
        };
        for (;;) {
            bool configChanged = false, shouldRun = false, newEnable = false;
            uint64_t runRevision = 0, runSource = 0;
            {
                std::unique_lock<std::mutex> lock(controlMutex);
                changed.wait_for(lock, std::chrono::milliseconds(25), [&] {
                    return terminate || revision != handled || sourceRevision != handledSource;
                });
                if (terminate) break;
                configChanged = revision != handled;
                handled = runRevision = revision; handledSource = runSource = sourceRevision;
                shouldRun = enabled;
                newEnable = enableEpoch != handledEnable; handledEnable = enableEpoch;
            }
            try {
                if (configChanged) startPending = shouldRun;
                if (newEnable) {
                    attempts = 0; exhausted = recoveryPending = recoveryFallback = false;
                    modelLoaded = false; provider = options.requestedProvider;
                    recoveryErrorBaseline.store(worker.GetMetrics().totalInferenceErrors, std::memory_order_release);
                }
                if (!shouldRun) {
                    if (!configChanged) continue;
                    ready.store(false, std::memory_order_release); worker.Stop();
                    { std::lock_guard<std::mutex> source(submitMutex); ResetCurrentSource(); }
                    // Only an explicit disable starts a fresh automatic retry budget.
                    attempts = 0; exhausted = recoveryPending = recoveryFallback = false;
                    modelLoaded = false; startPending = false; provider = options.requestedProvider;
                    SetState(SegmentationPhase::Disabled, "Background effects are off", info, attempts, false);
                    continue;
                }
                if (exhausted) continue;
                if (!configChanged && !recoveryPending && ready.load(std::memory_order_acquire)) {
                    const auto errors = worker.GetMetrics().totalInferenceErrors;
                    const auto baseline = std::max(lastSuccessErrors.load(std::memory_order_acquire),
                        recoveryErrorBaseline.load(std::memory_order_acquire));
                    if (errors >= baseline && errors - baseline >= kErrorThreshold)
                        beginRecovery("Person inference repeatedly failed; recovering");
                    continue;
                }
                if (!configChanged && !recoveryPending && !startPending) continue;
                ready.store(false, std::memory_order_release);
                worker.Stop(); // Cancellation/join/model creation never run on Submit's thread.
                {
                    std::lock_guard<std::mutex> source(submitMutex);
                    workerAvailable.store(false, std::memory_order_release);
                    ResetCurrentSource(false);
                }
                if (recoveryPending) {
                    std::unique_lock<std::mutex> lock(controlMutex);
                    const auto delay = std::chrono::milliseconds(250u << (attempts - 1));
                    if (changed.wait_for(lock, delay, [&] {
                        return terminate || !enabled || revision != runRevision || sourceRevision != runSource;
                    })) continue;
                } else SetState(SegmentationPhase::Loading, "Loading person segmentation", info, attempts, recoveryFallback);
                {
                    std::lock_guard<std::mutex> lock(controlMutex);
                    if (terminate || !enabled || revision != runRevision || sourceRevision != runSource) continue;
                }
                if (!modelLoaded) {
                    ModelManifest manifest;
                    auto directory = std::filesystem::path(options.modelDirectory);
                    std::string loadError;
                    if (!directory.is_absolute() || !ModelManifest::LoadFromFile((directory / L"manifest.json").wstring(), manifest))
                        loadError = "Person model manifest is missing or invalid";
                    else if (!runtimeLoaded && !worker.InitializeRuntime(options.runtimeDllPath))
                        loadError = "Person inference runtime is unavailable";
                    else {
                        runtimeLoaded = true;
                        const auto result = worker.LoadModel((directory / L"model.onnx").wstring(), manifest, provider, options.deviceId);
                        if (result != RunnerStatusCode::Success)
                            loadError = "Person model could not load (status " + std::to_string(static_cast<int>(result)) + ")";
                        else { modelLoaded = true; info = worker.GetSessionInfo(); }
                    }
                    if (!loadError.empty()) {
                        if (recoveryPending) beginRecovery(loadError);
                        else { startPending = false; SetState(SegmentationPhase::Failed, loadError, info, attempts, recoveryFallback); }
                        continue;
                    }
                }
                // Configuration/source and Start commit under one gate. No stale
                // controller revision can start a disabled worker or mark it Ready.
                std::lock_guard<std::mutex> readyGuard(controlMutex);
                if (terminate || !enabled || revision != runRevision || sourceRevision != runSource) continue;
                {
                    std::lock_guard<std::mutex> source(submitMutex);
                    workerAvailable.store(true, std::memory_order_release);
                    ResetCurrentSource(false);
                    if (!worker.Start()) {
                        SetState(SegmentationPhase::Failed, "Person worker could not start", info, attempts, recoveryFallback);
                        recoveryPending = startPending = false; continue;
                    }
                }
                if (recoveryPending) recoveryErrorBaseline.store(worker.GetMetrics().totalInferenceErrors, std::memory_order_release);
                recoveryPending = startPending = false;
                SetState(SegmentationPhase::Ready,
                    info.activeProvider == ExecutionProviderType::DirectML ? "Person segmentation ready on GPU" :
                    recoveryFallback ? "Person segmentation ready on CPU (GPU inference repeatedly failed)" :
                    info.fallbackOccurred ? "Person segmentation ready on CPU (GPU unavailable)" : "Person segmentation ready on CPU",
                    info, attempts, recoveryFallback);
                ready.store(true, std::memory_order_release);
            } catch (const std::exception& error) {
                ready.store(false, std::memory_order_release); worker.Stop(); recoveryPending = startPending = false;
                SetState(SegmentationPhase::Failed, std::string("Person segmentation failed: ") + error.what(), info, attempts, recoveryFallback);
            } catch (...) {
                ready.store(false, std::memory_order_release); worker.Stop(); recoveryPending = startPending = false;
                SetState(SegmentationPhase::Failed, "Person segmentation failed", info, attempts, recoveryFallback);
            }
        }
        ready.store(false, std::memory_order_release); worker.Stop();
        SetState(SegmentationPhase::Stopped, "Person segmentation stopped", info, attempts, recoveryFallback);
    }
};

SegmentationService::SegmentationService(const SegmentationOptions& options) : impl_(std::make_unique<Impl>(options)) {}
SegmentationService::~SegmentationService() { Stop(); }
void SegmentationService::Configure(bool enabled, bool highQuality) {
    auto& p = *impl_;
    {
        std::lock_guard<std::mutex> lock(p.controlMutex);
        if (p.terminate || (p.enabled == enabled && p.highQuality == highQuality)) return;
        const bool newEnable = !p.enabled && enabled;
        if (newEnable) ++p.enableEpoch;
        p.enabled = enabled; p.highQuality = highQuality; p.quality.store(highQuality);
        p.accepting.store(enabled, std::memory_order_release); p.ready.store(false, std::memory_order_release);
        ++p.revision;
        if (newEnable) {
            std::lock_guard<std::mutex> state(p.stateMutex);
            p.state.phase = SegmentationPhase::Loading; p.state.message = "Loading person segmentation";
            p.state.recoveryAttempts = 0; p.state.fallbackOccurred = false;
        }
        std::lock_guard<std::mutex> source(p.submitMutex);
        p.ResetCurrentSource(!enabled);
    }
    p.changed.notify_one();
}
void SegmentationService::ResetSource(uint64_t generation) {
    auto& p = *impl_;
    {
        std::lock_guard<std::mutex> control(p.controlMutex);
        std::lock_guard<std::mutex> source(p.submitMutex);
        p.sourceGeneration = generation; p.sourceWidth = p.sourceHeight = 0; ++p.sourceRevision;
        p.ResetCurrentSource();
    }
    p.changed.notify_one();
}
bool SegmentationService::Submit(const AVFrame* frame, uint64_t generation, uint64_t sequence, int64_t pts) {
    auto& p = *impl_;
    if (!frame || !p.ready.load(std::memory_order_acquire) || !p.accepting.load(std::memory_order_acquire)) return false;
    std::unique_lock<std::mutex> lock(p.submitMutex, std::try_to_lock);
    if (!lock.owns_lock() || generation != p.sourceGeneration || !p.ready.load() || !p.accepting.load()) return false;
    if (frame->width != p.sourceWidth || frame->height != p.sourceHeight) {
        p.ResetCurrentSource(); p.sourceWidth = frame->width; p.sourceHeight = frame->height;
    }
    const bool highQuality = p.quality.load();
    const int64_t interval = highQuality ? 33333 : 66666;
    if (p.haveSubmitted) {
        if (sequence <= p.lastSubmittedSequence || pts == p.lastSubmittedPts) return false;
        if (pts < p.lastSubmittedPts) p.ResetCurrentSource();
        else if (static_cast<uint64_t>(pts) - static_cast<uint64_t>(p.lastSubmittedPts) < static_cast<uint64_t>(interval)) return false;
    }
    auto request = FrameRequest::CreateFromAVFrameScaled(generation, sequence, pts, frame, highQuality ? 256 : 192);
    if (!request || !p.worker.SubmitFrame(std::move(request))) return false;
    p.lastSubmittedPts = pts; p.lastSubmittedSequence = sequence; p.haveSubmitted = true; return true;
}
std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> SegmentationService::GetMailbox() const { return impl_->mailbox; }
SegmentationState SegmentationService::GetState() const {
    auto& p = *impl_; std::lock_guard<std::mutex> lock(p.stateMutex); auto state = p.state;
    const auto metrics = p.worker.GetMetrics(); state.completedFrames = metrics.totalCompleted; state.inferenceErrors = metrics.totalInferenceErrors;
    const auto sourceBaseline = p.sourceErrorBaseline.load(std::memory_order_acquire);
    const auto goodBaseline = p.lastSuccessErrors.load(std::memory_order_acquire);
    state.currentCompletedFrames = p.currentCompleted.load(std::memory_order_acquire);
    state.currentInferenceErrors = metrics.totalInferenceErrors >= sourceBaseline ? metrics.totalInferenceErrors - sourceBaseline : 0;
    state.consecutiveInferenceErrors = metrics.totalInferenceErrors >= goodBaseline ? metrics.totalInferenceErrors - goodBaseline : 0;
    state.highQuality = p.quality.load();
    if (!p.accepting.load(std::memory_order_acquire) && state.phase != SegmentationPhase::Stopped) {
        state.phase = SegmentationPhase::Disabled; state.message = "Background effects are off";
    }
    if (state.phase == SegmentationPhase::Ready && !p.ready.load()) state.phase = p.accepting.load() ? SegmentationPhase::Loading : SegmentationPhase::Disabled;
    return state;
}
void SegmentationService::SetInferenceErrorInjectionForTesting(bool enabled, uint32_t delayMilliseconds) {
    auto& p = *impl_;
    if (!enabled) { p.worker.SetMockInferenceFunction({}); return; }
    const uint32_t delay = std::min(delayMilliseconds, 5000u);
    p.worker.SetMockInferenceFunction([&p, delay](const std::vector<float>&, std::vector<float>&,
        const std::atomic<bool>* cancelled) {
        p.injectedStarts.fetch_add(1, std::memory_order_release);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
        while (std::chrono::steady_clock::now() < until) {
            if (cancelled && cancelled->load(std::memory_order_acquire)) return RunnerStatusCode::Cancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return cancelled && cancelled->load() ? RunnerStatusCode::Cancelled : RunnerStatusCode::InferenceFailed;
    });
}
uint64_t SegmentationService::GetInjectedInferenceStartsForTesting() const { return impl_->injectedStarts.load(std::memory_order_acquire); }
void SegmentationService::Stop() {
    if (!impl_) return; auto& p = *impl_;
    std::lock_guard<std::mutex> shutdown(p.stopMutex);
    {
        std::lock_guard<std::mutex> lock(p.controlMutex);
        p.terminate = true; p.accepting.store(false); p.ready.store(false); ++p.revision;
        std::lock_guard<std::mutex> source(p.submitMutex); p.ResetCurrentSource();
    }
    p.changed.notify_one();
    if (p.controller.joinable()) p.controller.join();
}
} // namespace PrismForge::Providers