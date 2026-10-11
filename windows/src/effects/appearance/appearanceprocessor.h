#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

struct AVFrame;

namespace PrismForge::Effects::Appearance {

/// <summary>
/// Status outcome of an AppearanceProcessor execution.
/// </summary>
enum class AppearanceOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    OutOfMemory = 5
};

/// <summary>
/// Configuration for appearance touchup, edge-aware smoothing, and detail preservation.
/// </summary>
struct AppearanceConfig {
    float smoothingStrength = 0.0f;    // [0.0, 1.0] Restrained edge-aware smoothing amount
    float detailPreservation = 0.5f;   // [0.0, 1.0] Retention ratio of high-frequency surface detail
    float sharpenStrength = 0.0f;      // [0.0, 1.0] Subtle unsharp mask with bounded halos
    bool bypass = false;               // If true, exact bit-level passthrough
    float touchupBoost = 0.0f;        // [0.0, 1.0] Optional wider/stronger smoothing; zero preserves legacy kernels

    /// <summary>
    /// Creates neutral identity default (bypass true, zero adjustments).
    /// </summary>
    static AppearanceConfig Neutral() {
        AppearanceConfig cfg;
        cfg.smoothingStrength = 0.0f;
        cfg.detailPreservation = 1.0f;
        cfg.sharpenStrength = 0.0f;
        cfg.bypass = true;
        return cfg;
    }

    /// <summary>
    /// Creates a natural, subtle portrait touchup preset.
    /// </summary>
    static AppearanceConfig SubtleTouchup() {
        AppearanceConfig cfg;
        cfg.smoothingStrength = 0.45f;
        cfg.detailPreservation = 0.65f;
        cfg.sharpenStrength = 0.20f;
        cfg.bypass = false;
        return cfg;
    }
};

/// <summary>
/// Descriptor for externally supplied skin/face mask.
/// Values are normalized floats in [0.0, 1.0] (1.0 = skin/face region, 0.0 = background/other).
/// </summary>
struct SkinMask {
    const float* data = nullptr;
    int width = 0;
    int height = 0;
    int strideFloats = 0;
};

/// <summary>
/// Descriptor for optional eye/mouth/hair protection mask.
/// Values are normalized floats in [0.0, 1.0] (1.0 = fully protected from smoothing, 0.0 = unprotected).
/// </summary>
struct ProtectionMask {
    const float* data = nullptr;
    int width = 0;
    int height = 0;
    int strideFloats = 0;
};

/// <summary>
/// Standalone CPU Appearance Processor.
///
/// Implements restrained edge-aware skin smoothing with independent detail preservation
/// and bounded-halo unsharp sharpening.
///
/// Guarantees:
/// - Deterministic neutral/bypass identity (diff = 0).
/// - Exact background preservation (where skinMask == 0).
/// - Feature protection: regions under protectionMask (eyes, mouth, brows) remain sharp and unsmoothed.
/// - Zero color/hue shifting: operates on luminance and preserves original chrominance.
/// - Thread safety: per-instance caller-thread owned.
/// </summary>
class AppearanceProcessor {
public:
    AppearanceProcessor();
    ~AppearanceProcessor();

    AppearanceProcessor(const AppearanceProcessor&) = delete;
    AppearanceProcessor& operator=(const AppearanceProcessor&) = delete;
    AppearanceProcessor(AppearanceProcessor&&) noexcept;
    AppearanceProcessor& operator=(AppearanceProcessor&&) noexcept;

    /// <summary>
    /// Processes a single SDR BGR24 software frame.
    /// </summary>
    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const SkinMask* skinMask,
        const ProtectionMask* protectionMask,
        const AppearanceConfig& config,
        AppearanceOutcome& outcome
    );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::Appearance
