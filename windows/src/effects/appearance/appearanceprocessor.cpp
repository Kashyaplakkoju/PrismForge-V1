#include "appearanceprocessor.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <limits>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace PrismForge::Effects::Appearance {

namespace {

inline bool IsValidFloat(float v) {
    return !std::isnan(v) && !std::isinf(v);
}

// Precomputed Gaussian spatial weights for radius R=3 (7x7 window)
struct SpatialKernel {
    float weights[7][7];
    SpatialKernel() {
        const float sigmaSq2 = 2.0f * 2.5f * 2.5f;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                weights[dy + 3][dx + 3] = std::exp(-static_cast<float>(dx * dx + dy * dy) / sigmaSq2);
            }
        }
    }
};

static const SpatialKernel g_spatialKernel;

inline float CalcLuminance(float b, float g, float r) {
    return 0.114f * b + 0.587f * g + 0.299f * r;
}

} // anonymous namespace

struct AppearanceProcessor::Impl {
    AVFrame* m_cachedFrame = nullptr;

    ~Impl() {
        if (m_cachedFrame) {
            av_frame_free(&m_cachedFrame);
        }
    }

    bool EnsureWritableBuffer(int width, int height, AVPixelFormat format) {
        if (m_cachedFrame != nullptr) {
            if (m_cachedFrame->width == width &&
                m_cachedFrame->height == height &&
                m_cachedFrame->format == format &&
                av_frame_is_writable(m_cachedFrame)) {
                return true;
            }
            av_frame_free(&m_cachedFrame);
        }

        m_cachedFrame = av_frame_alloc();
        if (!m_cachedFrame) return false;

        m_cachedFrame->width = width;
        m_cachedFrame->height = height;
        m_cachedFrame->format = format;

        if (av_frame_get_buffer(m_cachedFrame, 32) < 0) {
            av_frame_free(&m_cachedFrame);
            return false;
        }
        return true;
    }
};

AppearanceProcessor::AppearanceProcessor()
    : m_impl(std::make_unique<Impl>()) {
}

AppearanceProcessor::~AppearanceProcessor() = default;

AppearanceProcessor::AppearanceProcessor(AppearanceProcessor&&) noexcept = default;
AppearanceProcessor& AppearanceProcessor::operator=(AppearanceProcessor&&) noexcept = default;

