#pragma once
#include "effects/analysisbridge/analysis_types.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace PrismForge::Effects::Relighting {
enum class ShadowDepthPreparation { Success, NotReady, OutOfMemory };

// Approximate shadows consume normalized relative depth, not inverse/metric depth.
// depthScale calibrates normal gradients; it does not rescale normalized depth here.
// Stretch uses endpoint-aligned nearest sampling, matching ray-march coordinates.
inline ShadowDepthPreparation PrepareShadowDepth(
    const AnalysisBridge::DepthBuffer& source, int width, int height,
    AnalysisBridge::MaskBuffer& output) {
    if (!source.IsValid() || source.format != 0 || !std::isfinite(source.depthScale) ||
        source.depthScale <= 0 || width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return ShadowDepthPreparation::NotReady;
    const size_t count = static_cast<size_t>(width) * height;
    if (output.width != width || output.height != height || output.strideFloats != width || output.data.size() != count) {
        AnalysisBridge::MaskBuffer candidate;
        candidate.width = width; candidate.height = height; candidate.strideFloats = width;
        try { candidate.data.resize(count); } catch (const std::bad_alloc&) { return ShadowDepthPreparation::OutOfMemory; }
        output = std::move(candidate);
    }
    for (int y = 0; y < height; ++y) {
        const size_t sy = height == 1 ? 0 : static_cast<size_t>((static_cast<uint64_t>(y) * (source.height - 1) * 2 + height - 1) / (2ULL * (height - 1)));
        for (int x = 0; x < width; ++x) {
            const size_t sx = width == 1 ? 0 : static_cast<size_t>((static_cast<uint64_t>(x) * (source.width - 1) * 2 + width - 1) / (2ULL * (width - 1)));
            const size_t at = sy * static_cast<size_t>(source.strideFloats) + sx;
            const float depth = source.data[at];
            const float confidence = source.confidence.empty() ? 1.0f : source.confidence[at];
            output.data[static_cast<size_t>(y) * width + x] =
                std::isfinite(depth) && depth >= 0 && depth <= 1 && std::isfinite(confidence) &&
                confidence >= 0.5f && confidence <= 1 ? depth : AnalysisBridge::DepthBuffer::kDefaultInvalidSentinel;
        }
    }
    return ShadowDepthPreparation::Success;
}
} // namespace PrismForge::Effects::Relighting
