#pragma once

#include "overlay_types.h"
#include "overlay_validation.h"
#include "procedural_assets.h"
#include "windows/src/effects/analysisbridge/analysis_types.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <mutex>

namespace PrismForge::Effects::Overlays {

/// <summary>
/// Status of an individual overlay layer evaluation.
/// </summary>
enum class LayerStatus : int {
    Applied = 0,
    Bypassed = 1,
    NotReady = 2,
    Failed = 3
};

/// <summary>
/// Stateful runtime tracking state for an individual accessory layer.
/// Tracks current smoothed transform, snap status, and loss-of-tracking frames.
/// </summary>
struct TrackedLayerState {
    TrackedTransform current;
    bool hasSnapped = false;
    int consecutiveLostFrames = 0;
    bool nativeGeometry = false;
};

/// <summary>
/// Small, stateful tracking transform evaluator shared between CPU and GPU renderers.
/// Handles source/frame-aligned face landmarks, confidence policies, eye/forehead anchors,
/// scale/offset/rotation, temporal exponential smoothing, and bounded loss-of-tracking fade.
/// Guarantees that CPU and GPU execution backends produce identical transform matrices.
/// </summary>
class TrackingEvaluator {
public:
    TrackingEvaluator() {
        Reset();
    }

    /// <summary>
    /// Resets all internal temporal smoothing and tracking state.
    /// Call on source change, discontinuity, or session reset.
    /// </summary>
    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_glassesState = TrackedLayerState();
        m_hatState = TrackedLayerState();
        m_currentSourceGeneration = 0;
        m_lastTimestampPts = -1;
        m_lastSequenceId = 0;
        m_lastWidth = m_lastHeight = 0;
    }

    /// <summary>
    /// Checks for source generation discontinuity and resets state if superseded.
    /// </summary>
    void CheckSourceDiscontinuity(uint64_t sourceGen, int64_t timestampPts) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (sourceGen != 0 && m_currentSourceGeneration != 0 && sourceGen != m_currentSourceGeneration) {
            m_glassesState = TrackedLayerState();
            m_hatState = TrackedLayerState();
        }
        // Also detect backward timestamp jumps (> 1 second)
        if (m_lastTimestampPts >= 0 && timestampPts < m_lastTimestampPts - 10000000LL) {
            m_glassesState = TrackedLayerState();
            m_hatState = TrackedLayerState();
        }
        m_currentSourceGeneration = sourceGen;
        m_lastTimestampPts = timestampPts;
    }

    /// <summary>
    /// Evaluates target landmark transform for a given accessory without mutating state.
    /// Validates all inputs: finite numbers, positive dimensions, bounded scales and offsets.
    /// </summary>
    static TrackedTransform ComputeTargetTransform(
        AnchorMode mode,
        const TrackedAccessoryConfig& config,
        const AnalysisBridge::FaceLandmarkBundle& landmarks,
        int frameWidth,
        int frameHeight,
        const AssetAnchorMetrics& metrics)
    {
        TrackedTransform t;
        t.valid = false;
        t.opacity = 0.0f;

        if (!config.enabled || !ValidAccessory(config) || !landmarks.detected ||
            landmarks.overallConfidence > 1 || landmarks.leftEye.confidence > 1 || landmarks.rightEye.confidence > 1 ||
            landmarks.leftEye.x < 0 || landmarks.leftEye.x > 1 || landmarks.rightEye.x < 0 || landmarks.rightEye.x > 1 ||
            landmarks.leftEye.y < 0 || landmarks.leftEye.y > 1 || landmarks.rightEye.y < 0 || landmarks.rightEye.y > 1) {
            return t;
        }

        // Validate frame dimensions
        if (frameWidth <= 0 || frameHeight <= 0 || frameWidth > 16384 || frameHeight > 16384) {
            return t;
        }

        // Validate config parameters for arithmetic safety
        if (!std::isfinite(config.minConfidence) || config.minConfidence < 0.0f || config.minConfidence > 1.0f) {
            return t;
        }
        if (!std::isfinite(config.scaleMultiplier) || config.scaleMultiplier <= 0.001f || config.scaleMultiplier > 20.0f) {
            return t;
        }
        if (!std::isfinite(config.baseOpacity) || config.baseOpacity < 0.0f || config.baseOpacity > 1.0f) {
            return t;
        }
        if (!std::isfinite(config.offsetX) || std::abs(config.offsetX) > 20.0f ||
            !std::isfinite(config.offsetY) || std::abs(config.offsetY) > 20.0f) {
            return t;
        }
        if (!std::isfinite(metrics.referenceBaseline) || metrics.referenceBaseline <= 0.1f) {
            return t;
        }
        if (!std::isfinite(metrics.anchorX) || !std::isfinite(metrics.anchorY)) {
            return t;
        }

        const bool native = landmarks.evidence == AnalysisBridge::FaceLandmarkEvidence::ValidatedNativeGeometry;
        if (native && !landmarks.HasValidatedNativeGeometry(frameWidth,frameHeight)) return t;
        // Calibrated external providers keep their confidence policy. Native
        // validity uses the explicit geometrical contract, with confidence=0.
        if (!native && (!std::isfinite(landmarks.overallConfidence) || landmarks.overallConfidence < config.minConfidence)) {
            return t;
        }

        if (mode == AnchorMode::FaceGlasses) {
            if (!native && (!std::isfinite(landmarks.leftEye.confidence) || landmarks.leftEye.confidence < config.minConfidence ||
                !std::isfinite(landmarks.rightEye.confidence) || landmarks.rightEye.confidence < config.minConfidence)) {
                return t;
            }

            if (!std::isfinite(landmarks.leftEye.x) || !std::isfinite(landmarks.leftEye.y) ||
                !std::isfinite(landmarks.rightEye.x) || !std::isfinite(landmarks.rightEye.y)) {
                return t;
            }

            float xL = landmarks.leftEye.x * frameWidth;
            float yL = landmarks.leftEye.y * frameHeight;
            float xR = landmarks.rightEye.x * frameWidth;
            float yR = landmarks.rightEye.y * frameHeight;

            float dx = xR - xL;
            float dy = yR - yL;
            float eyeDist = std::sqrt(dx * dx + dy * dy);
            if (!std::isfinite(eyeDist) || eyeDist < 2.0f) {
                return t;
            }

            float angle = std::atan2(dy, dx);
            float midX = (xL + xR) * 0.5f;
            float midY = (yL + yR) * 0.5f;

            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            float offX = config.offsetX * eyeDist;
            float offY = config.offsetY * eyeDist;

            t.posX = midX + (cosA * offX - sinA * offY);
            t.posY = midY + (sinA * offX + cosA * offY);
            t.rotationRadians = angle;
            t.scaleX = (eyeDist / metrics.referenceBaseline) * config.scaleMultiplier;
            t.scaleY = t.scaleX;
            t.opacity = std::max(0.0f, std::min(1.0f, config.baseOpacity));
            t.valid = std::isfinite(t.posX) && std::isfinite(t.posY) &&
                      std::isfinite(t.scaleX) && std::isfinite(t.rotationRadians) && ValidTransform(t);
        }
        else if (mode == AnchorMode::FaceHat) {
            if (!native && (!std::isfinite(landmarks.leftEye.confidence) || landmarks.leftEye.confidence < config.minConfidence ||
                !std::isfinite(landmarks.rightEye.confidence) || landmarks.rightEye.confidence < config.minConfidence)) {
                return t;
            }

            if (!std::isfinite(landmarks.leftEye.x) || !std::isfinite(landmarks.leftEye.y) ||
                !std::isfinite(landmarks.rightEye.x) || !std::isfinite(landmarks.rightEye.y)) {
                return t;
            }

            float xL = landmarks.leftEye.x * frameWidth;
            float yL = landmarks.leftEye.y * frameHeight;
            float xR = landmarks.rightEye.x * frameWidth;
            float yR = landmarks.rightEye.y * frameHeight;

            float dx = xR - xL;
            float dy = yR - yL;
            float eyeDist = std::sqrt(dx * dx + dy * dy);
            if (!std::isfinite(eyeDist) || eyeDist < 2.0f) {
                return t;
            }

            float angle = std::atan2(dy, dx);
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);

            float anchorPxX = 0.0f;
            float anchorPxY = 0.0f;

            if ((native || (std::isfinite(landmarks.foreheadCenter.confidence) &&
                landmarks.foreheadCenter.confidence >= config.minConfidence && landmarks.foreheadCenter.confidence <= 1)) &&
                landmarks.foreheadCenter.x >= 0 && landmarks.foreheadCenter.x <= 1 &&
                landmarks.foreheadCenter.y >= 0 && landmarks.foreheadCenter.y <= 1 &&
                std::isfinite(landmarks.foreheadCenter.x) &&
                std::isfinite(landmarks.foreheadCenter.y))
            {
                anchorPxX = landmarks.foreheadCenter.x * frameWidth;
                anchorPxY = landmarks.foreheadCenter.y * frameHeight;
            } else {
                // Extrapolate forehead point along perpendicular upward normal
                float upX = dy / eyeDist;
                float upY = -dx / eyeDist;
                float midX = (xL + xR) * 0.5f;
                float midY = (yL + yR) * 0.5f;
                anchorPxX = midX + upX * (eyeDist * 0.65f);
                anchorPxY = midY + upY * (eyeDist * 0.65f);
            }

            float offX = config.offsetX * eyeDist;
            float offY = config.offsetY * eyeDist;

            t.posX = anchorPxX + (cosA * offX - sinA * offY);
            t.posY = anchorPxY + (sinA * offX + cosA * offY);
            t.rotationRadians = angle;
            t.scaleX = (eyeDist / metrics.referenceBaseline) * 1.15f * config.scaleMultiplier;
            t.scaleY = t.scaleX;
            t.opacity = std::max(0.0f, std::min(1.0f, config.baseOpacity));
            t.valid = std::isfinite(t.posX) && std::isfinite(t.posY) &&
                      std::isfinite(t.scaleX) && std::isfinite(t.rotationRadians) && ValidTransform(t);
        }

        return t;
    }

    /// <summary>
    /// Evaluates and advances the temporal smoothing state for all active layers.
    /// Thread-safe and shared across CPU and GPU pipelines.
    /// </summary>
    void Update(
        const OverlayStackConfig& config,
        const AnalysisBridge::FaceLandmarkBundle* landmarks,
        int frameWidth,
        int frameHeight,
        uint64_t sourceGen = 0,
        int64_t timestampPts = 0,
        uint64_t sequenceId = 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Native observations are exact-frame only. Missing/late geometry must
        // not leave a faded accessory attached to another face or source.
        if (landmarks && landmarks->evidence == AnalysisBridge::FaceLandmarkEvidence::ValidatedNativeGeometry &&
            !landmarks->NativeGeometryMatches(frameWidth,frameHeight,sourceGen,sequenceId,timestampPts)) landmarks=nullptr;
        if (!landmarks) {
            if (m_glassesState.nativeGeometry) m_glassesState={};
            if (m_hatState.nativeGeometry) m_hatState={};
        }
        // Advance at most once for a tagged frame, including GPU -> CPU fallback.
        if (sequenceId && sequenceId == m_lastSequenceId && sourceGen == m_currentSourceGeneration &&
            timestampPts == m_lastTimestampPts && frameWidth == m_lastWidth && frameHeight == m_lastHeight) return;
        if ((m_lastWidth && (frameWidth != m_lastWidth || frameHeight != m_lastHeight)) ||
            (sequenceId && m_lastSequenceId && sequenceId < m_lastSequenceId) ||
            (m_lastTimestampPts >= 0 && (timestampPts < m_lastTimestampPts || timestampPts - m_lastTimestampPts > 10000000LL))) {
            m_glassesState = {}; m_hatState = {};
        }
        m_lastWidth = frameWidth; m_lastHeight = frameHeight;
        // Check discontinuity
        if (sourceGen != 0 && m_currentSourceGeneration != 0 && sourceGen != m_currentSourceGeneration) {
            m_glassesState = TrackedLayerState();
            m_hatState = TrackedLayerState();
        }
        if (m_lastTimestampPts >= 0 && timestampPts < m_lastTimestampPts - 10000000LL) {
            m_glassesState = TrackedLayerState();
            m_hatState = TrackedLayerState();
        }
        m_currentSourceGeneration = sourceGen;
        m_lastTimestampPts = timestampPts;
        m_lastSequenceId = sequenceId;

        // Retrieve procedural metrics
        AssetAnchorMetrics glassesMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioAviatorGlasses);
        AssetAnchorMetrics hatMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioCap);

        UpdateLayer(m_glassesState, config.glasses, landmarks, frameWidth, frameHeight, glassesMetrics);
        UpdateLayer(m_hatState, config.hat, landmarks, frameWidth, frameHeight, hatMetrics);
    }

    TrackedTransform GetTrackedTransform(AnchorMode mode) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (mode == AnchorMode::FaceGlasses) {
            return m_glassesState.current;
        } else if (mode == AnchorMode::FaceHat) {
            return m_hatState.current;
        }
        TrackedTransform t;
        t.valid = false;
        return t;
    }

    TrackedTransform GetGlassesTransform() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_glassesState.current;
    }

    TrackedTransform GetHatTransform() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_hatState.current;
    }

    /// <summary>
    /// Explicit per-layer outcome determination for pipeline telemetry and truthful status reporting.
    /// </summary>
    void QueryLayerStatuses(
        const OverlayStackConfig& config,
        LayerStatus& outGlasses,
        LayerStatus& outHat,
        LayerStatus& outFrame) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Glasses
        if (!config.glasses.enabled || config.glasses.baseOpacity <= .001f || config.bypass) {
            outGlasses = LayerStatus::Bypassed;
        } else if (m_glassesState.current.opacity > 0.001f && m_glassesState.current.valid) {
            outGlasses = LayerStatus::Applied;
        } else {
            outGlasses = LayerStatus::NotReady;
        }

        // Hat
        if (!config.hat.enabled || config.hat.baseOpacity <= .001f || config.bypass) {
            outHat = LayerStatus::Bypassed;
        } else if (m_hatState.current.opacity > 0.001f && m_hatState.current.valid) {
            outHat = LayerStatus::Applied;
        } else {
            outHat = LayerStatus::NotReady;
        }

        // Screen Frame
        if (!config.studioFrame.enabled || config.studioFrame.opacity <= .001f || config.bypass) {
            outFrame = LayerStatus::Bypassed;
        } else if (config.studioFrame.opacity > 0.001f) {
            outFrame = LayerStatus::Applied;
        } else {
            outFrame = LayerStatus::NotReady;
        }
    }

