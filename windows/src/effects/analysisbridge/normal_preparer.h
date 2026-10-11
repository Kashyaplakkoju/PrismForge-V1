#pragma once

#include "analysis_types.h"
#include "mask_preparer.h"
#include <memory>

namespace PrismForge::Effects::AnalysisBridge {

struct NormalPreparationConfig {
    MaskResizeMode resizeMode = MaskResizeMode::Stretch;
};

/// <summary>
/// Prepares, resizes, and normalizes surface normal buffers from model/detection resolution
/// to target video frame dimensions.
/// Enforces checked row/stride bounds, checked 64-bit size arithmetic, coordinate mapping,
/// bilinear interpolation, and safe unit vector normalization.
/// </summary>
class NormalPreparer {
public:
    NormalPreparer();
    ~NormalPreparer();

    NormalPreparer(const NormalPreparer&) = delete;
    NormalPreparer& operator=(const NormalPreparer&) = delete;
    NormalPreparer(NormalPreparer&&) noexcept;
    NormalPreparer& operator=(NormalPreparer&&) noexcept;

    /// <summary>
    /// Resizes and normalizes source surface normals to target video frame dimensions.
    /// Safely handles coordinate scaling, bilinear interpolation, non-finite values,
    /// and ensures unit vector normalization on every valid normal.
    /// </summary>
    bool PrepareNormals(
        const NormalBuffer& srcNormals,
        int targetWidth,
        int targetHeight,
        const NormalPreparationConfig& config,
        NormalBuffer& dstNormals
    );

    /// <summary>
    /// Clears any cached internal geometry or scratch state.
    /// </summary>
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::AnalysisBridge
