#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include "effects/analysisbridge/analysis_types.h"

namespace PrismForge::Effects::Relighting {

/// <summary>
/// Depth coordinate representation convention.
/// Model-relative values represent normalized camera/face depth coordinates,
/// NOT absolute metric distances in physical meters.
/// </summary>
enum class DepthFormat : int {
    // Relative depth: z in [0.0, 1.0], where 0.0 is the near plane (closest to viewer)
    // and 1.0 is the far plane (background). Gradient dZ/dx > 0 on right slope.
    RelativeDepth = 0,

    // Inverse depth / disparity: d = 1/z in [0.0, 1.0], where 1.0 is the near plane
    // (closest to viewer) and 0.0 is infinity/far. Gradient dd/dx < 0 on right slope.
    InverseDepth = 1
};

/// <summary>
/// Alias to AnalysisBridge::DepthBuffer for seamless integration with analysis bundles.
/// </summary>
using DepthBuffer = PrismForge::Effects::AnalysisBridge::DepthBuffer;

/// <summary>
/// 3D vertex for validated mesh geometry.
/// </summary>
struct MeshVertex3D {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
};

/// <summary>
/// 3-index triangle.
/// </summary>
struct TriangleIndices {
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
};

/// <summary>
/// Validated indexed 3D surface mesh.
/// </summary>
struct TriangleMesh3D {
    std::vector<MeshVertex3D> vertices;
    std::vector<TriangleIndices> indices;

    bool IsValid() const {
        if (vertices.empty() || indices.empty()) return false;
        if (vertices.size() > std::numeric_limits<uint32_t>::max()) return false;
        for (const auto& vertex : vertices) {
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
                !std::isfinite(vertex.u) || !std::isfinite(vertex.v) ||
                vertex.u < 0.0f || vertex.u > 1.0f || vertex.v < 0.0f || vertex.v > 1.0f) return false;
        }
        uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
        for (const auto& tri : indices) {
            if (tri.v0 >= vertexCount || tri.v1 >= vertexCount || tri.v2 >= vertexCount) {
                return false;
            }
        }
        return true;
    }
};

/// <summary>
/// Configuration for depth-to-normal reconstruction.
/// </summary>
struct DepthToNormalConfig {
    DepthFormat format = DepthFormat::RelativeDepth;
    float depthScale = 1.0f;
    float minConfidence = 0.1f;
    float invalidDepthMin = 0.0f;
    float invalidDepthMax = 1.0f;
    bool useSobel = true; // 3x3 Sobel filter (true) vs central difference (false)
    bool flipX = false;   // Horizontal mirroring
    bool flipY = false;   // Vertical mirroring
    float fallbackZ = 1.0f; // Positive z component before normalization; invalid samples use (0,0,1).

    bool IsValid() const {
        // Bounds ensure gradient scaling and squared normal lengths remain finite.
        const float maxComponent = std::sqrt(std::numeric_limits<float>::max()) * 0.25f;
        return (format == DepthFormat::RelativeDepth || format == DepthFormat::InverseDepth) &&
            std::isfinite(depthScale) && depthScale > 0.0f && depthScale <= maxComponent / DepthBuffer::kMaxDimension &&
            std::isfinite(minConfidence) && minConfidence >= 0.0f && minConfidence <= 1.0f &&
            std::isfinite(invalidDepthMin) && std::isfinite(invalidDepthMax) &&
            invalidDepthMin >= 0.0f && invalidDepthMax <= 1.0f && invalidDepthMin <= invalidDepthMax &&
            std::isfinite(fallbackZ) && fallbackZ > 0.0f && fallbackZ <= maxComponent;
    }
};

/// <summary>
/// Injectable fault seams for GPU depth-to-normal processing.
/// </summary>
enum class GpuDepthToNormalFaultSeam : int {
    None = 0,
    DepthUpload_Map = 1,
    OutputAlloc = 2,
    ShaderCompile = 3,
    Dispatch = 4,
    DeviceRemoved = 5
};

/// <summary>
/// GPU execution outcome.
/// </summary>
enum class GpuDepthToNormalOutcome : int {
    Success = 0,
    InvalidInput = 1,
    DeviceLoss = 2,
    AllocationFailed = 3,
    ShaderFailed = 4,
    DispatchFailed = 5
};

} // namespace PrismForge::Effects::Relighting
