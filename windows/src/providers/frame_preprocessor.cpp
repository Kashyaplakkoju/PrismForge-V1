#include "frame_preprocessor.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace PrismForge::Providers {
namespace {
constexpr size_t kMaxElements = FramePreprocessor::kMaxTensorElements;
bool BoundedDimensions(int w, int h, size_t channels = 1) {
    return w > 0 && h > 0 && w <= FrameRequest::kMaxDimension && h <= FrameRequest::kMaxDimension &&
        static_cast<uint64_t>(w) * h * channels <= kMaxElements;
}
bool ValidAspect(AspectMode mode) { return mode == AspectMode::LetterboxPad || mode == AspectMode::CropToFill || mode == AspectMode::Stretch; }
bool ValidOrientation(FrameOrientation mode) { return mode == FrameOrientation::Normal || mode == FrameOrientation::Rotate90 || mode == FrameOrientation::Rotate180 || mode == FrameOrientation::Rotate270; }
bool SupportedSoftwareFormat(AVPixelFormat f) {
    switch (f) {
    case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGBA: case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ARGB: case AV_PIX_FMT_ABGR: case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ420P: case AV_PIX_FMT_YUVJ422P: case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_NV12: case AV_PIX_FMT_NV21: case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422: return true;
    default: return false;
    }
}
bool IsJpegFormat(AVPixelFormat f) { return f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVJ444P; }
uint8_t ExpandLimited(uint8_t v) { return static_cast<uint8_t>(std::clamp(((static_cast<int>(v) - 16) * 255 + 109) / 219, 0, 255)); }
void OrientedToRaw(int ox, int oy, const FrameRequest& r, int& x, int& y) {
    switch (r.orientation) {
    case FrameOrientation::Normal: x = ox; y = oy; break;
    case FrameOrientation::Rotate90: x = oy; y = r.height - 1 - ox; break;
    case FrameOrientation::Rotate180: x = r.width - 1 - ox; y = r.height - 1 - oy; break;
    case FrameOrientation::Rotate270: x = r.width - 1 - oy; y = ox; break;
    }
}
void RawToOriented(double x, double y, const GeometryTransform& t, double& ox, double& oy) {
    switch (t.orientation) {
    case FrameOrientation::Normal: ox = x; oy = y; break;
    case FrameOrientation::Rotate90: ox = t.srcHeight - 1 - y; oy = x; break;
    case FrameOrientation::Rotate180: ox = t.srcWidth - 1 - x; oy = t.srcHeight - 1 - y; break;
    case FrameOrientation::Rotate270: ox = y; oy = t.srcWidth - 1 - x; break;
    }
    if (t.mirror) ox = t.orientedWidth - 1 - ox;
}
bool ValidateTransform(const GeometryTransform& t) {
    if (!BoundedDimensions(t.srcWidth, t.srcHeight) || !BoundedDimensions(t.targetWidth, t.targetHeight) ||
        !ValidAspect(t.aspectMode) || !ValidOrientation(t.orientation)) return false;
    bool swap = t.orientation == FrameOrientation::Rotate90 || t.orientation == FrameOrientation::Rotate270;
    if (t.orientedWidth != (swap ? t.srcHeight : t.srcWidth) || t.orientedHeight != (swap ? t.srcWidth : t.srcHeight)) return false;
    if (t.validTensorX < 0 || t.validTensorY < 0 || t.validTensorW <= 0 || t.validTensorH <= 0 ||
        t.validTensorW > t.targetWidth || t.validTensorH > t.targetHeight ||
        t.validTensorX > t.targetWidth - t.validTensorW || t.validTensorY > t.targetHeight - t.validTensorH) return false;
    if (t.cropSrcX < 0 || t.cropSrcY < 0 || t.cropSrcW <= 0 || t.cropSrcH <= 0 ||
        t.cropSrcW > t.orientedWidth || t.cropSrcH > t.orientedHeight ||
        t.cropSrcX > t.orientedWidth - t.cropSrcW || t.cropSrcY > t.orientedHeight - t.cropSrcH) return false;
    double expectedX = static_cast<double>(t.validTensorW) / t.cropSrcW;
    double expectedY = static_cast<double>(t.validTensorH) / t.cropSrcH;
    return std::isfinite(t.scaleX) && std::isfinite(t.scaleY) && t.scaleX > 0 && t.scaleY > 0 &&
        std::abs(t.scaleX - expectedX) <= std::max(1.0, expectedX) * 1e-6 &&
        std::abs(t.scaleY - expectedY) <= std::max(1.0, expectedY) * 1e-6;
}
} // namespace

