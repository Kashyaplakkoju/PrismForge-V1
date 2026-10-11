#include "studiolighting.h"
#include "effects/relighting/approximate_shadows.h"

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

namespace PrismForge::Effects::Lighting {

namespace {

// Precomputed sRGB byte [0, 255] to linear float [0.0, 1.0] table.
class SrgbLinearTable {
public:
    float table[256];

    SrgbLinearTable() {
        for (int i = 0; i < 256; ++i) {
            float c = i / 255.0f;
            table[i] = (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }
};

static const SrgbLinearTable g_srgbLinearLut;

// Exact IEC 61966-2-1 conversion from linear float to sRGB byte [0, 255].
inline uint8_t LinearToSrgbByte(float L) {
    if (L <= 0.0f) return 0;
    if (L >= 1.0f) return 255;
    float s = (L <= 0.0031308f) ? (12.92f * L) : (1.055f * std::pow(L, 1.0f / 2.4f) - 0.055f);
    int val = static_cast<int>(s * 255.0f + 0.5f);
    return static_cast<uint8_t>(std::clamp(val, 0, 255));
}

inline bool IsValidFloat(float v) {
    return !std::isnan(v) && !std::isinf(v);
}

} // anonymous namespace

float StudioLighting::CalculateSpatialLightApproximation(
    float u, float v,
    float aspect,
    const StudioLight& light
) {
    if (!light.enabled || light.intensity <= 0.0f) {
        return 0.0f;
    }

    const float posX = std::clamp(light.posX, -1.0f, 1.0f);
    const float posY = std::clamp(light.posY, -1.0f, 1.0f);
    const float posZ = std::max(0.1f, light.posZ);

    const float deltaX = (posX - u) * aspect;
    const float deltaY = posY - v;
    const float deltaZ = posZ;

    const float distSq = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
    const float dist = std::sqrt(distSq);
    if (dist < 1e-5f) {
        return 1.0f;
    }

    const float lx = deltaX / dist;
    const float ly = deltaY / dist;
    const float lz = deltaZ / dist;

    // Convex virtual frontal curvature approximation representing natural face/torso surface
    float nx = 0.45f * u;
    float ny = 0.35f * v;
    float nz = std::sqrt(std::max(0.05f, 1.0f - nx * nx - ny * ny));
    const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    nx *= invLen;
    ny *= invLen;
    nz *= invLen;

    const float dotNL = nx * lx + ny * ly + nz * lz;
    const float diffuse = std::max(0.0f, 0.35f + 0.65f * dotNL);

    // Soft distance falloff
    const float falloff = 1.0f / (1.0f + 0.4f * std::max(0.0f, dist - posZ));

    return diffuse * falloff;
}

struct StudioLighting::Impl {
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
        if (!m_cachedFrame) {
            return false;
        }

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

StudioLighting::StudioLighting()
    : m_impl(std::make_unique<Impl>()) {
}

StudioLighting::~StudioLighting() = default;

StudioLighting::StudioLighting(StudioLighting&&) noexcept = default;
StudioLighting& StudioLighting::operator=(StudioLighting&&) noexcept = default;

AVFrame* StudioLighting::ProcessFrame(
    const AVFrame* inputFrame,
    const AlphaMask* mask,
    const NormalField* normals,
    const DepthField* depth,
    const LightingConfig& config,
    LightingOutcome& outcome
) {
    if (!inputFrame || !inputFrame->data[0]) {
        outcome = LightingOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->width <= 0 || inputFrame->height <= 0) {
        outcome = LightingOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        outcome = LightingOutcome::UnsupportedFormat;
        return nullptr;
    }

    const int width = inputFrame->width;
    const int height = inputFrame->height;
    const int srcLinesize = inputFrame->linesize[0];

    if (srcLinesize < width * 3) {
        outcome = LightingOutcome::InvalidInput;
        return nullptr;
    }

    // Allocate / reuse writable destination frame
    if (!m_impl->EnsureWritableBuffer(width, height, AV_PIX_FMT_BGR24)) {
        outcome = LightingOutcome::OutOfMemory;
        return nullptr;
    }

    AVFrame* outputFrame = m_impl->m_cachedFrame;
    av_frame_copy_props(outputFrame, inputFrame);
    outputFrame->width = width;
    outputFrame->height = height;
    outputFrame->format = AV_PIX_FMT_BGR24;

    const int dstLinesize = outputFrame->linesize[0];

    // Helper: Bit-exact copy of input to output
    auto copyBitExact = [&]() {
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
            uint8_t* dstRow = outputFrame->data[0] + y * dstLinesize;
            std::memcpy(dstRow, srcRow, width * 3);
        }
    };

    // 1. Handle Explicit Bypass
    if (config.bypass) {
        copyBitExact();
        outcome = LightingOutcome::Bypass;
        return outputFrame;
    }

    // 2. Validate Mask Descriptors if provided
    const bool hasMask = (mask != nullptr && mask->data != nullptr);
    if (hasMask) {
        if (mask->width != width || mask->height != height || mask->strideFloats < width) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
    }

    // 3. Validate Normal Field Descriptors if provided
    const bool hasNormals = (normals != nullptr && normals->data != nullptr);
    if (hasNormals) {
        if (normals->width != width || normals->height != height || normals->strideFloats < width * 3) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
    }

    // 3b. Validate Depth Field Descriptors if provided
    const bool hasDepth = (depth != nullptr && depth->data != nullptr);
    if (hasDepth) {
        if (depth->width != width || depth->height != height || depth->strideFloats < width) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
    }

    // 4. Handle Subject-Only Mode without Mask: passthrough, never tint background
    if (config.subjectOnlyMode && (!mask || !mask->data)) {
        copyBitExact();
        outcome = LightingOutcome::NotReady;
        return outputFrame;
    }

    // 5. Handle Normal-Guided Mode without Normals: passthrough, never invent normals
    if (config.lightingMode == LightingMode::NormalGuided && !hasNormals) {
        copyBitExact();
        outcome = LightingOutcome::NotReady;
        return outputFrame;
    }

    if(config.shadowConfig.enabled && !hasDepth) {
        copyBitExact(); outcome=LightingOutcome::NotReady; return outputFrame;
    }
    // Prepare clamped parameters
    const float globalStrength = std::clamp(config.globalStrength, 0.0f, 2.0f);
    const float ambient = std::clamp(config.ambientIntensity, 0.0f, 2.0f);
    const float fill = std::clamp(config.fillIntensity, 0.0f, 2.0f);
    const float baseFactor = ambient + fill;

    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    // Prepare Light 1 parameters
    const bool l1_enabled = config.light1.enabled && (config.light1.intensity > 0.0f);
    const float l1_posX = std::clamp(config.light1.posX, -1.0f, 1.0f);
    const float l1_posY = std::clamp(config.light1.posY, -1.0f, 1.0f);
    const float l1_posZ = std::max(0.1f, config.light1.posZ);
    const float l1_intensity = std::clamp(config.light1.intensity, 0.0f, 2.0f) * globalStrength;
    float l1_rawR, l1_rawG, l1_rawB;
    config.light1.GetEffectiveLinearColor(l1_rawR, l1_rawG, l1_rawB);
    const float l1_colR = std::clamp(l1_rawR, 0.0f, 1.0f);
    const float l1_colG = std::clamp(l1_rawG, 0.0f, 1.0f);
    const float l1_colB = std::clamp(l1_rawB, 0.0f, 1.0f);

    // Prepare Light 2 parameters
    const bool l2_enabled = config.light2.enabled && (config.light2.intensity > 0.0f);
    const float l2_posX = std::clamp(config.light2.posX, -1.0f, 1.0f);
    const float l2_posY = std::clamp(config.light2.posY, -1.0f, 1.0f);
    const float l2_posZ = std::max(0.1f, config.light2.posZ);
    const float l2_intensity = std::clamp(config.light2.intensity, 0.0f, 2.0f) * globalStrength;
    float l2_rawR, l2_rawG, l2_rawB;
    config.light2.GetEffectiveLinearColor(l2_rawR, l2_rawG, l2_rawB);
    const float l2_colR = std::clamp(l2_rawR, 0.0f, 1.0f);
    const float l2_colG = std::clamp(l2_rawG, 0.0f, 1.0f);
    const float l2_colB = std::clamp(l2_rawB, 0.0f, 1.0f);

    // If both lights are disabled and baseFactor is roughly 1.0 (neutral without bypass),
    // and no mask is present, input equals output.
    if (!l1_enabled && !l2_enabled && std::abs(baseFactor - 1.0f) < 1e-4f) {
        copyBitExact();
        outcome = LightingOutcome::Success;
        return outputFrame;
    }

