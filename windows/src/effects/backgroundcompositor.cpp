#include "backgroundcompositor.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace PrismForge::Effects {

namespace {

struct CoordX {
    int x0 = 0;
    int x1 = 0;
    float fx = 0.0f;
};

struct CoordY {
    int y0 = 0;
    int y1 = 0;
    float fy = 0.0f;
};

} // anonymous namespace

struct BackgroundCompositor::Impl {
    AVFrame* m_outputFrame = nullptr;
    std::vector<uint8_t> m_horizBuffer;
    std::vector<uint8_t> m_blurredBg;

    // Precomputed mask coordinate interpolation tables
    std::vector<CoordX> m_coordsX;
    std::vector<CoordY> m_coordsY;
    int m_lastCoordW = 0;
    int m_lastCoordH = 0;
    int m_lastMaskW = 0;
    int m_lastMaskH = 0;

    Impl() = default;

    ~Impl() {
        Cleanup();
    }

    void Cleanup() noexcept {
        if (m_outputFrame) {
            av_frame_free(&m_outputFrame);
            m_outputFrame = nullptr;
        }
        m_horizBuffer.clear();
        m_blurredBg.clear();
        m_coordsX.clear();
        m_coordsY.clear();
        m_lastCoordW = m_lastCoordH = m_lastMaskW = m_lastMaskH = 0;
    }

    void UpdateMaskCoords(int frameW, int frameH, int maskW, int maskH) {
        if (frameW == m_lastCoordW && frameH == m_lastCoordH &&
            maskW == m_lastMaskW && maskH == m_lastMaskH) {
            return;
        }

        m_coordsX.resize(frameW);
        const float scaleX = static_cast<float>(maskW) / static_cast<float>(frameW);
        const float maxMaskX = static_cast<float>(maskW - 1);

        for (int x = 0; x < frameW; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
            const float cu = std::clamp(u, 0.0f, maxMaskX);
            const int x0 = static_cast<int>(cu);
            const int x1 = std::min(x0 + 1, maskW - 1);
            m_coordsX[x].x0 = x0;
            m_coordsX[x].x1 = x1;
            m_coordsX[x].fx = cu - static_cast<float>(x0);
        }

        m_coordsY.resize(frameH);
        const float scaleY = static_cast<float>(maskH) / static_cast<float>(frameH);
        const float maxMaskY = static_cast<float>(maskH - 1);

        for (int y = 0; y < frameH; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;
            const float cv = std::clamp(v, 0.0f, maxMaskY);
            const int y0 = static_cast<int>(cv);
            const int y1 = std::min(y0 + 1, maskH - 1);
            m_coordsY[y].y0 = y0;
            m_coordsY[y].y1 = y1;
            m_coordsY[y].fy = cv - static_cast<float>(y0);
        }

        m_lastCoordW = frameW;
        m_lastCoordH = frameH;
        m_lastMaskW = maskW;
        m_lastMaskH = maskH;
    }

