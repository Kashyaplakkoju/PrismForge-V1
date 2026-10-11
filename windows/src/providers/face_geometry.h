#pragma once
#include "face_types.h"
#include <string>

namespace PrismForge::Providers {
// Parses only a bounded, local OBJ: exactly468 vertices and triangular faces
// referring to that canonical surface. No materials or external files are loaded.
bool LoadFaceTopology(const std::wstring& path, FaceTopology& outTopology, std::string& error);

// Synthetic geometry math and actual inference share this adapter; this function
// performs no inference and assigns no model detection confidence. XYZ is scaled
// isotropically to source pixels: (x*imageWidth,y*imageHeight,-z*imageWidth).
// The output relative-depth calibration is fixed: depth=0.5+modelZ, near=0/far=1;
// out-of-range samples are unsupported, never normalized independently per frame.
// Coverage/depth.confidence mark visible geometric support (0/1), not confidence
// in the ML model. Uncovered normals are zero and depth is the invalid sentinel.
// Empty/no-visible/invalid input clears output and returnsfalse with an explanation.
// Touch-up masks preserve padded eyes/lips/brows and erode the face silhouette.
// They describe geometric eligibility only; hair/beard/skin semantics are not inferred.
// Source dimensions<=8192; raster axes<=2048; pixels<=4Mi, triangles<=2048.
bool BuildFaceGeometry(const FaceObservation& observation, const FaceTopology& topology,
    int rasterWidth, int rasterHeight, FaceGeometry& outGeometry, std::string& error);
}