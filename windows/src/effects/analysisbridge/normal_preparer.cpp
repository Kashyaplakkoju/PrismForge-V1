#include "normal_preparer.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <new>

namespace PrismForge::Effects::AnalysisBridge {

struct NormalPreparer::Impl {
    std::vector<float> m_scratch;

    void Reset() {
        m_scratch.clear();
        m_scratch.shrink_to_fit();
    }
};

NormalPreparer::NormalPreparer()
    : m_impl(std::make_unique<Impl>()) {
}

NormalPreparer::~NormalPreparer() = default;
NormalPreparer::NormalPreparer(NormalPreparer&&) noexcept = default;
NormalPreparer& NormalPreparer::operator=(NormalPreparer&&) noexcept = default;

void NormalPreparer::Reset() {
    if (m_impl) {
        m_impl->Reset();
    }
}

bool NormalPreparer::PrepareNormals(
    const NormalBuffer& srcNormals,
    int targetWidth,
    int targetHeight,
    const NormalPreparationConfig& config,
    NormalBuffer& dstNormals
) {
    if (!srcNormals.IsValid() || static_cast<int>(config.resizeMode)<0 || static_cast<int>(config.resizeMode)>2) {
        return false;
    }

    if (targetWidth <= 0 || targetHeight <= 0) {
        return false;
    }

    if (targetWidth > NormalBuffer::kMaxDimension || targetHeight > NormalBuffer::kMaxDimension) {
        return false;
    }

    const uint64_t dstStrideFloats = static_cast<uint64_t>(targetWidth) * 3ULL;
    const uint64_t totalFloats = dstStrideFloats * static_cast<uint64_t>(targetHeight);

    if (totalFloats > (SIZE_MAX / sizeof(float))) {
        return false;
    }

    std::vector<float> outputData;
    try {
        outputData.resize(static_cast<size_t>(totalFloats));
    } catch (const std::bad_alloc&) {
        return false;
    }

    const int srcW = srcNormals.width;
    const int srcH = srcNormals.height;
    const size_t srcStride = static_cast<size_t>(srcNormals.strideFloats);
    const float* srcData = srcNormals.data.data();

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    const float dstAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);

    if (config.resizeMode == MaskResizeMode::Stretch) {
        scaleX = static_cast<float>(targetWidth);
        scaleY = static_cast<float>(targetHeight);
        offsetX = 0.0f;
        offsetY = 0.0f;
    } else if (config.resizeMode == MaskResizeMode::Letterbox) {
        if (srcAspect > dstAspect) {
            scaleX = static_cast<float>(targetWidth);
            scaleY = static_cast<float>(targetWidth) / srcAspect;
            offsetX = 0.0f;
            offsetY = (static_cast<float>(targetHeight) - scaleY) * 0.5f;
        } else {
            scaleY = static_cast<float>(targetHeight);
            scaleX = static_cast<float>(targetHeight) * srcAspect;
            offsetX = (static_cast<float>(targetWidth) - scaleX) * 0.5f;
            offsetY = 0.0f;
        }
    } else if (config.resizeMode == MaskResizeMode::CoverCrop) {
        if (srcAspect > dstAspect) {
            scaleY = static_cast<float>(targetHeight);
            scaleX = static_cast<float>(targetHeight) * srcAspect;
            offsetX = (static_cast<float>(targetWidth) - scaleX) * 0.5f;
            offsetY = 0.0f;
        } else {
            scaleX = static_cast<float>(targetWidth);
            scaleY = static_cast<float>(targetWidth) / srcAspect;
            offsetX = 0.0f;
            offsetY = (static_cast<float>(targetHeight) - scaleY) * 0.5f;
        }
    }

    const float invScaleX = (scaleX > 1e-5f) ? (1.0f / scaleX) : 1.0f;
    const float invScaleY = (scaleY > 1e-5f) ? (1.0f / scaleY) : 1.0f;

