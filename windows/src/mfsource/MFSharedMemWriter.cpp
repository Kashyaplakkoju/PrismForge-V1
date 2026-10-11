#include "MFSharedMemWriter.h"
#include <algorithm>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif
extern "C" {
#include <libavutil/pixdesc.h>
}

using namespace PrismForge::CameraTransport;
namespace {
bool ValidInput(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384 ||
        int64_t(frame->width) * frame->height > 64LL * 1024 * 1024 ||
        frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        frame->color_primaries == AVCOL_PRI_BT2020 || frame->colorspace == AVCOL_SPC_BT2020_CL ||
        frame->colorspace == AVCOL_SPC_BT2020_NCL) return false;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    switch (format) {
    case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGRA: case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGR0: case AV_PIX_FMT_RGB0: case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_NV12: case AV_PIX_FMT_NV21: case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422:
    case AV_PIX_FMT_GRAY8: break;
    default: return false;
    }
    const auto* descriptor = av_pix_fmt_desc_get(format);
    int minimum[4]{};
    if (!descriptor || av_image_fill_linesizes(minimum, format, frame->width) < 0) return false;
    const int planes = av_pix_fmt_count_planes(format);
    if (planes <= 0 || planes > 4) return false;
    bool hasBuffers = false;
    for (const auto* buffer : frame->buf) if (buffer) hasBuffers = true;
    int64_t total = 0;
    for (int p = 0; p < planes; ++p) {
        if (!frame->data[p] || frame->linesize[p] <= 0 || frame->linesize[p] < minimum[p]) return false;
        int rows = frame->height;
        if ((p == 1 || p == 2) && !(descriptor->flags & AV_PIX_FMT_FLAG_RGB))
            rows = (rows + (1 << descriptor->log2_chroma_h) - 1) >> descriptor->log2_chroma_h;
        const auto span = uint64_t(frame->linesize[p]) * (rows - 1) + minimum[p];
        total += span;
        if (total > (1LL << 30)) return false;
        const auto* owner = av_frame_get_plane_buffer(const_cast<AVFrame*>(frame), p);
        if (hasBuffers && !owner) return false;
        if (owner) {
            const auto base = reinterpret_cast<uintptr_t>(owner->data);
            const auto start = reinterpret_cast<uintptr_t>(frame->data[p]);
            if (start < base || start - base > owner->size || span > owner->size - (start - base)) return false;
        }
    }
    return true;
}
void InterleaveChroma(uint8_t* destination, const uint8_t* u, const uint8_t* v, int samples) {
    int x = 0;
#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    // Unaligned input/outputs and scalar tails are supported. Only complete
    // 16-sample blocks are loaded, within the planes validated by WriteFrame.
    for (; x + 16 <= samples; x += 16) {
        const __m128i uu = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u + x));
        const __m128i vv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + x));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 2 * x), _mm_unpacklo_epi8(uu, vv));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 2 * x + 16), _mm_unpackhi_epi8(uu, vv));
    }
#endif
    for (; x < samples; ++x) {
        destination[2 * x] = u[x]; destination[2 * x + 1] = v[x];
    }
}
bool Prepare(AVFrame*& stored, int width, int height, AVPixelFormat format) {
    if (stored && stored->width == width && stored->height == height && stored->format == format) return true;
    AVFrame* next = av_frame_alloc();
    if (!next) return false;
    next->width = width; next->height = height; next->format = format;
    if (av_frame_get_buffer(next, 64) < 0) { av_frame_free(&next); return false; }
    av_frame_free(&stored); stored = next;
    return true;
}
int Matrix(const AVFrame* frame) {
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    default: return SWS_CS_ITU601;
    }
}
bool FullRange(const AVFrame* frame) {
    const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    return frame->color_range == AVCOL_RANGE_JPEG || (descriptor->flags & AV_PIX_FMT_FLAG_RGB) ||
        frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;
}
}

MFSharedMemWriter::MFSharedMemWriter(Names names) : _names(std::move(names)) {}
MFSharedMemWriter::~MFSharedMemWriter() { Close(); }
bool MFSharedMemWriter::Initialize(int width, int height) {
    std::lock_guard<std::mutex> lock(_mtx);
    return InitializeLocked(width, height);
}
bool MFSharedMemWriter::InitializeLocked(int width, int height) {
    DWORD bytes = 0;
    if (width <= 0 || height <= 0 || !FrameSize(width, height, bytes)) return false;
    if (_mapping.header) {
        if (width != _targetWidth || height != _targetHeight) {
            MutexLock lock(_mapping.mutex);
            if (!lock.Owned()) return false;
            _mapping.header->isStreaming = 0; // old pixels retain their matching geometry
        }
    } else {
        Mapping next;
        bool primary = next.Open(_names.primaryMapping, _names.primaryMutex, true);
        if (!primary && !next.Open(_names.fallbackMapping, _names.fallbackMutex, true)) return false;
        {
            MutexLock lock(next.mutex);
            if (!lock.Owned()) return false;
            next.header->isStreaming = 0;
        }
        _mapping.Swap(next); _usingPrimary = primary;
    }
    _targetWidth = width; _targetHeight = height;
    return true;
}

