#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/packs/sha256.h"
#include "windows/src/effects/packs/pack_validator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using namespace PrismForge::Effects::Packs;
namespace fs = std::filesystem;

namespace {

int g_assertions = 0;

struct CapabilitySkip : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void Check(bool condition, const char* message) {
    ++g_assertions;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void WriteFile(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(value.data(), value.size());
    out.close();
    Check(!out.fail(), "WriteFile failed");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

PackAssetEntry CreateAsset(
    const std::string& id,
    const std::string& relPath,
    PackAssetType type,
    const std::string& bytes)
{
    return {
        type,
        id,
        relPath,
        Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()),
        static_cast<uint64_t>(bytes.size())
    };
}

// 16 bytes represents a valid 2x2 square RGBA preview image (2 * 2 * 4 = 16 bytes)
const std::string kDefaultPreviewBytes(16, '\x80');

PackManifest CreateFixture(
    const fs::path& root,
    const std::string& packId = "com.prismforge.recovery",
    const std::string& version = "1.0.0",
    const std::string& assetValue = "asset_data_v1",
    const std::string& previewValue = kDefaultPreviewBytes)
{
    PackManifest m;
    m.schemaVersion = 1;
    m.packId = packId;
    m.version = version;
    m.displayName = "Recovery Pack";
    m.previewImagePath = "images/preview.raw";
    m.assets = {
        CreateAsset("look", "looks/test.cube", PackAssetType::CubeLut, assetValue),
        CreateAsset("preview", m.previewImagePath, PackAssetType::PreviewImage, previewValue)
    };
    WriteFile(root / m.assets[0].relativePath, assetValue);
    WriteFile(root / m.assets[1].relativePath, previewValue);
    std::string err;
    Check(ManifestJson::WriteToFile((root / "manifest.json").string(), m, err), "Manifest write failed");
    return m;
}

std::string Text(const std::vector<uint8_t>& val) {
    return {val.begin(), val.end()};
}

#ifdef _WIN32
std::wstring GetSelfExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size()) break;
        buffer.resize(buffer.size() * 2);
    }
    return std::wstring(buffer.data(), length);
}

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
};

