#pragma once
#include "analysis_worker.h"
#include <memory>
#include <string>

namespace PrismForge::Providers {
enum class SegmentationPhase { Disabled, Loading, Ready, Failed, Stopped, Recovering };
struct SegmentationState {
    SegmentationPhase phase = SegmentationPhase::Disabled;
    bool highQuality = true;
    ExecutionProviderType activeProvider = ExecutionProviderType::CPU;
    bool fallbackOccurred = false;
    std::string message = "Background effects are off";
    uint64_t completedFrames = 0, inferenceErrors = 0; // Lifetime metrics.
    uint64_t currentCompletedFrames = 0, currentInferenceErrors = 0;
    uint64_t consecutiveInferenceErrors = 0;
    uint32_t recoveryAttempts = 0, maxRecoveryAttempts = 3;
};
struct SegmentationOptions {
    // Empty defaults are executable-local, independent of launch working directory.
    std::wstring modelDirectory;
    std::wstring runtimeDllPath;
    ExecutionProviderType requestedProvider = ExecutionProviderType::DirectML;
    int deviceId = 0;
};
// One control thread loads/stops the reviewed one-inference + latest-pending worker.
// Configure never loads a model or waits on inference. Submit does small RGB resize only,
// skips frames while unavailable/busy, and throttles analysis to 15/30 Hz independent of
// capture/render FPS. Immutable low-resolution masks represent the entire source image.
class SegmentationService {
public:
    explicit SegmentationService(const SegmentationOptions& options = {});
    ~SegmentationService();
    SegmentationService(const SegmentationService&) = delete;
    SegmentationService& operator=(const SegmentationService&) = delete;
    void Configure(bool enabled, bool highQuality = true);
    void ResetSource(uint64_t sourceGeneration);
    bool Submit(const AVFrame* frame, uint64_t sourceGeneration, uint64_t sequenceId, int64_t microPts);
    std::shared_ptr<Effects::AnalysisBridge::AnalysisMailbox> GetMailbox() const;
    SegmentationState GetState() const;
    void Stop();
    // Explicit error/cancellation-only test seam. Clear before real inference; never produces a mask.
    void SetInferenceErrorInjectionForTesting(bool enabled, uint32_t delayMilliseconds = 0);
    uint64_t GetInjectedInferenceStartsForTesting() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace PrismForge::Providers
