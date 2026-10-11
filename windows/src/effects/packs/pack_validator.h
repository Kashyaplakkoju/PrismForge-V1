#pragma once

#include "pack_manifest.h"
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace PrismForge::Effects::Packs {

struct PackValidationResult {
    bool valid = false;
    std::string errorReason;
    std::vector<std::string> warnings;

    static PackValidationResult Success() {
        PackValidationResult r;
        r.valid = true;
        return r;
    }

    static PackValidationResult Failure(const std::string& reason) {
        PackValidationResult r;
        r.valid = false;
        r.errorReason = reason;
        return r;
    }
};

class PackValidator {
public:
    /// <summary>
    /// Validates an individual path component (packId segment, version, txId, etc.).
    /// Enforces rejection of traversal, separators, control bytes, trailing dots/spaces,
    /// and Windows reserved names.
    /// </summary>
    static bool ValidatePathComponent(
        const std::string& component,
        std::string& outError);

    /// <summary>
    /// Validates that descendant is strictly an owned descendant of root.
    /// Verifies canonical containment and checks that no existing ancestor or candidate
    /// component is a symlink or reparse point.
    /// </summary>
    static bool ValidateOwnedDescendant(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate,
        std::string& outError);

    /// <summary>
    /// Validates pack identifier (reverse-DNS format) or version string (strict SemVer 2.0.0).
    /// Enforces rejection of traversal, Windows reserved names, control bytes, trailing dots/spaces,
    /// leading-zero numbers in SemVer, non-numeric prerelease zeros, and aliases.
    /// </summary>
    static bool ValidateIdentifier(
        const std::string& id,
        bool isVersion,
        std::string& outError);

    /// <summary>
    /// Validates target library root path and all existing ancestor components.
    /// Rejects reparse points, junctions, symlinks, device paths, UNC, alternate data streams,
    /// trailing dots/spaces, control characters, and reserved Windows device names.
    /// </summary>
    static bool ValidateLibraryRoot(
        const std::string& rootPath,
        std::string& outError);

    /// <summary>
    /// Validates path containment, traversal prevention, and extension whitelist.
    /// </summary>
    static bool ValidateRelativePath(
        const std::string& path,
        PackAssetType assetType,
        std::string& outError);

    /// <summary>
    /// Validates raw or encoded preview image data against bounded dimensions and format magic.
    /// Supported formats: .raw (exactly64x64 straight RGBA8), complete .png/.bmp. JPEG previews explicitly unsupported for v1.
    /// Uses the same bounded full decoder as typed directory validation; no header-only success.
    /// </summary>
    static bool ValidatePreviewImageBytes(
        const uint8_t* data,
        size_t size,
        const std::string& pathOrExtension,
        int& outWidth,
        int& outHeight,
        std::string& outError);

    /// <summary>
    /// Validates manifest schema, duplicate IDs, bounds, and security contracts.
    /// </summary>
    static PackValidationResult ValidateManifest(const PackManifest& manifest);

    /// <summary>
    /// Validates on-disk files against declared manifest entries:
    /// verifies existence, sizes, regular file types, symlink/reparse point rejection, and SHA-256 checksums.
    /// </summary>
    static PackValidationResult ValidateDirectoryPack(
        const std::string& packDirectoryPath,
        const PackManifest& manifest);
};

} // namespace PrismForge::Effects::Packs
