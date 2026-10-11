#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif

struct AVFrame;

namespace PrismForge::Effects::AnalysisBridge {
    struct FaceLandmarkBundle;
    struct MaskBuffer;
}

namespace PrismForge::Effects::Overlays {

/// <summary>
/// Status outcome of an overlay processing pass.
/// </summary>
enum class OverlayOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    OutOfMemory = 5
};

/// <summary>
/// Color alpha blending contract for rasterized assets.
/// </summary>
enum class AlphaContract : int {
    Straight = 0,       // RGB are unmultiplied; blend formula: dst = src * alpha + dst * (1 - alpha)
    Premultiplied = 1  // RGB are pre-scaled by alpha; blend formula: dst = src + dst * (1 - alpha)
};

/// <summary>
/// Layer placement relative to video subject.
/// </summary>
enum class OverlayPlacement : int {
    ForegroundOverSubject = 0, // In front of subject and background
    SubjectBehindForeground = 0,
    BehindSubject = 1,          // Modulated by inverted subject mask (behind person)
    ScreenSpace = 2             // Fixed to viewport coordinates
};

/// <summary>
/// Anchoring mode for tracked accessories.
/// </summary>
enum class AnchorMode : int {
    FaceGlasses = 0,      // Anchored between eyes (leftEye, rightEye, noseBridge)
    FaceHat = 1,          // Anchored above forehead (foreheadCenter, eye baseline angle)
    CustomLandmarks = 2,  // Custom anchor point calculation
    ScreenSpace = 3       // Viewport relative [0, 1]
};

/// <summary>
/// 32-bit RGBA raster bitmap buffer with explicit alpha contract.
/// </summary>
struct RgbaImageBuffer {
    int width = 0;
    int height = 0;
    int strideBytes = 0; // bytes per row (>= width * 4)
    AlphaContract alphaContract = AlphaContract::Straight;
    std::vector<uint8_t> data; // RGBA 8-bit per channel

    bool IsValid() const {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || strideBytes < width * 4) {
            return false;
        }
        int64_t expectedSize = static_cast<int64_t>(strideBytes) * static_cast<int64_t>(height);
        if (expectedSize <= 0 || expectedSize > 1024LL * 1024LL * 1024LL) {
            return false;
        }
        return (alphaContract == AlphaContract::Straight || alphaContract == AlphaContract::Premultiplied) &&
            expectedSize <= 64LL * 1024 * 1024 && data.size() >= static_cast<size_t>(expectedSize);
    }
};

/// <summary>
/// 2D affine transform parameters for tracked accessory rendering.
/// </summary>
struct TrackedTransform {
    float posX = 0.0f;           // Center X in destination frame pixels
    float posY = 0.0f;           // Center Y in destination frame pixels
    float scaleX = 1.0f;         // Horizontal scale factor
    float scaleY = 1.0f;         // Vertical scale factor
    float rotationRadians = 0.0f;// Rotation in radians (positive = clockwise)
    float opacity = 0.0f;        // Current effective opacity [0.0, 1.0] (for smooth fades)
    bool valid = false;          // True if recent landmarks provided valid anchor
};

/// <summary>
/// Smoothing parameters for temporal tracking stability.
/// </summary>
struct SmoothingConfig {
    bool enabled = true;
    float positionAlpha = 0.35f;   // Exponential moving average weight for position [0.05, 1.0]
    float scaleAlpha = 0.35f;      // Exponential moving average weight for scale
    float rotationAlpha = 0.30f;   // Exponential moving average weight for rotation
    float fadeInSpeed = 0.20f;     // Opacity delta increase per frame when tracked
    float fadeOutSpeed = 0.15f;    // Opacity delta decrease per frame when tracking is lost
};

/// <summary>
/// Configuration for an individual tracked accessory layer.
/// </summary>
struct TrackedAccessoryConfig {
    bool enabled = false;
    AnchorMode anchorMode = AnchorMode::FaceGlasses;
    float minConfidence = 0.40f;   // Minimum required landmark confidence
    float scaleMultiplier = 1.0f;  // User fine-tuning scale adjustment
    float offsetX = 0.0f;          // Normalized horizontal offset relative to eye distance
    float offsetY = 0.0f;          // Normalized vertical offset relative to eye distance
    float baseOpacity = 1.0f;      // Target opacity when fully visible [0.0, 1.0]
    OverlayPlacement placement = OverlayPlacement::ForegroundOverSubject;
    SmoothingConfig smoothing;
};

/// <summary>
/// Configuration for a screen-space overlay (e.g. decorative studio border).
/// </summary>
struct ScreenOverlayConfig {
    bool enabled = false;
    float opacity = 1.0f;          // [0.0, 1.0]
};

/// <summary>
/// Unified configuration for all active overlays and accessories.
/// </summary>
struct OverlayStackConfig {
    bool bypass = false;
    TrackedAccessoryConfig glasses;
    TrackedAccessoryConfig hat{false, AnchorMode::FaceHat};
    ScreenOverlayConfig studioFrame;

    static OverlayStackConfig Neutral() {
        OverlayStackConfig cfg;
        cfg.bypass = true;
        cfg.glasses.enabled = false;
        cfg.hat.enabled = false;
        cfg.studioFrame.enabled = false;
        return cfg;
    }
};

} // namespace PrismForge::Effects::Overlays