    // Process rows
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
        uint8_t* dstRow = outputFrame->data[0] + y * dstLinesize;

        const float* maskRow = hasMask ? (mask->data + y * mask->strideFloats) : nullptr;
        const float* normalsRow = hasNormals ? (normals->data + y * normals->strideFloats) : nullptr;
        const float* depthRow = hasDepth ? (depth->data + y * depth->strideFloats) : nullptr;

        const float v = 2.0f * ((y + 0.5f) / static_cast<float>(height)) - 1.0f;

        for (int x = 0; x < width; ++x) {
            // Check mask value
            float alpha = 1.0f;
            if (maskRow) {
                float mVal = maskRow[x];
                if (!IsValidFloat(mVal)) {
                    mVal = 0.0f;
                }
                alpha = std::clamp(mVal, 0.0f, 1.0f);
            }

            // Bit-exact preservation of background pixels
            if (alpha <= 0.0f) {
                dstRow[3 * x + 0] = srcRow[3 * x + 0];
                dstRow[3 * x + 1] = srcRow[3 * x + 1];
                dstRow[3 * x + 2] = srcRow[3 * x + 2];
                continue;
            }

            const float u = 2.0f * ((x + 0.5f) / static_cast<float>(width)) - 1.0f;

            // Surface normal determination
            const bool useNormals = (config.lightingMode == LightingMode::NormalGuided);
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            bool validNormal = false;
            if (useNormals && normalsRow) {
                float rawNx = normalsRow[3 * x + 0];
                float rawNy = normalsRow[3 * x + 1];
                float rawNz = normalsRow[3 * x + 2];
                if (IsValidFloat(rawNx) && IsValidFloat(rawNy) && IsValidFloat(rawNz)) {
                    float nLenSq = rawNx * rawNx + rawNy * rawNy + rawNz * rawNz;
                    if (nLenSq > 1e-6f) {
                        float invLen = 1.0f / std::sqrt(nLenSq);
                        nx = rawNx * invLen;
                        ny = rawNy * invLen;
                        nz = rawNz * invLen;
                        validNormal = true;
                    }
                }
                if (!validNormal) {
                    nx = 0.0f; ny = 0.0f; nz = 1.0f;
                    validNormal = true;
                }
            }

            // Accumulate Light 1
            float l1_factor = 0.0f;
            if (l1_enabled) {
                if (validNormal) {
                    const float deltaX = (l1_posX - u) * aspect;
                    const float deltaY = l1_posY - v;
                    const float deltaZ = l1_posZ;
                    const float dist = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                    if (dist > 1e-5f) {
                        const float lx = deltaX / dist;
                        const float ly = deltaY / dist;
                        const float lz = deltaZ / dist;
                        const float dotNL = std::max(0.0f, nx * lx + ny * ly + nz * lz);
                        const float falloff = 1.0f / (1.0f + 0.4f * std::max(0.0f, dist - l1_posZ));
                        l1_factor = dotNL * falloff * l1_intensity;
                    }
                } else {
                    const float approx = CalculateSpatialLightApproximation(u, v, aspect, config.light1);
                    l1_factor = approx * l1_intensity;
                }
            }

            // Accumulate Light 2
            float l2_factor = 0.0f;
            if (l2_enabled) {
                if (validNormal) {
                    const float deltaX = (l2_posX - u) * aspect;
                    const float deltaY = l2_posY - v;
                    const float deltaZ = l2_posZ;
                    const float dist = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                    if (dist > 1e-5f) {
                        const float lx = deltaX / dist;
                        const float ly = deltaY / dist;
                        const float lz = deltaZ / dist;
                        const float dotNL = std::max(0.0f, nx * lx + ny * ly + nz * lz);
                        const float falloff = 1.0f / (1.0f + 0.4f * std::max(0.0f, dist - l2_posZ));
                        l2_factor = dotNL * falloff * l2_intensity;
                    }
                } else {
                    const float approx = CalculateSpatialLightApproximation(u, v, aspect, config.light2);
                    l2_factor = approx * l2_intensity;
                }
            }

            // Approximate Geometry Shadows modulation
            if (config.shadowConfig.enabled && hasDepth && depthRow) {
                const float zSurface = depthRow[x];
                if (zSurface >= 0.0f && zSurface <= 1.0f && IsValidFloat(zSurface)) {
                    if (l1_enabled && l1_factor > 0.0f) {
                        float v1 = PrismForge::Effects::Relighting::ApproximateShadows::ComputeLightVisibilityRaw(
                            u, v, zSurface, nx, ny, nz,
                            depth->data, width, height, depth->strideFloats,
                            config.light1, config.shadowConfig, aspect
                        );
                        l1_factor *= v1;
                    }
                    if (l2_enabled && l2_factor > 0.0f) {
                        float v2 = PrismForge::Effects::Relighting::ApproximateShadows::ComputeLightVisibilityRaw(
                            u, v, zSurface, nx, ny, nz,
                            depth->data, width, height, depth->strideFloats,
                            config.light2, config.shadowConfig, aspect
                        );
                        l2_factor *= v2;
                    }
                }
            }

            // Total per-channel multipliers in linear RGB
            const float multR = baseFactor + (l1_colR * l1_factor) + (l2_colR * l2_factor);
            const float multG = baseFactor + (l1_colG * l1_factor) + (l2_colG * l2_factor);
            const float multB = baseFactor + (l1_colB * l1_factor) + (l2_colB * l2_factor);

            // Read original sRGB BGR bytes and convert to linear float
            const uint8_t srcB = srcRow[3 * x + 0];
            const uint8_t srcG = srcRow[3 * x + 1];
            const uint8_t srcR = srcRow[3 * x + 2];

            const float linB = g_srgbLinearLut.table[srcB];
            const float linG = g_srgbLinearLut.table[srcG];
            const float linR = g_srgbLinearLut.table[srcR];

            // Multiply reflectance in linear space (preserves high-frequency facial textures & detail)
            const float litLinB = linB * multB;
            const float litLinG = linG * multG;
            const float litLinR = linR * multR;

            // Convert back to sRGB bytes
            const uint8_t litB = LinearToSrgbByte(litLinB);
            const uint8_t litG = LinearToSrgbByte(litLinG);
            const uint8_t litR = LinearToSrgbByte(litLinR);

            // Blend with original input using alpha mask
            if (alpha >= 1.0f) {
                dstRow[3 * x + 0] = litB;
                dstRow[3 * x + 1] = litG;
                dstRow[3 * x + 2] = litR;
            } else {
                const float invA = 1.0f - alpha;
                dstRow[3 * x + 0] = static_cast<uint8_t>(std::clamp(static_cast<int>(invA * srcB + alpha * litB + 0.5f), 0, 255));
                dstRow[3 * x + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(invA * srcG + alpha * litG + 0.5f), 0, 255));
                dstRow[3 * x + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(invA * srcR + alpha * litR + 0.5f), 0, 255));
            }
        }
    }