private:
    static void UpdateLayer(
        TrackedLayerState& state,
        const TrackedAccessoryConfig& config,
        const AnalysisBridge::FaceLandmarkBundle* landmarks,
        int frameWidth,
        int frameHeight,
        const AssetAnchorMetrics& metrics)
    {
        float fadeOut = (std::isfinite(config.smoothing.fadeOutSpeed) && config.smoothing.fadeOutSpeed > 0.001f)
            ? config.smoothing.fadeOutSpeed : 0.15f;
        float fadeIn = (std::isfinite(config.smoothing.fadeInSpeed) && config.smoothing.fadeInSpeed > 0.001f)
            ? config.smoothing.fadeInSpeed : 0.20f;

        if (!config.enabled) { state = {}; return; }
        if (!landmarks) {
            if (state.nativeGeometry) {state={};return;}
            state.current.opacity = std::max(0.0f, state.current.opacity - fadeOut);
            state.current.valid = (state.current.opacity > 0.001f);
            if (!state.current.valid) {
                state.hasSnapped = false;
            }
            if (++state.consecutiveLostFrames >= 60) state = {};
            return;
        }

        TrackedTransform target = ComputeTargetTransform(
            config.anchorMode, config, *landmarks, frameWidth, frameHeight, metrics);

        if (!target.valid) {
            if (state.nativeGeometry || landmarks->evidence == AnalysisBridge::FaceLandmarkEvidence::ValidatedNativeGeometry) {state={};return;}
            state.current.opacity = std::max(0.0f, state.current.opacity - fadeOut);
            state.current.valid = (state.current.opacity > 0.001f);
            if (!state.current.valid) {
                state.hasSnapped = false;
            }
            if (++state.consecutiveLostFrames >= 60) state = {};
            return;
        }

        // Target is valid and tracked. Switching evidence modes snaps once.
        const bool native=landmarks->evidence == AnalysisBridge::FaceLandmarkEvidence::ValidatedNativeGeometry;
        if (state.hasSnapped && state.nativeGeometry != native) state={};
        state.nativeGeometry=native;
        state.consecutiveLostFrames = 0;
        if (!state.hasSnapped || state.current.opacity <= 0.001f || !config.smoothing.enabled) {
            // Snap immediately on first valid detection
            state.current.posX = target.posX;
            state.current.posY = target.posY;
            state.current.scaleX = target.scaleX;
            state.current.scaleY = target.scaleY;
            state.current.rotationRadians = target.rotationRadians;
            state.current.opacity = config.smoothing.enabled ? std::min(target.opacity, fadeIn) : target.opacity;
            state.current.valid = true;
            state.hasSnapped = true;
        } else {
            // Temporal exponential moving average smoothing
            float alphaP = (std::isfinite(config.smoothing.positionAlpha))
                ? std::max(0.05f, std::min(1.0f, config.smoothing.positionAlpha)) : 0.35f;
            state.current.posX = alphaP * target.posX + (1.0f - alphaP) * state.current.posX;
            state.current.posY = alphaP * target.posY + (1.0f - alphaP) * state.current.posY;

            float alphaS = (std::isfinite(config.smoothing.scaleAlpha))
                ? std::max(0.05f, std::min(1.0f, config.smoothing.scaleAlpha)) : 0.35f;
            state.current.scaleX = alphaS * target.scaleX + (1.0f - alphaS) * state.current.scaleX;
            state.current.scaleY = state.current.scaleX;

            // Angular difference with wrap-around [-pi, pi]
            float deltaAngle = std::atan2(
                std::sin(target.rotationRadians - state.current.rotationRadians),
                std::cos(target.rotationRadians - state.current.rotationRadians));
            float alphaR = (std::isfinite(config.smoothing.rotationAlpha))
                ? std::max(0.05f, std::min(1.0f, config.smoothing.rotationAlpha)) : 0.30f;
            state.current.rotationRadians += alphaR * deltaAngle;

            // Smooth fade-in
            state.current.opacity = std::min(target.opacity, state.current.opacity + fadeIn);
            state.current.valid = true;
        }
    }

private:
    mutable std::mutex m_mutex;
    TrackedLayerState m_glassesState;
    TrackedLayerState m_hatState;
    uint64_t m_currentSourceGeneration = 0;
    int64_t m_lastTimestampPts = -1;
    uint64_t m_lastSequenceId = 0;
    int m_lastWidth = 0, m_lastHeight = 0;
};

} // namespace PrismForge::Effects::Overlays
