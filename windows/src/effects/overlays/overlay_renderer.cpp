#include "overlay_renderer.h"
#include "overlay_validation.h"
#include "windows/src/effects/analysisbridge/analysis_types.h"

#include <cmath>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace PrismForge::Effects::Overlays {

namespace {

inline uint8_t ClampU8(int v) {
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
}

inline float ClampF(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

} // anonymous namespace

OverlayRenderer::OverlayRenderer() {
    Reset();
}

OverlayRenderer::~OverlayRenderer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cachedFrame) {
        av_frame_free(&m_cachedFrame);
    }
}

void OverlayRenderer::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_evaluator.Reset();
}

TrackedTransform OverlayRenderer::GetTrackedTransform(AnchorMode mode) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_evaluator.GetTrackedTransform(mode);
}

TrackedTransform OverlayRenderer::ComputeTargetTransform(
    AnchorMode mode,
    const TrackedAccessoryConfig& config,
    const AnalysisBridge::FaceLandmarkBundle& landmarks,
    int frameWidth,
    int frameHeight,
    const AssetAnchorMetrics& metrics)
{
    return TrackingEvaluator::ComputeTargetTransform(mode, config, landmarks, frameWidth, frameHeight, metrics);
}


AVFrame* OverlayRenderer::AllocateOrReuseOutput(const AVFrame* inputFrame) {
    if (m_cachedFrame != nullptr) {
        if (m_cachedFrame->width == inputFrame->width &&
            m_cachedFrame->height == inputFrame->height &&
            m_cachedFrame->format == inputFrame->format &&
            av_frame_is_writable(m_cachedFrame)) {
            return m_cachedFrame;
        }
        av_frame_free(&m_cachedFrame);
    }

    m_cachedFrame = av_frame_alloc();
    if (!m_cachedFrame) return nullptr;

    m_cachedFrame->width = inputFrame->width;
    m_cachedFrame->height = inputFrame->height;
    m_cachedFrame->format = inputFrame->format;

    if (av_frame_get_buffer(m_cachedFrame, 32) < 0) {
        av_frame_free(&m_cachedFrame);
        return nullptr;
    }

    return m_cachedFrame;
}

