#pragma once

#include "pack_manifest.h"
#include "pack_validator.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace PrismForge::Effects::Packs {

/// <summary>
/// Fault injection seam for testing transactional rollback and recovery.
/// </summary>
enum class PackFaultSeam : int {
    None = 0,
    SimulateCopyFailure = 1,
    SimulateCommitRenameFailure = 2,
    SimulateValidationFailure = 3,
    SimulateCrashAfterBackup = 4,
    SimulateCrashAfterPublish = 5,
    SimulateJournalWriteFailure = 6,
    SimulateRollbackFailure = 7,
    SimulateCrashAfterPrepared = 8,
    SimulateJournalAfterBackupFailure = 9,
    SimulateJournalAfterPublishFailure = 10,
    SimulateJournalAfterCommitFailure = 11,
    SimulateJournalFlushFailure = 12,
    SimulateJournalRenameFailure = 13,
    SimulateCleanupFailure = 14
};

/// <summary>
/// Integrity state reported for catalogue entries.
/// </summary>
enum class PackIntegrityState : int {
    Valid = 0,
    CorruptManifest = 1,
    CorruptAsset = 2,
    MissingFile = 3,
    UnverifiedPreview = 4,
    SecurityViolation = 5
};

/// <summary>
/// Detailed catalogue entry with typed descriptors, verified preview, and actionable error.
/// </summary>
struct PackCatalogueEntry {
    std::string packId;
    std::string version;
    std::string displayName;
    std::string description;
    std::string author;
    std::string previewAssetId;
    std::vector<PackAssetEntry> assets;
    PackIntegrityState integrityState = PackIntegrityState::Valid;
    std::string actionableError;
    bool isUsable = true;
};

struct VerifiedPackSnapshot;

/// <summary>
/// Runtime representation of an installed, validated, and loaded effect pack.
/// Holds immutable verified manifest and provides bounded, verified asset reading.
/// </summary>
struct LoadedPack {
    PackManifest manifest;
    std::string packDirectory;

    /// <summary>
    /// Resolves an asset's absolute file path on disk by its unique asset ID.
    /// Informational location only: updates/uninstall can invalidate it. Use ReadAssetBytes.
    /// Returns empty string if not found or if path escapes pack directory.
    /// </summary>
    std::string GetAssetAbsolutePath(const std::string& assetId) const;

    /// <summary>
    /// Copies previously verified immutable bytes. Never opens the live asset path.
    /// Returns empty vector on I/O error or integrity failure.
    /// </summary>
    std::vector<uint8_t> ReadAssetBytes(const std::string& assetId) const;

    /// <summary>
    /// Reads preview image raw bytes into memory if declared in manifest.
    /// </summary>
    std::vector<uint8_t> GetPreviewBytes() const;

    /// <summary>
    /// Decodes and returns dimensions of the verified preview image.
    /// </summary>
    bool GetPreviewDimensions(int& outWidth, int& outHeight) const;

    // Stable, verified metadata owned by this lease; public manifest is a legacy copy.
    const PackManifest* GetVerifiedManifest() const;
    const PackAssetEntry* GetAssetDescriptor(const std::string& assetId) const;
    const PackAssetEntry* GetPreviewDescriptor() const;

private:
    friend class PackManager;
    std::shared_ptr<const VerifiedPackSnapshot> snapshot_;
};

/// <summary>
/// Offline Effect-Pack Manager handling staged installation (from directory or ZIP archive),
/// atomic commits, transactional rollback on failure, enumeration, loading, and safe uninstallation.
/// </summary>
enum class PackTransactionPhase : int {
    PreparedJournal = 1, BackupMoved = 2, BackupJournal = 3,
    PublishMoved = 4, PublishJournal = 5, CommitJournal = 6,
    BeforeCleanup = 7, Complete = 8, LockAcquired = 9
};
using PackTransactionCheckpoint = void (*)(PackTransactionPhase, void*);

class PackManager {
public:
    PackManager() = default;
    ~PackManager() = default;

    /// <summary>
    /// Installs a pack from a source directory into the target library root.
    /// Uses cross-process locking, staging, full SHA-256 verification, and journaled atomic commit.
    /// </summary>
    static bool InstallPack(
        const std::string& sourcePackDir,
        const std::string& targetLibraryRoot,
        std::string& outError);

    /// <summary>
    /// Installs a pack from a ZIP archive file into the target library root.
    /// Uses wxBase zip streams, bounds checking, payload whitelisting, and journaled commit.
    /// </summary>
    static bool InstallPackFromArchive(
        const std::string& archivePath,
        const std::string& targetLibraryRoot,
        std::string& outError);

    /// <summary>
    /// Loads and verifies an installed pack from disk.
    /// </summary>
    static std::unique_ptr<LoadedPack> LoadPack(
        const std::string& packDirectory,
        std::string& outError);

    /// <summary>
    /// Enumerates all valid installed packs in the target library root.
    /// Results are returned in deterministic sorted order (by packId, then version).
    /// </summary>
    static std::vector<PackManifest> EnumeratePacks(const std::string& targetLibraryRoot);

    /// <summary>
    /// Alias for EnumeratePacks providing deterministic sorted catalog of installed packs.
    /// </summary>
    static std::vector<PackManifest> ListInstalledPacks(const std::string& targetLibraryRoot) {
        return EnumeratePacks(targetLibraryRoot);
    }

    /// <summary>
    /// Returns full catalogue of installed packs with typed descriptors and integrity state.
    /// Truthfully reports corrupt or unavailable entries with actionable error messages.
    /// </summary>
    static std::vector<PackCatalogueEntry> GetCatalogue(const std::string& targetLibraryRoot);

    /// <summary>
    /// Recovers interrupted transactions from the journal in target library root.
    /// Deterministically restores backed-up intact packages and cleans staging.
    /// </summary>
    static bool RecoverInterruptedTransactions(const std::string& targetLibraryRoot, std::string& outError);

    /// <summary>
    /// Uninstalls an installed pack (or specific version) from the target library root.
    /// Validates identity and root containment before deletion.
    /// </summary>
    static bool UninstallPack(
        const std::string& targetLibraryRoot,
        const std::string& packId,
        const std::string& version,
        std::string& outError);

    /// <summary>
    /// Safely sweeps and cleans orphaned staging directories and completed journals.
    /// </summary>
    static void CleanOrphanStaging(const std::string& targetLibraryRoot);

    /// <summary>
    /// Fault injection control for testing.
    /// </summary>
    static void SetFaultSeam(PackFaultSeam seam);
    static PackFaultSeam GetFaultSeam();

    // Test-only deterministic child handshake; installed before starting calls.
    // Callback executes synchronously while the production owner/lock is held.
    static void SetTransactionCheckpoint(PackTransactionCheckpoint callback, void* context);
    static void SetLockTimeoutForTests(uint32_t milliseconds);
    static uint32_t GetLastProcessLockResult();

    /// <summary>
    /// Gets the most recent transaction recovery error, if any.
    /// </summary>
    static std::string GetLastRecoveryError();

    /// <summary>
    /// Checks whether an unrecovered transaction error is currently recorded.
    /// </summary>
    static bool HasRecoveryError();

    /// <summary>
    /// Clears any recorded recovery error.
    /// </summary>
    static void ClearRecoveryError();
};

} // namespace PrismForge::Effects::Packs
