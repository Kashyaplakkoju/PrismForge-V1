#include "approximate_shadows.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace PrismForge::Effects::Relighting {

namespace {

inline bool IsFinite(float v) {
    return std::isfinite(v);
}

inline float Clamp(float val, float low, float high) {
    return std::clamp(val, low, high);
}

} // anonymous namespace

ApproximateShadows::ApproximateShadows() = default;
ApproximateShadows::~ApproximateShadows() = default;

float ApproximateShadows::ComputeLightVisibilityRaw(
    float u, float v,
    float zSurface,
    float nx, float ny, float nz,
    const float* depthData,
    int depthW,
    int depthH,
    int depthStride,
    const PrismForge::Effects::Lighting::StudioLight& light,
    const PrismForge::Effects::Lighting::ApproximateShadowConfig& shadowConfig,
    float aspect,
    const float* confidenceData,
    float minConfidence
) {
    if (!depthData || depthW<=0 || depthH<=0 || depthW>8192 || depthH>8192 || depthStride<depthW ||
        !IsFinite(aspect) || aspect<=0.0f || !IsFinite(u) || !IsFinite(v) ||
        !IsFinite(nx) || !IsFinite(ny) || !IsFinite(nz) ||
        !IsFinite(light.posX) || !IsFinite(light.posY) || !IsFinite(light.posZ) || !IsFinite(light.intensity) ||
        !IsFinite(shadowConfig.maxDistance) || !IsFinite(shadowConfig.rayElevation) ||
        !IsFinite(shadowConfig.shadowIntensity) || !IsFinite(shadowConfig.shadowSoftness)) return 1.0f;
    if (!light.enabled || light.intensity <= 0.0f || shadowConfig.shadowIntensity <= 0.0f) {
        return 1.0f;
    }

    if (zSurface < 0.0f || zSurface > 1.0f || !IsFinite(zSurface)) {
        return 1.0f; // Invalid depth fades predictably to neutral 1.0f, never black blocks!
    }

    const float lx = Clamp(light.posX, -1.0f, 1.0f);
    const float ly = Clamp(light.posY, -1.0f, 1.0f);
    const float lz = std::max(0.1f, light.posZ);

    const float deltaX = (lx - u) * aspect;
    const float deltaY = ly - v;
    const float dist2D = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if (dist2D < 1e-4f) {
        return 1.0f; // Directly overhead in 2D
    }

    const float dist3D = std::sqrt(dist2D * dist2D + lz * lz);
    const float L3x = deltaX / dist3D;
    const float L3y = deltaY / dist3D;
    const float L3z = lz / dist3D;

    const float dotNL = nx * L3x + ny * L3y + nz * L3z;
    if (dotNL <= 0.0f) {
        // Backfacing surface
        return std::max(0.0f, 1.0f - shadowConfig.shadowIntensity);
    }

    const float maxDist = std::min(dist2D * 0.5f, Clamp(shadowConfig.maxDistance, 0.05f, 0.50f));
    const int steps = Clamp(shadowConfig.maxSteps, 4, 32);
    const float stepDist = maxDist / static_cast<float>(steps);

    const float raySlope = (lz / (dist3D + 0.1f)) + shadowConfig.rayElevation;
    const float tStart = stepDist * 0.5f;
    const float bias = 0.003f + 0.005f * (1.0f - std::max(0.0f, nz));

    const float dirX = deltaX / dist2D;
    const float dirY = deltaY / dist2D;

    float minVis = 1.0f;

    for (int i = 0; i < steps; ++i) {
        const float t = tStart + static_cast<float>(i) * stepDist;
        const float su = u + (dirX / aspect) * t;
        const float sv = v + dirY * t;

        if (su < -1.0f || su > 1.0f || sv < -1.0f || sv > 1.0f) {
            continue; // Ray has marched off screen, no occluder
        }

        int px = static_cast<int>((su + 1.0f) * 0.5f * static_cast<float>(depthW - 1) + 0.5f);
        int py = static_cast<int>((sv + 1.0f) * 0.5f * static_cast<float>(depthH - 1) + 0.5f);
        px = Clamp(px, 0, depthW - 1);
        py = Clamp(py, 0, depthH - 1);

        const size_t at=static_cast<size_t>(py)*depthStride+px;
        if(confidenceData && (!IsFinite(confidenceData[at]) || confidenceData[at]<minConfidence || confidenceData[at]>1.0f)) continue;
        const float zSample = depthData[at];
        if (zSample < 0.0f || zSample > 1.0f || !IsFinite(zSample)) {
            continue; // Invalid depth sample ignored
        }

        const float zRay = zSurface - t * raySlope;

        if (zSample < (zRay - bias)) {
            // Occlusion: sample is closer to camera/light than the ray
            const float deltaZ = (zRay - bias) - zSample;
            if (deltaZ < 0.35f) {
                // Soft penumbra transition
                const float penumbra = shadowConfig.shadowSoftness * t + 0.02f;
                const float stepVis = Clamp(1.0f - (deltaZ / penumbra), 0.0f, 1.0f);
                minVis = std::min(minVis, stepVis);
                if (minVis <= 0.0f) {
                    break;
                }
            }
        }
    }

    const float effVis = 1.0f - shadowConfig.shadowIntensity * (1.0f - minVis);
    return Clamp(effVis, 0.0f, 1.0f);
}

