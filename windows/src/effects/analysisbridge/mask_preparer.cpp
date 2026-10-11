#include "mask_preparer.h"
#include "../maskstabilizer.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace PrismForge::Effects::AnalysisBridge {

namespace {

constexpr int kMaxDimension = 8192;
constexpr size_t kMaxPixels = 64ULL * 1024 * 1024; // Bounded allocation guard (64M floats = 256MB)

inline bool IsValidFloat(float v) {
    return !std::isnan(v) && !std::isinf(v);
}

struct ResampleCoord1D {
    int idx0 = 0;
    int idx1 = 0;
    float w0 = 1.0f; // 1.0f - d
    float w1 = 0.0f; // d
    bool inBounds = true; // For letterbox zero-padding
};

struct ResampleMapKey {
    int srcW = 0;
    int srcH = 0;
    int targetW = 0;
    int targetH = 0;
    MaskResizeMode mode = MaskResizeMode::Stretch;

    bool operator==(const ResampleMapKey& o) const {
        return srcW == o.srcW && srcH == o.srcH &&
               targetW == o.targetW && targetH == o.targetH &&
               mode == o.mode;
    }
};

void ComputeCoordinateMaps(
    int srcW, int srcH,
    int targetW, int targetH,
    MaskResizeMode mode,
    std::vector<ResampleCoord1D>& xMap,
    std::vector<ResampleCoord1D>& yMap
) {
    xMap.resize(targetW);
    yMap.resize(targetH);

    if (mode == MaskResizeMode::Stretch) {
        for (int x = 0; x < targetW; ++x) {
            float u = (targetW <= 1) ? 0.0f : static_cast<float>(x) / (targetW - 1);
            u = std::clamp(u, 0.0f, 1.0f);
            float fx = u * (srcW - 1);
            int x0 = static_cast<int>(fx);
            int x1 = std::min(x0 + 1, srcW - 1);
            float dx = fx - x0;
            xMap[x] = { x0, x1, 1.0f - dx, dx, true };
        }
        for (int y = 0; y < targetH; ++y) {
            float v = (targetH <= 1) ? 0.0f : static_cast<float>(y) / (targetH - 1);
            v = std::clamp(v, 0.0f, 1.0f);
            float fy = v * (srcH - 1);
            int y0 = static_cast<int>(fy);
            int y1 = std::min(y0 + 1, srcH - 1);
            float dy = fy - y0;
            yMap[y] = { y0, y1, 1.0f - dy, dy, true };
        }
    } else if (mode == MaskResizeMode::Letterbox) {
        float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
        float dstAspect = static_cast<float>(targetW) / static_cast<float>(targetH);

        float boxW, boxH, offsetX, offsetY;
        if (srcAspect > dstAspect) {
            boxW = static_cast<float>(targetW);
            boxH = boxW / srcAspect;
            offsetX = 0.0f;
            offsetY = (targetH - boxH) * 0.5f;
        } else {
            boxH = static_cast<float>(targetH);
            boxW = boxH * srcAspect;
            offsetX = (targetW - boxW) * 0.5f;
            offsetY = 0.0f;
        }

        int startX = std::max(0, static_cast<int>(offsetX));
        int endX = std::min(targetW, static_cast<int>(offsetX + boxW));
        int startY = std::max(0, static_cast<int>(offsetY));
        int endY = std::min(targetH, static_cast<int>(offsetY + boxH));

        float denomX = std::max(1.0f, boxW - 1.0f);
        for (int x = 0; x < targetW; ++x) {
            if (x < startX || x >= endX) {
                xMap[x] = { 0, 0, 0.0f, 0.0f, false };
            } else {
                float u = (x - offsetX) / denomX;
                u = std::clamp(u, 0.0f, 1.0f);
                float fx = u * (srcW - 1);
                int x0 = static_cast<int>(fx);
                int x1 = std::min(x0 + 1, srcW - 1);
                float dx = fx - x0;
                xMap[x] = { x0, x1, 1.0f - dx, dx, true };
            }
        }

        float denomY = std::max(1.0f, boxH - 1.0f);
        for (int y = 0; y < targetH; ++y) {
            if (y < startY || y >= endY) {
                yMap[y] = { 0, 0, 0.0f, 0.0f, false };
            } else {
                float v = (y - offsetY) / denomY;
                v = std::clamp(v, 0.0f, 1.0f);
                float fy = v * (srcH - 1);
                int y0 = static_cast<int>(fy);
                int y1 = std::min(y0 + 1, srcH - 1);
                float dy = fy - y0;
                yMap[y] = { y0, y1, 1.0f - dy, dy, true };
            }
        }
    } else { // CoverCrop
        float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
        float dstAspect = static_cast<float>(targetW) / static_cast<float>(targetH);

        float scale = (srcAspect > dstAspect) ? (static_cast<float>(targetH) / srcH)
                                              : (static_cast<float>(targetW) / srcW);

        float cropW = targetW / scale;
        float cropH = targetH / scale;
        float startCropX = (srcW - cropW) * 0.5f;
        float startCropY = (srcH - cropH) * 0.5f;

        float fltTargetW = static_cast<float>(targetW);
        float fltSrcW = static_cast<float>(srcW);
        for (int x = 0; x < targetW; ++x) {
            float u = (startCropX + (x / fltTargetW) * cropW) / fltSrcW;
            u = std::clamp(u, 0.0f, 1.0f);
            float fx = u * (srcW - 1);
            int x0 = static_cast<int>(fx);
            int x1 = std::min(x0 + 1, srcW - 1);
            float dx = fx - x0;
            xMap[x] = { x0, x1, 1.0f - dx, dx, true };
        }

        float fltTargetH = static_cast<float>(targetH);
        float fltSrcH = static_cast<float>(srcH);
        for (int y = 0; y < targetH; ++y) {
            float v = (startCropY + (y / fltTargetH) * cropH) / fltSrcH;
            v = std::clamp(v, 0.0f, 1.0f);
            float fy = v * (srcH - 1);
            int y0 = static_cast<int>(fy);
            int y1 = std::min(y0 + 1, srcH - 1);
            float dy = fy - y0;
            yMap[y] = { y0, y1, 1.0f - dy, dy, true };
        }
    }
}

} // anonymous namespace

