#include "procedural_assets.h"
#include <cmath>
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace PrismForge::Effects::Overlays {

namespace {

inline float ClampF(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

inline uint8_t ClampU8(int v) {
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
}

// Blend a single RGBA pixel over an existing RGBA pixel in straight alpha
void PlotPixelStraight(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    float sa = a / 255.0f;
    float da = dst[3] / 255.0f;
    float outA = sa + da * (1.0f - sa);
    if (outA <= 0.001f) return;

    float sr = r / 255.0f;
    float sg = g / 255.0f;
    float sb = b / 255.0f;
    float dr = dst[0] / 255.0f;
    float dg = dst[1] / 255.0f;
    float db = dst[2] / 255.0f;

    float outR = (sr * sa + dr * da * (1.0f - sa)) / outA;
    float outG = (sg * sa + dg * da * (1.0f - sa)) / outA;
    float outB = (sb * sa + db * da * (1.0f - sa)) / outA;

    dst[0] = ClampU8(static_cast<int>(outR * 255.0f + 0.5f));
    dst[1] = ClampU8(static_cast<int>(outG * 255.0f + 0.5f));
    dst[2] = ClampU8(static_cast<int>(outB * 255.0f + 0.5f));
    dst[3] = ClampU8(static_cast<int>(outA * 255.0f + 0.5f));
}

} // namespace

RgbaImageBuffer ProceduralAssetGenerator::GetAsset(ProceduralAssetId assetId) {
    static std::mutex s_mutex;
    static std::unordered_map<int, RgbaImageBuffer> s_cache;

    std::lock_guard<std::mutex> lock(s_mutex);
    int key = static_cast<int>(assetId);
    auto it = s_cache.find(key);
    if (it != s_cache.end()) {
        return it->second;
    }

    RgbaImageBuffer buf;
    switch (assetId) {
        case ProceduralAssetId::StudioAviatorGlasses:
            buf = GenerateGlasses(640, 320);
            break;
        case ProceduralAssetId::StudioCap:
            buf = GenerateCap(640, 480);
            break;
        case ProceduralAssetId::StudioDecorativeFrame:
            buf = GenerateFrame(1280, 720);
            break;
    }

    s_cache[key] = buf;
    return buf;
}

AssetAnchorMetrics ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId assetId) {
    AssetAnchorMetrics metrics;
    switch (assetId) {
        case ProceduralAssetId::StudioAviatorGlasses:
            metrics.anchorX = 320.0f;
            metrics.anchorY = 160.0f;
            metrics.referenceBaseline = 240.0f; // Inter-pupillary distance in asset pixels (440 - 200)
            break;
        case ProceduralAssetId::StudioCap:
            metrics.anchorX = 320.0f;
            metrics.anchorY = 410.0f;          // Bottom center of inner head band
            metrics.referenceBaseline = 280.0f; // Reference head width
            break;
        case ProceduralAssetId::StudioDecorativeFrame:
            metrics.anchorX = 640.0f;
            metrics.anchorY = 360.0f;
            metrics.referenceBaseline = 1280.0f; // Full viewport width
            break;
    }
    return metrics;
}

RgbaImageBuffer ProceduralAssetGenerator::GenerateGlasses(int width, int height) {
    if (width < 2 || height < 2 || width > 16384 || height > 16384 || int64_t(width)*height > 16LL*1024*1024) return {};
    RgbaImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.strideBytes = width * 4;
    buf.alphaContract = AlphaContract::Straight;
    buf.data.assign(buf.strideBytes * height, 0);

    const float leftCx = 200.0f;
    const float rightCx = 440.0f;
    const float cy = 160.0f;
    const float rx = 90.0f;
    const float ry = 72.0f;
    const float rimThickness = 7.0f;

    for (int y = 0; y < height; ++y) {
        uint8_t* row = buf.data.data() + y * buf.strideBytes;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;

            // Compute distance to left and right lens teardrop contours
            float dxL = (x - leftCx);
            float dyL = (y - cy);
            // Slight teardrop contour widening towards bottom
            float factorL = 1.0f + 0.12f * (dyL / ry);
            float distL = std::sqrt((dxL * dxL) / (rx * rx * factorL * factorL) + (dyL * dyL) / (ry * ry));

            float dxR = (x - rightCx);
            float dyR = (y - cy);
            float factorR = 1.0f + 0.12f * (dyR / ry);
            float distR = std::sqrt((dxR * dxR) / (rx * rx * factorR * factorR) + (dyR * dyR) / (ry * ry));

            float minDist = std::min(distL, distR);
            bool isLeft = (distL <= distR);
            float lensDx = isLeft ? dxL : dxR;
            float lensDy = isLeft ? dyL : dyR;

            // Metallic rim
            if (minDist <= 1.0f && minDist >= (1.0f - rimThickness / rx)) {
                float edgeT = (1.0f - minDist) / (rimThickness / rx);
                float rimAlpha = ClampF(edgeT * 3.0f, 0.0f, 1.0f);
                // Antialias outer edge
                if (minDist > 0.96f) {
                    rimAlpha *= (1.0f - minDist) / 0.04f;
                }
                uint8_t r = static_cast<uint8_t>(215 + 30 * std::sin(lensDx * 0.05f));
                uint8_t g = static_cast<uint8_t>(175 + 25 * std::sin(lensDx * 0.05f));
                uint8_t b = 85;
                PlotPixelStraight(px, r, g, b, static_cast<uint8_t>(rimAlpha * 255.0f));
            }
            // Lens interior tint
            else if (minDist < (1.0f - rimThickness / rx)) {
                // Vertical gradient tint
                float tY = (lensDy + ry) / (2.0f * ry);
                tY = ClampF(tY, 0.0f, 1.0f);

                uint8_t tintR = static_cast<uint8_t>(35 + 25 * (1.0f - tY));
                uint8_t tintG = static_cast<uint8_t>(28 + 20 * (1.0f - tY));
                uint8_t tintB = static_cast<uint8_t>(22 + 15 * (1.0f - tY));
                uint8_t tintA = static_cast<uint8_t>(165 - 35 * tY); // Transmittance ~0.65-0.75

                // Diagonal specular highlight streak across each lens
                float specCoord = lensDx + 1.2f * lensDy;
                if (std::abs(specCoord + 15.0f) < 14.0f) {
                    float specT = 1.0f - std::abs(specCoord + 15.0f) / 14.0f;
                    tintR = ClampU8(tintR + static_cast<int>(80 * specT));
                    tintG = ClampU8(tintG + static_cast<int>(80 * specT));
                    tintB = ClampU8(tintB + static_cast<int>(90 * specT));
                    tintA = ClampU8(tintA + static_cast<int>(50 * specT));
                }

                PlotPixelStraight(px, tintR, tintG, tintB, tintA);
            }

            // Top brow bar connecting both lenses
            if (y >= 96 && y <= 104 && x >= 150 && x <= 490) {
                float dY = std::abs((y - 100) + 0.015f * (x - 320) * (x - 320) / 40.0f);
                if (dY < 4.0f) {
                    float barAlpha = (4.0f - dY) / 4.0f;
                    PlotPixelStraight(px, 220, 180, 95, static_cast<uint8_t>(barAlpha * 255.0f));
                }
            }

            // Nose bridge
            if (y >= 148 && y <= 168 && x >= 270 && x <= 370) {
                float bridgeCurveY = 158.0f - 10.0f * (1.0f - ((x - 320.0f) / 50.0f) * ((x - 320.0f) / 50.0f));
                float dBridge = std::abs(y - bridgeCurveY);
                if (dBridge < 4.5f) {
                    float brAlpha = (4.5f - dBridge) / 4.5f;
                    PlotPixelStraight(px, 225, 185, 100, static_cast<uint8_t>(brAlpha * 255.0f));
                }
            }

            // Side hinges
            if ((x >= 95 && x <= 120 && std::abs(y - 150) < 5) ||
                (x >= 520 && x <= 545 && std::abs(y - 150) < 5)) {
                PlotPixelStraight(px, 210, 170, 90, 240);
            }
        }
    }

    return buf;
}

