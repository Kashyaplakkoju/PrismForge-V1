#pragma once

#include "rtsp/ffmpeg.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PrismForge::Capture {

class SampleConverter {
public:
    // Raw format conversions to owned AVFrame.
    // Handles custom strides, signed pitch (negative pitch for inverted row orders),
    // and bottom-up/top-down orientation.
    // Rejects odd geometry in subsampled formats (e.g. NV12) and undersized buffers safely.
    static bool ConvertNv12ToFrame(
        const uint8_t* data, size_t dataSize,
        int width, int height,
        int pitchY, int pitchUV,
        AVFrame* outFrame,
        AVColorRange range = AVCOL_RANGE_MPEG,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED);

    static bool ConvertYuy2ToFrame(
        const uint8_t* data, size_t dataSize,
        int width, int height,
        int pitch,
        AVFrame* outFrame,
        AVColorRange range = AVCOL_RANGE_MPEG,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED);

    static bool ConvertRgb32ToFrame(
        const uint8_t* data, size_t dataSize,
        int width, int height,
        int pitch, bool bottomUp,
        AVFrame* outFrame,
        AVColorRange range = AVCOL_RANGE_JPEG,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED);

    static bool ConvertRgb24ToFrame(
        const uint8_t* data, size_t dataSize,
        int width, int height,
        int pitch, bool bottomUp,
        AVFrame* outFrame,
        AVColorRange range = AVCOL_RANGE_JPEG,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED);

    // Allocates or reconfigures an AVFrame buffer if dimensions or format changed,
    // ensuring writability on reused frames without unconditional color space tagging.
    static bool EnsureFrameBuffer(
        AVFrame*& frame, int width, int height, AVPixelFormat format,
        AVColorRange range = AVCOL_RANGE_UNSPECIFIED,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED);
};

enum class DecodeResult : int {
    FrameReady = 0,
    NeedMorePackets = 1,
    MalformedData = 2,
    DecoderError = 3
};

// Decoder for compressed camera streams (MJPEG / H264) using FFmpeg avcodec
class CompressedFrameDecoder {
public:
    explicit CompressedFrameDecoder(AVCodecID codecId);
    ~CompressedFrameDecoder();

    CompressedFrameDecoder(const CompressedFrameDecoder&) = delete;
    CompressedFrameDecoder& operator=(const CompressedFrameDecoder&) = delete;

    bool Initialize(int width = 0, int height = 0);
    void Reset();
    void Flush();

    // Decodes compressed sample bytes into an owned AVFrame with owned padded buffer storage.
    // Handles decoder backpressure, draining, and returns borrowed AVFrame* owned by decoder.
    AVFrame* DecodePacket(const uint8_t* data, size_t dataSize, int64_t pts = 0);

    // Explicit outcome decode method
    DecodeResult Decode(const uint8_t* data, size_t dataSize, int64_t pts, AVFrame*& outFrame);

    // Drain additional decoded frames if the packet produced multiple frames or when flushing
    bool DrainNextFrame(AVFrame*& outFrame);

    AVCodecID GetCodecId() const { return m_codecId; }
    bool IsInitialized() const { return m_codecCtx != nullptr; }

private:
    AVCodecID m_codecId;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_decodedFrame = nullptr;
    AVPacket* m_packet = nullptr;
    std::vector<uint8_t> m_paddedStorage;
};

} // namespace PrismForge::Capture
