#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "effects/relighting/depth_types.h"
#include "effects/analysisbridge/analysis_types.h"
#include "effects/lighting/studiolighting.h"

namespace PrismForge::Effects::Relighting {

using AnalysisBridge::NormalBuffer;

/// <summary>
/// 2-channel visibility shadow map storing (V1, V2) in [0.0, 1.0] per pixel.
/// V1: visibility of Light 1 (1.0 = fully lit, 0.0 = occluded)
/// V2: visibility of Light 2 (1.0 = fully lit, 0.0 = occluded)
/// </summary>
struct ShadowMapBuffer {
    int width = 0;
    int height = 0;
    int strideFloats = 0; // Number of float elements per row (>= width * 2)
    std::vector<float> data;

    bool IsValid() const {
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
        if (strideFloats < width * 2) return false;
        uint64_t total = static_cast<uint64_t>(height) * static_cast<uint64_t>(strideFloats);
        return data.size() >= total;
    }
};

/// <summary>
/// Approximate Geometry Shadows - CPU Reference Evaluator.
/// 
/// Evaluates screen-space ray-marched self-occlusion visibility for the two studio lights
/// using accepted relative depth and surface normals.
/// 
/// Features:
/// - Bounded ray marching: steps capped by shadowConfig.maxSteps, distance capped by maxDistance.
/// - Adaptive elevation and normal-facing bias to prevent self-shadow acne.
/// - Natural soft penumbra transition based on occluder distance.
/// - Invalid/low-confidence/occluded regions fade predictably to neutral (1.0 visibility), not black blocks.
/// - No absolute-depth claim or fake shadows when geometry is missing.
/// - Background pixels where mask alpha is 0 are preserved bit-exact without shadow.
/// </summary>
class ApproximateShadows {
public:
    ApproximateShadows();
    ~ApproximateShadows();

    /// <summary>
    /// Computes two-light visibility shadow map from depth, normals, and lighting config.
    /// </summary>
    bool ComputeShadows(
        const DepthBuffer& depth,
        const NormalBuffer* normals,
        const PrismForge::Effects::Lighting::AlphaMask* mask,
        const PrismForge::Effects::Lighting::LightingConfig& lightingConfig,
        ShadowMapBuffer& outShadows
    );

    /// <summary>
    /// Computes light visibility factor V in [0.0, 1.0] for a single surface point.
    /// </summary>
    static float ComputeLightVisibility(
        float u, float v,
        float zSurface,
        float nx, float ny, float nz,
        const DepthBuffer& depth,
        const PrismForge::Effects::Lighting::StudioLight& light,
        const PrismForge::Effects::Lighting::ApproximateShadowConfig& shadowConfig,
        float aspect
    );

    /// <summary>
    /// Computes light visibility factor V using a raw depth array with explicit stride.
    /// </summary>
    static float ComputeLightVisibilityRaw(
        float u, float v,
        float zSurface,
        float nx, float ny, float nz,
        const float* depthData,
        int depthW,
        int depthH,
        int depthStride,
        const PrismForge::Effects::Lighting::StudioLight& light,
        const PrismForge::Effects::Lighting::ApproximateShadowConfig& shadowConfig,
        float aspect,
        const float* confidenceData = nullptr,
        float minConfidence = 0.1f
    );
};

} // namespace PrismForge::Effects::Relighting