RgbaImageBuffer ProceduralAssetGenerator::GenerateCap(int width, int height) {
    if (width < 2 || height < 2 || width > 16384 || height > 16384 || int64_t(width)*height > 16LL*1024*1024) return {};
    RgbaImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.strideBytes = width * 4;
    buf.alphaContract = AlphaContract::Straight;
    buf.data.assign(buf.strideBytes * height, 0);

    const float cx = 320.0f;
    const float crownBaseY = 380.0f;
    const float crownRx = 160.0f;
    const float crownRy = 190.0f;

    for (int y = 0; y < height; ++y) {
        uint8_t* row = buf.data.data() + y * buf.strideBytes;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;

            float dx = (x - cx);
            float dyCrown = (y - crownBaseY);

            // 1. Cap Crown (dome above crownBaseY)
            if (y <= crownBaseY + 10 && y >= crownBaseY - crownRy) {
                float distCrown = std::sqrt((dx * dx) / (crownRx * crownRx) + (dyCrown * dyCrown) / (crownRy * crownRy));
                if (distCrown <= 1.0f) {
                    float alpha = 1.0f;
                    if (distCrown > 0.96f) {
                        alpha = (1.0f - distCrown) / 0.04f;
                    }

                    // Simulated directional studio lighting from upper-left
                    float lightDx = (x - (cx - 50.0f)) / crownRx;
                    float lightDy = (y - (crownBaseY - 110.0f)) / crownRy;
                    float lightDist = std::sqrt(lightDx * lightDx + lightDy * lightDy);
                    float lightFactor = ClampF(1.0f - lightDist * 0.65f, 0.2f, 1.0f);

                    uint8_t r = ClampU8(static_cast<int>(30 * lightFactor + 15));
                    uint8_t g = ClampU8(static_cast<int>(35 * lightFactor + 18));
                    uint8_t b = ClampU8(static_cast<int>(48 * lightFactor + 25));

                    // Subtle panel stitching lines (central seam and radial seams)
                    if (std::abs(dx) < 1.5f || std::abs(dx - 60.0f) < 1.2f || std::abs(dx + 60.0f) < 1.2f) {
                        r = ClampU8(r + 25);
                        g = ClampU8(g + 25);
                        b = ClampU8(b + 35);
                    }

                    PlotPixelStraight(px, r, g, b, static_cast<uint8_t>(alpha * 255.0f));
                }
            }

            // 2. Front Insignia / Studio Badge
            if (y >= 280 && y <= 330 && std::abs(dx) <= 30.0f) {
                float badgeY = (y - 305.0f) / 25.0f;
                float badgeX = dx / 30.0f;
                float badgeDist = std::abs(badgeX) + std::abs(badgeY); // Diamond
                if (badgeDist <= 0.9f) {
                    float bAlpha = 1.0f;
                    if (badgeDist > 0.8f) bAlpha = (0.9f - badgeDist) / 0.1f;
                    // Cyan/gold studio emblem
                    PlotPixelStraight(px, 70, 180, 220, static_cast<uint8_t>(bAlpha * 240.0f));
                } else if (badgeDist <= 1.0f) {
                    PlotPixelStraight(px, 220, 190, 100, 220); // Gold rim
                }
            }

            // 3. Cap Brim (curved visor extending forward and outward)
            float brimCurveY = 410.0f - 40.0f * (1.0f - (dx * dx) / (220.0f * 220.0f));
            if (std::abs(dx) <= 220.0f && y >= brimCurveY - 18.0f && y <= brimCurveY + 14.0f) {
                float brimDist = std::abs(y - brimCurveY);
                if (brimDist <= 15.0f) {
                    float alpha = 1.0f;
                    if (std::abs(dx) > 200.0f) {
                        alpha *= (220.0f - std::abs(dx)) / 20.0f;
                    }
                    if (brimDist > 12.0f) {
                        alpha *= (15.0f - brimDist) / 3.0f;
                    }

                    // Brim shading: top surface highlight, bottom edge shadow
                    float tBrim = (y - (brimCurveY - 18.0f)) / 32.0f;
                    uint8_t r = ClampU8(static_cast<int>(55 - 35 * tBrim));
                    uint8_t g = ClampU8(static_cast<int>(60 - 38 * tBrim));
                    uint8_t b = ClampU8(static_cast<int>(75 - 45 * tBrim));

                    // Brim edge trim stitching
                    if (std::abs(y - (brimCurveY + 10.0f)) < 2.0f) {
                        r = 170; g = 175; b = 190;
                    }

                    PlotPixelStraight(px, r, g, b, static_cast<uint8_t>(alpha * 255.0f));
                }
            }
        }
    }

    return buf;
}