float ApproximateShadows::ComputeLightVisibility(
    float u, float v,
    float zSurface,
    float nx, float ny, float nz,
    const DepthBuffer& depth,
    const PrismForge::Effects::Lighting::StudioLight& light,
    const PrismForge::Effects::Lighting::ApproximateShadowConfig& shadowConfig,
    float aspect
) {
    if (!depth.IsValid() || depth.format!=0 || depth.data.empty()) {
        return 1.0f;
    }
    return ComputeLightVisibilityRaw(
        u, v, zSurface, nx, ny, nz,
        depth.data.data(), depth.width, depth.height, depth.strideFloats,
        light, shadowConfig, aspect, depth.confidence.empty()?nullptr:depth.confidence.data()
    );
}

bool ApproximateShadows::ComputeShadows(
    const DepthBuffer& depth,
    const NormalBuffer* normals,
    const PrismForge::Effects::Lighting::AlphaMask* mask,
    const PrismForge::Effects::Lighting::LightingConfig& lightingConfig,
    ShadowMapBuffer& outShadows
) {
    outShadows=ShadowMapBuffer{};
    if (!depth.IsValid() || depth.format!=0 || depth.data.empty() ||
        (normals && (!normals->IsValid() || normals->width!=depth.width || normals->height!=depth.height)) ||
        (mask && (!mask->data || mask->width!=depth.width || mask->height!=depth.height || mask->strideFloats<depth.width))) {
        return false;
    }

    const int width = depth.width;
    const int height = depth.height;
    const int stride = width * 2;

    uint64_t totalFloats = static_cast<uint64_t>(height) * static_cast<uint64_t>(stride);
    if (totalFloats > (SIZE_MAX / sizeof(float))) {
        return false;
    }

    outShadows.width = width;
    outShadows.height = height;
    outShadows.strideFloats = stride;
    try { outShadows.data.assign(static_cast<size_t>(totalFloats),1.0f); } catch (...) { outShadows=ShadowMapBuffer{}; return false; }

    const auto& shadowCfg = lightingConfig.shadowConfig;
    if (!shadowCfg.enabled) {
        // Disabled: leave all visibilities at 1.0f (neutral unshadowed)
        return true;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const bool hasMask = mask!=nullptr;
    const bool hasNormals = (normals != nullptr && normals->IsValid() && normals->width == width && normals->height == height);

    for (int y = 0; y < height; ++y) {
        const float v = 2.0f * ((y + 0.5f) / static_cast<float>(height)) - 1.0f;
        const float* maskRow = hasMask ? (mask->data + y * mask->strideFloats) : nullptr;
        const float* normalsRow = hasNormals ? (normals->data.data() + y * normals->strideFloats) : nullptr;
        const float* depthRow = depth.data.data() + y * depth.strideFloats;
        float* dstRow = outShadows.data.data() + y * stride;

        for (int x = 0; x < width; ++x) {
            // Mask check: preserve background pixels
            if (maskRow) {
                float mVal = maskRow[x];
                if (!IsFinite(mVal) || mVal <= 0.0f) {
                    dstRow[2 * x + 0] = 1.0f;
                    dstRow[2 * x + 1] = 1.0f;
                    continue;
                }
            }

            if(!depth.confidence.empty()) {
                const float confidence=depth.confidence[static_cast<size_t>(y)*depth.strideFloats+x];
                if(!IsFinite(confidence) || confidence<0.1f || confidence>1.0f) continue;
            }
            const float zSurface = depthRow[x];
            if (zSurface < 0.0f || zSurface > 1.0f || !IsFinite(zSurface)) {
                dstRow[2 * x + 0] = 1.0f;
                dstRow[2 * x + 1] = 1.0f;
                continue;
            }

            const float u = 2.0f * ((x + 0.5f) / static_cast<float>(width)) - 1.0f;

            // Surface normal
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            if (normalsRow) {
                float rnx = normalsRow[3 * x + 0];
                float rny = normalsRow[3 * x + 1];
                float rnz = normalsRow[3 * x + 2];
                if (IsFinite(rnx) && IsFinite(rny) && IsFinite(rnz)) {
                    float lenSq = rnx * rnx + rny * rny + rnz * rnz;
                    if (lenSq > 1e-6f && IsFinite(lenSq)) {
                        float invLen = 1.0f / std::sqrt(lenSq);
                        nx = rnx * invLen;
                        ny = rny * invLen;
                        nz = rnz * invLen;
                    }
                }
            }

            const float v1 = ComputeLightVisibilityRaw(
                u, v, zSurface, nx, ny, nz,
                depth.data.data(), width, height, depth.strideFloats,
                lightingConfig.light1, shadowCfg, aspect, depth.confidence.empty()?nullptr:depth.confidence.data()
            );

            const float v2 = ComputeLightVisibilityRaw(
                u, v, zSurface, nx, ny, nz,
                depth.data.data(), width, height, depth.strideFloats,
                lightingConfig.light2, shadowCfg, aspect, depth.confidence.empty()?nullptr:depth.confidence.data()
            );

            dstRow[2 * x + 0] = v1;
            dstRow[2 * x + 1] = v2;
        }
    }

    return true;
}

} // namespace PrismForge::Effects::Relighting