AVFrame* AppearanceProcessor::ProcessFrame(
    const AVFrame* inputFrame,
    const SkinMask* skinMask,
    const ProtectionMask* protectionMask,
    const AppearanceConfig& config,
    AppearanceOutcome& outcome
) {
    if (!inputFrame || !inputFrame->data[0]) {
        outcome = AppearanceOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->width <= 0 || inputFrame->height <= 0) {
        outcome = AppearanceOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        outcome = AppearanceOutcome::UnsupportedFormat;
        return nullptr;
    }

    const int width = inputFrame->width;
    const int height = inputFrame->height;
    const int srcLinesize = inputFrame->linesize[0];

    if (srcLinesize < width * 3) {
        outcome = AppearanceOutcome::InvalidInput;
        return nullptr;
    }

    if (!m_impl->EnsureWritableBuffer(width, height, AV_PIX_FMT_BGR24)) {
        outcome = AppearanceOutcome::OutOfMemory;
        return nullptr;
    }

    AVFrame* outputFrame = m_impl->m_cachedFrame;
    av_frame_copy_props(outputFrame, inputFrame);
    outputFrame->width = width;
    outputFrame->height = height;
    outputFrame->format = AV_PIX_FMT_BGR24;

    const int dstLinesize = outputFrame->linesize[0];

    auto copyBitExact = [&]() {
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
            uint8_t* dstRow = outputFrame->data[0] + y * dstLinesize;
            std::memcpy(dstRow, srcRow, width * 3);
        }
    };

    // 1. Explicit Bypass
    if (config.bypass) {
        copyBitExact();
        outcome = AppearanceOutcome::Bypass;
        return outputFrame;
    }

    // 2. Required Skin Mask Check: absent mask gives explicit passthrough/NotReady
    if (!skinMask || !skinMask->data) {
        copyBitExact();
        outcome = AppearanceOutcome::NotReady;
        return outputFrame;
    }

    // Validate skin mask descriptors
    if (skinMask->width != width || skinMask->height != height || skinMask->strideFloats < width) {
        outcome = AppearanceOutcome::InvalidInput;
        return nullptr;
    }

    // Validate protection mask descriptors if provided
    const bool hasProt = (protectionMask != nullptr && protectionMask->data != nullptr);
    if (hasProt) {
        if (protectionMask->width != width || protectionMask->height != height || protectionMask->strideFloats < width) {
            outcome = AppearanceOutcome::InvalidInput;
            return nullptr;
        }
    }

    const float smoothing = IsValidFloat(config.smoothingStrength)
        ? std::clamp(config.smoothingStrength, 0.0f, 1.0f) : 0.0f;
    const float detailPreserve = IsValidFloat(config.detailPreservation)
        ? std::clamp(config.detailPreservation, 0.0f, 1.0f) : 1.0f;
    const float sharpen = IsValidFloat(config.sharpenStrength)
        ? std::clamp(config.sharpenStrength, 0.0f, 1.0f) : 0.0f;
    // The optional stronger end preserves all existing presets when boost is zero.
    // Retain forty-nine samples instead of growing the kernel quadratically.
    const float boost = IsValidFloat(config.touchupBoost)
        ? std::clamp(config.touchupBoost, 0.0f, 1.0f) : 0.0f;
    const float boostedSmoothing = smoothing > 0.0f
        ? smoothing + boost * (1.0f - smoothing) : 0.0f;

    // If all adjustments are zero/neutral, passthrough
    if (smoothing <= 0.0f && sharpen <= 0.0f) {
        copyBitExact();
        outcome = AppearanceOutcome::Success;
        return outputFrame;
    }

    const float rangeSigma = 18.0f + 18.0f * boost;
    const float sigmaRangeSq2 = 2.0f * rangeSigma * rangeSigma;
    int offsets[7] = {-3, -2, -1, 0, 1, 2, 3};
    float spatialWeights[7][7];
    const int wideOffsets[7] = {-9, -5, -2, 0, 2, 5, 9};
    const float spatialSigma = 2.5f + 3.5f * boost;
    const float spatialSigmaSq2 = 2.0f * spatialSigma * spatialSigma;
    for (int i = 0; i < 7; ++i) {
        const float magnitude = static_cast<float>(std::abs(offsets[i])) + boost *
            static_cast<float>(std::abs(wideOffsets[i]) - std::abs(offsets[i]));
        offsets[i] = (i < 3 ? -1 : 1) * static_cast<int>(magnitude + 0.5f);
    }
    for (int iy = 0; iy < 7; ++iy) for (int ix = 0; ix < 7; ++ix) {
        spatialWeights[iy][ix] = boost == 0.0f ? g_spatialKernel.weights[iy][ix] :
            std::exp(-static_cast<float>(offsets[ix] * offsets[ix] + offsets[iy] * offsets[iy]) /
                spatialSigmaSq2);
    }

    // Process rows
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
        uint8_t* dstRow = outputFrame->data[0] + y * dstLinesize;

        const float* skinRow = skinMask->data + y * skinMask->strideFloats;
        const float* protRow = hasProt ? (protectionMask->data + y * protectionMask->strideFloats) : nullptr;

        for (int x = 0; x < width; ++x) {
            float sVal = skinRow[x];
            if (!IsValidFloat(sVal)) sVal = 0.0f;
            const float skinAlpha = std::clamp(sVal, 0.0f, 1.0f);

            float pVal = 0.0f;
            if (protRow) {
                pVal = protRow[x];
                if (!IsValidFloat(pVal)) pVal = 0.0f;
                pVal = std::clamp(pVal, 0.0f, 1.0f);
            }

            const float effSmoothing = skinAlpha * (1.0f - pVal) * boostedSmoothing;
            const float effSharpen = skinAlpha * (1.0f - pVal) * sharpen;

            // If feature is protected or no smoothing/sharpening applies, exact copy
            if (pVal >= 0.999f || (effSmoothing <= 1e-4f && effSharpen <= 1e-4f)) {
                dstRow[3 * x + 0] = srcRow[3 * x + 0];
                dstRow[3 * x + 1] = srcRow[3 * x + 1];
                dstRow[3 * x + 2] = srcRow[3 * x + 2];
                continue;
            }

            const float b0 = srcRow[3 * x + 0];
            const float g0 = srcRow[3 * x + 1];
            const float r0 = srcRow[3 * x + 2];
            const float y0 = CalcLuminance(b0, g0, r0);

            // Edge-preserving bilateral filter; boost widens the original footprint.
            float sumW = 0.0f;
            float sumB = 0.0f;
            float sumG = 0.0f;
            float sumR = 0.0f;
            float sumY = 0.0f;

            for (int iy = 0; iy < 7; ++iy) {
                const int ny = std::clamp(y + offsets[iy], 0, height - 1);
                const uint8_t* nRow = inputFrame->data[0] + ny * srcLinesize;

                for (int ix = 0; ix < 7; ++ix) {
                    const int nx = std::clamp(x + offsets[ix], 0, width - 1);

                    const float nb = nRow[3 * nx + 0];
                    const float ng = nRow[3 * nx + 1];
                    const float nr = nRow[3 * nx + 2];
                    const float nyLum = CalcLuminance(nb, ng, nr);

                    const float dLum = nyLum - y0;
                    const float rangeW = std::exp(-(dLum * dLum) / sigmaRangeSq2);
                    // Wider sampling must not pull protected features or outside-face
                    // colours into the cheek. Legacy boost=0 remains byte-compatible.
                    float neighborWeight = 1.0f;
                    if (boost > 0.0f) {
                        float eligible = skinMask->data[ny * skinMask->strideFloats + nx];
                        if (!IsValidFloat(eligible)) eligible = 0.0f;
                        eligible = std::clamp(eligible, 0.0f, 1.0f);
                        float protectedValue = hasProt
                            ? protectionMask->data[ny * protectionMask->strideFloats + nx] : 0.0f;
                        if (!IsValidFloat(protectedValue)) protectedValue = 0.0f;
                        protectedValue = std::clamp(protectedValue, 0.0f, 1.0f);
                        neighborWeight = eligible * (1.0f - protectedValue);
                    }
                    const float w = spatialWeights[iy][ix] * rangeW * neighborWeight;

                    sumW += w;
                    sumB += w * nb;
                    sumG += w * ng;
                    sumR += w * nr;
                    sumY += w * nyLum;
                }
            }

            const float invW = 1.0f / std::max(1e-5f, sumW);
            const float bSmooth = sumB * invW;
            const float gSmooth = sumG * invW;
            const float rSmooth = sumR * invW;
            const float ySmooth = sumY * invW;

            // Detail preservation: retain high-frequency delta
            const float bDetail = b0 - bSmooth;
            const float gDetail = g0 - gSmooth;
            const float rDetail = r0 - rSmooth;

            const float bRestored = bSmooth + detailPreserve * bDetail;
            const float gRestored = gSmooth + detailPreserve * gDetail;
            const float rRestored = rSmooth + detailPreserve * rDetail;

            // Blend smoothed result with original
            float bBlend = (1.0f - effSmoothing) * b0 + effSmoothing * bRestored;
            float gBlend = (1.0f - effSmoothing) * g0 + effSmoothing * gRestored;
            float rBlend = (1.0f - effSmoothing) * r0 + effSmoothing * rRestored;

            // Bounded-halo sharpening
            if (sharpen > 1e-4f && skinAlpha > 1e-4f) {
                const float yBlend = CalcLuminance(bBlend, gBlend, rBlend);
                const float hp = yBlend - ySmooth;
                // Bound halo to avoid unnatural edge ringing
                const float hpClamped = std::clamp(hp, -12.0f, 12.0f);
                const float boost = sharpen * hpClamped * skinAlpha * (1.0f - 0.5f * pVal);

                bBlend = std::clamp(bBlend + boost, 0.0f, 255.0f);
                gBlend = std::clamp(gBlend + boost, 0.0f, 255.0f);
                rBlend = std::clamp(rBlend + boost, 0.0f, 255.0f);
            }

            dstRow[3 * x + 0] = static_cast<uint8_t>(std::clamp(static_cast<int>(bBlend + 0.5f), 0, 255));
            dstRow[3 * x + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(gBlend + 0.5f), 0, 255));
            dstRow[3 * x + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(rBlend + 0.5f), 0, 255));
        }
    }

    outcome = AppearanceOutcome::Success;
    return outputFrame;
}

} // namespace PrismForge::Effects::Appearance