    outcome = LightingOutcome::Success;
    return outputFrame;
}

void StudioLight::KelvinToLinearRgb(float kelvin, float tint, float& outR, float& outG, float& outB) {
    float k = std::clamp(kelvin, 1500.0f, 15000.0f);
    float t = k / 100.0f;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // Red
    if (t <= 66.0f) {
        r = 255.0f;
    } else {
        r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
    }

    // Green
    if (t <= 66.0f) {
        g = 99.4708025861f * std::log(std::max(1.0f, t)) - 161.1195681661f;
    } else {
        g = 288.1221695283f * std::pow(std::max(1.0f, t - 60.0f), -0.0755148492f);
    }

    // Blue
    if (t >= 66.0f) {
        b = 255.0f;
    } else if (t <= 19.0f) {
        b = 0.0f;
    } else {
        b = 138.5177312231f * std::log(std::max(1.0f, t - 10.0f)) - 305.0447927307f;
    }

    r = std::clamp(r, 0.0f, 255.0f) / 255.0f;
    g = std::clamp(g, 0.0f, 255.0f) / 255.0f;
    b = std::clamp(b, 0.0f, 255.0f) / 255.0f;

    // Convert sRGB to linear light
    auto toLinear = [](float c) -> float {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };

    float linR = toLinear(r);
    float linG = toLinear(g);
    float linB = toLinear(b);

    // Apply tint (green-magenta axis [-1.0, 1.0])
    float clampedTint = std::clamp(tint, -1.0f, 1.0f);
    linG *= (1.0f + clampedTint * 0.25f);

    float maxChan = std::max({ linR, linG, linB });
    if (maxChan > 1e-5f) {
        outR = std::clamp(linR / maxChan, 0.0f, 1.0f);
        outG = std::clamp(linG / maxChan, 0.0f, 1.0f);
        outB = std::clamp(linB / maxChan, 0.0f, 1.0f);
    } else {
        outR = 1.0f;
        outG = 1.0f;
        outB = 1.0f;
    }
}

