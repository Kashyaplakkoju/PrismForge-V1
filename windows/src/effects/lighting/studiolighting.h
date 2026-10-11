#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

struct AVFrame;

namespace PrismForge::Effects::Lighting {

/// <summary>
/// Status outcome of a StudioLighting processing pass.
/// Distinguishes success, bit-exact bypass, unready state, and explicit errors.
/// </summary>
enum class LightingOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    OutOfMemory = 5
};

/// <summary>
/// Explicit lighting mode distinguishing spatial approximation from normal-guided lighting.
/// </summary>
enum class LightingMode : int {
    SpatialApproximation = 0, // Directional / radial gradient simulation without geometric surface normals
    NormalGuided = 1          // True Lambertian diffuse shading driven by 3D surface normal field
};

/// <summary>
/// Configuration for an individual studio light.
/// Position is in normalized preview coordinates [-1.0, 1.0] where:
///   X: -1.0 (left) to +1.0 (right)
///   Y: -1.0 (top) to +1.0 (bottom)
///   Z: nominal depth distance in front of the subject plane (default 0.6)
/// Colors are linear normalized floats in [0.0, 1.0].
/// Intensity is a scalar in [0.0, 2.0].
/// </summary>
struct StudioLight {
    bool enabled = false;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.6f;
    float colorR = 1.0f;
    float colorG = 1.0f;
    float colorB = 1.0f;
    float intensity = 1.0f;

    // Additive Kelvin color temperature [1500, 15000].
    // If <= 0, explicit colorR, colorG, colorB values are used.
    float colorTemperature = 0.0f;

    // Optional green-magenta tint [-1.0, 1.0] applied to Kelvin daylight chromaticity.
    float tint = 0.0f;

    /// <summary>
    /// Deterministic Planckian blackbody / daylight approximation converting Kelvin + tint to linear RGB.
    /// Supports warm tungsten (2700K), neutral daylight (5500K-6500K), and cool overcast (8500K).
    /// </summary>
    static void KelvinToLinearRgb(float kelvin, float tint, float& outR, float& outG, float& outB);

    /// <summary>
    /// Evaluates effective linear RGB color (using Kelvin if colorTemperature > 0, else explicit RGB).
    /// </summary>
    void GetEffectiveLinearColor(float& outR, float& outG, float& outB) const;
};

/// <summary>
/// Engine-level immutable coordinate conversion helpers for mapping between normalized light space [-1, 1],
/// preview screen pixel rectangles, and letterboxed/aspect-fit video rects.
/// </summary>
struct LightCoordinateHelper {
    static StudioLight SanitizeLight(const StudioLight& inLight);

    static void LightToScreenCoords(
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
    );

    static void ScreenToLightCoords(
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
    );
};

/// <summary>
/// Quality and behavior controls for approximate geometry self-occlusion shadows.
/// Cap steps and distance to bound GPU ray-march costs.
/// </summary>
struct ApproximateShadowConfig {
    bool enabled = false;          // Opt-in: false preserves standard cheaper normal/spatial modes
    int maxSteps = 16;             // Bounded ray march steps [4, 32]
    float maxDistance = 0.25f;     // Screen-space ray march distance limit [0.05, 0.50]
    float rayElevation = 0.05f;    // Elevation angle / bias to prevent self-shadow acne [0.01, 0.20]
    float shadowIntensity = 0.60f; // Max shadow darkening factor [0.0, 1.0], preserving ambient
    float shadowSoftness = 0.50f;  // Penumbra soften factor [0.1, 1.0]
};

/// <summary>
/// Complete lighting engine configuration snapshot.
/// Immutable per frame.
/// </summary>
struct LightingConfig {
    StudioLight light1;          // Primary key light (typically warm orange/amber)
    StudioLight light2;          // Secondary fill light (typically cool blue/cyan)
    float globalStrength = 1.0f; // [0.0, 2.0] master lighting multiplier
    float ambientIntensity = 1.0f; // [0.0, 2.0] base ambient level
    float fillIntensity = 0.0f;    // [0.0, 2.0] uniform fill light
    bool bypass = false;           // If true, exact bit-level passthrough
    bool subjectOnlyMode = true;   // If true and no mask provided, returns passthrough/NotReady
    LightingMode lightingMode = LightingMode::SpatialApproximation; // Explicit lighting mode
    ApproximateShadowConfig shadowConfig; // Optional approximate geometry shadows

    /// <summary>
    /// Creates a neutral default configuration (bypass active, neutral unity factors).
    /// </summary>
    static LightingConfig Neutral() {
        LightingConfig cfg;
        cfg.light1.enabled = false;
        cfg.light2.enabled = false;
        cfg.ambientIntensity = 1.0f;
        cfg.fillIntensity = 0.0f;
        cfg.globalStrength = 1.0f;
        cfg.bypass = true;
        cfg.subjectOnlyMode = true;
        cfg.shadowConfig.enabled = false;
        return cfg;
    }