    AVFrame* Process(AVFrame* bgrInput, const AlphaMask* mask, const BackgroundSettings& settings) {
        if (!bgrInput) {
            return nullptr;
        }

        // Validate mode
        if (settings.mode != BackgroundMode::Original &&
            settings.mode != BackgroundMode::Blur &&
            settings.mode != BackgroundMode::Solid) {
            return bgrInput; // Invalid mode fallback
        }

        // Original mode: zero-overhead bypass, exact pointer identity preserved
        if (settings.mode == BackgroundMode::Original) {
            return bgrInput;
        }

        // Validate input frame dimensions
        if (bgrInput->width <= 0 || bgrInput->height <= 0) {
            return bgrInput;
        }

        // Arithmetic overflow check on width * 3
        if (bgrInput->width > (INT_MAX / 3)) {
            return bgrInput;
        }

        if (bgrInput->format != AV_PIX_FMT_BGR24) {
            return bgrInput;
        }
        if (!bgrInput->data[0]) {
            return bgrInput;
        }

        const int64_t minRowBytes = static_cast<int64_t>(bgrInput->width) * 3;
        if (static_cast<int64_t>(bgrInput->linesize[0]) < minRowBytes || bgrInput->linesize[0] <= 0) {
            return bgrInput;
        }

        // Checked arithmetic on total buffer allocation/read range
        const int64_t totalImageBytes = static_cast<int64_t>(bgrInput->height) * bgrInput->linesize[0];
        if (totalImageBytes <= 0 || totalImageBytes > (1LL << 30)) { // 1 GiB safe limit
            return bgrInput;
        }

        // Validate mask for active modes (Blur and Solid require valid mask)
        if (!mask || !mask->data) {
            return bgrInput;
        }
        if (mask->width <= 0 || mask->height <= 0 || mask->strideFloats < mask->width) {
            return bgrInput;
        }

        // Checked size arithmetic: check mask memory bounds
        const int64_t totalMaskFloats = static_cast<int64_t>(mask->height) * mask->strideFloats;
        if (totalMaskFloats <= 0 || totalMaskFloats > (1LL << 30)) {
            return bgrInput;
        }

        // Verify all mask floats are finite (reject NaN and Inf)
        for (int y = 0; y < mask->height; ++y) {
            const float* row = mask->data + y * mask->strideFloats;
            for (int x = 0; x < mask->width; ++x) {
                if (!std::isfinite(row[x])) {
                    return bgrInput; // Non-finite value rejected safely
                }
            }
        }

        const int width = bgrInput->width;
        const int height = bgrInput->height;

        // Reallocate output frame if dimensions change or not allocated
        if (!m_outputFrame || m_outputFrame->width != width || m_outputFrame->height != height) {
            if (m_outputFrame) {
                av_frame_free(&m_outputFrame);
                m_outputFrame = nullptr;
            }

            m_outputFrame = av_frame_alloc();
            if (!m_outputFrame) {
                return bgrInput;
            }

            m_outputFrame->format = AV_PIX_FMT_BGR24;
            m_outputFrame->width = width;
            m_outputFrame->height = height;

            if (av_frame_get_buffer(m_outputFrame, 32) < 0) {
                av_frame_free(&m_outputFrame);
                m_outputFrame = nullptr;
                return bgrInput;
            }
        }

        if (av_frame_make_writable(m_outputFrame) < 0) {
            return bgrInput;
        }

        // Preserve timing metadata
        m_outputFrame->pts = bgrInput->pts;
        m_outputFrame->pkt_dts = bgrInput->pkt_dts;
        m_outputFrame->duration = bgrInput->duration;
        m_outputFrame->time_base = bgrInput->time_base;

        // Preserve incoming colour metadata; do not force sRGB/BT709 unless specified on input
        m_outputFrame->color_range = (bgrInput->color_range != AVCOL_RANGE_UNSPECIFIED)
                                         ? bgrInput->color_range
                                         : AVCOL_RANGE_JPEG;
        m_outputFrame->colorspace = (bgrInput->colorspace != AVCOL_SPC_UNSPECIFIED)
                                        ? bgrInput->colorspace
                                        : AVCOL_SPC_RGB;
        m_outputFrame->color_primaries = bgrInput->color_primaries;
        m_outputFrame->color_trc = bgrInput->color_trc;

        const uint8_t* inBase = bgrInput->data[0];
        const int inStride = bgrInput->linesize[0];
        uint8_t* outBase = m_outputFrame->data[0];
        const int outStride = m_outputFrame->linesize[0];

        const bool is1x1 = (mask->width == 1 && mask->height == 1);
        if (!is1x1) {
            UpdateMaskCoords(width, height, mask->width, mask->height);
        }

        if (settings.mode == BackgroundMode::Solid) {
            const float solidB = static_cast<float>(settings.solidB);
            const float solidG = static_cast<float>(settings.solidG);
            const float solidR = static_cast<float>(settings.solidR);

            if (is1x1) {
                // Fast-path: uniform mask broadcast
                const float alpha = std::clamp(mask->data[0], 0.0f, 1.0f);
                const float invAlpha = 1.0f - alpha;

                for (int y = 0; y < height; ++y) {
                    const uint8_t* inRow = inBase + y * inStride;
                    uint8_t* outRow = outBase + y * outStride;

                    for (int x = 0; x < width; ++x) {
                        const float fgB = static_cast<float>(inRow[x * 3 + 0]);
                        const float fgG = static_cast<float>(inRow[x * 3 + 1]);
                        const float fgR = static_cast<float>(inRow[x * 3 + 2]);

                        outRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(std::round(fgB * alpha + solidB * invAlpha), 0.0f, 255.0f));
                        outRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(std::round(fgG * alpha + solidG * invAlpha), 0.0f, 255.0f));
                        outRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(std::round(fgR * alpha + solidR * invAlpha), 0.0f, 255.0f));
                    }
                }
            } else {
                // Optimized precomputed coordinate bilinear sampling
                for (int y = 0; y < height; ++y) {
                    const uint8_t* inRow = inBase + y * inStride;
                    uint8_t* outRow = outBase + y * outStride;

                    const auto& cy = m_coordsY[y];
                    const float* maskRow0 = mask->data + cy.y0 * mask->strideFloats;
                    const float* maskRow1 = mask->data + cy.y1 * mask->strideFloats;
                    const float fy = cy.fy;
                    const float invFy = 1.0f - fy;

                    for (int x = 0; x < width; ++x) {
                        const auto& cx = m_coordsX[x];
                        const float top = maskRow0[cx.x0] * (1.0f - cx.fx) + maskRow0[cx.x1] * cx.fx;
                        const float bot = maskRow1[cx.x0] * (1.0f - cx.fx) + maskRow1[cx.x1] * cx.fx;
                        const float alpha = std::clamp(top * invFy + bot * fy, 0.0f, 1.0f);
                        const float invAlpha = 1.0f - alpha;

                        const float fgB = static_cast<float>(inRow[x * 3 + 0]);
                        const float fgG = static_cast<float>(inRow[x * 3 + 1]);
                        const float fgR = static_cast<float>(inRow[x * 3 + 2]);

                        outRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(std::round(fgB * alpha + solidB * invAlpha), 0.0f, 255.0f));
                        outRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(std::round(fgG * alpha + solidG * invAlpha), 0.0f, 255.0f));
                        outRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(std::round(fgR * alpha + solidR * invAlpha), 0.0f, 255.0f));
                    }
                }
            }
        } else if (settings.mode == BackgroundMode::Blur) {
            // Clamped practical radius [0, 64]
            const int radius = std::clamp(settings.blurRadius, 0, 64);

            if (radius == 0) {
                // Radius 0: identity background
                for (int y = 0; y < height; ++y) {
                    const uint8_t* inRow = inBase + y * inStride;
                    uint8_t* outRow = outBase + y * outStride;
                    std::memcpy(outRow, inRow, width * 3);
                }
            } else {
                // Separable box blur on CPU: O(width * height) linear time
                const size_t bufferBytes = static_cast<size_t>(width) * height * 3;
                if (m_horizBuffer.size() < bufferBytes) {
                    m_horizBuffer.resize(bufferBytes);
                }
                if (m_blurredBg.size() < bufferBytes) {
                    m_blurredBg.resize(bufferBytes);
                }

                const int K = 2 * radius + 1;
                const int halfK = K / 2;

                // 1. Horizontal Pass (with edge clamping)
                for (int y = 0; y < height; ++y) {
                    const uint8_t* inRow = inBase + y * inStride;
                    uint8_t* hRow = m_horizBuffer.data() + static_cast<size_t>(y) * width * 3;

                    int sumB = 0, sumG = 0, sumR = 0;
                    for (int i = -radius; i <= radius; ++i) {
                        const int srcX = std::clamp(i, 0, width - 1);
                        sumB += inRow[srcX * 3 + 0];
                        sumG += inRow[srcX * 3 + 1];
                        sumR += inRow[srcX * 3 + 2];
                    }

                    hRow[0] = static_cast<uint8_t>((sumB + halfK) / K);
                    hRow[1] = static_cast<uint8_t>((sumG + halfK) / K);
                    hRow[2] = static_cast<uint8_t>((sumR + halfK) / K);

                    for (int x = 1; x < width; ++x) {
                        const int addX = std::clamp(x + radius, 0, width - 1);
                        const int subX = std::clamp(x - 1 - radius, 0, width - 1);

                        sumB += inRow[addX * 3 + 0] - inRow[subX * 3 + 0];
                        sumG += inRow[addX * 3 + 1] - inRow[subX * 3 + 1];
                        sumR += inRow[addX * 3 + 2] - inRow[subX * 3 + 2];

                        hRow[x * 3 + 0] = static_cast<uint8_t>((sumB + halfK) / K);
                        hRow[x * 3 + 1] = static_cast<uint8_t>((sumG + halfK) / K);
                        hRow[x * 3 + 2] = static_cast<uint8_t>((sumR + halfK) / K);
                    }
                }

                // 2. Vertical Pass (with edge clamping)
                for (int x = 0; x < width; ++x) {
                    int sumB = 0, sumG = 0, sumR = 0;
                    for (int j = -radius; j <= radius; ++j) {
                        const int srcY = std::clamp(j, 0, height - 1);
                        const uint8_t* px = m_horizBuffer.data() + static_cast<size_t>(srcY) * width * 3 + x * 3;
                        sumB += px[0];
                        sumG += px[1];
                        sumR += px[2];
                    }

                    uint8_t* dst0 = m_blurredBg.data() + 0 * width * 3 + x * 3;
                    dst0[0] = static_cast<uint8_t>((sumB + halfK) / K);
                    dst0[1] = static_cast<uint8_t>((sumG + halfK) / K);
                    dst0[2] = static_cast<uint8_t>((sumR + halfK) / K);

                    for (int y = 1; y < height; ++y) {
                        const int addY = std::clamp(y + radius, 0, height - 1);
                        const int subY = std::clamp(y - 1 - radius, 0, height - 1);

                        const uint8_t* addPx = m_horizBuffer.data() + static_cast<size_t>(addY) * width * 3 + x * 3;
                        const uint8_t* subPx = m_horizBuffer.data() + static_cast<size_t>(subY) * width * 3 + x * 3;

                        sumB += addPx[0] - subPx[0];
                        sumG += addPx[1] - subPx[1];
                        sumR += addPx[2] - subPx[2];

                        uint8_t* dst = m_blurredBg.data() + static_cast<size_t>(y) * width * 3 + x * 3;
                        dst[0] = static_cast<uint8_t>((sumB + halfK) / K);
                        dst[1] = static_cast<uint8_t>((sumG + halfK) / K);
                        dst[2] = static_cast<uint8_t>((sumR + halfK) / K);
                    }
                }

                // 3. Composite Foreground and Blurred Background
                if (is1x1) {
                    const float alpha = std::clamp(mask->data[0], 0.0f, 1.0f);
                    const float invAlpha = 1.0f - alpha;

                    for (int y = 0; y < height; ++y) {
                        const uint8_t* inRow = inBase + y * inStride;
                        const uint8_t* bgRow = m_blurredBg.data() + static_cast<size_t>(y) * width * 3;
                        uint8_t* outRow = outBase + y * outStride;

                        for (int x = 0; x < width; ++x) {
                            const float fgB = static_cast<float>(inRow[x * 3 + 0]);
                            const float fgG = static_cast<float>(inRow[x * 3 + 1]);
                            const float fgR = static_cast<float>(inRow[x * 3 + 2]);

                            const float bgB = static_cast<float>(bgRow[x * 3 + 0]);
                            const float bgG = static_cast<float>(bgRow[x * 3 + 1]);
                            const float bgR = static_cast<float>(bgRow[x * 3 + 2]);

                            outRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(std::round(fgB * alpha + bgB * invAlpha), 0.0f, 255.0f));
                            outRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(std::round(fgG * alpha + bgG * invAlpha), 0.0f, 255.0f));
                            outRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(std::round(fgR * alpha + bgR * invAlpha), 0.0f, 255.0f));
                        }
                    }
                } else {
                    for (int y = 0; y < height; ++y) {
                        const uint8_t* inRow = inBase + y * inStride;
                        const uint8_t* bgRow = m_blurredBg.data() + static_cast<size_t>(y) * width * 3;
                        uint8_t* outRow = outBase + y * outStride;

                        const auto& cy = m_coordsY[y];
                        const float* maskRow0 = mask->data + cy.y0 * mask->strideFloats;
                        const float* maskRow1 = mask->data + cy.y1 * mask->strideFloats;
                        const float fy = cy.fy;
                        const float invFy = 1.0f - fy;

                        for (int x = 0; x < width; ++x) {
                            const auto& cx = m_coordsX[x];
                            const float top = maskRow0[cx.x0] * (1.0f - cx.fx) + maskRow0[cx.x1] * cx.fx;
                            const float bot = maskRow1[cx.x0] * (1.0f - cx.fx) + maskRow1[cx.x1] * cx.fx;
                            const float alpha = std::clamp(top * invFy + bot * fy, 0.0f, 1.0f);
                            const float invAlpha = 1.0f - alpha;

                            const float fgB = static_cast<float>(inRow[x * 3 + 0]);
                            const float fgG = static_cast<float>(inRow[x * 3 + 1]);
                            const float fgR = static_cast<float>(inRow[x * 3 + 2]);

                            const float bgB = static_cast<float>(bgRow[x * 3 + 0]);
                            const float bgG = static_cast<float>(bgRow[x * 3 + 1]);
                            const float bgR = static_cast<float>(bgRow[x * 3 + 2]);

                            outRow[x * 3 + 0] = static_cast<uint8_t>(std::clamp(std::round(fgB * alpha + bgB * invAlpha), 0.0f, 255.0f));
                            outRow[x * 3 + 1] = static_cast<uint8_t>(std::clamp(std::round(fgG * alpha + bgG * invAlpha), 0.0f, 255.0f));
                            outRow[x * 3 + 2] = static_cast<uint8_t>(std::clamp(std::round(fgR * alpha + bgR * invAlpha), 0.0f, 255.0f));
                        }
                    }
                }
            }
        }

        return m_outputFrame;
    }
};

BackgroundCompositor::BackgroundCompositor() : m_impl(std::make_unique<Impl>()) {}
BackgroundCompositor::~BackgroundCompositor() = default;

AVFrame* BackgroundCompositor::Process(AVFrame* bgrInput, const AlphaMask* mask,
                                       const BackgroundSettings& settings) {
    return m_impl ? m_impl->Process(bgrInput, mask, settings) : bgrInput;
}

} // namespace PrismForge::Effects
