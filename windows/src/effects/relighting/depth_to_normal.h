#pragma once

#include "depth_types.h"
#include "effects/analysisbridge/analysis_types.h"

namespace PrismForge::Effects::Relighting {

/// <summary>
/// CPU reference implementation for reconstructing 3D surface normals from depth maps
/// and evaluating 3D surface mesh vertex/triangle normals.
/// </summary>
class DepthToNormal {
public:
    DepthToNormal() = default;
    ~DepthToNormal() = default;

    /// <summary>
    /// Reconstructs a NormalBuffer from a DepthBuffer using either Sobel 3x3 or central differences.
    /// Handles invalid depth sentinels, NaNs, Infs, confidence masks, and boundary padding.
    /// Output normals are guaranteed to be normalized unit vectors (or (0,0,1) for invalid points)
    /// with zero non-finite values.
    /// </summary>
    bool ReconstructNormals(
        const DepthBuffer& depth,
        AnalysisBridge::NormalBuffer& outNormals,
        const DepthToNormalConfig& config = DepthToNormalConfig{}
    );

    /// <summary>
    /// Evaluates per-vertex surface normals on an indexed 3D triangle mesh.
    /// </summary>
    bool EvaluateMeshNormals(
        const TriangleMesh3D& inMesh,
        std::vector<MeshVertex3D>& outVerticesWithNormals
    );

    /// <summary>
    /// Rasterizes indexed 3D mesh surface normals onto a 2D NormalBuffer grid.
    /// </summary>
    bool RasterizeMeshNormals(
        const TriangleMesh3D& mesh,
        int width,
        int height,
        AnalysisBridge::NormalBuffer& outNormals
    );

private:
    static inline bool IsValidSample(
        float d,
        float minD,
        float maxD,
        float conf,
        float minConf
    ) {
        if (!std::isfinite(d)) return false;
        if (d < minD || d > maxD) return false;
        if (!std::isfinite(conf) || conf < minConf || conf > 1.0f) return false;
        return true;
    }
};

} // namespace PrismForge::Effects::Relighting
