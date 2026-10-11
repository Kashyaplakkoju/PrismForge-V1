#pragma once

#include "analysis_types.h"
#include <memory>

namespace PrismForge::Effects::AnalysisBridge {

enum class MaskResizeMode : int {
    Stretch = 0,    // Stretch non-uniformly to target dimensions
    Letterbox = 1,  // Fit preserving aspect ratio with zero padding
    CoverCrop = 2   // Fill preserving aspect ratio, cropping excess
};

struct MaskPreparationConfig {
    MaskResizeMode resizeMode = MaskResizeMode::Stretch;
    float featherRadius = 2.0f; // Radius in pixels for edge smoothing (0 = disabled)
    bool enableTemporalStabilization = true; // Call MaskStabilizer on person mask
};

/// <summary>
/// Prepares, resizes, and feathers model-generated masks to match target video frame geometry.
/// Supports aspect ratio preservation, edge feathering, and optional temporal stabilization.
/// </summary>
class MaskPreparer {
public:
    MaskPreparer();
    ~MaskPreparer();

    MaskPreparer(const MaskPreparer&) = delete;
    MaskPreparer& operator=(const MaskPreparer&) = delete;
    MaskPreparer(MaskPreparer&&) noexcept;
    MaskPreparer& operator=(MaskPreparer&&) noexcept;

    /// <summary>
    /// Resizes, crops/letterboxes, and feathers a source mask to target video frame dimensions.
    /// </summary>
    bool PrepareMask(
        const MaskBuffer& srcMask,
        int targetWidth,
        int targetHeight,
        const MaskPreparationConfig& config,
        MaskBuffer& dstMask
    );

    /// <summary>
    /// Clears temporal stabilization history on source or scene changes.
    /// </summary>
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::AnalysisBridge
