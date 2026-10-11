#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <cmath>
#include <array>

namespace PrismForge::Effects::AnalysisBridge {

/// <summary>
/// Status of an analysis bundle query against a specific target frame.
/// </summary>
enum class AnalysisAlignmentStatus : int {
    Aligned = 0,            // Valid analysis aligned with requested frame
    NotReady = 1,           // Producer has not emitted analysis yet
    Expired = 2,            // Analysis is older than allowed maxAge threshold
    FutureResult = 3,       // Analysis timestamp is ahead of current frame
    SourceGenerationMismatch = 4, // Analysis belongs to a previous camera source
    SequenceOutOfOrder = 5  // Stale or dropped sequence ID
};

/// <summary>
/// Normalized 2D coordinate in preview screen space [0.0, 1.0].
/// (0, 0) = top-left, (1, 1) = bottom-right.
/// </summary>
struct Landmark2D {
    float x = 0.0f;
    float y = 0.0f;
    float confidence = 0.0f;
};

/// <summary>
/// Standardized landmark bundle for facial tracking and accessory anchoring.
/// </summary>
// Native mesh validity is geometrical evidence, not calibrated detection confidence.
// The default preserves the external-provider confidence policy.
struct FaceMeshVertex3D { float x=0, y=0, z=0; };
// Immutable exact native evidence for perspective fitting and depth-only occlusion.
// Image-normalizedXY; relativeZ in image-width units, smaller is nearer.
// Transform has the native adapter's ROW-major layout, not Three.js column-major.
struct FaceAccessoryGeometry {
    std::vector<FaceMeshVertex3D> vertices; // Exactly478; only first468 enter face topology.
    std::vector<std::array<uint32_t,3>> triangles;
    std::array<float,16> transform{};
    bool hasTransform=false;
    int sourceWidth=0, sourceHeight=0;
    uint64_t sourceGeneration=0, sequenceId=0;
    int64_t timestampPts=0;
    bool Matches(int width,int height,uint64_t generation,uint64_t sequence,int64_t pts) const {
        if(!hasTransform||width<=0||height<=0||width>8192||height>8192||sourceWidth!=width||sourceHeight!=height||
            !generation||!sequence||sourceGeneration!=generation||sequenceId!=sequence||timestampPts!=pts||
            vertices.size()!=478||triangles.empty()||triangles.size()>2048)return false;
        for(float v:transform)if(!std::isfinite(v))return false;
        for(const auto& p:vertices)if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
            std::abs(p.x)>2||std::abs(p.y)>2||std::abs(p.z)>1)return false;
        for(const auto& t:triangles)if(t[0]>=468||t[1]>=468||t[2]>=468||t[0]==t[1]||t[1]==t[2]||t[2]==t[0])return false;
        return true;
    }
};

enum class FaceLandmarkEvidence : int { ConfidenceScores = 0, ValidatedNativeGeometry = 1 };
struct FaceLandmarkBundle {
    bool detected = false;
    float overallConfidence = 0.0f; // [0.0, 1.0]

    // Primary facial anchor points
    Landmark2D leftEye;
    Landmark2D rightEye;
    Landmark2D noseBridge;
    Landmark2D noseTip;
    Landmark2D mouthCenter;
    Landmark2D mouthLeft;
    Landmark2D mouthRight;
    Landmark2D chinTip;
    Landmark2D foreheadCenter;

    // Optional contour points (e.g. face perimeter)
    std::vector<Landmark2D> perimeterContour;

    // Assigned only after the pinned 478-point observation produced a visible,
    // bounded canonical surface and usable on-screen anchors. These fields are
    // not a confidence score and do not change HasFaceLandmarks().
    FaceLandmarkEvidence evidence = FaceLandmarkEvidence::ConfidenceScores;
    uint32_t geometryRevision = 0;
    int geometrySourceWidth = 0, geometrySourceHeight = 0;
    uint64_t geometrySourceGeneration = 0, geometrySequenceId = 0;
    int64_t geometryTimestampPts = 0;
    std::shared_ptr<const FaceAccessoryGeometry> accessoryMesh;

    bool HasValidatedNativeGeometry(int width, int height) const {
        if (!detected || evidence != FaceLandmarkEvidence::ValidatedNativeGeometry || geometryRevision != 1 ||
            width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
            geometrySourceWidth != width || geometrySourceHeight != height ||
            !geometrySourceGeneration || !geometrySequenceId) return false;
        const auto point = [](const Landmark2D& p) {return std::isfinite(p.x) && std::isfinite(p.y) &&
            p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1;};
        if (!point(leftEye) || !point(rightEye) || !point(foreheadCenter) || !point(noseBridge)) return false;
        const double dx = double(rightEye.x-leftEye.x)*width, dy = double(rightEye.y-leftEye.y)*height;
        const double eyes = std::hypot(dx,dy);
        if (eyes < 2 || eyes > std::hypot(double(width),double(height))*.75) return false;
        const double mx = double(leftEye.x+rightEye.x)*width*.5, my = double(leftEye.y+rightEye.y)*height*.5;
        const double forehead = std::hypot(double(foreheadCenter.x)*width-mx,double(foreheadCenter.y)*height-my);
        const double bridge = std::hypot(double(noseBridge.x)*width-mx,double(noseBridge.y)*height-my);
        const double upper = ((double(foreheadCenter.x)*width-mx)*dy-
            (double(foreheadCenter.y)*height-my)*dx)/eyes;
        return forehead >= eyes*.15 && forehead <= eyes*2.5 && upper >= eyes*.15 && bridge <= eyes*1.25;
    }
    bool NativeGeometryMatches(int width,int height,uint64_t generation,uint64_t sequence,int64_t pts) const {
        return HasValidatedNativeGeometry(width,height) && geometrySourceGeneration == generation &&
            geometrySequenceId == sequence && geometryTimestampPts == pts;
    }
};