    for (int y = 0; y < targetHeight; ++y) {
        const float v = (config.resizeMode == MaskResizeMode::Stretch)
            ? ((static_cast<float>(y) + 0.5f) / static_cast<float>(targetHeight))
            : ((static_cast<float>(y) + 0.5f - offsetY) * invScaleY);

        const float srcY = v * static_cast<float>(srcH) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(srcY)), 0, srcH - 1);
        const int y1 = std::clamp(static_cast<int>(std::floor(srcY))+1, 0, srcH - 1);
        const float fy = srcY - std::floor(srcY);

        const float* row0 = srcData + static_cast<size_t>(y0) * srcStride;
        const float* row1 = srcData + static_cast<size_t>(y1) * srcStride;

        float* dstRow = outputData.data() + static_cast<size_t>(y) * static_cast<size_t>(dstStrideFloats);

        for (int x = 0; x < targetWidth; ++x) {
            const float u = (config.resizeMode == MaskResizeMode::Stretch)
                ? ((static_cast<float>(x) + 0.5f) / static_cast<float>(targetWidth))
                : ((static_cast<float>(x) + 0.5f - offsetX) * invScaleX);

            const float srcX = u * static_cast<float>(srcW) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(srcX)), 0, srcW - 1);
            const int x1 = std::clamp(static_cast<int>(std::floor(srcX))+1, 0, srcW - 1);
            const float fx = srcX - std::floor(srcX);

            // Fetch 4 corners (nx, ny, nz)
            const bool valid00=std::isfinite(row0[3*x0]) && std::isfinite(row0[3*x0+1]) && std::isfinite(row0[3*x0+2]);
            const float n00x = valid00 ? row0[3 * x0 + 0] : 0.0f;
            const float n00y = valid00 ? row0[3 * x0 + 1] : 0.0f;
            const float n00z = valid00 ? row0[3 * x0 + 2] : 1.0f;

            const bool valid10=std::isfinite(row0[3*x1]) && std::isfinite(row0[3*x1+1]) && std::isfinite(row0[3*x1+2]);
            const float n10x = valid10 ? row0[3 * x1 + 0] : 0.0f;
            const float n10y = valid10 ? row0[3 * x1 + 1] : 0.0f;
            const float n10z = valid10 ? row0[3 * x1 + 2] : 1.0f;

            const bool valid01=std::isfinite(row1[3*x0]) && std::isfinite(row1[3*x0+1]) && std::isfinite(row1[3*x0+2]);
            const float n01x = valid01 ? row1[3 * x0 + 0] : 0.0f;
            const float n01y = valid01 ? row1[3 * x0 + 1] : 0.0f;
            const float n01z = valid01 ? row1[3 * x0 + 2] : 1.0f;

            const bool valid11=std::isfinite(row1[3*x1]) && std::isfinite(row1[3*x1+1]) && std::isfinite(row1[3*x1+2]);
            const float n11x = valid11 ? row1[3 * x1 + 0] : 0.0f;
            const float n11y = valid11 ? row1[3 * x1 + 1] : 0.0f;
            const float n11z = valid11 ? row1[3 * x1 + 2] : 1.0f;

            // Bilinear interpolation
            const float topX = n00x + fx * (n10x - n00x);
            const float topY = n00y + fx * (n10y - n00y);
            const float topZ = n00z + fx * (n10z - n00z);

            const float botX = n01x + fx * (n11x - n01x);
            const float botY = n01y + fx * (n11y - n01y);
            const float botZ = n01z + fx * (n11z - n01z);

            const float interpX = topX + fy * (botX - topX);
            const float interpY = topY + fy * (botY - topY);
            const float interpZ = topZ + fy * (botZ - topZ);

            // Sanitize & normalize
            const float lenSq = interpX * interpX + interpY * interpY + interpZ * interpZ;
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            if (std::isfinite(lenSq) && lenSq > 1e-6f) {
                const float invLen = 1.0f / std::sqrt(lenSq);
                nx = interpX * invLen;
                ny = interpY * invLen;
                nz = interpZ * invLen;
            }

            dstRow[3 * x + 0] = nx;
            dstRow[3 * x + 1] = ny;
            dstRow[3 * x + 2] = nz;
        }
    }

    dstNormals.width = targetWidth;
    dstNormals.height = targetHeight;
    dstNormals.strideFloats = static_cast<int>(dstStrideFloats);
    dstNormals.data = std::move(outputData);

    return true;
}

} // namespace PrismForge::Effects::AnalysisBridge