struct MaskPreparer::Impl {
    PrismForge::Effects::MaskStabilizer m_stabilizer;

    // Cache for coordinate maps (reused only when geometry & mode match)
    ResampleMapKey m_cachedKey;
    bool m_temporalEnabled=false;
    int m_temporalFeather=-1;
    std::vector<ResampleCoord1D> m_xMap;
    std::vector<ResampleCoord1D> m_yMap;

    // Reusable bounded scratch buffers
    std::vector<float> m_sanitizedSrc;
    std::vector<float> m_resampleTemp;
    std::vector<float> m_resampled;
    std::vector<float> m_featherTemp;
    std::vector<double> m_colSum;

    void Reset() {
        m_stabilizer.Reset();
        m_cachedKey = {};
        m_xMap.clear();
        m_yMap.clear();
    }
};

MaskPreparer::MaskPreparer()
    : m_impl(std::make_unique<Impl>()) {
}

MaskPreparer::~MaskPreparer() = default;

MaskPreparer::MaskPreparer(MaskPreparer&&) noexcept = default;
MaskPreparer& MaskPreparer::operator=(MaskPreparer&&) noexcept = default;

void MaskPreparer::Reset() {
    if (m_impl) m_impl->Reset();
}

bool MaskPreparer::PrepareMask(
    const MaskBuffer& srcMask,
    int targetWidth,
    int targetHeight,
    const MaskPreparationConfig& config,
    MaskBuffer& dstMask
) {
    // 1. Strict transactional input & geometry validation
    if (!srcMask.IsValid() ||
        srcMask.width > kMaxDimension || srcMask.height > kMaxDimension ||
        targetWidth <= 0 || targetHeight <= 0 ||
        targetWidth > kMaxDimension || targetHeight > kMaxDimension)
    {
        return false;
    }

    uint64_t targetPixels64 = static_cast<uint64_t>(targetWidth) * static_cast<uint64_t>(targetHeight);
    if (targetPixels64 > kMaxPixels) {
        return false;
    }

    int modeVal = static_cast<int>(config.resizeMode);
    if (modeVal < 0 || modeVal > 2) {
        return false; // Enum error
    }

    if (!IsValidFloat(config.featherRadius) || config.featherRadius < 0.0f) {
        return false; // Nonfinite or negative feather radius error
    }

    try {
        if(!m_impl) m_impl=std::make_unique<Impl>();
        const ResampleMapKey temporalKey{srcMask.width,srcMask.height,targetWidth,targetHeight,config.resizeMode};
        const int temporalFeather=config.featherRadius>=1.0f ? static_cast<int>(std::round(std::min(5.0f,config.featherRadius))) : 0;
        if(!config.enableTemporalStabilization || !m_impl->m_temporalEnabled || !(m_impl->m_cachedKey==temporalKey) || m_impl->m_temporalFeather!=temporalFeather) m_impl->m_stabilizer.Reset();
        m_impl->m_temporalEnabled=config.enableTemporalStabilization;
        m_impl->m_temporalFeather=temporalFeather;
        const int srcW = srcMask.width;
        const int srcH = srcMask.height;
        const int srcStride = srcMask.strideFloats;
        const float* srcRaw = srcMask.data.data();

        // 2. NaN/Inf sanitization into bounded contiguous source buffer
        size_t srcTotal = static_cast<size_t>(srcW) * srcH;
        m_impl->m_sanitizedSrc.resize(srcTotal);
        float* sanitized = m_impl->m_sanitizedSrc.data();

        for (int y = 0; y < srcH; ++y) {
            const float* inRow = srcRaw + y * srcStride;
            float* outRow = sanitized + y * srcW;
            for (int x = 0; x < srcW; ++x) {
                float v = inRow[x];
                outRow[x] = IsValidFloat(v) ? std::clamp(v,0.0f,1.0f) : 0.0f;
            }
        }

        // 3. Fast Separable Resampling using Precomputed Coordinate Maps
        ResampleMapKey currentKey{ srcW, srcH, targetWidth, targetHeight, config.resizeMode };
        if (!(m_impl->m_cachedKey == currentKey)) {
            ComputeCoordinateMaps(srcW, srcH, targetWidth, targetHeight, config.resizeMode,
                                  m_impl->m_xMap, m_impl->m_yMap);
            m_impl->m_cachedKey = currentKey;
        }

        const auto& xMap = m_impl->m_xMap;
        const auto& yMap = m_impl->m_yMap;

        // Pass 1: Horizontal Resampling (srcW -> targetWidth) for each source row
        m_impl->m_resampleTemp.resize(static_cast<size_t>(srcH) * targetWidth);
        float* tempResample = m_impl->m_resampleTemp.data();

        for (int sy = 0; sy < srcH; ++sy) {
            const float* sRow = sanitized + sy * srcW;
            float* tRow = tempResample + sy * targetWidth;
            for (int x = 0; x < targetWidth; ++x) {
                const auto& cx = xMap[x];
                if (!cx.inBounds) {
                    tRow[x] = 0.0f;
                } else {
                    tRow[x] = cx.w0 * sRow[cx.idx0] + cx.w1 * sRow[cx.idx1];
                }
            }
        }

        // Pass 2: Vertical Resampling (srcH -> targetHeight) into m_resampled
        m_impl->m_resampled.resize(static_cast<size_t>(targetWidth) * targetHeight);
        float* resampled = m_impl->m_resampled.data();

        for (int y = 0; y < targetHeight; ++y) {
            const auto& cy = yMap[y];
            float* dstRow = resampled + y * targetWidth;
            if (!cy.inBounds) {
                std::memset(dstRow, 0, targetWidth * sizeof(float));
            } else {
                const float* r0 = tempResample + cy.idx0 * targetWidth;
                const float* r1 = tempResample + cy.idx1 * targetWidth;
                float w0 = cy.w0;
                float w1 = cy.w1;
                for (int x = 0; x < targetWidth; ++x) {
                    dstRow[x] = w0 * r0[x] + w1 * r1[x];
                }
            }
        }

        // 4. Linear-work Sliding-Sum Box Feathering (O(1) work per pixel, independent of radius)
        float* processedMask = resampled;
        if (config.featherRadius >= 1.0f) {
            float clampedRadius = std::min(5.0f, config.featherRadius);
            int r = static_cast<int>(std::round(clampedRadius));
            int windowSize = 2 * r + 1;
            double invW = 1.0 / static_cast<double>(windowSize);

            m_impl->m_featherTemp.resize(static_cast<size_t>(targetWidth) * targetHeight);
            float* temp = m_impl->m_featherTemp.data();

            // Horizontal sliding-sum pass
            for (int y = 0; y < targetHeight; ++y) {
                const float* inRow = resampled + y * targetWidth;
                float* outRow = temp + y * targetWidth;

                // Initial window sum at x = 0 with replicated boundary clamp
                double sum = static_cast<double>(inRow[0]) * (r + 1);
                for (int dx = 1; dx <= r; ++dx) {
                    int nx = std::min(dx, targetWidth - 1);
                    sum += static_cast<double>(inRow[nx]);
                }
                outRow[0] = static_cast<float>(sum * invW);

                // Slide window across row
                for (int x = 1; x < targetWidth; ++x) {
                    int inIdx = std::min(x + r, targetWidth - 1);
                    int outIdx = std::max(x - 1 - r, 0);
                    sum += static_cast<double>(inRow[inIdx]) - static_cast<double>(inRow[outIdx]);
                    outRow[x] = static_cast<float>(sum * invW);
                }
            }

            // Vertical sliding-sum pass with row-major cache streaming
            m_impl->m_colSum.resize(targetWidth);
            double* colSum = m_impl->m_colSum.data();

            // Initial column sum at y = 0
            for (int x = 0; x < targetWidth; ++x) {
                double s = static_cast<double>(temp[x]) * (r + 1);
                for (int dy = 1; dy <= r; ++dy) {
                    int ny = std::min(dy, targetHeight - 1);
                    s += static_cast<double>(temp[ny * targetWidth + x]);
                }
                colSum[x] = s;
                resampled[x] = std::clamp(static_cast<float>(s * invW), 0.0f, 1.0f);
            }

            // Slide window down rows
            for (int y = 1; y < targetHeight; ++y) {
                int inY = std::min(y + r, targetHeight - 1);
                int outY = std::max(y - 1 - r, 0);
                const float* inRow = temp + inY * targetWidth;
                const float* outRow = temp + outY * targetWidth;
                float* dstRow = resampled + y * targetWidth;

                for (int x = 0; x < targetWidth; ++x) {
                    colSum[x] += static_cast<double>(inRow[x]) - static_cast<double>(outRow[x]);
                    dstRow[x] = std::clamp(static_cast<float>(colSum[x] * invW), 0.0f, 1.0f);
                }
            }
        }

        // 5. Optional Temporal Stabilization via MaskStabilizer
        if (config.enableTemporalStabilization) {
            const float* stabilized = m_impl->m_stabilizer.Process(processedMask, targetWidth, targetHeight, targetWidth);
            if (stabilized) {
                processedMask = const_cast<float*>(stabilized);
            }
        }

        // 6. Safe destination assignment (alias safe: scratch buffers guarantee decoupling)
        dstMask.width = targetWidth;
        dstMask.height = targetHeight;
        dstMask.strideFloats = targetWidth;
        dstMask.data.assign(processedMask, processedMask + targetWidth * targetHeight);

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace PrismForge::Effects::AnalysisBridge