    /// <summary>
    /// Creates a standard two-light studio preset:
    /// Light 1: Warm key light at upper-left (-0.45, -0.35)
    /// Light 2: Cool fill light at mid-right (+0.45, -0.15)
    /// </summary>
    static LightingConfig DefaultTwoLight() {
        LightingConfig cfg;
        cfg.light1.enabled = true;
        cfg.light1.posX = -0.45f;
        cfg.light1.posY = -0.35f;
        cfg.light1.posZ = 0.55f;
        cfg.light1.colorR = 1.0f;
        cfg.light1.colorG = 0.78f;
        cfg.light1.colorB = 0.55f; // Warm amber/orange
        cfg.light1.intensity = 1.0f;

        cfg.light2.enabled = true;
        cfg.light2.posX = 0.45f;
        cfg.light2.posY = -0.15f;
        cfg.light2.posZ = 0.65f;
        cfg.light2.colorR = 0.60f;
        cfg.light2.colorG = 0.80f;
        cfg.light2.colorB = 1.0f;  // Cool cyan/blue
        cfg.light2.intensity = 0.7f;

        cfg.globalStrength = 1.0f;
        cfg.ambientIntensity = 0.65f;
        cfg.fillIntensity = 0.15f;
        cfg.bypass = false;
        cfg.subjectOnlyMode = true;
        return cfg;
    }
};

/// <summary>
/// Jitter smoothing for draggable light controls with bounded lag and immediate snap/reset.
/// </summary>
class LightJitterSmoother {
public:
    explicit LightJitterSmoother(float smoothingFactor = 0.35f, float snapThreshold = 0.25f);
    void Reset();
    StudioLight SmoothLight1(const StudioLight& target);
    StudioLight SmoothLight2(const StudioLight& target);
    LightingConfig SmoothConfig(const LightingConfig& inConfig);

private:
    StudioLight SmoothInternal(const StudioLight& target, StudioLight& history, bool& hasHistory);

    float m_alpha;
    float m_snapThreshold;
    StudioLight m_smoothed1;
    StudioLight m_smoothed2;
    bool m_hasHistory1 = false;
    bool m_hasHistory2 = false;
};

/// <summary>
/// Validated descriptor for an externally supplied subject alpha mask.
/// Values must be floats in [0.0, 1.0] (0 = background, 1 = subject).
/// </summary>
struct AlphaMask {
    const float* data = nullptr;
    int width = 0;
    int height = 0;
    int strideFloats = 0; // Number of float elements per row
};

/// <summary>
/// Validated descriptor for an optional externally supplied surface normal field.
/// 3 contiguous floats per pixel (nx, ny, nz) representing surface normal vector.
/// </summary>
struct NormalField {
    const float* data = nullptr; // nx, ny, nz per pixel
    int width = 0;
    int height = 0;
    int strideFloats = 0; // Number of float elements per row (>= width * 3)
};

/// <summary>
/// Validated descriptor for an optional externally supplied relative depth map.
/// 1 float per pixel (z in [0.0, 1.0], 0 = near, 1 = far).
/// </summary>
struct DepthField {
    const float* data = nullptr;
    int width = 0;
    int height = 0;
    int strideFloats = 0; // Number of float elements per row (>= width)
};

/// <summary>
/// Standalone CPU Two-Light Studio Lighting Engine.
/// 
/// Processes SDR BGR24 software AVFrames in linear color space, applying independently
/// positioned warm/cool lights, ambient/fill, and subject blending.
/// 
/// Shading Model:
/// - With NormalField: True Lambertian directional diffuse shading (N . L).
/// - Without NormalField (Mask only): Clearly named lightweight Spatial Light Approximation
///   with smooth radial/directional gradients. Face detail and surface textures are preserved
///   by multiplicative reflectance modulation in linear RGB.
/// - With DepthField & Approximate Geometry Shadows: Screen-space ray-marched self-occlusion
///   visibility attenuation for Light 1 and Light 2, preserving ambient and fill.
/// - Background Preservation: Pixels with mask <= 0 are 100% bit-exact preserved from input.
/// - Thread Safety: Per-instance caller-thread owned. Output frame is cached internally and
///   borrowed by the caller until the next ProcessFrame call or instance destruction.
/// </summary>
class StudioLighting {
public:
    StudioLighting();
    ~StudioLighting();

    StudioLighting(const StudioLighting&) = delete;
    StudioLighting& operator=(const StudioLighting&) = delete;
    StudioLighting(StudioLighting&&) noexcept;
    StudioLighting& operator=(StudioLighting&&) noexcept;

    /// <summary>
    /// Processes a single BGR24 SDR frame according to the provided configuration, mask, normals, and depth.
    /// </summary>
    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const AlphaMask* mask,
        const NormalField* normals,
        const DepthField* depth,
        const LightingConfig& config,
        LightingOutcome& outcome
    );

    /// <summary>
    /// Overload for backwards compatibility (depth = nullptr).
    /// </summary>
    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const AlphaMask* mask,
        const NormalField* normals,
        const LightingConfig& config,
        LightingOutcome& outcome
    ) {
        return ProcessFrame(inputFrame, mask, normals, nullptr, config, outcome);
    }

    /// <summary>
    /// Lightweight spatial light approximation factor calculation.
    /// Evaluates directional gradient across normalized preview coordinates (u, v).
    /// </summary>
    static float CalculateSpatialLightApproximation(
        float u, float v,
        float aspect,
        const StudioLight& light
    );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::Lighting