std::shared_ptr<const FrameRequest> FrameRequest::CreateFromAVFrame(
    uint64_t sourceGen, uint64_t seq, int64_t pts, const AVFrame* frame, bool isMirrored) {
    return CreateFromAVFrameScaled(sourceGen, seq, pts, frame, 0, isMirrored);
}

std::shared_ptr<const FrameRequest> FrameRequest::CreateFromAVFrameScaled(
    uint64_t sourceGen, uint64_t seq, int64_t pts, const AVFrame* frame, int maxLongEdge, bool isMirrored) {
    if (maxLongEdge < 0 || maxLongEdge > kMaxDimension) return nullptr;
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->width > kMaxDimension || frame->height > kMaxDimension ||
        frame->hw_frames_ctx || frame->format < 0 || frame->format >= AV_PIX_FMT_NB) return nullptr;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if (!SupportedSoftwareFormat(format)) return nullptr;
    const auto* desc = av_pix_fmt_desc_get(format);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return nullptr;
    const int planes = av_pix_fmt_count_planes(format);
    if (planes <= 0 || planes > 4 || av_image_check_size(frame->width, frame->height, 0, nullptr) < 0) return nullptr;
    ptrdiff_t strides[4] = {};
    for (int p = 0; p < planes; ++p) {
        int minimum = av_image_get_linesize(format, frame->width, p);
        if (!frame->data[p] || minimum <= 0 || frame->linesize[p] < minimum) return nullptr;
        strides[p] = frame->linesize[p];
    }
    size_t sizes[4] = {};
    if (av_image_fill_plane_sizes(sizes, format, frame->height, strides) < 0) return nullptr;
    size_t ownedRequired = 0;
    for (int p = 0; p < planes; ++p) {
        // The source must have reference-counted storage covering the entire advertised
        // plane, including positive row padding. Merely non-null data/stride is insufficient.
        if (!sizes[p] || sizes[p] > kMaxOwnedBytes - ownedRequired) return nullptr;
        AVBufferRef* buffer = av_frame_get_plane_buffer(frame, p);
        if (!buffer || !buffer->data) return nullptr;
        ownedRequired += sizes[p];
        const uintptr_t start = reinterpret_cast<uintptr_t>(frame->data[p]);
        const uintptr_t base = reinterpret_cast<uintptr_t>(buffer->data);
        if (start < base || start - base > buffer->size || sizes[p] > buffer->size - (start - base)) return nullptr;
    }
    const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool gray = format == AV_PIX_FMT_GRAY8;
    int srcRange;
    if (frame->color_range == AVCOL_RANGE_JPEG) srcRange = 1;
    else if (frame->color_range == AVCOL_RANGE_MPEG) srcRange = 0;
    else if (frame->color_range == AVCOL_RANGE_UNSPECIFIED) srcRange = rgb || gray || IsJpegFormat(format) ? 1 : 0;
    else return nullptr;
    int matrix;
    if (rgb || gray) matrix = SWS_CS_ITU709;
    else if (frame->colorspace == AVCOL_SPC_BT709) matrix = SWS_CS_ITU709;
    else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) matrix = SWS_CS_SMPTE170M;
    else if (frame->colorspace == AVCOL_SPC_UNSPECIFIED) matrix = frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
    else return nullptr; // Do not silently convert BT2020 or unsupported matrix metadata with a601/709 guess.
    int outputW = frame->width, outputH = frame->height;
    if (maxLongEdge && std::max(outputW, outputH) > maxLongEdge) {
        const double scale = static_cast<double>(maxLongEdge) / std::max(outputW, outputH);
        outputW = std::max(1, static_cast<int>(std::lround(outputW * scale)));
        outputH = std::max(1, static_cast<int>(std::lround(outputH * scale)));
    }
    const int stride = outputW * 3;
    const size_t bytes = static_cast<size_t>(stride) * outputH;
    if (bytes > kMaxOwnedBytes) return nullptr;
    try {
        std::vector<uint8_t> pixels(bytes);
        SwsContext* raw = sws_getContext(frame->width, frame->height, format, outputW, outputH,
            AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!raw) return nullptr;
        std::unique_ptr<SwsContext, decltype(&sws_freeContext)> context(raw, sws_freeContext);
        // swscale leaves RGB-to-RGB range unchanged. Explicitly expand limited packedRGB below.
        if (sws_setColorspaceDetails(raw, sws_getCoefficients(matrix), rgb || gray ? 1 : srcRange,
            sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16) < 0) return nullptr;
        uint8_t* destination[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int destinationStrides[4] = {stride, 0, 0, 0};
        if (sws_scale(raw, frame->data, frame->linesize, 0, frame->height, destination, destinationStrides) != outputH) return nullptr;
        if ((rgb || gray) && !srcRange) for (uint8_t& v : pixels) v = ExpandLimited(v);
        return CreateFromPixels(sourceGen, seq, pts, outputW, outputH, stride, PixelFormat::RGB24,
            std::move(pixels), ColorRange::Full, ColorMatrix::Identity, FrameOrientation::Normal, isMirrored);
    } catch (...) { return nullptr; }
}

FramePreprocessor::FramePreprocessor() = default;
FramePreprocessor::~FramePreprocessor() = default;

bool FramePreprocessor::Preprocess(const FrameRequest& request, int targetWidth, int targetHeight,
    TensorLayout layout, const NormalizationConfig& norm, AspectMode aspectMode,
    std::vector<float>& outTensorData, GeometryTransform& outTransform) {
    outTensorData.clear(); outTransform = {};
    if (!request.IsValid() || !BoundedDimensions(targetWidth, targetHeight, 3) || !ValidAspect(aspectMode) ||
        (layout != TensorLayout::NCHW && layout != TensorLayout::NHWC) || norm.mean.size() != 3 || norm.std.size() != 3) return false;
    for (int c = 0; c < 3; ++c) if (!std::isfinite(norm.mean[c]) || !std::isfinite(norm.std[c]) || norm.std[c] < 1e-6f) return false;
    GeometryTransform t;
    t.srcWidth = request.width; t.srcHeight = request.height;
    const bool swap = request.orientation == FrameOrientation::Rotate90 || request.orientation == FrameOrientation::Rotate270;
    t.orientedWidth = swap ? request.height : request.width; t.orientedHeight = swap ? request.width : request.height;
    t.targetWidth = targetWidth; t.targetHeight = targetHeight; t.aspectMode = aspectMode;
    t.mirror = request.mirror; t.orientation = request.orientation;
    t.cropSrcW = t.orientedWidth; t.cropSrcH = t.orientedHeight;
    t.validTensorW = targetWidth; t.validTensorH = targetHeight;
    const double srcAspect = static_cast<double>(t.orientedWidth) / t.orientedHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
    if (aspectMode == AspectMode::LetterboxPad) {
        if (srcAspect > targetAspect) t.validTensorH = std::clamp(static_cast<int>(std::lround(targetWidth / srcAspect)), 1, targetHeight);
        else t.validTensorW = std::clamp(static_cast<int>(std::lround(targetHeight * srcAspect)), 1, targetWidth);
        t.validTensorX = (targetWidth - t.validTensorW) / 2; t.validTensorY = (targetHeight - t.validTensorH) / 2;
    } else if (aspectMode == AspectMode::CropToFill) {
        if (srcAspect > targetAspect) t.cropSrcW = std::clamp(static_cast<int>(std::lround(t.orientedHeight * targetAspect)), 1, t.orientedWidth);
        else t.cropSrcH = std::clamp(static_cast<int>(std::lround(t.orientedWidth / targetAspect)), 1, t.orientedHeight);
        t.cropSrcX = (t.orientedWidth - t.cropSrcW) / 2; t.cropSrcY = (t.orientedHeight - t.cropSrcH) / 2;
    }
    t.scaleX = static_cast<float>(t.validTensorW) / t.cropSrcW; t.scaleY = static_cast<float>(t.validTensorH) / t.cropSrcH;
    try { outTensorData.resize(static_cast<size_t>(targetWidth) * targetHeight * 3); } catch (...) { return false; }
    float pad[3];
    for (int c = 0; c < 3; ++c) { pad[c] = -norm.mean[c] / norm.std[c]; if (!std::isfinite(pad[c])) { outTensorData.clear(); return false; } }
    const size_t bpp = FrameRequest::GetBytesPerPixel(request.format);
    const size_t spatial = static_cast<size_t>(targetWidth) * targetHeight;
    for (int y = 0; y < targetHeight; ++y) for (int x = 0; x < targetWidth; ++x) {
        float rgb[3] = {pad[0], pad[1], pad[2]};
        if (x >= t.validTensorX && x < t.validTensorX + t.validTensorW && y >= t.validTensorY && y < t.validTensorY + t.validTensorH) {
            // Center-aligned nearest neighbour sampling. Rotate clockwise first, then mirror.
            int ox = std::min(t.cropSrcX + static_cast<int>((static_cast<double>(x - t.validTensorX) + .5) * t.cropSrcW / t.validTensorW), t.cropSrcX + t.cropSrcW - 1);
            int oy = std::min(t.cropSrcY + static_cast<int>((static_cast<double>(y - t.validTensorY) + .5) * t.cropSrcH / t.validTensorH), t.cropSrcY + t.cropSrcH - 1);
            if (request.mirror) ox = t.orientedWidth - 1 - ox;
            int rx, ry; OrientedToRaw(ox, oy, request, rx, ry);
            const uint8_t* p = request.pixelData->data() + static_cast<size_t>(ry) * request.strideBytes + static_cast<size_t>(rx) * bpp;
            const bool bgr = request.format == PixelFormat::BGR24 || request.format == PixelFormat::BGRA32;
            for (int c = 0; c < 3; ++c) {
                uint8_t byte = p[bgr && c != 1 ? 2 - c : c];
                if (request.colorRange == ColorRange::Limited) byte = ExpandLimited(byte);
                rgb[c] = ((norm.scaleTo01 ? byte / 255.0f : static_cast<float>(byte)) - norm.mean[c]) / norm.std[c];
                if (!std::isfinite(rgb[c])) { outTensorData.clear(); return false; }
            }
        }
        const size_t index = static_cast<size_t>(y) * targetWidth + x;
        for (int c = 0; c < 3; ++c) outTensorData[layout == TensorLayout::NCHW ? c * spatial + index : index * 3 + c] = rgb[c];
    }
    outTransform = t; return true;
}

bool FramePreprocessor::MapMaskBack(const float* tensorMask, int tensorW, int tensorH,
    const GeometryTransform& t, Effects::AnalysisBridge::MaskBuffer& outMask, int targetOutW, int targetOutH) {
    outMask = {};
    if (!tensorMask || !BoundedDimensions(tensorW, tensorH) || !ValidateTransform(t) || targetOutW < 0 || targetOutH < 0) return false;
    const int outW = targetOutW ? targetOutW : t.srcWidth;
    const int outH = targetOutH ? targetOutH : t.srcHeight;
    if (!BoundedDimensions(outW, outH)) return false;
    for (size_t i = 0, n = static_cast<size_t>(tensorW) * tensorH; i < n; ++i)
        if (!std::isfinite(tensorMask[i]) || tensorMask[i] < 0 || tensorMask[i] > 1) return false;
    const double sx = static_cast<double>(tensorW) / t.targetWidth;
    const double sy = static_cast<double>(tensorH) / t.targetHeight;
    const double left = t.validTensorX * sx, top = t.validTensorY * sy;
    const double right = (t.validTensorX + t.validTensorW) * sx, bottom = (t.validTensorY + t.validTensorH) * sy;
    // Clamp interpolation to centers inside the active image. Sampling padded neighbours
    // would otherwise leak a model's padding predictions into the subject's outer edge.
    const int firstX = std::max(0, static_cast<int>(std::ceil(left - .5)));
    const int lastX = std::min(tensorW - 1, static_cast<int>(std::ceil(right - .5)) - 1);
    const int firstY = std::max(0, static_cast<int>(std::ceil(top - .5)));
    const int lastY = std::min(tensorH - 1, static_cast<int>(std::ceil(bottom - .5)) - 1);
    if (firstX > lastX || firstY > lastY) return false; // Output resolution cannot represent this active region.
    Effects::AnalysisBridge::MaskBuffer candidate;
    candidate.width = outW; candidate.height = outH; candidate.strideFloats = outW;
    try { candidate.data.resize(static_cast<size_t>(outW) * outH); } catch (...) { return false; }
    for (int y = 0; y < outH; ++y) for (int x = 0; x < outW; ++x) {
        double rawX = (x + .5) * t.srcWidth / outW - .5;
        double rawY = (y + .5) * t.srcHeight / outH - .5;
        rawX = std::clamp(rawX, 0.0, static_cast<double>(t.srcWidth - 1));
        rawY = std::clamp(rawY, 0.0, static_cast<double>(t.srcHeight - 1));
        double ox = 0, oy = 0; RawToOriented(rawX, rawY, t, ox, oy);
        float value = 0;
        if (ox >= t.cropSrcX - .5 && ox < t.cropSrcX + t.cropSrcW - .5 &&
            oy >= t.cropSrcY - .5 && oy < t.cropSrcY + t.cropSrcH - .5) {
            double mx = (t.validTensorX + (ox - t.cropSrcX + .5) * t.validTensorW / t.cropSrcW) * sx - .5;
            double my = (t.validTensorY + (oy - t.cropSrcY + .5) * t.validTensorH / t.cropSrcH) * sy - .5;
            mx = std::clamp(mx, static_cast<double>(firstX), static_cast<double>(lastX));
            my = std::clamp(my, static_cast<double>(firstY), static_cast<double>(lastY));
            int x0 = static_cast<int>(std::floor(mx)), y0 = static_cast<int>(std::floor(my));
            int x1 = std::min(x0 + 1, lastX), y1 = std::min(y0 + 1, lastY);
            double fx = mx - x0, fy = my - y0;
            double a = tensorMask[static_cast<size_t>(y0) * tensorW + x0] * (1 - fx) + tensorMask[static_cast<size_t>(y0) * tensorW + x1] * fx;
            double b = tensorMask[static_cast<size_t>(y1) * tensorW + x0] * (1 - fx) + tensorMask[static_cast<size_t>(y1) * tensorW + x1] * fx;
            value = static_cast<float>(a * (1 - fy) + b * fy);
        }
        candidate.data[static_cast<size_t>(y) * outW + x] = value;
    }
    outMask = std::move(candidate); return true;
}
} // namespace PrismForge::Providers
