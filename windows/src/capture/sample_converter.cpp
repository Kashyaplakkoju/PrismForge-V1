#include "sample_converter.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <climits>

namespace PrismForge::Capture {

bool SampleConverter::EnsureFrameBuffer(
    AVFrame*& frame, int width, int height, AVPixelFormat format,
    AVColorRange range, AVColorSpace space, AVColorPrimaries primaries, AVColorTransferCharacteristic trc)
{
    if (width <= 0 || width > 16384 || height <= 0 || height > 16384 || format == AV_PIX_FMT_NONE) {
        return false;
    }

    if (frame != nullptr) {
        if (frame->width == width && frame->height == height && frame->format == format && frame->data[0] != nullptr) {
            // Ensure writability of existing allocated buffers
            if (av_frame_is_writable(frame) <= 0) {
                if (av_frame_make_writable(frame) < 0) {
                    av_frame_unref(frame);
                }
            }
            if (frame->data[0] != nullptr) {
                frame->color_range = range;
                frame->colorspace = space;
                frame->color_primaries = primaries;
                frame->color_trc = trc;
                return true;
            }
        }
        av_frame_unref(frame);
    } else {
        frame = av_frame_alloc();
        if (!frame) {
            return false;
        }
    }

    frame->width = width;
    frame->height = height;
    frame->format = format;
    frame->color_range = range;
    frame->colorspace = space;
    frame->color_primaries = primaries;
    frame->color_trc = trc;
    frame->sample_aspect_ratio = AVRational{1, 1};

    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        av_frame_unref(frame); // Converter callers retain the AVFrame object itself.
        return false;
    }

    return true;
}

bool SampleConverter::ConvertNv12ToFrame(
    const uint8_t* data, size_t dataSize,
    int width, int height,
    int pitchY, int pitchUV,
    AVFrame* outFrame,
    AVColorRange range, AVColorSpace space, AVColorPrimaries primaries, AVColorTransferCharacteristic trc)
{
    if (!data || width <= 0 || width > 16384 || height <= 0 || height > 16384 || !outFrame) {
        return false;
    }

    // NV12 requires even dimensions for 4:2:0 subsampling
    if (width % 2 != 0 || height % 2 != 0) {
        return false;
    }

    if (pitchY == INT_MIN || pitchUV == INT_MIN) return false;
    int absPitchY = std::abs(pitchY);
    int absPitchUV = std::abs(pitchUV);
    if (absPitchY <= 0) absPitchY = width;
    if (absPitchUV <= 0) absPitchUV = width;

    if (absPitchY < width || absPitchUV < width) {
        return false;
    }

    if (static_cast<size_t>(absPitchY) > SIZE_MAX / static_cast<size_t>(height) ||
        static_cast<size_t>(absPitchUV) > SIZE_MAX / static_cast<size_t>(height)) return false;
    const size_t yPlaneSize = static_cast<size_t>(absPitchY) * height;
    const size_t uvPlaneSize = static_cast<size_t>(absPitchUV) * (height / 2 - 1) + width;
    if (uvPlaneSize > SIZE_MAX - yPlaneSize) return false;
    const size_t requiredSize = yPlaneSize + uvPlaneSize;

    if (dataSize < requiredSize) {
        return false;
    }

    if (!EnsureFrameBuffer(outFrame, width, height, AV_PIX_FMT_NV12, range, space, primaries, trc)) {
        return false;
    }

    // Copy Y plane (handling negative signed pitch if applicable)
    for (int y = 0; y < height; ++y) {
        const int srcY = (pitchY < 0) ? (height - 1 - y) : y;
        const uint8_t* srcRow = data + static_cast<size_t>(srcY) * absPitchY;
        uint8_t* dstRow = outFrame->data[0] + static_cast<size_t>(y) * outFrame->linesize[0];
        std::memcpy(dstRow, srcRow, width);
    }

    // Copy interleaved UV plane
    const uint8_t* uvSrcStart = data + yPlaneSize;
    const int uvHeight = height / 2;
    for (int y = 0; y < uvHeight; ++y) {
        const int srcY = (pitchUV < 0) ? (uvHeight - 1 - y) : y;
        const uint8_t* srcRow = uvSrcStart + static_cast<size_t>(srcY) * absPitchUV;
        uint8_t* dstRow = outFrame->data[1] + static_cast<size_t>(y) * outFrame->linesize[1];
        std::memcpy(dstRow, srcRow, width);
    }

    return true;
}

