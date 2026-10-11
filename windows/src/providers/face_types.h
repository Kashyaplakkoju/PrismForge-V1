#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "effects/analysisbridge/analysis_types.h"
namespace PrismForge::Providers {
struct FacePoint3D { float x=0, y=0, z=0; }; // Image-normalized XY; relative Z in image-width units, smaller is nearer.
struct FaceFrameTags { uint64_t generation=0, sequence=0; int64_t microPts=0; };
struct FaceObservation {
    FaceFrameTags tags;
    int imageWidth=0, imageHeight=0;
    bool detected=false;
    std::vector<FacePoint3D> vertices; // Exactly478 for the pinned model; surface topology uses the first468.
    std::array<float,52> blendshapes{};
    bool hasBlendshapes=false;
    std::array<float,16> transform{};
    bool hasTransform=false;
};
struct FaceTopology { std::vector<std::array<uint32_t,3>> triangles; };
struct FaceGeometry {
    FaceFrameTags tags;
    Effects::AnalysisBridge::NormalBuffer normals;
    Effects::AnalysisBridge::DepthBuffer depth;
    Effects::AnalysisBridge::MaskBuffer coverage;
    // Geometric facial surface eligibility, not semantic skin segmentation.
    // Padded eyes/lips/brows and the silhouette margin are excluded.
    Effects::AnalysisBridge::MaskBuffer touchupSurface;
    Effects::AnalysisBridge::MaskBuffer featureProtection;
    Effects::AnalysisBridge::FaceLandmarkBundle anchors;
};
} // namespace PrismForge::Providers
