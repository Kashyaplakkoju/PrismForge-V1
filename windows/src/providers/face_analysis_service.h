#pragma once
#include "effects/analysisbridge/analysis_types.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
extern "C" { struct AVFrame; }

namespace PrismForge::Providers {
struct FaceAnalysisOptions {
    // All three paths must be absolute local files. The DLL, task, and canonical
    // topology are pinned assets. Resolve executable-relative defaults at the app.
    std::wstring runtimeDllPath;
    std::wstring modelPath;
    std::wstring topologyPath;
};
enum class FaceAnalysisPhase { Disabled, Loading, Ready, Failed, Stopped };
struct FaceAnalysisState {
    FaceAnalysisPhase phase = FaceAnalysisPhase::Disabled;
    bool enabled = false, ready = false, inFlight = false, hasPending = false;
    bool lastDetected = false;
    uint64_t sourceGeneration = 0, submittedFrames = 0, completedFrames = 0;
    uint64_t droppedFrames = 0, timedOutFrames = 0, inferenceErrors = 0;
    uint64_t lastCompletedSequence = 0;
    int64_t lastCompletedPts = 0;
    double lastInferenceMilliseconds = 0;
    std::string message = "Face geometry is off";
    std::string lastError;
};
// One native CPU worker, one owned RGB frame in flight and one latest pending.
// Configure/ResetSource never call the native runtime or join inference. Analyse
// is called on the effects-processing worker, not the GUI or preview-render thread.
// It waits at most the supplied budget (clamped to 0..1000ms), excluding validation
// and the bounded owned BGR->RGB copy. A timeout discards that caller's result.
// Only the exact generation, sequence and original microPTS may be returned.
// personMask in this SPECIAL face-effect bundle is geometric face coverage;
// never publish it to the body segmentation/background mailbox. Coverage is not
// model confidence. Off/no-face/timeout/error return empty, allowing fallback.
class FaceAnalysisService {
public:
    explicit FaceAnalysisService(const FaceAnalysisOptions& options);
    ~FaceAnalysisService();
    FaceAnalysisService(const FaceAnalysisService&) = delete;
    FaceAnalysisService& operator=(const FaceAnalysisService&) = delete;
    void Configure(bool enabled);
    // Equal generation deliberately resets tracking after a resolution change; older generations are rejected.
    void ResetSource(uint64_t sourceGeneration);
    std::shared_ptr<const Effects::AnalysisBridge::AnalysisBundle> Analyse(
        const AVFrame* bgrFrame, uint64_t sourceGeneration, uint64_t sequenceId,
        int64_t microPts, std::chrono::milliseconds budget = std::chrono::milliseconds(16));
    FaceAnalysisState GetState() const;
    // Cooperative join: native initialization/inference/close have no hard bound.
    // Call during application shutdown, never from a GUI toggle or camera reset.
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