bool SampleConverter::ConvertYuy2ToFrame(
    const uint8_t* data, size_t dataSize,
    int width, int height,
    int pitch,
    AVFrame* outFrame,
    AVColorRange range, AVColorSpace space, AVColorPrimaries primaries, AVColorTransferCharacteristic trc)
{
    if (!data || width <= 0 || width > 16384 || height <= 0 || height > 16384 || !outFrame) {
        return false;
    }

    if (width % 2) return false;
    const int minPitch = width * 2;
    if (pitch == INT_MIN) return false;
    int absPitch = std::abs(pitch);
    if (absPitch <= 0) absPitch = minPitch;
    if (absPitch < minPitch) {
        return false;
    }

    if (static_cast<size_t>(absPitch) > SIZE_MAX / static_cast<size_t>(height)) return false;
    const size_t requiredSize = static_cast<size_t>(absPitch) * (height - 1) + minPitch;
    if (dataSize < requiredSize) {
        return false;
    }

    if (!EnsureFrameBuffer(outFrame, width, height, AV_PIX_FMT_YUYV422, range, space, primaries, trc)) {
        return false;
    }

    for (int y = 0; y < height; ++y) {
        const int srcY = (pitch < 0) ? (height - 1 - y) : y;
        const uint8_t* srcRow = data + static_cast<size_t>(srcY) * absPitch;
        uint8_t* dstRow = outFrame->data[0] + static_cast<size_t>(y) * outFrame->linesize[0];
        std::memcpy(dstRow, srcRow, minPitch);
    }

    return true;
}

bool SampleConverter::ConvertRgb32ToFrame(
    const uint8_t* data, size_t dataSize,
    int width, int height,
    int pitch, bool bottomUp,
    AVFrame* outFrame,
    AVColorRange range, AVColorSpace space, AVColorPrimaries primaries, AVColorTransferCharacteristic trc)
{
    if (!data || width <= 0 || width > 16384 || height <= 0 || height > 16384 || !outFrame) {
        return false;
    }

    const int minPitch = width * 4;
    if (pitch == INT_MIN) return false;
    int absPitch = std::abs(pitch);
    if (absPitch <= 0) absPitch = minPitch;
    if (absPitch < minPitch) {
        return false;
    }

    if (static_cast<size_t>(absPitch) > SIZE_MAX / static_cast<size_t>(height)) return false;
    const size_t requiredSize = static_cast<size_t>(absPitch) * (height - 1) + minPitch;
    if (dataSize < requiredSize) {
        return false;
    }

    if (!EnsureFrameBuffer(outFrame, width, height, AV_PIX_FMT_BGR0, range, space, primaries, trc)) {
        return false;
    }

    const bool invertRows = bottomUp || (pitch < 0);

    for (int y = 0; y < height; ++y) {
        const int srcY = invertRows ? (height - 1 - y) : y;
        const uint8_t* srcRow = data + static_cast<size_t>(srcY) * absPitch;
        uint8_t* dstRow = outFrame->data[0] + static_cast<size_t>(y) * outFrame->linesize[0];
        std::memcpy(dstRow, srcRow, minPitch);
    }

    return true;
}

bool SampleConverter::ConvertRgb24ToFrame(
    const uint8_t* data, size_t dataSize,
    int width, int height,
    int pitch, bool bottomUp,
    AVFrame* outFrame,
    AVColorRange range, AVColorSpace space, AVColorPrimaries primaries, AVColorTransferCharacteristic trc)
{
    if (!data || width <= 0 || width > 16384 || height <= 0 || height > 16384 || !outFrame) {
        return false;
    }

    const int minPitch = width * 3;
    if (pitch == INT_MIN) return false;
    int absPitch = std::abs(pitch);
    if (absPitch <= 0) absPitch = minPitch;
    if (absPitch < minPitch) {
        return false;
    }

    if (static_cast<size_t>(absPitch) > SIZE_MAX / static_cast<size_t>(height)) return false;
    const size_t requiredSize = static_cast<size_t>(absPitch) * (height - 1) + minPitch;
    if (dataSize < requiredSize) {
        return false;
    }

    if (!EnsureFrameBuffer(outFrame, width, height, AV_PIX_FMT_BGR24, range, space, primaries, trc)) {
        return false;
    }

    const bool invertRows = bottomUp || (pitch < 0);

    for (int y = 0; y < height; ++y) {
        const int srcY = invertRows ? (height - 1 - y) : y;
        const uint8_t* srcRow = data + static_cast<size_t>(srcY) * absPitch;
        uint8_t* dstRow = outFrame->data[0] + static_cast<size_t>(y) * outFrame->linesize[0];
        std::memcpy(dstRow, srcRow, minPitch);
    }

    return true;
}

