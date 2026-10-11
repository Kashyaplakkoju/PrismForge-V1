#pragma once

#include "overlay_types.h"
#include "procedural_assets.h"
#include "tracking_evaluator.h"
#include "overlay_validation.h"
#include <memory>
#include <vector>
#include <mutex>

struct AVFrame;

namespace PrismForge::Effects::AnalysisBridge {
    struct FaceLandmarkBundle;
    struct MaskBuffer;
    struct AnalysisBundle;
}

namespace PrismForge::Effects::Overlays {

class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    // Non-copyable
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /// <summary>
    /// Resets all internal temporal smoothing and tracking states.
    /// Call when switching camera sources or upon sequence disconnection.
    /// </summary>
    void Reset();

    /// <summary>
    /// Processes a single BGR24 AVFrame with optional landmarks, subject mask, and outcome.
    /// </summary>
    AVFrame* ProcessFrame(
        AVFrame* inputFrame,
        const OverlayStackConfig& config,
        const AnalysisBridge::FaceLandmarkBundle* landmarks = nullptr,
        const AnalysisBridge::MaskBuffer* subjectMask = nullptr,
        OverlayOutcome* outOutcome = nullptr,
        TrackingEvaluator* externalEvaluator = nullptr,
        uint64_t sourceGeneration = 0, int64_t timestampPts = 0, uint64_t sequenceId = 0);

    /// <summary>
    /// Overload taking full AnalysisBundle for seamless stack integration.
    /// </summary>
    AVFrame* ProcessFrameWithAnalysis(
        AVFrame* inputFrame,
        const OverlayStackConfig& config,
        const AnalysisBridge::AnalysisBundle* analysisBundle,
        OverlayOutcome* outOutcome = nullptr,
        TrackingEvaluator* externalEvaluator = nullptr,
        uint64_t sourceGeneration = 0, int64_t timestampPts = 0, uint64_t sequenceId = 0);

    /// <summary>
    /// Retrieves current runtime tracking transform for an accessory (for telemetry / debugging).
    /// </summary>
    TrackedTransform GetTrackedTransform(AnchorMode mode) const;

    /// <summary>
    /// Evaluates target landmark transform for a given accessory without mutating renderer state.
    /// </summary>
    static TrackedTransform ComputeTargetTransform(
        AnchorMode mode,
        const TrackedAccessoryConfig& config,
        const AnalysisBridge::FaceLandmarkBundle& landmarks,
        int frameWidth,
        int frameHeight,
        const AssetAnchorMetrics& metrics);

private:
    void CompositeLayer(
        AVFrame* targetFrame,
        const RgbaImageBuffer& asset,
        const AssetAnchorMetrics& metrics,
        const TrackedTransform& transform,
        OverlayPlacement placement,
        const AnalysisBridge::MaskBuffer* subjectMask);

    void CompositeScreenFrame(
        AVFrame* targetFrame,
        const RgbaImageBuffer& frameAsset,
        float opacity);

    AVFrame* AllocateOrReuseOutput(const AVFrame* inputFrame);

private:
    mutable std::mutex m_mutex;
    AVFrame* m_cachedFrame = nullptr;
    TrackingEvaluator m_evaluator;
};

} // namespace PrismForge::Effects::Overlays
