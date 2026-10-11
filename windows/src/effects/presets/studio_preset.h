#pragma once

#include "windows/src/effects/lighting/studiolighting.h"
#include "windows/src/effects/appearance/appearanceprocessor.h"
#include "windows/src/effects/overlays/overlay_types.h"
#include "windows/src/effects/overlays/face_filter_asset.h"
#include "windows/src/effects/backgroundcompositor.h"

#include <string>
#include <vector>
#include <memory>

namespace PrismForge::Effects::Presets {

/// <summary>
/// <summary>
/// Background composition modes supported by StudioStack (additive to legacy BackgroundMode).
/// </summary>
enum class StudioBackgroundMode : int {
    Original = 0,
    Blur = 1,
    Solid = 2,
    Image = 3
};

/// <summary>
/// Fit and scaling modes for background image replacement.
/// </summary>
enum class BackgroundFitMode : int {
    CropFill = 0,    // Center and crop to fill destination, preserving aspect ratio
    FitLetterbox = 1,// Letterbox with neutral bars to fit entirely inside destination
    Stretch = 2      // Stretch to fill destination
};

/// <summary>
/// Unified typed studio preset encapsulating lighting, appearance, overlays,
/// background compositing, and LUT configuration snapshots.
/// </summary>
struct StudioPreset {
    int schemaVersion = 1;
    std::string presetId;
    std::string displayName;
    std::string description;
    bool bypassAll = false;
    // Additive opt-in: exact native perspective meshes; legacy overlays remain compatible.
    bool realisticAccessories = false;
    // Additive: 0 retains legacy procedural cap, 1 white tennis visor, 2 Pokemon cap.
    int realisticCapStyle = 0;
    int realisticGlassesStyle = 0;
    float realisticCapScale = 1.f;
    float realisticCapVerticalOffset = 0.f;
    // Original native masks/helmets: 0 Off, 1 Aurora fox, 2 Nocturne owl, 3 Atlas explorer.
    int faceFilterStyle = 0; // 1 fox, 2 owl, 3 explorer, 4 local, 5 optional King's crown.
    uint32_t faceFilterRenderEdge=640; // Bounded raster quality; geometry keeps its authored proportions.
    float faceFilterScale = 1.f; // .75..1.5, preserves authored proportions.
    float faceFilterVerticalOffset = 0.f; // -3..3cm in head-local coordinates.

    std::shared_ptr<const Overlays::Realistic::NativeFaceFilter> faceFilterCustomModel; // Immutable, transient snapshot.
    std::string faceFilterCustomPath,faceFilterCustomName; // Owned library PFM, UTF-8; never the source GLB.
    Overlays::Realistic::CustomFaceFilterFit faceFilterCustomFit;

    // Stage configurations
    Lighting::LightingConfig lighting;
    Appearance::AppearanceConfig appearance;
    Overlays::OverlayStackConfig overlays;
    BackgroundSettings background;
    StudioBackgroundMode backgroundMode = StudioBackgroundMode::Original;
    BackgroundFitMode backgroundFitMode = BackgroundFitMode::CropFill;

    // Input colour look — applied BEFORE appearance/lighting in the GPU graph.
    // 0=Original (bypass), 1=Warm, 2=Cool, 3=Mono.
    // Matches Codex FramePipeline::Look enum values exactly.
    int   inputLook          = 0;    // DesktopLook enum value
    float inputLookStrength  = 1.0f; // [0.0, 1.0]

    std::string selectedInputLutPath;
    float inputLutIntensity = 0.0f; // independent before appearance/lighting

    // LUT creative grade
    std::string selectedLutPath; // Relative path or pack asset path
    float lutIntensity = 0.0f;   // [0.0, 1.0]

    // Optional pack dependency reference
    std::string packIdReference;
    std::string packVersionReference;

    /// <summary>
    /// Creates a neutral built-in preset (all stages bypassed, identity passthrough).
    /// </summary>
    static StudioPreset Neutral();

    /// <summary>
    /// Creates a warm studio key light + cool fill preset with subtle smoothing and frame.
    /// </summary>
    static StudioPreset WarmKeyCoolFill();

    /// <summary>
    /// Creates a subtle broadcast skin touchup preset with feature protection.
    /// </summary>
    static StudioPreset SubtleTouchup();

    /// <summary>
    /// Validates all parameters are finite, non-NaN, and within valid mathematical bounds.
    /// </summary>
    bool Validate(std::string& outError) const;
};

/// <summary>
/// Preset JSON serialization and atomic file store manager.
/// Preserves existing files on failed writes and validates numerical bounds.
/// </summary>
class PresetStore {
public:
    /// <summary>
    /// Serializes preset into JSON string.
    /// </summary>
    static std::string Serialize(const StudioPreset& preset);

    /// <summary>
    /// Deserializes preset from JSON string with validation and bounds clamping.
    /// </summary>
    static bool Deserialize(const std::string& jsonText, StudioPreset& outPreset, std::string& outError);

    /// <summary>
    /// Atomically saves preset to store directory via temporary file staging and rename.
    /// Preserves existing file if write or serialization fails.
    /// </summary>
    static bool SavePreset(const std::string& storeDir, const StudioPreset& preset, std::string& outError);

    /// <summary>
    /// Loads a preset from a file on disk.
    /// </summary>
    static bool LoadPreset(const std::string& filePath, StudioPreset& outPreset, std::string& outError);

    /// <summary>
    /// Deletes a preset by ID from the store directory.
    /// </summary>
    static bool DeletePreset(const std::string& storeDir, const std::string& presetId, std::string& outError);

    /// <summary>
    /// Enumerates all valid presets in the store directory.
    /// </summary>
    static std::vector<StudioPreset> EnumeratePresets(const std::string& storeDir);
};

} // namespace PrismForge::Effects::Presets