AVFrame* OverlayRenderer::ProcessFrame(
    AVFrame* inputFrame,
    const OverlayStackConfig& config,
    const AnalysisBridge::FaceLandmarkBundle* landmarks,
    const AnalysisBridge::MaskBuffer* subjectMask,
    OverlayOutcome* outOutcome,
    TrackingEvaluator* externalEvaluator,
    uint64_t sourceGeneration, int64_t timestampPts, uint64_t sequenceId)
{
    if (!inputFrame || !inputFrame->data[0]) {
        if (outOutcome) *outOutcome = OverlayOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->width <= 0 || inputFrame->height <= 0 || inputFrame->width > 16384 || inputFrame->height > 16384 ||
        int64_t(inputFrame->width)*inputFrame->height > 64LL*1024*1024 || !ValidConfiguration(config)) {
        if (outOutcome) *outOutcome = OverlayOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        if (outOutcome) *outOutcome = OverlayOutcome::UnsupportedFormat;
        return nullptr;
    }

    const int width = inputFrame->width;
    const int height = inputFrame->height;
    const int srcLinesize = inputFrame->linesize[0];
    if (srcLinesize < width * 3) {
        if (outOutcome) *outOutcome = OverlayOutcome::InvalidInput;
        return nullptr;
    }

    // Raw BGR descriptors may omit extended_data. Inspect the standard owner
    // slots directly instead of dereferencing auxiliary planes in FFmpeg.
    const auto start = reinterpret_cast<uintptr_t>(inputFrame->data[0]);
    const uint64_t span = uint64_t(srcLinesize) * (height - 1) + uint64_t(width) * 3;
    if (span > (1ULL << 30) || span > UINTPTR_MAX-start) {
        if (outOutcome) *outOutcome = OverlayOutcome::InvalidInput;
        return nullptr;
    }
    bool hasOwner=false, covered=false;
    for (const AVBufferRef* owner : inputFrame->buf) {
        if (!owner) continue;
        hasOwner=true;
        if (!owner->data || !owner->size) continue;
        const auto base=reinterpret_cast<uintptr_t>(owner->data);
        if (owner->size > UINTPTR_MAX-base || start < base || start > base+owner->size) continue;
        if (span <= base+owner->size-start) {covered=true;break;}
    }
    // A legitimate synchronous borrowed plane has no owner and remains valid.
    // For an owned descriptor every row must fit an actual standard buffer.
    if (hasOwner && !covered) {
        if (outOutcome) *outOutcome = OverlayOutcome::InvalidInput;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    TrackingEvaluator& evaluator = externalEvaluator ? *externalEvaluator : m_evaluator;

    // Bypass check
    if (config.bypass || (!config.glasses.enabled && !config.hat.enabled && !config.studioFrame.enabled)) {
        evaluator.Reset();
        if (outOutcome) *outOutcome = OverlayOutcome::Bypass;
        return inputFrame;
    }

    try {
    // Retrieve procedural assets
    AssetAnchorMetrics glassesMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioAviatorGlasses);
    AssetAnchorMetrics hatMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioCap);

    // Update layer transforms using evaluator
    evaluator.Update(config, landmarks, width, height, sourceGeneration, timestampPts, sequenceId);

    TrackedTransform curGlasses = evaluator.GetGlassesTransform();
    TrackedTransform curHat = evaluator.GetHatTransform();

    const bool maskReady = subjectMask && ValidMask(*subjectMask);
    bool needGlasses = config.glasses.enabled && curGlasses.opacity > 0.001f && curGlasses.valid &&
        (config.glasses.placement != OverlayPlacement::BehindSubject || maskReady);
    bool needHat = config.hat.enabled && curHat.opacity > 0.001f && curHat.valid &&
        (config.hat.placement != OverlayPlacement::BehindSubject || maskReady);
    bool needFrame = (config.studioFrame.enabled && config.studioFrame.opacity > 0.001f);

    if (!needGlasses && !needHat && !needFrame) {
        if (outOutcome) *outOutcome = (config.glasses.enabled && config.glasses.baseOpacity > .001f || config.hat.enabled && config.hat.baseOpacity > .001f) ? OverlayOutcome::NotReady : OverlayOutcome::Bypass;
        return inputFrame;
    }

    // Allocate writable output buffer
    AVFrame* outputFrame = AllocateOrReuseOutput(inputFrame);
    if (!outputFrame) {
        if (outOutcome) *outOutcome = OverlayOutcome::OutOfMemory;
        return nullptr;
    }

    // Preserve metadata
    av_frame_copy_props(outputFrame, inputFrame);

    // Deep copy input frame BGR24 pixels
    const int dstLinesize = outputFrame->linesize[0];
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            outputFrame->data[0] + y * dstLinesize,
            inputFrame->data[0] + y * srcLinesize,
            width * 3);
    }

    // Composite layers in logical order: Hat -> Glasses -> Screen Frame
    if (needHat) {
        RgbaImageBuffer capAsset = ProceduralAssetGenerator::GetAsset(ProceduralAssetId::StudioCap);
        CompositeLayer(outputFrame, capAsset, hatMetrics, curHat, config.hat.placement, subjectMask);
    }

    if (needGlasses) {
        RgbaImageBuffer glassesAsset = ProceduralAssetGenerator::GetAsset(ProceduralAssetId::StudioAviatorGlasses);
        CompositeLayer(outputFrame, glassesAsset, glassesMetrics, curGlasses, config.glasses.placement, subjectMask);
    }

    if (needFrame) {
        RgbaImageBuffer frameAsset = ProceduralAssetGenerator::GetAsset(ProceduralAssetId::StudioDecorativeFrame);
        CompositeScreenFrame(outputFrame, frameAsset, config.studioFrame.opacity);
    }

    if (outOutcome) *outOutcome = OverlayOutcome::Success;
    return outputFrame;
    } catch (const std::bad_alloc&) {
        if (outOutcome) *outOutcome = OverlayOutcome::OutOfMemory;
        return nullptr;
    }
}

AVFrame* OverlayRenderer::ProcessFrameWithAnalysis(
    AVFrame* inputFrame,
    const OverlayStackConfig& config,
    const AnalysisBridge::AnalysisBundle* analysisBundle,
    OverlayOutcome* outOutcome,
    TrackingEvaluator* externalEvaluator,
    uint64_t sourceGeneration, int64_t timestampPts, uint64_t sequenceId)
{
    const AnalysisBridge::FaceLandmarkBundle* landmarks = nullptr;
    const AnalysisBridge::MaskBuffer* subjectMask = nullptr;

    if (analysisBundle && !sourceGeneration && !sequenceId) {
        sourceGeneration = analysisBundle->sourceGeneration; timestampPts = analysisBundle->timestampPts; sequenceId = analysisBundle->sequenceId;
    }
    if (MatchesFrame(analysisBundle,sourceGeneration,sequenceId,timestampPts)) {
        landmarks = &analysisBundle->faceLandmarks;
        if (ValidMask(analysisBundle->personMask)) {
            subjectMask = &analysisBundle->personMask;
        }
    }

    return ProcessFrame(inputFrame, config, landmarks, subjectMask, outOutcome, externalEvaluator, sourceGeneration, timestampPts, sequenceId);
}