RgbaImageBuffer ProceduralAssetGenerator::GenerateFrame(int width, int height) {
    if (width < 2 || height < 2 || width > 16384 || height > 16384 || int64_t(width)*height > 16LL*1024*1024) return {};
    RgbaImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.strideBytes = width * 4;
    buf.alphaContract = AlphaContract::Straight;
    buf.data.assign(buf.strideBytes * height, 0);

    const int cornerSize = 130;
    const int margin = 28;
    const int lineWidth = 3;

    for (int y = 0; y < height; ++y) {
        uint8_t* row = buf.data.data() + y * buf.strideBytes;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;

            // Distance to viewport edges
            int edgeX = std::min(x, width - 1 - x);
            int edgeY = std::min(y, height - 1 - y);

            // 1. Perimeter Vignette (soft falloff near edge, 100% transparent in center)
            int minEdge = std::min(edgeX, edgeY);
            if (minEdge < 48) {
                float vT = 1.0f - (minEdge / 48.0f);
                float vAlpha = vT * vT * 0.70f;
                PlotPixelStraight(px, 10, 14, 20, static_cast<uint8_t>(vAlpha * 255.0f));
            }

            // 2. Precision Corner Brackets
            bool isCornerX = (edgeX >= margin && edgeX <= margin + cornerSize);
            bool isCornerY = (edgeY >= margin && edgeY <= margin + cornerSize);

            if (isCornerX && isCornerY) {
                int localX = edgeX - margin;
                int localY = edgeY - margin;

                // Outer bracket lines
                bool onHorizLine = (localY < lineWidth);
                bool onVertLine = (localX < lineWidth);

                // Chamfer 45-degree notch on inner corner
                bool onChamfer = std::abs((localX + localY) - cornerSize) < lineWidth && (localX > 15 && localY > 15);

                if (onHorizLine || onVertLine || onChamfer) {
                    // Studio cyan/silver metallic accent
                    PlotPixelStraight(px, 120, 205, 245, 235);
                }

                // Inner accent line
                if (localX >= 10 && localY >= 10) {
                    bool onInnerH = (localY >= 10 && localY < 10 + 2 && localX <= 50);
                    bool onInnerV = (localX >= 10 && localX < 10 + 2 && localY <= 50);
                    if (onInnerH || onInnerV) {
                        PlotPixelStraight(px, 220, 185, 100, 200); // Gold accent
                    }
                }
            }

            // 3. Subtle Broadcast Aspect Tick Marks at Midpoints
            int midX = width / 2;
            int midY = height / 2;
            if (std::abs(x - midX) < 12 && edgeY >= margin && edgeY < margin + 6) {
                PlotPixelStraight(px, 140, 210, 245, 180);
            }
            if (std::abs(y - midY) < 12 && edgeX >= margin && edgeX < margin + 6) {
                PlotPixelStraight(px, 140, 210, 245, 180);
            }
        }
    }

    return buf;
}

} // namespace PrismForge::Effects::Overlays
