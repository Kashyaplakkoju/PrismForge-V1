#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <limits>

struct AVFrame;

namespace PrismForge::Providers {

enum class PixelFormat : int { RGB24 = 0, BGR24 = 1, RGBA32 = 2, BGRA32 = 3 };
enum class ColorRange : int { Full = 0, Limited = 1 };
enum class ColorMatrix : int { Identity = 0, BT709 = 1, BT601 = 2 };
// Clockwise rotation, then horizontal mirroring in the oriented coordinate space.
enum class FrameOrientation : int { Normal = 0, Rotate90 = 1, Rotate180 = 2, Rotate270 = 3 };

struct FrameRequest {
    static constexpr int kMaxDimension = 8192;
    static constexpr size_t kMaxOwnedBytes = 128ULL * 1024 * 1024;

    uint64_t sourceGeneration = 0;
    uint64_t sequenceId = 0;
    int64_t timestampPts = 0; // Microseconds.
    int width = 0;
    int height = 0;
    int strideBytes = 0;
    PixelFormat format = PixelFormat::RGB24;
    ColorRange colorRange = ColorRange::Full;
    // Packed channels are already RGB; the matrix describes provenance, not another YUV conversion.
    ColorMatrix colorMatrix = ColorMatrix::Identity;
    FrameOrientation orientation = FrameOrientation::Normal;
    bool mirror = false;
    std::shared_ptr<const std::vector<uint8_t>> pixelData;

    static size_t GetBytesPerPixel(PixelFormat fmt) {
        switch (fmt) {
        case PixelFormat::RGB24: case PixelFormat::BGR24: return 3;
        case PixelFormat::RGBA32: case PixelFormat::BGRA32: return 4;
        default: return 0;
        }
    }

    bool IsValid() const {
        if (width <= 0 || width > kMaxDimension || height <= 0 || height > kMaxDimension || strideBytes <= 0)
            return false;
        const size_t bpp = GetBytesPerPixel(format);
        if (!bpp || static_cast<uint64_t>(strideBytes) < static_cast<uint64_t>(width) * bpp)
            return false;
        if (colorRange != ColorRange::Full && colorRange != ColorRange::Limited) return false;
        if (colorMatrix != ColorMatrix::Identity && colorMatrix != ColorMatrix::BT601 && colorMatrix != ColorMatrix::BT709)
            return false;
        if (orientation != FrameOrientation::Normal && orientation != FrameOrientation::Rotate90 &&
            orientation != FrameOrientation::Rotate180 && orientation != FrameOrientation::Rotate270) return false;
        const uint64_t bytes = static_cast<uint64_t>(strideBytes) * height;
        return bytes <= kMaxOwnedBytes && pixelData && pixelData->size() >= bytes &&
            pixelData->size() <= kMaxOwnedBytes && pixelData->capacity() <= kMaxOwnedBytes;
    }

    static std::shared_ptr<const FrameRequest> CreateFromPixels(
        uint64_t sourceGen, uint64_t seq, int64_t pts, int w, int h, int stride,
        PixelFormat fmt, std::vector<uint8_t> pixels, ColorRange range = ColorRange::Full,
        ColorMatrix matrix = ColorMatrix::Identity, FrameOrientation orient = FrameOrientation::Normal,
        bool isMirrored = false) {
        try {
            auto req = std::make_shared<FrameRequest>();
            req->sourceGeneration = sourceGen; req->sequenceId = seq; req->timestampPts = pts;
            req->width = w; req->height = h; req->strideBytes = stride; req->format = fmt;
            req->colorRange = range; req->colorMatrix = matrix; req->orientation = orient; req->mirror = isMirrored;
            req->pixelData = std::make_shared<const std::vector<uint8_t>>(std::move(pixels));
            return req->IsValid() ? req : nullptr;
        } catch (...) { return nullptr; }
    }

    // Only owned, positive-stride, supported 8-bit software frames are accepted.
    // Output is full-range RGB24 with Identity matrix; unspecified YUV colour uses
    // limited range and BT709 at >=720 rows, otherwise BT601. Unsupported colour is rejected.
    static std::shared_ptr<const FrameRequest> CreateFromAVFrame(
        uint64_t sourceGen, uint64_t seq, int64_t pts, const AVFrame* avFrame, bool isMirrored = false);

    // Validates original owned planes, but converts directly to a bounded RGB raster.
    // The long edge never grows above the original; source gen/sequence/PTS stay exact.
    static std::shared_ptr<const FrameRequest> CreateFromAVFrameScaled(
        uint64_t sourceGen, uint64_t seq, int64_t pts, const AVFrame* avFrame,
        int maxLongEdge, bool isMirrored = false);
};

} // namespace PrismForge::Providers