void OverlayRenderer::CompositeLayer(
    AVFrame* targetFrame,
    const RgbaImageBuffer& asset,
    const AssetAnchorMetrics& metrics,
    const TrackedTransform& transform,
    OverlayPlacement placement,
    const AnalysisBridge::MaskBuffer* subjectMask)
{
    if (!asset.IsValid() || transform.opacity <= 0.001f || transform.scaleX <= 0.001f) {
        return;
    }

    const int targetWidth = targetFrame->width;
    const int targetHeight = targetFrame->height;
    const int dstLinesize = targetFrame->linesize[0];
    uint8_t* dstData = targetFrame->data[0];

    const int assetW = asset.width;
    const int assetH = asset.height;
    const int assetStride = asset.strideBytes;
    const uint8_t* assetData = asset.data.data();

    const float ax = metrics.anchorX;
    const float ay = metrics.anchorY;
    const float cx = transform.posX;
    const float cy = transform.posY;
    const float scale = transform.scaleX;
    const float scaleY = transform.scaleY;
    const float angle = transform.rotationRadians;
    const float cosA = std::cos(angle);
    const float sinA = std::sin(angle);

    // Compute bounding box of transformed asset
    const float cornersU[4] = { 0.0f, static_cast<float>(assetW), static_cast<float>(assetW), 0.0f };
    const float cornersV[4] = { 0.0f, 0.0f, static_cast<float>(assetH), static_cast<float>(assetH) };

    float minX = 1e9f, maxX = -1e9f;
    float minY = 1e9f, maxY = -1e9f;

    for (int i = 0; i < 4; ++i) {
        float du = cornersU[i] - ax;
        float dv = cornersV[i] - ay;
        float x = cx + cosA * scale * du - sinA * scaleY * dv;
        float y = cy + sinA * scale * du + cosA * scaleY * dv;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }

    int startX = std::max(0, static_cast<int>(std::floor(minX)));
    int endX = std::min(targetWidth - 1, static_cast<int>(std::ceil(maxX)));
    int startY = std::max(0, static_cast<int>(std::floor(minY)));
    int endY = std::min(targetHeight - 1, static_cast<int>(std::ceil(maxY)));

    if (startX > endX || startY > endY) {
        return; // Completely clipped outside frame
    }

    // Inverse affine mapping coefficients
    const float invScale = 1.0f / scale;
    const float m00 = cosA * invScale;
    const float m01 = sinA * invScale;
    const float m10 = -sinA / scaleY;
    const float m11 = cosA / scaleY;

    const bool hasMask = (placement == OverlayPlacement::BehindSubject && subjectMask && ValidMask(*subjectMask));
    const float maskScaleX = hasMask ? static_cast<float>(subjectMask->width) / static_cast<float>(targetWidth) : 0.0f;
    const float maskScaleY = hasMask ? static_cast<float>(subjectMask->height) / static_cast<float>(targetHeight) : 0.0f;

    for (int dstY = startY; dstY <= endY; ++dstY) {
        uint8_t* dstRow = dstData + dstY * dstLinesize;
        float dy = dstY - cy;

        for (int dstX = startX; dstX <= endX; ++dstX) {
            float dx = dstX - cx;

            float u = ax + (m00 * dx + m01 * dy);
            float v = ay + (m10 * dx + m11 * dy);

            if (u < 0.0f || u >= assetW - 1.0f || v < 0.0f || v >= assetH - 1.0f) {
                continue;
            }

            // Bilinear sampling
            int u0 = static_cast<int>(u);
            int v0 = static_cast<int>(v);
            float s = u - u0;
            float t = v - v0;

            const uint8_t* p00 = assetData + v0 * assetStride + u0 * 4;
            const uint8_t* p10 = p00 + 4;
            const uint8_t* p01 = p00 + assetStride;
            const uint8_t* p11 = p01 + 4;

            float w00 = (1.0f - s) * (1.0f - t);
            float w10 = s * (1.0f - t);
            float w01 = (1.0f - s) * t;
            float w11 = s * t;

            float srcR = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
            float srcG = w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
            float srcB = w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];
            float srcA = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];

            float effAlpha = (srcA / 255.0f) * transform.opacity;
            float maskMod = 1.0f;

            if (hasMask) {
                int mx = std::min(subjectMask->width - 1, static_cast<int>(dstX * maskScaleX));
                int my = std::min(subjectMask->height - 1, static_cast<int>(dstY * maskScaleY));
                float maskVal = subjectMask->data[my * subjectMask->strideFloats + mx];
                if (!std::isfinite(maskVal)) maskVal = 0;
                maskMod = std::max(0.0f, std::min(1.0f, 1.0f - maskVal));
                effAlpha *= maskMod;
            }

            if (effAlpha <= 0.001f) {
                continue;
            }

            uint8_t* dstPx = dstRow + dstX * 3;
            float invAlpha = 1.0f - effAlpha;

            if (asset.alphaContract == AlphaContract::Straight) {
                dstPx[0] = ClampU8(static_cast<int>(srcB * effAlpha + dstPx[0] * invAlpha + 0.5f));
                dstPx[1] = ClampU8(static_cast<int>(srcG * effAlpha + dstPx[1] * invAlpha + 0.5f));
                dstPx[2] = ClampU8(static_cast<int>(srcR * effAlpha + dstPx[2] * invAlpha + 0.5f));
            } else {
                // Premultiplied
                float premulFactor = transform.opacity * maskMod;
                dstPx[0] = ClampU8(static_cast<int>(srcB * premulFactor + dstPx[0] * invAlpha + 0.5f));
                dstPx[1] = ClampU8(static_cast<int>(srcG * premulFactor + dstPx[1] * invAlpha + 0.5f));
                dstPx[2] = ClampU8(static_cast<int>(srcR * premulFactor + dstPx[2] * invAlpha + 0.5f));
            }
        }
    }
}

void OverlayRenderer::CompositeScreenFrame(
    AVFrame* targetFrame,
    const RgbaImageBuffer& frameAsset,
    float opacity)
{
    if (!frameAsset.IsValid() || opacity <= 0.001f) {
        return;
    }

    const int targetWidth = targetFrame->width;
    const int targetHeight = targetFrame->height;
    const int dstLinesize = targetFrame->linesize[0];
    uint8_t* dstData = targetFrame->data[0];

    const int assetW = frameAsset.width;
    const int assetH = frameAsset.height;
    const int assetStride = frameAsset.strideBytes;
    const uint8_t* assetData = frameAsset.data.data();

    const float scaleU = static_cast<float>(assetW - 1) / static_cast<float>(std::max(1, targetWidth - 1));
    const float scaleV = static_cast<float>(assetH - 1) / static_cast<float>(std::max(1, targetHeight - 1));

    for (int dstY = 0; dstY < targetHeight; ++dstY) {
        uint8_t* dstRow = dstData + dstY * dstLinesize;
        float v = dstY * scaleV;
        int v0 = std::max(0, std::min(assetH - 2, static_cast<int>(v)));
        float t = ClampF(v - v0, 0.0f, 1.0f);

        for (int dstX = 0; dstX < targetWidth; ++dstX) {
            float u = dstX * scaleU;
            int u0 = std::max(0, std::min(assetW - 2, static_cast<int>(u)));
            float s = ClampF(u - u0, 0.0f, 1.0f);

            const uint8_t* p00 = assetData + v0 * assetStride + u0 * 4;
            const uint8_t* p10 = p00 + 4;
            const uint8_t* p01 = p00 + assetStride;
            const uint8_t* p11 = p01 + 4;

            float srcA = (1.0f - s) * (1.0f - t) * p00[3] +
                         s * (1.0f - t) * p10[3] +
                         (1.0f - s) * t * p01[3] +
                         s * t * p11[3];

            if (srcA <= 0.1f) {
                continue; // Transparent center
            }

            float srcR = (1.0f - s) * (1.0f - t) * p00[0] +
                         s * (1.0f - t) * p10[0] +
                         (1.0f - s) * t * p01[0] +
                         s * t * p11[0];
            float srcG = (1.0f - s) * (1.0f - t) * p00[1] +
                         s * (1.0f - t) * p10[1] +
                         (1.0f - s) * t * p01[1] +
                         s * t * p11[1];
            float srcB = (1.0f - s) * (1.0f - t) * p00[2] +
                         s * (1.0f - t) * p10[2] +
                         (1.0f - s) * t * p01[2] +
                         s * t * p11[2];

            float effAlpha = (srcA / 255.0f) * opacity;
            float invAlpha = 1.0f - effAlpha;

            uint8_t* dstPx = dstRow + dstX * 3;
            dstPx[0] = ClampU8(static_cast<int>(srcB * effAlpha + dstPx[0] * invAlpha + 0.5f));
            dstPx[1] = ClampU8(static_cast<int>(srcG * effAlpha + dstPx[1] * invAlpha + 0.5f));
            dstPx[2] = ClampU8(static_cast<int>(srcR * effAlpha + dstPx[2] * invAlpha + 0.5f));
        }
    }
}

} // namespace PrismForge::Effects::Overlays