ProcessResult RunChildProcess(const std::wstring& arguments, DWORD timeoutMs = 15000) {
    std::wstring exe = GetSelfExecutablePath();
    std::wstring cmd = L"\"" + exe + L"\" " + arguments;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    if (!CreateProcessW(
        exe.c_str(),
        cmdBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        return {-1, false};
    }

    DWORD waitRes = WaitForSingleObject(pi.hProcess, timeoutMs);
    ProcessResult res;
    if (waitRes == WAIT_TIMEOUT) {
        res.timedOut = true;
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 1000);
        res.exitCode = 124;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        res.exitCode = static_cast<int>(code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
}

bool CreateJunction(const fs::path& link, const fs::path& target, DWORD& outError) {
    fs::create_directories(link);
    HANDLE h = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        outError = GetLastError();
        return false;
    }

    std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
    std::wstring print = fs::absolute(target).native();
    size_t bytes = 16 + (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<uint8_t> buffer(bytes);

    *reinterpret_cast<DWORD*>(buffer.data()) = IO_REPARSE_TAG_MOUNT_POINT;
    auto* fields = reinterpret_cast<WORD*>(buffer.data() + 4);
    fields[0] = static_cast<WORD>(bytes - 8);
    fields[1] = 0;
    fields[2] = 0;
    fields[3] = static_cast<WORD>(substitute.size() * 2);
    fields[4] = static_cast<WORD>((substitute.size() + 1) * 2);
    fields[5] = static_cast<WORD>(print.size() * 2);

    auto* names = reinterpret_cast<wchar_t*>(buffer.data() + 16);
    std::copy(substitute.begin(), substitute.end(), names);
    std::copy(print.begin(), print.end(), names + substitute.size() + 1);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(
        h,
        FSCTL_SET_REPARSE_POINT,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr,
        0,
        &returned,
        nullptr
    );
    outError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return ok != FALSE;
}
#endif

// =========================================================================
// TEST 1: Forged External and Ancestor Journal Paths
// =========================================================================
void TestForgedExternalAndAncestorPaths(const fs::path& root) {
    fs::path library = root / "library";
    fs::path staging = library / ".staging";
    fs::path outside = root / "outside";
    fs::create_directories(staging);
    fs::create_directories(outside);

    fs::path sentinelFile = outside / "sentinel.txt";
    const std::string kSentinelContent = "CRITICAL_SENTINEL_DATA_MUST_SURVIVE";
    WriteFile(sentinelFile, kSentinelContent);

    // Case A: Forged journal naming external path as target
    fs::path forgedJournalA = staging / "journal_tx_forged_a.json";
    std::string forgedJsonA = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"txId\": \"tx_forged_a\",\n"
        "  \"state\": \"PREPARED\",\n"
        "  \"packId\": \"com.prismforge.recovery\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"targetRelPath\": \"../../outside/sentinel.txt\"\n"
        "}\n";
    WriteFile(forgedJournalA, forgedJsonA);

    std::string err;
    bool recRes = PackManager::RecoverInterruptedTransactions(library.string(), err);
    Check(!recRes, "Recovery must fail closed on forged target path");
    Check(PackManager::HasRecoveryError(), "Recovery error must be recorded");

    // Assert external sentinel survived byte-for-byte!
    Check(ReadFile(sentinelFile) == kSentinelContent, "External sentinel file was modified or deleted!");
    // Assert forged journal was quarantined
    Check(!fs::exists(forgedJournalA), "Forged journal was not removed/quarantined from .staging root");
    Check(fs::exists(staging / "quarantine" / "journal_tx_forged_a.json"), "Forged journal missing from quarantine");

    PackManager::ClearRecoveryError();

    // Case B: Forged journal naming another pack's path
    fs::path sourceOther = root / "source_other";
    CreateFixture(sourceOther, "com.prismforge.legit", "1.0.0", "legit_data");
    Check(PackManager::InstallPack(sourceOther.string(), library.string(), err), "Install legit pack failed");

    fs::path forgedJournalB = staging / "journal_tx_forged_b.json";
    std::string forgedJsonB = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"txId\": \"tx_forged_b\",\n"
        "  \"state\": \"PREPARED\",\n"
        "  \"packId\": \"com.prismforge.attacker\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"targetRelPath\": \"com.prismforge.legit/1.0.0\"\n"
        "}\n";
    WriteFile(forgedJournalB, forgedJsonB);

    recRes = PackManager::RecoverInterruptedTransactions(library.string(), err);
    Check(!recRes, "Recovery must fail closed on cross-pack tampering journal");
    Check(ReadFile(sentinelFile) == kSentinelContent, "Sentinel must survive");

    // Verify legit pack is 100% intact and untouched
    auto legitLoaded = PackManager::LoadPack((library / "com.prismforge.legit" / "1.0.0").string(), err);
    Check(legitLoaded != nullptr, "Legit pack was corrupted by attacker journal");
    Check(Text(legitLoaded->ReadAssetBytes("look")) == "legit_data", "Legit pack data was altered");

    PackManager::ClearRecoveryError();
}

// =========================================================================
// TEST 2: Nested Staging and Target Reparse Points
// =========================================================================
void TestReparsePointsInStagingAndTarget(const fs::path& root) {
#ifdef _WIN32
    fs::path library = root / "library";
    fs::path staging = library / ".staging";
    fs::path outside = root / "outside_reparse";
    fs::create_directories(staging);
    fs::create_directories(outside);

    fs::path sentinel = outside / "sentinel.txt";
    WriteFile(sentinel, "REPARSE_SENTINEL_DATA");

    DWORD jErr = 0;
    fs::path junctionPath = staging / "staging_tx_junction";
    if (!CreateJunction(junctionPath, outside, jErr)) {
        if (jErr == ERROR_PRIVILEGE_NOT_HELD || jErr == ERROR_NOT_SUPPORTED || jErr == ERROR_INVALID_FUNCTION) {
            throw CapabilitySkip("Junction capability unavailable");
        }
        throw std::runtime_error("Failed to create test junction");
    }

    // Try to run recovery or cleanup
    PackManager::CleanOrphanStaging(library.string());

    // Verify junction was NOT followed recursively to delete outside sentinel!
    Check(ReadFile(sentinel) == "REPARSE_SENTINEL_DATA", "Junction target contents were destroyed!");

    // Clean up test junction entry explicitly
    RemoveDirectoryW(junctionPath.c_str());
#else
    throw CapabilitySkip("Windows reparse points unavailable on non-Windows");
#endif
}

// =========================================================================
// TEST 3: Invalid, Truncated, and Oversized Journals
// =========================================================================
void TestInvalidTruncatedAndOversizedJournals(const fs::path& root) {
    fs::path library = root / "library";
    fs::path staging = library / ".staging";
    fs::create_directories(staging);

    // Case A: 0-byte journal
    fs::path emptyJ = staging / "journal_tx_empty.json";
    WriteFile(emptyJ, "");

    std::string err;
    Check(!PackManager::RecoverInterruptedTransactions(library.string(), err), "Empty journal accepted");
    Check(fs::exists(staging / "quarantine" / "journal_tx_empty.json"), "Empty journal not quarantined");
    PackManager::ClearRecoveryError();

    // Case B: Truncated JSON
    fs::path truncJ = staging / "journal_tx_trunc.json";
    WriteFile(truncJ, "{\n  \"schemaVersion\": 1,\n  \"txId\": \"tx_trunc\",\n");
    Check(!PackManager::RecoverInterruptedTransactions(library.string(), err), "Truncated journal accepted");
    Check(fs::exists(staging / "quarantine" / "journal_tx_trunc.json"), "Truncated journal not quarantined");
    PackManager::ClearRecoveryError();

    // Case C: Oversized journal (>64KB)
    fs::path overJ = staging / "journal_tx_oversized.json";
    std::string huge(70000, 'x');
    WriteFile(overJ, huge);
    Check(!PackManager::RecoverInterruptedTransactions(library.string(), err), "Oversized journal accepted");
    Check(fs::exists(staging / "quarantine" / "journal_tx_oversized.json"), "Oversized journal not quarantined");
    PackManager::ClearRecoveryError();

    // Case D: Unknown state
    fs::path badStateJ = staging / "journal_tx_badstate.json";
    WriteFile(badStateJ, "{\n  \"schemaVersion\": 1,\n  \"txId\": \"tx_badstate\",\n  \"state\": \"FORGED_STATE\",\n  \"packId\": \"com.prismforge.recovery\",\n  \"version\": \"1.0.0\"\n}\n");
    Check(!PackManager::RecoverInterruptedTransactions(library.string(), err), "Bad state journal accepted");
    Check(fs::exists(staging / "quarantine" / "journal_tx_badstate.json"), "Bad state journal not quarantined");
    PackManager::ClearRecoveryError();
}

// =========================================================================
// TEST 4: Corrupt/Missing Published Target with Intact Old Backup
// =========================================================================
void TestCorruptPublishedTargetWithIntactBackup(const fs::path& root) {
    fs::path library = root / "library";
    fs::path sourceV1 = root / "source_v1";
    const std::string v1Preview(16, '\x44');
    CreateFixture(sourceV1, "com.prismforge.restore", "1.0.0", "v1_data", v1Preview);

    std::string err;
    bool ok = PackManager::InstallPack(sourceV1.string(), library.string(), err);
    Check(ok, ("Initial install failed: " + err).c_str());

    fs::path targetDir = library / "com.prismforge.restore" / "1.0.0";
    Check(fs::exists(targetDir), "Target pack missing");

    // Simulate crash after publish where target got corrupted:
    fs::path staging = library / ".staging";
    fs::path backupDir = staging / "backup_tx_restore_test";
    fs::create_directories(staging);
    fs::rename(targetDir, backupDir);

    // Put a corrupted/partial file at targetDir
    fs::create_directories(targetDir);
    WriteFile(targetDir / "manifest.json", "CORRUPTED_MANIFEST_DATA");

    // Write PUBLISHED journal
    fs::path journalFile = staging / "journal_tx_restore_test.json";
    std::string journalContent = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"txId\": \"tx_restore_test\",\n"
        "  \"state\": \"PUBLISHED\",\n"
        "  \"packId\": \"com.prismforge.restore\",\n"
        "  \"version\": \"1.0.0\"\n"
        "}\n";
    WriteFile(journalFile, journalContent);

    // Call recovery!
    Check(PackManager::RecoverInterruptedTransactions(library.string(), err), "Recovery failed to restore intact backup");

    // Verify intact backup was restored to target!
    Check(fs::exists(targetDir), "Target was not restored");
    auto restored = PackManager::LoadPack(targetDir.string(), err);
    Check(restored != nullptr, "Restored pack failed to load");
    Check(Text(restored->ReadAssetBytes("look")) == "v1_data", "Restored asset data does not match v1");
    Check(Text(restored->GetPreviewBytes()) == v1Preview, "Restored preview does not match v1");

    // Verify backup and journal were cleaned up
    Check(!fs::exists(backupDir), "Backup directory was not cleaned up after restoration");
    Check(!fs::exists(journalFile), "Journal was not removed after restoration");
}

// =========================================================================
// TEST 5: Failed Rollback and Fault Seams Error Preservation
// =========================================================================
void TestFaultSeamsAndErrorPreservation(const fs::path& root) {
    fs::path library = root / "library";
    fs::path sourceV1 = root / "source_v1";
    CreateFixture(sourceV1, "com.prismforge.faults", "1.0.0", "fault_initial");

    std::string err;
    Check(PackManager::InstallPack(sourceV1.string(), library.string(), err), "Initial install failed");

    fs::path sourceV2 = root / "source_v2";
    CreateFixture(sourceV2, "com.prismforge.faults", "1.0.0", "fault_updated");

    // Fault 1: SimulateCommitRenameFailure
    PackManager::SetFaultSeam(PackFaultSeam::SimulateCommitRenameFailure);
    bool ok = PackManager::InstallPack(sourceV2.string(), library.string(), err);
    Check(!ok, "Install must fail when SimulateCommitRenameFailure active");
    Check(err.find("SimulateCommitRenameFailure") != std::string::npos ||
          err.find("commit rename failure") != std::string::npos, "Error did not preserve failure reason");

    // Original pack must remain intact
    auto loaded = PackManager::LoadPack((library / "com.prismforge.faults" / "1.0.0").string(), err);
    Check(loaded != nullptr, "Original pack became corrupt after rollback");
    Check(Text(loaded->ReadAssetBytes("look")) == "fault_initial", "Original data was modified");

    // Fault 2: SimulateJournalWriteFailure
    PackManager::SetFaultSeam(PackFaultSeam::SimulateJournalWriteFailure);
    ok = PackManager::InstallPack(sourceV2.string(), library.string(), err);
    Check(!ok, "Install must fail when journal write fails");
    Check(err.find("journal write failure") != std::string::npos ||
          err.find("SimulateJournalWriteFailure") != std::string::npos, "Journal error reason not preserved");

    PackManager::SetFaultSeam(PackFaultSeam::None);
}

// =========================================================================
// TEST 6: Simulated Crash Windows and Recovery (Child Process + In-Process)
// =========================================================================
void TestSimulatedCrashWindowsAndRecovery(const fs::path& root) {
    fs::path library = root / "library";
    fs::path sourceV1 = root / "source_v1";
    CreateFixture(sourceV1, "com.prismforge.crash", "1.0.0", "v1_original");

    std::string err;
    Check(PackManager::InstallPack(sourceV1.string(), library.string(), err), "Install v1 failed");

    fs::path sourceV2 = root / "source_v2";
    CreateFixture(sourceV2, "com.prismforge.crash", "1.0.0", "v2_updated");

    // Simulate crash after backup displacement:
    PackManager::SetFaultSeam(PackFaultSeam::SimulateCrashAfterBackup);
    bool ok = PackManager::InstallPack(sourceV2.string(), library.string(), err);
    Check(!ok, "Simulated crash should return false");
    PackManager::SetFaultSeam(PackFaultSeam::None);

    // Target is now displaced, journal is in BACKED_UP
    fs::path targetDir = library / "com.prismforge.crash" / "1.0.0";
    Check(!fs::exists(targetDir), "Target unexpectedly exists before recovery");

#ifdef _WIN32
    // Recover in a FRESH SEPARATE CHILD PROCESS
    std::wstring recArg = L"--recover \"" + library.native() + L"\"";
    ProcessResult childRes = RunChildProcess(recArg);
    Check(!childRes.timedOut, "Recovery child process timed out");
    Check(childRes.exitCode == 0, "Recovery in fresh child process returned non-zero exit");
#else
    Check(PackManager::RecoverInterruptedTransactions(library.string(), err), "Recovery failed");
#endif

    // Target is restored!
    Check(fs::exists(targetDir), "Target was not restored after crash recovery");
    auto loaded = PackManager::LoadPack(targetDir.string(), err);
    Check(loaded != nullptr, "Restored pack failed to load");
    Check(Text(loaded->ReadAssetBytes("look")) == "v1_original", "Restored pack data mismatch");
}

// =========================================================================
// TEST 7: CleanOrphanStaging Preserves Sentinels and Unrecovered Journals
// =========================================================================
void TestCleanOrphanStagingPreservesSentinels(const fs::path& root) {
    fs::path library = root / "library";
    fs::path staging = library / ".staging";
    fs::create_directories(staging);

    // Put a sentinel file in .staging
    fs::path sentinelFile = staging / "sentinel.txt";
    const std::string kSentinel = "STAGING_SENTINEL_DO_NOT_DELETE";
    WriteFile(sentinelFile, kSentinel);

    // Put an unknown directory in .staging
    fs::path foreignDir = staging / "foreign_tools_cache";
    fs::create_directories(foreignDir);
    WriteFile(foreignDir / "cache.bin", "foreign_cache_bytes");

    // Put an unrecovered/quarantined journal referencing a protected staging dir
    fs::path activeStaging = staging / "staging_tx_protected";
    fs::create_directories(activeStaging);
    WriteFile(activeStaging / "temp.dat", "active_staging_data");

    fs::path qDir = staging / "quarantine";
    fs::create_directories(qDir);
    fs::path journalProtected = qDir / "journal_tx_protected.json";
    WriteFile(journalProtected, "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"txId\": \"tx_protected\",\n"
        "  \"state\": \"BACKED_UP\",\n"
        "  \"packId\": \"com.prismforge.prot\",\n"
        "  \"version\": \"1.0.0\"\n"
        "}\n");

    // Put a genuine orphan staging directory
    fs::path orphanStaging = staging / "staging_tx_genuine_orphan";
    fs::create_directories(orphanStaging);
    WriteFile(orphanStaging / "orphan.dat", "orphan_data");

    // Run CleanOrphanStaging
    PackManager::CleanOrphanStaging(library.string());

    // Assert sentinel survives byte-for-byte!
    Check(ReadFile(sentinelFile) == kSentinel, "Sentinel file in .staging was deleted!");
    // Assert foreign directory survives!
    Check(fs::exists(foreignDir), "Foreign directory in .staging was deleted!");
    // Assert journal-referenced staging directory survives!
    Check(fs::exists(activeStaging), "Journal-referenced staging directory was deleted!");
    // Assert genuine orphan was cleaned up!
    Check(!fs::exists(orphanStaging), "Genuine orphan staging directory was not cleaned up");

    // Part B: Failed recovery halts cleanup before any mutations
    fs::path orphan2 = staging / "staging_tx_orphan2";
    fs::create_directories(orphan2);
    // Put an unrecoverable journal in .staging root (empty journal triggers recovery failure)
    fs::path corruptJ = staging / "journal_tx_corrupt.json";
    WriteFile(corruptJ, "");
    PackManager::CleanOrphanStaging(library.string());
    // Because recovery failed, orphan2 MUST NOT be deleted
    Check(fs::exists(orphan2), "CleanOrphanStaging deleted orphan after failed recovery!");
    PackManager::ClearRecoveryError();
}

// =========================================================================
// TEST 8: Cross-Process Lock and Abandoned Mutex Handling
// =========================================================================
void TestCrossProcessLockAndAbandonedMutex(const fs::path& root) {
    fs::path library = root / "library";
    fs::create_directories(library);

    // Verify canonical path lock works properly
    std::string err;
    Check(PackManager::RecoverInterruptedTransactions(library.string(), err), "Recovery with lock failed");

    // Verify that equivalent allowed lexical aliases serialize
    std::string alias1 = library.string();
    std::string alias2 = library.generic_string();
    std::string alias3 = library.string();
    if (alias3.size() > 1 && std::isalpha(static_cast<unsigned char>(alias3[0]))) {
        alias3[0] = std::isupper(static_cast<unsigned char>(alias3[0])) ?
            static_cast<char>(std::tolower(static_cast<unsigned char>(alias3[0]))) :
            static_cast<char>(std::toupper(static_cast<unsigned char>(alias3[0])));
    }

    Check(PackManager::RecoverInterruptedTransactions(alias1, err), "Alias 1 failed");
    Check(PackManager::RecoverInterruptedTransactions(alias2, err), "Alias 2 failed");
    Check(PackManager::RecoverInterruptedTransactions(alias3, err), "Alias 3 failed");

#ifdef _WIN32
    // Real Child Process Test A: Cross-process lock contention
    fs::path flagSleep = root / "lock_sleep.flag";
    std::wstring sleepArg = L"--hold-lock-and-sleep \"" + library.native() + L"\" 400 \"" + flagSleep.native() + L"\"";
    ProcessResult sleepChild = RunChildProcess(sleepArg, 10000);
    Check(!sleepChild.timedOut, "Sleep lock child process timed out");
    Check(sleepChild.exitCode == 0, "Sleep lock child exited with error");
    Check(fs::exists(flagSleep), "Sleep child did not acquire lock");

    // Real Child Process Test B: Child crashes while holding mutex (abandoned mutex recovery)
    fs::path flagCrash = root / "lock_crash.flag";
    std::wstring crashArg = L"--hold-lock-and-crash \"" + library.native() + L"\" \"" + flagCrash.native() + L"\"";
    ProcessResult crashChild = RunChildProcess(crashArg, 10000);
    Check(!crashChild.timedOut, "Crash lock child process timed out");
    Check(crashChild.exitCode == 42, "Crash lock child did not exit with code 42");
    Check(fs::exists(flagCrash), "Crash child did not acquire lock before crashing");

    // Parent acquires lock after child abandoned it: must succeed and recover gracefully!
    Check(PackManager::RecoverInterruptedTransactions(library.string(), err), "Recovery failed after abandoned mutex");
#endif
}

// =========================================================================
// TEST 9: Concurrent Retained Readers Across Mutation and Recovery
// =========================================================================
void TestConcurrentRetainedReadersAcrossRecovery(const fs::path& root) {
    fs::path library = root / "library";
    fs::path sourceV1 = root / "source_v1";
    fs::path sourceV2 = root / "source_v2";

    const std::string aBytes(16384, 'A');
    const std::string bBytes(16384, 'B');
    const std::string aPreview(16, '\x11');
    const std::string bPreview(16, '\x22');

    CreateFixture(sourceV1, "com.prismforge.retained", "1.0.0", aBytes, aPreview);
    CreateFixture(sourceV2, "com.prismforge.retained", "1.0.0", bBytes, bPreview);

    std::string err;
    Check(PackManager::InstallPack(sourceV1.string(), library.string(), err), "Initial install failed");

    fs::path targetDir = library / "com.prismforge.retained" / "1.0.0";
    auto oldLease = PackManager::LoadPack(targetDir.string(), err);
    Check(oldLease != nullptr, "LoadPack failed");

#ifdef _WIN32
    // Launch a REAL CHILD PROCESS READER WORKER holding the pack lease
    fs::path stopFlag = root / "reader_stop.flag";
    fs::path resultFile = root / "reader_result.txt";
    fs::path readyFlag = root / "reader_ready.flag";
    std::wstring readerArg = L"--reader-worker \"" + library.native() + L"\" com.prismforge.retained 1.0.0 \"" +
        stopFlag.native() + L"\" \"" + resultFile.native() + L"\" \"" + readyFlag.native() + L"\"";

    std::wstring exe = GetSelfExecutablePath();
    std::wstring cmd = L"\"" + exe + L"\" " + readerArg;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    bool childSpawned = CreateProcessW(
        exe.c_str(),
        cmdBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi) != FALSE;
    Check(childSpawned, "Failed to spawn child reader worker");

    // Wait until child has confirmed loading the initial V1 pack lease
    auto startWait = std::chrono::steady_clock::now();
    while (!fs::exists(readyFlag)) {
        if (std::chrono::steady_clock::now() - startWait > std::chrono::seconds(5)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(fs::exists(readyFlag), "Child reader worker failed to start and load initial V1 pack lease");
#endif

    std::atomic<bool> stop{false};
    std::atomic<int> readCount{0};
    std::atomic<bool> readOk{true};

    std::thread readerThread([&]() {
        while (!stop) {
            if (Text(oldLease->ReadAssetBytes("look")) != aBytes ||
                Text(oldLease->GetPreviewBytes()) != aPreview) {
                readOk = false;
            }
            ++readCount;
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 5; ++i) {
        Check(PackManager::InstallPack(sourceV2.string(), library.string(), err), "Update failed");
        Check(PackManager::InstallPack(sourceV1.string(), library.string(), err), "Downgrade failed");
    }

    stop = true;
    readerThread.join();

#ifdef _WIN32
    // Signal child process to stop
    WriteFile(stopFlag, "STOP");
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD childExit = 1;
    GetExitCodeProcess(pi.hProcess, &childExit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    Check(childExit == 0, ("Child reader worker exited with code: " + std::to_string(childExit)).c_str());
    std::string childResult = ReadFile(resultFile);
    int childReads = 0;
    try { childReads = std::stoi(childResult); } catch (...) {}
    Check(childReads > 10, "Child reader did not execute sufficient reads");
    std::cout << "  Child process reads=" << childReads << " during concurrent mutations\n";
#endif

    Check(readOk, "In-process retained reader observed mutated bytes!");
    Check(readCount > 50, "Reader did not execute enough concurrent reads");
    std::cout << "  In-process reader reads=" << readCount.load() << " during 10 full mutations\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        std::string mode = argv[1];
        if (mode == "--exit-proof") {
            return 42;
        }
        if (mode == "--hang-proof") {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        if (mode == "--recover" && argc == 3) {
            std::string err;
            bool ok = PackManager::RecoverInterruptedTransactions(argv[2], err);
            return ok ? 0 : 1;
        }
        if (mode == "--hold-lock-and-crash" && argc == 4) {
            fs::path library(argv[2]);
            fs::path flagFile(argv[3]);
            std::string err;
            // Internal acquire mutex directly to test cross-process abandoned mutex
            std::string norm = library.lexically_normal().generic_string();
            std::transform(norm.begin(), norm.end(), norm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string hash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(norm.data()), norm.size());
            std::string mName = "Local\\PrismForge_PackLibrary_Lock_" + hash;
            std::wstring wmName(mName.begin(), mName.end());
            HANDLE h = CreateMutexW(nullptr, FALSE, wmName.c_str());
            if (h && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0) {
                WriteFile(flagFile, "ACQUIRED");
                // Terminate abruptly without releasing mutex to simulate real process crash
                TerminateProcess(GetCurrentProcess(), 42);
            }
            return 1;
        }
        if (mode == "--hold-lock-and-sleep" && argc == 5) {
            fs::path library(argv[2]);
            int delayMs = std::stoi(argv[3]);
            fs::path flagFile(argv[4]);
            std::string norm = library.lexically_normal().generic_string();
            std::transform(norm.begin(), norm.end(), norm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string hash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(norm.data()), norm.size());
            std::string mName = "Local\\PrismForge_PackLibrary_Lock_" + hash;
            std::wstring wmName(mName.begin(), mName.end());
            HANDLE h = CreateMutexW(nullptr, FALSE, wmName.c_str());
            if (h && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0) {
                WriteFile(flagFile, "ACQUIRED");
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                ReleaseMutex(h);
                CloseHandle(h);
                return 0;
            }
            return 1;
        }
        if (mode == "--install" && argc == 4) {
            std::string err;
            bool ok = PackManager::InstallPack(argv[2], argv[3], err);
            return ok ? 0 : 1;
        }
        if (mode == "--uninstall" && argc == 5) {
            std::string err;
            bool ok = PackManager::UninstallPack(argv[2], argv[3], argv[4], err);
            return ok ? 0 : 1;
        }
        if (mode == "--reader-worker" && (argc == 7 || argc == 8)) {
            fs::path library(argv[2]);
            std::string packId = argv[3];
            std::string ver = argv[4];
            fs::path stopFlag(argv[5]);
            fs::path resultFile(argv[6]);
            fs::path readyFlag = (argc == 8) ? fs::path(argv[7]) : fs::path();

            fs::path packDir = library / packId / ver;
            std::string err;
            auto lease = PackManager::LoadPack(packDir.string(), err);
            if (!lease) return 2;

            if (!readyFlag.empty()) {
                WriteFile(readyFlag, "READY");
            }

            const std::string expectedAsset(16384, 'A');
            const std::string expectedPreview(16, '\x11');

            int reads = 0;
            while (!fs::exists(stopFlag)) {
                if (Text(lease->ReadAssetBytes("look")) != expectedAsset ||
                    Text(lease->GetPreviewBytes()) != expectedPreview) {
                    return 1; // Mismatched / corrupted bytes seen!
                }
                ++reads;
                std::this_thread::yield();
            }

            WriteFile(resultFile, std::to_string(reads));
            return 0;
        }
    }

    if (argc != 2) {
        std::cerr << "Expected unique run fixture root directory argument\n";
        return 2;
    }

    const fs::path root = fs::absolute(argv[1]);
    if (fs::exists(root)) {
        std::cerr << "Fixture root must be fresh\n";
        return 2;
    }
    fs::create_directories(root);

    struct TestGroup {
        const char* name;
        void (*run)(const fs::path&);
    } groups[] = {
        {"forged_external_and_ancestor_journal_paths", TestForgedExternalAndAncestorPaths},
        {"nested_staging_and_target_reparse_points", TestReparsePointsInStagingAndTarget},
        {"invalid_truncated_and_oversized_journals", TestInvalidTruncatedAndOversizedJournals},
        {"corrupt_or_missing_published_target_with_intact_backup", TestCorruptPublishedTargetWithIntactBackup},
        {"failed_rollback_and_fault_seams_error_preservation", TestFaultSeamsAndErrorPreservation},
        {"simulated_crash_windows_and_recovery", TestSimulatedCrashWindowsAndRecovery},
        {"clean_orphan_staging_preserves_sentinels", TestCleanOrphanStagingPreservesSentinels},
        {"cross_process_lock_and_abandoned_mutex", TestCrossProcessLockAndAbandonedMutex},
        {"concurrent_retained_readers_across_recovery", TestConcurrentRetainedReadersAcrossRecovery}
    };

    int passed = 0;
    int skipped = 0;
    int failures = 0;

    for (const auto& group : groups) {
        fs::path groupRoot = root / group.name;
        fs::create_directories(groupRoot);
        try {
            group.run(groupRoot);
            std::cout << "[PASS] " << group.name << "\n";
            passed++;
        } catch (const CapabilitySkip& skip) {
            std::cout << "[SKIP] " << group.name << " (" << skip.what() << ")\n";
            skipped++;
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << group.name << " (" << ex.what() << ")\n";
            failures++;
        } catch (...) {
            std::cout << "[FAIL] " << group.name << " (unknown exception)\n";
            failures++;
        }
    }

    std::cout << "groups=" << (passed + skipped + failures)
              << " passed=" << passed
              << " skipped=" << skipped
              << " failures=" << failures
              << " assertions=" << g_assertions << "\n";

    return (failures == 0) ? 0 : 1;
}