void StudioLight::GetEffectiveLinearColor(float& outR, float& outG, float& outB) const {
    if (colorTemperature > 0.0f) {
        KelvinToLinearRgb(colorTemperature, tint, outR, outG, outB);
    } else {
        outR = std::clamp(colorR, 0.0f, 1.0f);
        outG = std::clamp(colorG, 0.0f, 1.0f);
        outB = std::clamp(colorB, 0.0f, 1.0f);
    }
}

StudioLight LightCoordinateHelper::SanitizeLight(const StudioLight& inLight) {
    StudioLight out = inLight;
    out.posX = std::clamp(inLight.posX, -1.0f, 1.0f);
    out.posY = std::clamp(inLight.posY, -1.0f, 1.0f);
    out.posZ = std::clamp(inLight.posZ, 0.1f, 5.0f);
    out.intensity = std::clamp(inLight.intensity, 0.0f, 2.0f);
    out.colorR = std::clamp(inLight.colorR, 0.0f, 1.0f);
    out.colorG = std::clamp(inLight.colorG, 0.0f, 1.0f);
    out.colorB = std::clamp(inLight.colorB, 0.0f, 1.0f);
    out.colorTemperature = (inLight.colorTemperature > 0.0f) ? std::clamp(inLight.colorTemperature, 1500.0f, 15000.0f) : 0.0f;
    out.tint = std::clamp(inLight.tint, -1.0f, 1.0f);
    return out;
}

void LightCoordinateHelper::LightToScreenCoords(
    float lightX,
    float lightY,
    int sourceWidth,
    int sourceHeight,
    int viewportX,
    int viewportY,
    int viewportWidth,
    int viewportHeight,
    bool mirrored,
    int rotationDegrees,
    float& outScreenX,
    float& outScreenY
) {
    if (sourceWidth <= 0 || sourceHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0) {
        outScreenX = static_cast<float>(viewportX);
        outScreenY = static_cast<float>(viewportY);
        return;
    }

    float lx = std::clamp(lightX, -1.0f, 1.0f);
    float ly = std::clamp(lightY, -1.0f, 1.0f);

    if (mirrored) {
        lx = -lx;
    }

    // Map [-1, 1] to normalized [0, 1]
    float u = (lx + 1.0f) * 0.5f;
    float v = (ly + 1.0f) * 0.5f;

    // Apply rotation
    int normRot = ((rotationDegrees % 360) + 360) % 360;
    if (normRot == 90) {
        float oldU = u;
        u = 1.0f - v;
        v = oldU;
    } else if (normRot == 180) {
        u = 1.0f - u;
        v = 1.0f - v;
    } else if (normRot == 270) {
        float oldU = u;
        u = v;
        v = 1.0f - oldU;
    }

    int effSrcW = (normRot == 90 || normRot == 270) ? sourceHeight : sourceWidth;
    int effSrcH = (normRot == 90 || normRot == 270) ? sourceWidth : sourceHeight;

    float srcAspect = static_cast<float>(effSrcW) / static_cast<float>(effSrcH);
    float viewAspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

    float fitW, fitH, fitX, fitY;
    if (srcAspect > viewAspect) {
        fitW = static_cast<float>(viewportWidth);
        fitH = fitW / srcAspect;
        fitX = static_cast<float>(viewportX);
        fitY = static_cast<float>(viewportY) + (static_cast<float>(viewportHeight) - fitH) * 0.5f;
    } else {
        fitH = static_cast<float>(viewportHeight);
        fitW = fitH * srcAspect;
        fitY = static_cast<float>(viewportY);
        fitX = static_cast<float>(viewportX) + (static_cast<float>(viewportWidth) - fitW) * 0.5f;
    }

    outScreenX = fitX + u * fitW;
    outScreenY = fitY + v * fitH;
}

