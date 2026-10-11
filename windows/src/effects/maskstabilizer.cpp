#include "maskstabilizer.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace PrismForge::Effects {

struct MaskStabilizer::Impl {
    std::atomic<bool> m_enabled{true};
    std::atomic<float> m_alpha{0.35f};
    std::atomic<float> m_changeThreshold{0.15f};
    std::atomic<bool> m_resetRequested{true};

    int m_width = 0;
    int m_height = 0;
    std::vector<float> m_history;
    bool m_hasHistory = false;

    void ResetInternal() noexcept {
        m_hasHistory = false;
    }
};

MaskStabilizer::MaskStabilizer() : m_impl(std::make_unique<Impl>()) {}
MaskStabilizer::~MaskStabilizer() = default;

void MaskStabilizer::SetSettings(const MaskStabilizerSettings& settings) noexcept {
    m_impl->m_enabled.store(settings.enabled, std::memory_order_relaxed);
    m_impl->m_alpha.store(std::clamp(settings.alpha, 0.0f, 1.0f), std::memory_order_relaxed);
    m_impl->m_changeThreshold.store(std::clamp(settings.changeThreshold, 0.0f, 1.0f), std::memory_order_relaxed);
}

MaskStabilizerSettings MaskStabilizer::GetSettings() const noexcept {
    MaskStabilizerSettings s;
    s.enabled = m_impl->m_enabled.load(std::memory_order_relaxed);
    s.alpha = m_impl->m_alpha.load(std::memory_order_relaxed);
    s.changeThreshold = m_impl->m_changeThreshold.load(std::memory_order_relaxed);
    return s;
}

void MaskStabilizer::Reset() noexcept {
    m_impl->m_resetRequested.store(true, std::memory_order_relaxed);
}

const float* MaskStabilizer::Process(const float* rawMask, int width, int height, int stride) {
    if (!rawMask || width <= 0 || height <= 0 || stride < width) {
        m_impl->ResetInternal();
        return nullptr;
    }

    // Checked arithmetic for buffer size
    const uint64_t totalPixels64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (totalPixels64 > 64ULL * 1024ULL * 1024ULL) { // 64 Mpixel maximum safe bound
        m_impl->ResetInternal();
        return nullptr;
    }
    const size_t totalPixels = static_cast<size_t>(totalPixels64);

    // Validate non-finite floats
    for (int y = 0; y < height; ++y) {
        const float* row = rawMask + y * stride;
        for (int x = 0; x < width; ++x) {
            if (!std::isfinite(row[x])) {
                m_impl->ResetInternal();
                return nullptr;
            }
        }
    }

    const bool enabled = m_impl->m_enabled.load(std::memory_order_relaxed);
    const float alpha = std::clamp(m_impl->m_alpha.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float threshold = std::clamp(m_impl->m_changeThreshold.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const bool resetRequested = m_impl->m_resetRequested.exchange(false, std::memory_order_relaxed);

    const bool dimensionsChanged = (width != m_impl->m_width || height != m_impl->m_height);

    if (dimensionsChanged) {
        m_impl->m_history.resize(totalPixels);
        m_impl->m_width = width;
        m_impl->m_height = height;
    }

    // If history is absent, reset was requested, or disabled, copy clamped input directly
    if (!m_impl->m_hasHistory || resetRequested || !enabled || dimensionsChanged) {
        for (int y = 0; y < height; ++y) {
            const float* inRow = rawMask + y * stride;
            float* outRow = m_impl->m_history.data() + y * width;
            for (int x = 0; x < width; ++x) {
                outRow[x] = std::clamp(inRow[x], 0.0f, 1.0f);
            }
        }

        m_impl->m_hasHistory = enabled; // Only preserve history if enabled
        return m_impl->m_history.data();
    }

    // Temporal Exponential Moving Average filtering with motion thresholding
    for (int y = 0; y < height; ++y) {
        const float* inRow = rawMask + y * stride;
        float* histRow = m_impl->m_history.data() + y * width;

        for (int x = 0; x < width; ++x) {
            const float rawVal = std::clamp(inRow[x], 0.0f, 1.0f);
            const float prevVal = histRow[x];
            const float delta = std::fabs(rawVal - prevVal);

            float effectiveAlpha = alpha;
            if (threshold > 0.0f && delta > threshold) {
                // Adaptive response: accelerate blending proportionally to excess change
                const float excess = (delta - threshold) / (1.0f - threshold + 1e-6f);
                effectiveAlpha = alpha + (1.0f - alpha) * std::clamp(excess, 0.0f, 1.0f);
            }

            const float smoothed = prevVal * (1.0f - effectiveAlpha) + rawVal * effectiveAlpha;
            histRow[x] = std::clamp(smoothed, 0.0f, 1.0f);
        }
    }

    return m_impl->m_history.data();
}

int MaskStabilizer::GetOutputWidth() const noexcept {
    return m_impl->m_width;
}

int MaskStabilizer::GetOutputHeight() const noexcept {
    return m_impl->m_height;
}

int MaskStabilizer::GetOutputStride() const noexcept {
    return m_impl->m_width;
}

} // namespace PrismForge::Effects