bool MFSharedMemWriter::Convert(const AVFrame* input) {
    if (!Prepare(_nv12, _targetWidth, _targetHeight, AV_PIX_FMT_NV12)) return false;
    // Exact-format copies preserve limited BT.709 samples without resampling. The
    // explicit metadata gate is re-evaluated for every frame: decoder semantics
    // can change while its geometry and pixel format remain unchanged.
    if (input->width == _targetWidth && input->height == _targetHeight &&
        input->color_range == AVCOL_RANGE_MPEG && input->colorspace == AVCOL_SPC_BT709 &&
        (input->format == AV_PIX_FMT_NV12 || input->format == AV_PIX_FMT_YUV420P)) {
        // Write only to the private staging frame; publication still owns the
        // named mutex and updates geometry/index after all pixels are complete.
        for (int y = 0; y < _targetHeight; ++y)
            std::memcpy(_nv12->data[0] + y * _nv12->linesize[0],
                input->data[0] + y * input->linesize[0], _targetWidth);
        for (int y = 0; y < _targetHeight / 2; ++y) {
            auto* destination = _nv12->data[1] + y * _nv12->linesize[1];
            if (input->format == AV_PIX_FMT_NV12) {
                std::memcpy(destination, input->data[1] + y * input->linesize[1], _targetWidth);
            } else {
                const auto* u = input->data[1] + y * input->linesize[1];
                const auto* v = input->data[2] + y * input->linesize[2];
                InterleaveChroma(destination, u, v, _targetWidth / 2);
            }
        }
        return true;
    }
    const auto* source = input;
    const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
    const int* outputMatrix = sws_getCoefficients(SWS_CS_ITU709);
    const int flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    // A same-format YUV shortcut can ignore matrix/range changes. Use full-range
    // RGB when the input is not already limited BT.709, then encode the advertised output.
    if (!(descriptor->flags & AV_PIX_FMT_FLAG_RGB) && (Matrix(input) != SWS_CS_ITU709 || FullRange(input))) {
        if (!Prepare(_rgb, input->width, input->height, AV_PIX_FMT_BGR24)) return false;
        _toRgb = sws_getCachedContext(_toRgb, input->width, input->height, static_cast<AVPixelFormat>(input->format),
            input->width, input->height, AV_PIX_FMT_BGR24, flags, nullptr, nullptr, nullptr);
        const int* coefficients = sws_getCoefficients(Matrix(input));
        if (!_toRgb || sws_setColorspaceDetails(_toRgb, coefficients, FullRange(input) ? 1 : 0,
            coefficients, 1, 0, 1 << 16, 1 << 16) < 0 ||
            sws_scale(_toRgb, input->data, input->linesize, 0, input->height, _rgb->data, _rgb->linesize) != input->height)
            return false;
        source = _rgb;
    }
    const auto fitted = Fit(input->width, input->height, _targetWidth, _targetHeight);
    const int width = fitted.width, height = fitted.height;
    const int offsetX = fitted.x, offsetY = fitted.y;
    _toNv12 = sws_getCachedContext(_toNv12, source->width, source->height, static_cast<AVPixelFormat>(source->format),
        width, height, AV_PIX_FMT_NV12, flags, nullptr, nullptr, nullptr);
    const bool rgb = (av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source->format))->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    if (!_toNv12 || sws_setColorspaceDetails(_toNv12, outputMatrix, rgb ? 1 : 0,
        outputMatrix, 0, 0, 1 << 16, 1 << 16) < 0) return false;
    uint8_t* destination[4]{_nv12->data[0] + offsetY * _nv12->linesize[0] + offsetX,
        _nv12->data[1] + (offsetY / 2) * _nv12->linesize[1] + offsetX, nullptr, nullptr};
    if (sws_scale(_toNv12, source->data, source->linesize, 0, source->height, destination, _nv12->linesize) != height)
        return false;
    // Full-frame conversion overwrites all active pixels. For an aspect fit,
    // clear only bars after conversion: swscale's aligned writes can spill past
    // a narrow fitted rectangle into neighboring pixels in the wider frame.
    if (width != _targetWidth || height != _targetHeight || offsetX || offsetY) {
        for (int y = 0; y < _targetHeight; ++y) {
            auto* row = _nv12->data[0] + y * _nv12->linesize[0];
            if (y < offsetY || y >= offsetY + height) std::memset(row, 16, _targetWidth);
            else {
                std::memset(row, 16, offsetX);
                std::memset(row + offsetX + width, 16, _targetWidth - offsetX - width);
            }
        }
        for (int y = 0; y < _targetHeight / 2; ++y) {
            auto* row = _nv12->data[1] + y * _nv12->linesize[1];
            if (y < offsetY / 2 || y >= (offsetY + height) / 2) std::memset(row, 128, _targetWidth);
            else {
                std::memset(row, 128, offsetX);
                std::memset(row + offsetX + width, 128, _targetWidth - offsetX - width);
            }
        }
    }
    return true;
}

