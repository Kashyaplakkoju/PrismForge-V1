#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <algorithm>
#include <cmath>

namespace PrismForge::Effects::StudioStack::Gpu {

/// <summary>
/// Desktop colour look enum matching Codex FramePipeline::Look for CPU/GPU parity.
/// Values are identical to the Codex enum so casts are safe.
/// </summary>
enum class DesktopLook : int {
    Original = 0,
    Warm     = 1,
    Cool     = 2,
    Mono     = 3
};

/// <summary>
/// Status outcome for GPU Desktop Looks operations.
/// </summary>
enum class GpuDesktopLooksOutcome : int {
    Success        = 0,
    Bypass         = 1,   // look == Original OR strength <= threshold
    NotReady       = 2,
    InvalidInput   = 3,
    UnsupportedFormat = 4,
    DeviceLost     = 5,
    ShaderError    = 6
};

enum class DesktopColourFault { None, ShaderCompile, ShaderCreation, BufferCreation, Map, Dispatch, DeviceRemoved, LutTexture, LutView };

// Shared strict view validation: one full RGBA8 mip, matching device/context and dimensions,
// no SRV/UAV resource alias. Returns false before any state mutation or GPU dispatch.
bool ValidateDesktopColourViews(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output,
    int width, int height) noexcept;

/// <summary>
/// Texture-native D3D11 input colour-look stage for StudioStack.
///
/// Executes one of four modes (Original / Warm / Cool / Mono) via Compute Shader on
/// caller-provided SRV/UAV pair (DXGI_FORMAT_R8G8B8A8_UNORM ping-pong textures).
///
/// Formula fidelity guarantee:
///   - Warm / Cool offsets match the Codex FramePipeline GradeTables exactly
///     (R+14, G+4, B-12 for Warm; R-12, G+2, B+14 for Cool).
///   - Mono uses the Rec.601 integer luminance (77R+150G+29B+128)>>8, matching
///     FramePipeline::ApplyLook bit-for-bit on full-range [0,255] SDR input.
///   - strength == 1.0 is an exact look replacement; fractional values lerp
///     between ungraded and graded in encoded 8-bit SDR space.
///   - strength == 0.0 or look == Original bypasses with GpuDesktopLooksOutcome::Bypass.
///
/// Changed same-size LUT/config/source takes effect on the next frame (stateless shader).
/// </summary>
class D3D11DesktopLooks {
public:
    D3D11DesktopLooks(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11DesktopLooks();

    D3D11DesktopLooks(const D3D11DesktopLooks&) = delete;
    D3D11DesktopLooks& operator=(const D3D11DesktopLooks&) = delete;
    D3D11DesktopLooks(D3D11DesktopLooks&&) noexcept;
    D3D11DesktopLooks& operator=(D3D11DesktopLooks&&) noexcept;

    /// <summary>
    /// Apply the requested colour look to inputSRV -> outputUAV.
    /// Both views must reference DXGI_FORMAT_R8G8B8A8_UNORM textures.
    /// Returns Bypass when look == Original or strength == 0.
    /// </summary>
    GpuDesktopLooksOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        int width, int height,
        DesktopLook look,
        float strength
    );

    void SetFault(DesktopColourFault fault);

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// CPU oracle (header-only inline) — mirrors GPU formula for test verification.
// ---------------------------------------------------------------------------

/// <summary>
/// CPU oracle matching GPU shader formulas exactly.
/// Applies the given look at the given strength to a single RGBA8 UNORM pixel
/// described as four bytes [R, G, B, A]. Operates in-place.
/// strength == 1.0 means full look; 0.0 means bypass.
/// </summary>
inline void DesktopLookApplyPixelCpu(
    uint8_t& r, uint8_t& g, uint8_t& b,
    DesktopLook look, float strength) noexcept
{
    if (!std::isfinite(strength) || strength < 0.0f || strength > 1.0f ||
        static_cast<unsigned>(look) > 3u || look == DesktopLook::Original || strength == 0.0f) return;

    uint8_t gr = r, gg = g, gb = b;

    if (look == DesktopLook::Warm) {
        gr = static_cast<uint8_t>(std::min(255, static_cast<int>(r) + 14));
        gg = static_cast<uint8_t>(std::min(255, static_cast<int>(g) +  4));
        gb = static_cast<uint8_t>(std::max(0,   static_cast<int>(b) - 12));
    } else if (look == DesktopLook::Cool) {
        gr = static_cast<uint8_t>(std::max(0,   static_cast<int>(r) - 12));
        gg = static_cast<uint8_t>(std::min(255, static_cast<int>(g) +  2));
        gb = static_cast<uint8_t>(std::min(255, static_cast<int>(b) + 14));
    } else if (look == DesktopLook::Mono) {
        // Rec. 601 integer luminance: (77*R + 150*G + 29*B + 128) >> 8
        const uint32_t luma = (77u * r + 150u * g + 29u * b + 128u) >> 8;
        gr = gg = gb = static_cast<uint8_t>(luma);
    }

    // Round the entire nonnegative interpolated value, including negative grade deltas.
    auto blend = [strength](uint8_t src, uint8_t graded) {
        return static_cast<uint8_t>(std::clamp((1.0f-strength)*src + strength*graded + 0.5f, 0.0f, 255.0f));
    };
    r = blend(r, gr); g = blend(g, gg); b = blend(b, gb);
}

} // namespace PrismForge::Effects::StudioStack::Gpu
