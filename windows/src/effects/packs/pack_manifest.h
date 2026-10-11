#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace PrismForge::Effects::Packs {

/// <summary>
/// Supported asset types within an offline effect pack.
/// Strictly excludes executable, script, or unverified binary payloads.
/// </summary>
enum class PackAssetType : int {
    CubeLut = 0,          // 3D Color LUT (.cube)
    TrackedAccessory = 1, // Procedural or raster accessory descriptor (.json/.raw)
    ScreenOverlay = 2,    // Screen-space border or graphic (.json/.raw)
    PresetConfig = 3,     // Studio preset parameters (.json)
    PreviewImage = 4      // Bounded preview image (.raw/.png/.jpg/.jpeg/.bmp)
};

/// <summary>
/// Declared individual asset entry in the pack manifest.
/// </summary>
struct PackAssetEntry {
    PackAssetType assetType = PackAssetType::CubeLut;
    std::string assetId;        // Unique identifier within the pack (e.g. "golden_hour_lut")
    std::string relativePath;   // Normalized relative path (e.g. "luts/golden_hour.cube")
    std::string sha256Hex;      // Expected SHA-256 hash in lowercase 64-char hex
    uint64_t sizeBytes = 0;     // Declared file size in bytes
};

/// <summary>
/// Complete versioned manifest for an offline effect pack.
/// </summary>
struct PackManifest {
    int schemaVersion = 1;      // Current supported schema version
    std::string packId;         // Reverse-DNS style ID (e.g. "com.prismforge.packs.warmstudio")
    std::string version;        // Semver string (e.g. "1.0.0")
    std::string displayName;    // Human-readable title
    std::string description;    // Pack summary
    std::string author;         // Author / studio creator
    std::string previewImagePath; // Path to preview thumbnail relative to pack root
    std::vector<PackAssetEntry> assets;

    bool IsValidBasic() const {
        return schemaVersion == 1 &&
               !packId.empty() &&
               !version.empty() &&
               !displayName.empty() &&
               !assets.empty();
    }
};

/// <summary>
/// Manifest JSON parser and serializer utilities.
/// </summary>
class ManifestJson {
public:
    static std::string Serialize(const PackManifest& manifest);
    static bool Deserialize(const std::string& jsonText, PackManifest& outManifest, std::string& outError);
    static bool ReadFromFile(const std::string& filePath, PackManifest& outManifest, std::string& outError);
    static bool WriteToFile(const std::string& filePath, const PackManifest& manifest, std::string& outError);
};

} // namespace PrismForge::Effects::Packs