bool MFSharedMemWriter::Publish(Mapping& mapping) {
    auto* header = mapping.header; // named mutex must already be owned
    const UINT64 previous = (std::max)(_frameCounter, header->frameIndex);
    if (previous == (std::numeric_limits<UINT64>::max)()) return false;
    auto* destination = header->data;
    for (int y = 0; y < _targetHeight; ++y)
        std::memcpy(destination + y * _targetWidth, _nv12->data[0] + y * _nv12->linesize[0], _targetWidth);
    destination += _targetWidth * _targetHeight;
    for (int y = 0; y < _targetHeight / 2; ++y)
        std::memcpy(destination + y * _targetWidth, _nv12->data[1] + y * _nv12->linesize[1], _targetWidth);
    header->width = _targetWidth; header->height = _targetHeight; header->stride = _targetWidth;
    header->frameSize = _targetWidth * _targetHeight * 3 / 2;
    const ULONGLONG publishedAt = GetTickCount64();
    header->reserved[0] = ProtocolTag;
    header->reserved[1] = UINT32(publishedAt); header->reserved[2] = UINT32(publishedAt >> 32);
    header->frameIndex = _frameCounter = previous + 1;
    header->isStreaming = 1;
    return true;
}
bool MFSharedMemWriter::TryPublishUpgrade() {
    Mapping next;
    // Adoption needs both existing objects. A partial pair is not a usable consumer.
    if (!next.Open(_names.primaryMapping, _names.primaryMutex, false)) return false;
    {
        MutexLock newLock(next.mutex);
        if (!newLock.Owned()) return false;
        MutexLock oldLock(_mapping.mutex, 0);
        if (!oldLock.Owned()) return false;
        _frameCounter = (std::max)(_frameCounter, _mapping.header->frameIndex);
        if (!Publish(next)) return false;
        _mapping.header->isStreaming = 0;
    }
    _mapping.Swap(next); _usingPrimary = true;
    return true;
}
bool MFSharedMemWriter::WriteFrame(const AVFrame* frame) {
    if (!ValidInput(frame)) return false;
    std::lock_guard<std::mutex> lock(_mtx);
    // This private initializer expects the process lock, so lazy startup cannot recurse.
    if (!_mapping.header && !InitializeLocked(_targetWidth, _targetHeight)) return false;
    if (!Convert(frame)) return false;
    if (!_usingPrimary && ++_upgradeCheckCounter >= 90) {
        _upgradeCheckCounter = 0;
        if (TryPublishUpgrade()) return true;
    }
    MutexLock sharedLock(_mapping.mutex);
    return sharedLock.Owned() && Publish(_mapping);
}
void MFSharedMemWriter::SetStreaming(bool streaming) {
    std::lock_guard<std::mutex> lock(_mtx);
    // Resume waits for WriteFrame: it must not advertise an old frame as new input.
    if (!streaming && _mapping.header) {
        MutexLock lock(_mapping.mutex);
        if (lock.Owned()) _mapping.header->isStreaming = 0;
    }
}
void MFSharedMemWriter::Close() {
    std::lock_guard<std::mutex> lock(_mtx);
    if (_mapping.header) {
        MutexLock lock(_mapping.mutex);
        if (lock.Owned()) _mapping.header->isStreaming = 0;
    }
    _mapping.Close();
    sws_freeContext(_toRgb); _toRgb = nullptr;
    sws_freeContext(_toNv12); _toNv12 = nullptr;
    av_frame_free(&_rgb); av_frame_free(&_nv12);
    _usingPrimary = false; _upgradeCheckCounter = 0;
}
