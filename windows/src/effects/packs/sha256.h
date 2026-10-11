#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace PrismForge::Effects::Packs {

/// <summary>
/// FIPS 180-2 compliant SHA-256 implementation.
/// Completely self-contained with zero external dependencies.
/// </summary>
class Sha256 {
public:
    Sha256();
    void Reset();
    void Update(const uint8_t* data, size_t length);
    void Update(const std::string& str);
    std::string Final();

    /// <summary>
    /// Helper to hash a full memory buffer and return the 64-char lowercase hex string.
    /// </summary>
    static std::string ComputeHex(const uint8_t* data, size_t length);

    /// <summary>
    /// Helper to hash a file on disk in streaming chunks. Returns empty string on I/O error.
    /// </summary>
    static std::string ComputeFileHex(const std::string& filePath, uint64_t* outFileSize = nullptr);

    // Hash exactly expectedBytes, never stream to an unbounded EOF. At most one
    // additional byte is read to reject growth/trailing data. The explicit cap is
    // checked before opening; failures return empty hash and actionable error.
    static std::string ComputeFileHexBounded(const std::string& filePath,
        uint64_t expectedBytes, uint64_t maxBytes, std::string& outError,
        uint64_t* outBytesRead = nullptr);

private:
    void Transform(const uint8_t* chunk);

private:
    uint32_t m_state[8];
    uint64_t m_bitLength = 0;
    uint8_t m_buffer[64];
    size_t m_bufferLen = 0;
};

} // namespace PrismForge::Effects::Packs
