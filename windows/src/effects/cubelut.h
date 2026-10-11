#pragma once

#include "rtsp/ffmpeg.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PrismForge::Effects {

// RGB float triple representation.
struct RgbFloat {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// Immutable parsed 3D colour LUT representation.
// Constructed strictly from bounded memory buffer/text; performs no disk or network I/O.
// Red-fastest indexing: index(r, g, b) = r + size * (g + size * b).
class CubeLut {
public:
    static constexpr size_t MaxPayloadBytes = 16 * 1024 * 1024;
    ~CubeLut() = default;
    CubeLut(const CubeLut&) = delete;
    CubeLut& operator=(const CubeLut&) = delete;
    CubeLut(CubeLut&&) noexcept = default;
    CubeLut& operator=(CubeLut&&) noexcept = default;

    // Parses a .cube format text buffer.
    // Rejects payloads > 16 MiB, 1D/mixed LUTs, sizes outside [2, 65], invalid domains,
    // unsupported directives, non-finite numbers, and incorrect row counts.
    // Returns nullptr on failure, optionally populating errorOut.
    static std::unique_ptr<CubeLut> Parse(std::string_view text, std::string* errorOut = nullptr);

    // Queries
    int GetSize() const noexcept { return m_size; }
    const std::string& GetTitle() const noexcept { return m_title; }
    RgbFloat GetDomainMin() const noexcept { return m_domainMin; }
    RgbFloat GetDomainMax() const noexcept { return m_domainMax; }

    // Evaluates the 3D LUT for an input (r, g, b) triple using trilinear interpolation.
    // Inputs outside the declared [DOMAIN_MIN, DOMAIN_MAX] range are clamped to domain boundaries
    // prior to lattice indexing. Output RGB floats may reflect creative extrapolations but are
    // clamped to [0.0f, 1.0f] when converted to 8-bit integer outputs.
    // NaN maps to the lower domain bound; infinities clamp to the appropriate bound.
    RgbFloat Sample(float r, float g, float b) const noexcept;

    // Optimized trilinear sampling for 8-bit channel inputs [0, 255] using precomputed lattice coordinates.
    RgbFloat SampleBytes(uint8_t r, uint8_t g, uint8_t b) const noexcept;

private:
    CubeLut(int size, std::string title, RgbFloat domainMin, RgbFloat domainMax, std::vector<RgbFloat> table);

    struct PrecomputedCoord {
        int i0 = 0;
        int i1 = 0;
        float fx = 0.0f;
    };

    RgbFloat SampleCoords(const PrecomputedCoord& r, const PrecomputedCoord& g, const PrecomputedCoord& b) const noexcept;

    bool m_extremeValues = false;
    int m_size = 0;
    std::string m_title;
    RgbFloat m_domainMin{0.0f, 0.0f, 0.0f};
    RgbFloat m_domainMax{1.0f, 1.0f, 1.0f};
    std::vector<RgbFloat> m_table; // Size: m_size^3
    PrecomputedCoord m_coordsR[256];
    PrecomputedCoord m_coordsG[256];
    PrecomputedCoord m_coordsB[256];
};

// Stateful processor for applying a 3D LUT to caller-owned BGR24 AVFrames.
// Follows compositor ownership semantics:
// - Caller owns input frame.
// - Returns exact input pointer on bypass (intensity == 0.0f or null lut) or validation failure.
// - Returns processor-owned AVFrame* when active. Buffer is reused and remains valid
//   until next Process call or processor destruction. av_frame_clone retains pixels safely.
class CubeLutProcessor {
public:
    CubeLutProcessor();
    ~CubeLutProcessor();

    CubeLutProcessor(const CubeLutProcessor&) = delete;
    CubeLutProcessor& operator=(const CubeLutProcessor&) = delete;

    // Processes a BGR24 input frame with the given LUT and intensity [0.0f, 1.0f].
    // If intensity == 0.0f or lut == nullptr, returns input directly without copy.
    AVFrame* Process(AVFrame* input, const CubeLut* lut, float intensity);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects
