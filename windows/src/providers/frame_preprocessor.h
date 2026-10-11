#pragma once

#include "frame_request.h"
#include "effects/analysisbridge/analysis_types.h"
#include <vector>
#include <cstdint>
#include <string>

namespace PrismForge::Providers {

enum class AspectMode {
    LetterboxPad = 0,
    CropToFill = 1,
    Stretch = 2
};

enum class TensorLayout {
    NCHW = 0, // [1, C, H, W]
    NHWC = 1  // [1, H, W, C]
};

struct NormalizationConfig {
    std::vector<float> mean = { 0.0f, 0.0f, 0.0f }; // Per RGB channel
    std::vector<float> std = { 1.0f, 1.0f, 1.0f };  // Per RGB channel
    bool scaleTo01 = true;                          // Divide uint8 by 255.0 before norm
};

/// <summary>
/// Recorded geometric transformation parameters from original frame to model tensor space.
/// Used to precisely invert the transformation when mapping segmentation/depth masks back.
/// </summary>
struct GeometryTransform {
    int srcWidth = 0;
    int srcHeight = 0;
    int orientedWidth = 0;
    int orientedHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;

    AspectMode aspectMode = AspectMode::LetterboxPad;
    bool mirror = false;
    FrameOrientation orientation = FrameOrientation::Normal;

    // Region within target tensor corresponding to valid active image
    int validTensorX = 0;
    int validTensorY = 0;
    int validTensorW = 0;
    int validTensorH = 0;

    // Crop offsets after clockwise orientation and horizontal mirror (if CropToFill).
    int cropSrcX = 0;
    int cropSrcY = 0;
    int cropSrcW = 0;
    int cropSrcH = 0;

    // Scale factors: validTensor / cropSrc
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

/// <summary>
/// Preprocessor for camera input frames.
/// Prepares normalized NCHW/NHWC float tensors, reuses the caller's output vector capacity,
/// records coordinate transformations, and projects inference masks back to original frame space.
/// </summary>
class FramePreprocessor {
public:
    static constexpr size_t kMaxTensorElements = 16ULL * 1024 * 1024;
    FramePreprocessor();
    ~FramePreprocessor();

    FramePreprocessor(const FramePreprocessor&) = delete;
    FramePreprocessor& operator=(const FramePreprocessor&) = delete;

    /// <summary>
    /// Processes an immutable FrameRequest into an inference float tensor.
    /// Samples source pixel centers with nearest neighbour interpolation. Requires exactly
    /// three finite means and positive finite standard deviations. Failed calls clear output.
    /// </summary>
    bool Preprocess(
        const FrameRequest& request,
        int targetWidth,
        int targetHeight,
        TensorLayout layout,
        const NormalizationConfig& normConfig,
        AspectMode aspectMode,
        std::vector<float>& outTensorData,
        GeometryTransform& outTransform
    );

    /// <summary>
    /// Maps a 2D float mask plane from inference tensor back into original source frame resolution,
    /// inverting letterbox padding, cropping, mirroring, and orientation.
    /// Handles differing input-tensor vs output-mask resolutions, downsampling, and upsampling.
    /// Uses center-aligned bilinear interpolation confined to active image centers, excluding
    /// padded predictions. A resolution with no center inside the active region is rejected.
    /// The caller must provide tensorW*tensorH readable floats; ResultAdapter checks its vector.
    /// Input/output masks are bounded to 16Mi floats; failed calls leave an invalid empty mask.
    /// </summary>
    bool MapMaskBack(
        const float* tensorMask,
        int tensorW,
        int tensorH,
        const GeometryTransform& transform,
        Effects::AnalysisBridge::MaskBuffer& outMask,
        int targetOutW = 0,
        int targetOutH = 0
    );

};

} // namespace PrismForge::Providers
