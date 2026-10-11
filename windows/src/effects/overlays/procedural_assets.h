#pragma once

#include "overlay_types.h"
#include <mutex>
#include <unordered_map>

namespace PrismForge::Effects::Overlays {

enum class ProceduralAssetId : int {
    StudioAviatorGlasses = 0,
    StudioCap = 1,
    StudioDecorativeFrame = 2
};

struct AssetAnchorMetrics {
    float anchorX = 0.0f;           // Anchor point X in asset pixel coordinates
    float anchorY = 0.0f;           // Anchor point Y in asset pixel coordinates
    float referenceBaseline = 1.0f; // Nominal baseline distance (e.g. eye distance or head width in asset pixels)
};

class ProceduralAssetGenerator {
public:
    /// <summary>
    /// Returns a shared, lazily generated procedural asset bitmap.
    /// Thread-safe and reused across frames.
    /// </summary>
    static RgbaImageBuffer GetAsset(ProceduralAssetId assetId);

    /// <summary>
    /// Returns the calibration anchor metrics for the specified procedural asset.
    /// </summary>
    static AssetAnchorMetrics GetAnchorMetrics(ProceduralAssetId assetId);

    /// <summary>
    /// Generates an original Studio Aviator Glasses asset.
    /// Vector rasterized: dual teardrop lenses, brow bar, nose bridge, semi-transparent tint.
    /// </summary>
    static RgbaImageBuffer GenerateGlasses(int width = 640, int height = 320);

    /// <summary>
    /// Generates an original Studio Cap / Hat asset.
    /// Vector rasterized: curved brim, multi-panel crown, studio insignia, highlight trim.
    /// </summary>
    static RgbaImageBuffer GenerateCap(int width = 640, int height = 480);

    /// <summary>
    /// Generates an original Studio Decorative Frame asset.
    /// Vector rasterized: precision corner brackets, broadcast border, peripheral vignette.
    /// </summary>
    static RgbaImageBuffer GenerateFrame(int width = 1280, int height = 720);
};

} // namespace PrismForge::Effects::Overlays
