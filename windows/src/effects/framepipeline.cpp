#include "framepipeline.h"
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace PrismForge::Effects {

namespace {

struct GradeTables {
    uint8_t warm_r[256];
    uint8_t warm_g[256];
    uint8_t warm_b[256];
    uint8_t cool_r[256];
    uint8_t cool_g[256];
    uint8_t cool_b[256];

    GradeTables() noexcept {
        for (int i = 0; i < 256; ++i) {
            // Gentle Warm grade: boost red, subtle amber-green, subdue blue
            warm_r[i] = static_cast<uint8_t>(std::min(255, i + 14));
            warm_g[i] = static_cast<uint8_t>(std::min(255, i + 4));
            warm_b[i] = static_cast<uint8_t>(std::max(0, i - 12));

            // Gentle Cool grade: boost blue, subtle cyan-green, subdue red
            cool_r[i] = static_cast<uint8_t>(std::max(0, i - 12));
            cool_g[i] = static_cast<uint8_t>(std::min(255, i + 2));
            cool_b[i] = static_cast<uint8_t>(std::min(255, i + 14));
        }
    }
};

static const GradeTables s_grades;

} // anonymous namespace

struct FramePipeline::Impl {
    std::atomic<Look> m_look{Look::Original};

    SwsContext* m_swsCtx = nullptr;
    AVFrame* m_outputFrame = nullptr;

    int m_lastWidth = 0;
    int m_lastHeight = 0;
    int m_lastFormat = AV_PIX_FMT_NONE;
    int m_lastColorRange = AVCOL_RANGE_UNSPECIFIED;
    int m_lastColorSpace = AVCOL_SPC_UNSPECIFIED;

    Impl() = default;

    ~Impl() {
        Cleanup();
    }

    void Cleanup() noexcept {
        if (m_swsCtx) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }
        if (m_outputFrame) {
            av_frame_free(&m_outputFrame);
            m_outputFrame = nullptr;
        }
        m_lastWidth = 0;
        m_lastHeight = 0;
        m_lastFormat = AV_PIX_FMT_NONE;
        m_lastColorRange = AVCOL_RANGE_UNSPECIFIED;
        m_lastColorSpace = AVCOL_SPC_UNSPECIFIED;
    }

    void SetLook(Look look) noexcept {
        switch (look) {
            case Look::Original:
            case Look::Warm:
            case Look::Cool:
            case Look::Mono:
                m_look.store(look, std::memory_order_relaxed);
                break;
            default:
                m_look.store(Look::Original, std::memory_order_relaxed);
                break;
        }
    }

    Look GetLook() const noexcept {
        Look look = m_look.load(std::memory_order_relaxed);
        switch (look) {
            case Look::Original:
            case Look::Warm:
            case Look::Cool:
            case Look::Mono:
                return look;
            default:
                return Look::Original;
        }
    }

    AVFrame* Process(AVFrame* input) {
        if (!input) {
            return nullptr;
        }

        const Look currentLook = GetLook();
        if (currentLook == Look::Original) {
            return input; // Zero-overhead bypass, exact pointer returned
        }

        // Validate input frame dimensions
        if (input->width <= 0 || input->height <= 0 ||
            av_image_check_size(input->width, input->height, 0, nullptr) < 0) {
            return input; // Graceful fallback, never crash or alter input
        }

        // Validate pixel format support before calling swscale
        if (input->format < 0) {
            return input;
        }
        const auto inPixFmt = static_cast<AVPixelFormat>(input->format);
        if (inPixFmt == AV_PIX_FMT_NONE) {
            return input;
        }
        if (sws_isSupportedInput(inPixFmt) <= 0) {
            return input; // Unsupported pixel format
        }
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(inPixFmt);
        if (!desc) {
            return input;
        }

        // Eight-bit software SDR only. Palette/hardware planes and HDR need
        // separate conversion/tone mapping; preserve the incoming frame instead.
        switch (inPixFmt) {
            case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGB24:
            case AV_PIX_FMT_BGRA: case AV_PIX_FMT_RGBA:
            case AV_PIX_FMT_BGR0: case AV_PIX_FMT_RGB0:
            case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
            case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P:
            case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P:
            case AV_PIX_FMT_NV12: case AV_PIX_FMT_NV21:
            case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422:
            case AV_PIX_FMT_GRAY8:
                break;
            default: return input;
        }
        if (input->color_trc == AVCOL_TRC_SMPTE2084 ||
            input->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
            input->colorspace == AVCOL_SPC_BT2020_CL) return input;

        // Validate required planes
        const int numPlanes = av_pix_fmt_count_planes(inPixFmt);
        if (numPlanes <= 0 || numPlanes > 4) {
            return input;
        }

        // Validate strides against minimum linesize required for each plane
        int minLinesizes[4] = {0};
        if (av_image_fill_linesizes(minLinesizes, inPixFmt, input->width) < 0) {
            return input;
        }

        for (int p = 0; p < numPlanes; ++p) {
            if (!input->data[p]) {
                return input; // Missing required plane
            }
            if (input->linesize[p] < minLinesizes[p] || input->linesize[p] <= 0) {
                return input; // Invalid or insufficient stride
            }
        }

        // Reallocate output frame if dimensions change or not allocated
        if (!m_outputFrame || m_outputFrame->width != input->width || m_outputFrame->height != input->height) {
            if (m_outputFrame) {
                av_frame_free(&m_outputFrame);
                m_outputFrame = nullptr;
            }

            m_outputFrame = av_frame_alloc();
            if (!m_outputFrame) {
                return input; // Graceful fallback
            }

            m_outputFrame->format = AV_PIX_FMT_BGR24;
            m_outputFrame->width = input->width;
            m_outputFrame->height = input->height;

            // Set appropriate colour metadata on the BGR output
            m_outputFrame->color_range = AVCOL_RANGE_JPEG;       // Full-range [0, 255] for BGR24
            m_outputFrame->colorspace = AVCOL_SPC_RGB;           // RGB colour space
            m_outputFrame->color_primaries = input->color_primaries; // No gamut transform performed
            m_outputFrame->color_trc = input->color_trc; // No transfer-function transform performed

            if (av_frame_get_buffer(m_outputFrame, 32) < 0) {
                av_frame_free(&m_outputFrame);
                m_outputFrame = nullptr;
                return input; // Graceful fallback
            }
        }

        // Reconfigure sws context if dimensions, format, or color properties change
        const bool swsNeedsRebuild = (!m_swsCtx ||
                                      input->width != m_lastWidth ||
                                      input->height != m_lastHeight ||
                                      input->format != m_lastFormat ||
                                      input->color_range != m_lastColorRange ||
                                      input->colorspace != m_lastColorSpace);

        if (swsNeedsRebuild) {
            if (m_swsCtx) {
                sws_freeContext(m_swsCtx);
                m_swsCtx = nullptr;
            }

            m_swsCtx = sws_getContext(
                input->width, input->height, static_cast<AVPixelFormat>(input->format),
                input->width, input->height, AV_PIX_FMT_BGR24,
                SWS_BILINEAR,
                nullptr, nullptr, nullptr
            );

            if (!m_swsCtx) {
                return input; // Graceful fallback
            }

            // Apply color range / space details where supported
            int in_full = (input->color_range == AVCOL_RANGE_JPEG ||
                (desc->flags & AV_PIX_FMT_FLAG_RGB) ||
                inPixFmt == AV_PIX_FMT_YUVJ420P || inPixFmt == AV_PIX_FMT_YUVJ422P ||
                inPixFmt == AV_PIX_FMT_YUVJ444P) ? 1 : 0;
            int out_full = 1; // BGR24 is full-range [0, 255]
            int brightness, contrast, saturation;
            int* inv_table = nullptr;
            int* table = nullptr;
            int srcRange = 0, dstRange = 0;
            if (sws_getColorspaceDetails(m_swsCtx, &inv_table, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation) >= 0) {
                int matrix = SWS_CS_ITU601;
                switch (input->colorspace) {
                    case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
                    case AVCOL_SPC_FCC: matrix = SWS_CS_FCC; break;
                    case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
                    case AVCOL_SPC_BT2020_NCL: matrix = SWS_CS_BT2020; break;
                    default: break;
                }
                const int* coefs = sws_getCoefficients(matrix);
                if (sws_setColorspaceDetails(m_swsCtx, coefs, in_full, table, out_full,
                                            brightness, contrast, saturation) < 0) {
                    sws_freeContext(m_swsCtx); m_swsCtx = nullptr; return input;
                }
            }

            m_lastWidth = input->width;
            m_lastHeight = input->height;
            m_lastFormat = input->format;
            m_lastColorRange = input->color_range;
            m_lastColorSpace = input->colorspace;
        }

        // Ensure output frame is writable
        if (av_frame_make_writable(m_outputFrame) < 0) {
            return input;
        }

        // Execute scale / color conversion into pipeline-owned output frame
        const int scaledLines = sws_scale(
            m_swsCtx,
            reinterpret_cast<const uint8_t* const*>(input->data),
            input->linesize,
            0,
            input->height,
            m_outputFrame->data,
            m_outputFrame->linesize
        );

        if (scaledLines != input->height) {
            return input; // Graceful fallback
        }

        // Preserve timing metadata
        m_outputFrame->pts = input->pts;
        m_outputFrame->pkt_dts = input->pkt_dts;
        m_outputFrame->duration = input->duration;
        m_outputFrame->time_base = input->time_base;
        m_outputFrame->sample_aspect_ratio = input->sample_aspect_ratio;
        m_outputFrame->best_effort_timestamp = input->best_effort_timestamp;

        // Set appropriate colour metadata on the BGR output
        m_outputFrame->color_range = AVCOL_RANGE_JPEG;       // Full-range [0, 255] for BGR24
        m_outputFrame->colorspace = AVCOL_SPC_RGB;           // RGB colour space
        m_outputFrame->color_primaries = input->color_primaries; // No gamut transform performed
        m_outputFrame->color_trc = input->color_trc; // No transfer-function transform performed

        // Apply active look (in-place on pipeline-owned BGR24 buffer)
        ApplyLook(currentLook);

        return m_outputFrame;
    }

private:
    void ApplyLook(Look look) noexcept {
        const int width = m_outputFrame->width;
        const int height = m_outputFrame->height;
        const int stride = m_outputFrame->linesize[0];
        uint8_t* const base = m_outputFrame->data[0];

        if (look == Look::Mono) {
            for (int y = 0; y < height; ++y) {
                uint8_t* const row = base + static_cast<ptrdiff_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    uint8_t* const px = row + x * 3;
                    const uint32_t b = px[0];
                    const uint32_t g = px[1];
                    const uint32_t r = px[2];
                    // Rec. 601 integer luminance: (77*R + 150*G + 29*B + 128) >> 8
                    const uint8_t y_val = static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
                    px[0] = y_val;
                    px[1] = y_val;
                    px[2] = y_val;
                }
            }
        } else if (look == Look::Warm) {
            for (int y = 0; y < height; ++y) {
                uint8_t* const row = base + static_cast<ptrdiff_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    uint8_t* const px = row + x * 3;
                    px[0] = s_grades.warm_b[px[0]];
                    px[1] = s_grades.warm_g[px[1]];
                    px[2] = s_grades.warm_r[px[2]];
                }
            }
        } else if (look == Look::Cool) {
            for (int y = 0; y < height; ++y) {
                uint8_t* const row = base + static_cast<ptrdiff_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    uint8_t* const px = row + x * 3;
                    px[0] = s_grades.cool_b[px[0]];
                    px[1] = s_grades.cool_g[px[1]];
                    px[2] = s_grades.cool_r[px[2]];
                }
            }
        }
    }
};

FramePipeline::FramePipeline() : m_impl(std::make_unique<Impl>()) {}
FramePipeline::~FramePipeline() = default;

void FramePipeline::SetLook(Look look) noexcept {
    if (m_impl) m_impl->SetLook(look);
}

Look FramePipeline::GetLook() const noexcept {
    return m_impl ? m_impl->GetLook() : Look::Original;
}

AVFrame* FramePipeline::Process(AVFrame* input) {
    return m_impl ? m_impl->Process(input) : input;
}

} // namespace PrismForge::Effects