/// <summary>
/// Immutable owned buffer for single-channel float masks.
/// </summary>
struct MaskBuffer {
    int width = 0;
    int height = 0;
    int strideFloats = 0;
    std::vector<float> data;

    static constexpr int kMaxDimension = 8192;

    bool IsValid() const {
        if (width <= 0 || height <= 0 || strideFloats <= 0) return false;
        if (width > kMaxDimension || height > kMaxDimension) return false;
        if (strideFloats < width) return false;
        uint64_t totalFloats = static_cast<uint64_t>(strideFloats) * static_cast<uint64_t>(height);
        if (totalFloats > (SIZE_MAX / sizeof(float))) return false;
        return data.size() >= totalFloats;
    }
};

/// <summary>
/// Immutable owned buffer for 3-channel normal maps (nx, ny, nz).
/// </summary>
struct NormalBuffer {
    static constexpr int kMaxDimension = 8192;

    int width = 0;
    int height = 0;
    int strideFloats = 0; // >= width * 3
    std::vector<float> data;

    bool IsValid() const {
        if (width <= 0 || height <= 0 || strideFloats <= 0) return false;
        if (width > kMaxDimension || height > kMaxDimension) return false;
        uint64_t minStride = static_cast<uint64_t>(width) * 3ULL;
        if (static_cast<uint64_t>(strideFloats) < minStride) return false;
        uint64_t totalFloats = static_cast<uint64_t>(strideFloats) * static_cast<uint64_t>(height);
        if (totalFloats > (SIZE_MAX / sizeof(float))) return false;
        return data.size() >= totalFloats;
    }
};

/// <summary>
/// Immutable owned buffer for single-channel float depth maps and optional confidence.
/// </summary>
struct DepthBuffer {
    static constexpr int kMaxDimension = 8192;
    static constexpr float kDefaultInvalidSentinel = -1.0f;

    int width = 0;
    int height = 0;
    int strideFloats = 0;
    int format = 0; // 0 = RelativeDepth, 1 = InverseDepth

    // Relative depth scale factor / calibration.
    float depthScale = 1.0f;

    // Monotonic camera generation, frame sequence, and PTS timebase (microseconds).
    uint64_t sourceGeneration = 0;
    uint64_t sequenceId = 0;
    int64_t timestampPts = 0;

    std::vector<float> data;
    std::vector<float> confidence; // Optional per-pixel confidence [0.0, 1.0]

    bool IsValid() const {
        if (width <= 0 || height <= 0 || strideFloats <= 0) return false;
        if (width > kMaxDimension || height > kMaxDimension) return false;
        if (strideFloats < width) return false;
        uint64_t totalFloats = static_cast<uint64_t>(strideFloats) * static_cast<uint64_t>(height);
        if (totalFloats > (SIZE_MAX / sizeof(float))) return false;
        if (data.size() < totalFloats) return false;
        if (!confidence.empty() && confidence.size() < totalFloats) return false;
        return true;
    }
};

/// <summary>
/// Complete, validated, immutable analysis bundle for a single captured frame.
/// Preserves explicit distinction between person segmentation and skin segmentation.
/// </summary>
struct AnalysisBundle {
    uint64_t sourceGeneration = 0; // Monotonic camera source session generation ID
    uint64_t sequenceId = 0;       // Frame sequence counter
    int64_t timestampPts = 0;      // Source camera presentation timestamp, microseconds
    int64_t timestampCompleted = 0; // Wall-clock timestamp (steady_clock ms)

    // Distinct masks
    MaskBuffer personMask;      // Full body/head subject segmentation (for background / studio lights)
    MaskBuffer skinMask;        // Skin surface segmentation (for appearance smoothing)
    MaskBuffer protectionMask;  // Eye / mouth / hair protection (to prevent smoothing features)

    // Optional normals and depth
    NormalBuffer normalMap;
    DepthBuffer depthMap;

    // Landmarks
    FaceLandmarkBundle faceLandmarks;

    bool HasPersonMask() const { return personMask.IsValid(); }
    bool HasSkinMask() const { return skinMask.IsValid(); }
    bool HasProtectionMask() const { return protectionMask.IsValid(); }
    bool HasNormals() const { return normalMap.IsValid(); }
    bool HasDepth() const {
        // Nested geometry must belong to this exact bundle; zero tags are not wildcards.
        return depthMap.IsValid() && depthMap.sourceGeneration == sourceGeneration &&
            depthMap.sequenceId == sequenceId && depthMap.timestampPts == timestampPts;
    }
    bool HasFaceLandmarks() const { return faceLandmarks.detected && faceLandmarks.overallConfidence > 0.0f; }
};

/// <summary>
/// Configuration for the alignment selector and mailbox.
/// </summary>
struct AnalysisMailboxConfig {
    int maxAgeMs = 150;            // Maximum allowable age before analysis is marked Expired
    int maxQueueDepth = 4;         // Maximum internal bounded queue capacity
    bool dropStaleImmediately = true; // Drop out-of-order sequence arrivals
};

} // namespace PrismForge::Effects::AnalysisBridge