void LightCoordinateHelper::ScreenToLightCoords(
    float screenX,
    float screenY,
    int sourceWidth,
    int sourceHeight,
    int viewportX,
    int viewportY,
    int viewportWidth,
    int viewportHeight,
    bool mirrored,
    int rotationDegrees,
    float& outLightX,
    float& outLightY
) {
    if (sourceWidth <= 0 || sourceHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0) {
        outLightX = 0.0f;
        outLightY = 0.0f;
        return;
    }

    int normRot = ((rotationDegrees % 360) + 360) % 360;
    int effSrcW = (normRot == 90 || normRot == 270) ? sourceHeight : sourceWidth;
    int effSrcH = (normRot == 90 || normRot == 270) ? sourceWidth : sourceHeight;

    float srcAspect = static_cast<float>(effSrcW) / static_cast<float>(effSrcH);
    float viewAspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

    float fitW, fitH, fitX, fitY;
    if (srcAspect > viewAspect) {
        fitW = static_cast<float>(viewportWidth);
        fitH = fitW / srcAspect;
        fitX = static_cast<float>(viewportX);
        fitY = static_cast<float>(viewportY) + (static_cast<float>(viewportHeight) - fitH) * 0.5f;
    } else {
        fitH = static_cast<float>(viewportHeight);
        fitW = fitH * srcAspect;
        fitY = static_cast<float>(viewportY);
        fitX = static_cast<float>(viewportX) + (static_cast<float>(viewportWidth) - fitW) * 0.5f;
    }

    float u = std::clamp((screenX - fitX) / std::max(1.0f, fitW), 0.0f, 1.0f);
    float v = std::clamp((screenY - fitY) / std::max(1.0f, fitH), 0.0f, 1.0f);

    if (normRot == 90) {
        float oldU = u;
        u = v;
        v = 1.0f - oldU;
    } else if (normRot == 180) {
        u = 1.0f - u;
        v = 1.0f - v;
    } else if (normRot == 270) {
        float oldU = u;
        u = 1.0f - v;
        v = oldU;
    }

    float lx = u * 2.0f - 1.0f;
    float ly = v * 2.0f - 1.0f;

    if (mirrored) {
        lx = -lx;
    }

    outLightX = std::clamp(lx, -1.0f, 1.0f);
    outLightY = std::clamp(ly, -1.0f, 1.0f);
}

LightJitterSmoother::LightJitterSmoother(float smoothingFactor, float snapThreshold)
    : m_alpha(std::clamp(smoothingFactor, 0.05f, 0.95f)),
      m_snapThreshold(snapThreshold) {
}

void LightJitterSmoother::Reset() {
    m_hasHistory1 = false;
    m_hasHistory2 = false;
}

StudioLight LightJitterSmoother::SmoothLight1(const StudioLight& target) {
    return SmoothInternal(target, m_smoothed1, m_hasHistory1);
}

StudioLight LightJitterSmoother::SmoothLight2(const StudioLight& target) {
    return SmoothInternal(target, m_smoothed2, m_hasHistory2);
}

LightingConfig LightJitterSmoother::SmoothConfig(const LightingConfig& inConfig) {
    LightingConfig out = inConfig;
    out.light1 = SmoothLight1(inConfig.light1);
    out.light2 = SmoothLight2(inConfig.light2);
    return out;
}

StudioLight LightJitterSmoother::SmoothInternal(const StudioLight& target, StudioLight& history, bool& hasHistory) {
    if (!target.enabled || !hasHistory) {
        history = target;
        hasHistory = true;
        return target;
    }

    float dx = target.posX - history.posX;
    float dy = target.posY - history.posY;
    float dz = target.posZ - history.posZ;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist > m_snapThreshold) {
        history = target;
        return target;
    }

    history.enabled = target.enabled;
    history.posX += m_alpha * dx;
    history.posY += m_alpha * dy;
    history.posZ += m_alpha * dz;
    history.intensity += m_alpha * (target.intensity - history.intensity);
    history.colorTemperature += m_alpha * (target.colorTemperature - history.colorTemperature);
    history.tint += m_alpha * (target.tint - history.tint);
    history.colorR += m_alpha * (target.colorR - history.colorR);
    history.colorG += m_alpha * (target.colorG - history.colorG);
    history.colorB += m_alpha * (target.colorB - history.colorB);

    return history;
}

} // namespace PrismForge::Effects::Lighting
