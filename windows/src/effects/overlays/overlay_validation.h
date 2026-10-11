#pragma once
#include "overlay_types.h"
#include <cmath>
#include "windows/src/effects/analysisbridge/analysis_types.h"

namespace PrismForge::Effects::Overlays {
inline bool MatchesFrame(const AnalysisBridge::AnalysisBundle* analysis, uint64_t generation, uint64_t sequence, int64_t pts) {
    return analysis && analysis->sourceGeneration == generation && analysis->sequenceId == sequence && analysis->timestampPts == pts;
}
inline bool ValidMask(const AnalysisBridge::MaskBuffer& mask) {
    return mask.width > 0 && mask.height > 0 && mask.width <= 16384 && mask.height <= 16384 &&
        mask.strideFloats >= mask.width && int64_t(mask.strideFloats)*mask.height <= 64LL*1024*1024 &&
        mask.data.size() >= uint64_t(mask.strideFloats)*mask.height;
}
inline bool ValidAccessory(const TrackedAccessoryConfig& c) {
    if (!c.enabled) return true;
    return (c.anchorMode == AnchorMode::FaceGlasses || c.anchorMode == AnchorMode::FaceHat) &&
        (c.placement == OverlayPlacement::ForegroundOverSubject || c.placement == OverlayPlacement::BehindSubject) &&
        std::isfinite(c.minConfidence) && c.minConfidence >= 0 && c.minConfidence <= 1 &&
        std::isfinite(c.scaleMultiplier) && c.scaleMultiplier > .001f && c.scaleMultiplier <= 20 &&
        std::isfinite(c.offsetX) && std::abs(c.offsetX) <= 20 && std::isfinite(c.offsetY) && std::abs(c.offsetY) <= 20 &&
        std::isfinite(c.baseOpacity) && c.baseOpacity >= 0 && c.baseOpacity <= 1;
}
inline bool ValidConfiguration(const OverlayStackConfig& c) {
    return ValidAccessory(c.glasses) && ValidAccessory(c.hat) &&
        (!c.studioFrame.enabled || (std::isfinite(c.studioFrame.opacity) && c.studioFrame.opacity >= 0 && c.studioFrame.opacity <= 1));
}
inline bool ValidTransform(const TrackedTransform& t) {
    if (!t.valid) return true;
    return std::isfinite(t.posX) && std::abs(t.posX) <= 1000000 && std::isfinite(t.posY) && std::abs(t.posY) <= 1000000 &&
        std::isfinite(t.scaleX) && t.scaleX > .001f && t.scaleX <= 64 &&
        std::isfinite(t.scaleY) && t.scaleY > .001f && t.scaleY <= 64 &&
        std::isfinite(t.rotationRadians) && std::abs(t.rotationRadians) <= 314.16f &&
        std::isfinite(t.opacity) && t.opacity >= 0 && t.opacity <= 1;
}
struct LayerReport {
    // 0 Applied, 1 Bypassed, 2 NotReady, 3 Failed; matches LayerStatus.
    int glasses = 1, hat = 1, frame = 1;
    int faceFilter = 1;
};
}