CompressedFrameDecoder::CompressedFrameDecoder(AVCodecID codecId)
    : m_codecId(codecId)
{
}

CompressedFrameDecoder::~CompressedFrameDecoder() {
    Reset();
}

void CompressedFrameDecoder::Reset() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
        m_decodedFrame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    m_paddedStorage.clear();
}

void CompressedFrameDecoder::Flush() {
    if (m_codecCtx) {
        avcodec_send_packet(m_codecCtx, nullptr);
        AVFrame* dummy = av_frame_alloc();
        if (dummy) {
            while (avcodec_receive_frame(m_codecCtx, dummy) == 0) {
                av_frame_unref(dummy);
            }
            av_frame_free(&dummy);
        }
        avcodec_flush_buffers(m_codecCtx);
    }
}

bool CompressedFrameDecoder::Initialize(int width, int height) {
    Reset();

    const AVCodec* codec = avcodec_find_decoder(m_codecId);
    if (!codec) {
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        return false;
    }

    if (width > 0 && height > 0) {
        m_codecCtx->width = width;
        m_codecCtx->height = height;
    }

    m_codecCtx->thread_count = 1; // Deterministic single-thread decoding for low latency

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        Reset();
        return false;
    }

    m_decodedFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    if (!m_decodedFrame || !m_packet) {
        Reset();
        return false;
    }

    return true;
}

DecodeResult CompressedFrameDecoder::Decode(const uint8_t* data, size_t dataSize, int64_t pts, AVFrame*& outFrame) {
    outFrame = nullptr;
    if (!data || dataSize == 0 || dataSize > 50 * 1024 * 1024) {
        return DecodeResult::MalformedData;
    }

    if (!m_codecCtx && !Initialize()) {
        return DecodeResult::DecoderError;
    }

    // Allocate owned storage with mandatory FFmpeg input padding
    m_paddedStorage.resize(dataSize + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(m_paddedStorage.data(), data, dataSize);
    std::memset(m_paddedStorage.data() + dataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(m_packet);
    m_packet->data = m_paddedStorage.data();
    m_packet->size = static_cast<int>(dataSize);
    m_packet->pts = pts;
    m_packet->dts = pts;

    int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
    if (sendRet == AVERROR(EAGAIN)) {
        // Backpressure! Drain pending frames first, then retry send
        AVFrame* drain = av_frame_alloc();
        if (drain) {
            while (avcodec_receive_frame(m_codecCtx, drain) == 0) {
                av_frame_unref(drain);
            }
            av_frame_free(&drain);
        }
        sendRet = avcodec_send_packet(m_codecCtx, m_packet);
    }

    if (sendRet == AVERROR_INVALIDDATA) {
        return DecodeResult::MalformedData;
    } else if (sendRet < 0) {
        return DecodeResult::DecoderError;
    }

    int recvRet = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
    if (recvRet == 0) {
        outFrame = m_decodedFrame;
        return DecodeResult::FrameReady;
    } else if (recvRet == AVERROR(EAGAIN)) {
        return DecodeResult::NeedMorePackets;
    } else if (recvRet == AVERROR_INVALIDDATA) {
        return DecodeResult::MalformedData;
    } else {
        return DecodeResult::DecoderError;
    }
}

bool CompressedFrameDecoder::DrainNextFrame(AVFrame*& outFrame) {
    outFrame = nullptr;
    if (!m_codecCtx || !m_decodedFrame) {
        return false;
    }
    if (avcodec_receive_frame(m_codecCtx, m_decodedFrame) == 0) {
        outFrame = m_decodedFrame;
        return true;
    }
    return false;
}

AVFrame* CompressedFrameDecoder::DecodePacket(const uint8_t* data, size_t dataSize, int64_t pts) {
    AVFrame* frame = nullptr;
    DecodeResult res = Decode(data, dataSize, pts, frame);
    if (res == DecodeResult::FrameReady) {
        return frame;
    }
    return nullptr;
}

} // namespace PrismForge::Capture
