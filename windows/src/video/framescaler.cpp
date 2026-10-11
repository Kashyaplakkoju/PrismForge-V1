#include "video/framescaler.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace Video {
namespace {
bool Dimensions(int width, int height) {
    return width > 0 && height > 0 && width <= 16384 && height <= 16384 &&
        int64_t(width) * height <= 64LL * 1024 * 1024;
}
// Do not use av_frame_get_plane_buffer here: callers can supply a malformed
// raw descriptor with extended_data == nullptr. Standard AVBufferRef slots
// give a bounded owner independently of that auxiliary pointer array.
bool ValidPlanes(const AVFrame* frame, bool& packedOwner) {
    packedOwner = false;
    if (!frame || !Dimensions(frame->width, frame->height)) return false;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL))) return false;
    int rowBytes[4]{};
    const int planes = av_pix_fmt_count_planes(format);
    if (planes <= 0 || planes > 4 || av_image_fill_linesizes(rowBytes, format, frame->width) < 0) return false;
    bool hasOwner = false;
    for (const auto* owner : frame->buf) if (owner) hasOwner = true;
    uint64_t total = 0;
    for (int p = 0; p < planes; ++p) {
        if (!frame->data[p] || rowBytes[p] <= 0 || frame->linesize[p] < rowBytes[p]) return false;
        int rows = frame->height;
        if (!(descriptor->flags & AV_PIX_FMT_FLAG_RGB) && (p == 1 || p == 2))
            rows = AV_CEIL_RSHIFT(rows, descriptor->log2_chroma_h);
        const uint64_t span = uint64_t(frame->linesize[p]) * (rows - 1) + rowBytes[p];
        total += span;
        if (total > (1ULL << 30)) return false;
        bool bounded = false;
        const auto start = reinterpret_cast<uintptr_t>(frame->data[p]);
        for (const auto* owner : frame->buf) {
            if (!owner || !owner->data) continue;
            const auto base = reinterpret_cast<uintptr_t>(owner->data);
            if (start >= base && start - base <= owner->size && span <= owner->size - (start - base)) { bounded = true; break; }
        }
        // Preserve synchronous borrowed-plane support, but never take a
        // direct borrowed-output fast path without a verified input owner.
        if (hasOwner && !bounded) return false;
        if (planes == 1 && bounded) packedOwner = true;
    }
    return true;
}
}
FrameScaler::FrameScaler(int format) : format(format) {}
FrameScaler::~FrameScaler() { Cleanup(); }
FrameScaler::FrameScaler(FrameScaler&& other) noexcept : format(other.format) { *this = std::move(other); }
FrameScaler& FrameScaler::operator=(FrameScaler&& other) noexcept {
    if (this == &other) return *this;
    Cleanup();
    format = other.format; sws_ctx = std::exchange(other.sws_ctx, nullptr);
    buffer = std::move(other.buffer); cachedTargetDims = other.cachedTargetDims;
    lastSrcWidth = std::exchange(other.lastSrcWidth, 0);
    lastSrcHeight = std::exchange(other.lastSrcHeight, 0);
    lastSrcFormat = std::exchange(other.lastSrcFormat, -1);
    return *this;
}
const uint8_t* FrameScaler::Process(const AVFrame* source, int& width, int& height) {
    width = height = 0;
    bool boundedOwner = false;
    if (!ValidPlanes(source, boundedOwner)) return nullptr;
    if (!PrepareContext(source->width, source->height, source->format)) return nullptr;
    const int bpp = GetBytesPerPixel(format);
    const int stride = cachedTargetDims.containerWidth * bpp;
    const bool copy = source->format == format && cachedTargetDims.width == source->width && cachedTargetDims.height == source->height;
    if (copy) {
        const int rowBytes = source->width * bpp;
        if (boundedOwner && cachedTargetDims.xOffset == 0 && cachedTargetDims.yOffset == 0 &&
            source->width == cachedTargetDims.containerWidth && source->height == cachedTargetDims.containerHeight && source->linesize[0] == rowBytes) {
            // DirectShow consumes synchronously; preview/snapshot copy before
            // returning. Ownership remains with the input AVFrame caller.
            width = source->width; height = source->height;
            return source->data[0];
        }
        auto* destination = buffer.data() + cachedTargetDims.yOffset * stride + cachedTargetDims.xOffset * bpp;
        for (int y = 0; y < source->height; ++y)
            std::memcpy(destination + size_t(y) * stride, source->data[0] + size_t(y) * source->linesize[0], rowBytes);
    } else {
        uint8_t* slices[4]{buffer.data() + cachedTargetDims.yOffset * stride + cachedTargetDims.xOffset * bpp};
        int strides[4]{stride};
        if (sws_scale(sws_ctx, source->data, source->linesize, 0, source->height, slices, strides) != cachedTargetDims.height) return nullptr;
    }
    // SIMD conversion may write past the fitted row into adjoining bars.
    // Restore only the bars after processing, without clearing active pixels.
    if (cachedTargetDims.width != cachedTargetDims.containerWidth || cachedTargetDims.height != cachedTargetDims.containerHeight ||
        cachedTargetDims.xOffset || cachedTargetDims.yOffset) {
        const int left = cachedTargetDims.xOffset * bpp;
        const int right = (cachedTargetDims.xOffset + cachedTargetDims.width) * bpp;
        for (int y = 0; y < cachedTargetDims.containerHeight; ++y) {
            auto* row = buffer.data() + size_t(y) * stride;
            if (y < cachedTargetDims.yOffset || y >= cachedTargetDims.yOffset + cachedTargetDims.height) std::memset(row, 0, stride);
            else { std::memset(row, 0, left); std::memset(row + right, 0, stride - right); }
        }
    }
    width = cachedTargetDims.containerWidth; height = cachedTargetDims.containerHeight;
    return buffer.data();
}
bool FrameScaler::PrepareContext(int sourceWidth, int sourceHeight, int sourceFormat) {
    if (sourceWidth == lastSrcWidth && sourceHeight == lastSrcHeight && sourceFormat == lastSrcFormat) return true;
    const auto next = CalculateTargetDimensions(sourceWidth, sourceHeight);
    const int bpp = GetBytesPerPixel(format);
    if (!Dimensions(next.width, next.height) || !Dimensions(next.containerWidth, next.containerHeight) || bpp <= 0 ||
        next.xOffset < 0 || next.yOffset < 0 || next.width > next.containerWidth - next.xOffset || next.height > next.containerHeight - next.yOffset) return false;
    const bool copy = sourceFormat == format && next.width == sourceWidth && next.height == sourceHeight;
    SwsContext* context = nullptr;
    if (!copy) {
        context = sws_getContext(sourceWidth, sourceHeight, static_cast<AVPixelFormat>(sourceFormat), next.width, next.height,
            static_cast<AVPixelFormat>(format), SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!context) return false;
    }
    const size_t needed = size_t(next.containerWidth) * next.containerHeight * bpp + AV_INPUT_BUFFER_PADDING_SIZE;
    try {
        if (buffer.size() < needed) buffer.resize(needed);
        if (next.hasPadding || next.width != next.containerWidth || next.height != next.containerHeight || next.xOffset || next.yOffset)
            std::memset(buffer.data(), 0, buffer.size());
    } catch (...) { sws_freeContext(context); return false; }
    sws_freeContext(sws_ctx); sws_ctx = context;
    cachedTargetDims = next; lastSrcWidth = sourceWidth; lastSrcHeight = sourceHeight; lastSrcFormat = sourceFormat;
    return true;
}
void FrameScaler::Cleanup() { sws_freeContext(sws_ctx); sws_ctx = nullptr; buffer.clear(); lastSrcWidth = lastSrcHeight = 0; lastSrcFormat = -1; }
int FrameScaler::GetBytesPerPixel(int value) {
    switch (value) {
    case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGR24: return 3;
    case AV_PIX_FMT_RGBA: case AV_PIX_FMT_BGRA: case AV_PIX_FMT_ARGB: case AV_PIX_FMT_ABGR: case AV_PIX_FMT_RGB0: case AV_PIX_FMT_BGR0: return 4;
    default: return 0; // This scaler publishes packed RGB bytes, not planar storage.
    }
}
}
