#pragma once

#include <cstdint>
#include <memory>

namespace PrismForge::Effects {

// Configuration settings for temporal mask stabilization.
struct MaskStabilizerSettings {
    bool enabled = true;
    float alpha = 0.35f;           // Base exponential moving average weight in [0.0f, 1.0f].
                                   // 1.0f = instantaneous update (no smoothing).
                                   // 0.0f = static history (frozen mask).
    float changeThreshold = 0.15f; // Per-pixel delta threshold in [0.0f, 1.0f].
                                   // Deltas exceeding this threshold increase responsiveness
                                   // to suppress trailing/ghosting during fast movement.
};

// Standalone CPU-based temporal mask stabilizer.
// Implements an adjustable exponential moving average (EMA) filter with per-pixel motion thresholding.
// Enforces finite outputs clamped to [0.0f, 1.0f].
//
// Threading & Memory:
// - SetSettings, GetSettings, and Reset are thread-safe.
// - Process executes sequentially on a single processing thread.
// - Caller retains ownership of input buffer.
// - Returns pointer to internal persistent float buffer (valid until next Process call or destruction).
class MaskStabilizer {
public:
    MaskStabilizer();
    ~MaskStabilizer();

    MaskStabilizer(const MaskStabilizer&) = delete;
    MaskStabilizer& operator=(const MaskStabilizer&) = delete;

    void SetSettings(const MaskStabilizerSettings& settings) noexcept;
    MaskStabilizerSettings GetSettings() const noexcept;

    // Explicitly clears temporal history (e.g., upon camera switch or scene transition)
    void Reset() noexcept;

    // Processes a raw alpha mask with explicit dimensions and row stride.
    // Rejects null buffers, invalid dimensions, stride < width, and non-finite floats gracefully.
    // Returns pointer to internally owned stabilized float mask (stride == width),
    // or nullptr on unrecoverable validation failure.
    const float* Process(const float* rawMask, int width, int height, int stride);

    int GetOutputWidth() const noexcept;
    int GetOutputHeight() const noexcept;
    int GetOutputStride() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects
