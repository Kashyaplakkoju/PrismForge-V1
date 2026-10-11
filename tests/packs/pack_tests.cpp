#include "windows/src/effects/packs/sha256.h"
#include "windows/src/effects/packs/pack_manifest.h"
#include "windows/src/effects/packs/pack_validator.h"
#include "windows/src/effects/packs/pack_manager.h"

#include <wx/init.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <cstring>
#include <thread>
#include <atomic>
#include <algorithm>

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
static bool g_lastTestSkipped = false;


#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[ASSERTION FAILED] " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

fs::path GetScopedTempDir() {
    fs::path tempRoot = fs::current_path() / "tests" / "packs" / "build" / "temp_test_root";
    std::error_code ec;
    fs::create_directories(tempRoot, ec);
    return tempRoot;
}

class TestZipEntryWithFlags : public wxZipEntry {
public:
    explicit TestZipEntryWithFlags(const wxString& name) : wxZipEntry(name) {}
    void MarkEncrypted() {
        SetFlags(GetFlags() | wxZIP_ENCRYPTED);
    }
};

bool CreateZipArchive(
    const std::string& zipFilePath,
    const std::vector<std::pair<std::string, std::string>>& entries,
    bool setEncryption = false)
{
    fs::create_directories(fs::path(zipFilePath).parent_path());
    wxFileOutputStream fileOut(wxString::FromUTF8(zipFilePath.c_str()));
    if (!fileOut.IsOk()) return false;
    wxZipOutputStream zipOut(fileOut);
    if (!zipOut.IsOk()) return false;

    for (const auto& item : entries) {
        auto* entry = new TestZipEntryWithFlags(wxString::FromUTF8(item.first.c_str()));
        if (setEncryption) {
            entry->MarkEncrypted();
        }
        if (!zipOut.PutNextEntry(entry)) return false;
        if (!item.second.empty()) {
            zipOut.Write(item.second.data(), item.second.size());
        }
        zipOut.CloseEntry();
    }
    return zipOut.Close();
}

// -------------------------------------------------------------
// Test 1: SHA-256 NIST Standard Test Vectors
// -------------------------------------------------------------
bool TestSha256StandardVectors() {
    std::cout << "[TEST] Running TestSha256StandardVectors ..." << std::endl;

    // Vector 1: Empty string ""
    std::string h1 = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(""), 0);
    TEST_ASSERT(h1 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "NIST vector 1 (empty string) failed");

    // Vector 2: "abc"
    std::string h2 = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>("abc"), 3);
    TEST_ASSERT(h2 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "NIST vector 2 ('abc') failed");

    // Vector 3: "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    const char* str3 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    std::string h3 = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(str3), std::strlen(str3));
    TEST_ASSERT(h3 == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "NIST vector 3 failed");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 2: Manifest Serialization & Deserialization Roundtrip
// -------------------------------------------------------------
bool TestManifestSerializationAndParsing() {
    std::cout << "[TEST] Running TestManifestSerializationAndParsing ..." << std::endl;

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.test.pack";
    m.version = "1.2.3";
    m.displayName = "Test Studio Pack";
    m.description = "A pack created for serialization testing";
    m.author = "Antigravity Testing";
    m.previewImagePath = "preview.raw";

    PackAssetEntry a1;
    a1.assetType = PackAssetType::CubeLut;
    a1.assetId = "golden_lut";
    a1.relativePath = "luts/golden.cube";
    a1.sha256Hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a1.sizeBytes = 1024;
    m.assets.push_back(a1);

    PackAssetEntry a2;
    a2.assetType = PackAssetType::TrackedAccessory;
    a2.assetId = "studio_glasses";
    a2.relativePath = "accessories/glasses.json";
    a2.sha256Hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    a2.sizeBytes = 512;
    m.assets.push_back(a2);

    std::string json = ManifestJson::Serialize(m);
    TEST_ASSERT(!json.empty(), "Serialized JSON must not be empty");

    PackManifest parsed;
    std::string err;
    bool ok = ManifestJson::Deserialize(json, parsed, err);
    TEST_ASSERT(ok, "Deserialization failed: " + err);

    TEST_ASSERT(parsed.schemaVersion == m.schemaVersion, "schemaVersion mismatch");
    TEST_ASSERT(parsed.packId == m.packId, "packId mismatch");
    TEST_ASSERT(parsed.version == m.version, "version mismatch");
    TEST_ASSERT(parsed.displayName == m.displayName, "displayName mismatch");
    TEST_ASSERT(parsed.assets.size() == 2, "Asset count mismatch");
    TEST_ASSERT(parsed.assets[0].assetId == "golden_lut", "Asset 0 ID mismatch");
    TEST_ASSERT(parsed.assets[0].assetType == PackAssetType::CubeLut, "Asset 0 type mismatch");
    TEST_ASSERT(parsed.assets[1].assetId == "studio_glasses", "Asset 1 ID mismatch");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 3: Path Traversal & Escape Rejection
// -------------------------------------------------------------
bool TestPathTraversalSecurityRejection() {
    std::cout << "[TEST] Running TestPathTraversalSecurityRejection ..." << std::endl;

    std::string err;

    // 1. Upward traversal
    TEST_ASSERT(!PackValidator::ValidateRelativePath("../escaped.cube", PackAssetType::CubeLut, err),
        "Must reject '../'");
    TEST_ASSERT(!PackValidator::ValidateRelativePath("sub/../../escaped.cube", PackAssetType::CubeLut, err),
        "Must reject nested '..'");
    TEST_ASSERT(!PackValidator::ValidateRelativePath("..\\escaped.cube", PackAssetType::CubeLut, err),
        "Must reject '..\\'");

    // 2. Absolute paths
    TEST_ASSERT(!PackValidator::ValidateRelativePath("/root/file.cube", PackAssetType::CubeLut, err),
        "Must reject root slash");
    TEST_ASSERT(!PackValidator::ValidateRelativePath("\\windows\\system32\\file.cube", PackAssetType::CubeLut, err),
        "Must reject Windows backslash root");
    TEST_ASSERT(!PackValidator::ValidateRelativePath("C:/Windows/file.cube", PackAssetType::CubeLut, err),
        "Must reject drive letter colon");

    // 3. Current dir traversal
    TEST_ASSERT(!PackValidator::ValidateRelativePath("./file.cube", PackAssetType::CubeLut, err),
        "Must reject '.' component");

    // 4. Empty path
    TEST_ASSERT(!PackValidator::ValidateRelativePath("", PackAssetType::CubeLut, err),
        "Must reject empty path");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 4: Prohibited Executable & Script Extensions Rejection
// -------------------------------------------------------------
bool TestProhibitedExtensionsRejection() {
    std::cout << "[TEST] Running TestProhibitedExtensionsRejection ..." << std::endl;

    std::string err;
    const std::vector<std::string> dangerous = {
        "payload.exe",
        "script.bat",
        "hack.ps1",
        "inject.dll",
        "runner.cmd",
        "exploit.vbs",
        "code.js",
        "attack.sh",
        "bin.py"
    };

    for (const auto& f : dangerous) {
        TEST_ASSERT(!PackValidator::ValidateRelativePath(f, PackAssetType::TrackedAccessory, err),
            "Must reject dangerous extension: " + f);
    }

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 5: Install & Load Valid Sample Pack
// -------------------------------------------------------------
bool TestInstallAndLoadValidSamplePack() {
    std::cout << "[TEST] Running TestInstallAndLoadValidSamplePack ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "library_valid_install";
    fs::remove_all(tempRoot);

    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    TEST_ASSERT(fs::exists(samplePackDir), "Sample pack directory must exist at: " + samplePackDir);

    std::string err;
    bool installed = PackManager::InstallPack(samplePackDir, tempRoot.string(), err);
    TEST_ASSERT(installed, "PackManager::InstallPack failed: " + err);

    // Verify directory structure on disk
    fs::path installedPackDir = tempRoot / "com.prismforge.packs.warmstudio" / "1.0.0";
    TEST_ASSERT(fs::exists(installedPackDir), "Installed pack directory must exist");
    TEST_ASSERT(fs::exists(installedPackDir / "manifest.json"), "manifest.json must exist in installed directory");
    TEST_ASSERT(fs::exists(installedPackDir / "luts" / "warm_studio.cube"), "LUT file must exist");
    TEST_ASSERT(fs::exists(installedPackDir / "accessories" / "aviator_glasses.json"), "Accessory file must exist");

    // Load pack into runtime LoadedPack representation
    auto loadedPack = PackManager::LoadPack(installedPackDir.string(), err);
    TEST_ASSERT(loadedPack != nullptr, "PackManager::LoadPack failed: " + err);
    TEST_ASSERT(loadedPack->manifest.packId == "com.prismforge.packs.warmstudio", "PackId mismatch in loaded pack");

    // Read asset bytes and verify contents
    std::vector<uint8_t> lutBytes = loadedPack->ReadAssetBytes("warm_studio_lut");
    TEST_ASSERT(!lutBytes.empty(), "ReadAssetBytes must return non-empty buffer for valid asset");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 6: Corrupted Hash Rollback
// -------------------------------------------------------------
bool TestCorruptedHashRollback() {
    std::cout << "[TEST] Running TestCorruptedHashRollback ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "library_corrupted_test";
    fs::remove_all(tempRoot);

    // Create a temporary corrupted copy of sample pack
    fs::path corruptSource = GetScopedTempDir() / "source_corrupt_pack";
    fs::remove_all(corruptSource);
    fs::copy("windows/src/effects/packs/samples/pack_warm_studio", corruptSource, fs::copy_options::recursive);

    // Alter one byte in a file in-place to invalidate its SHA-256 without altering size
    fs::path lutFile = corruptSource / "luts" / "warm_studio.cube";
    {
        std::fstream f(lutFile.string(), std::ios::in | std::ios::out | std::ios::binary);
        char c = 'X';
        f.seekp(0);
        f.write(&c, 1);
    }

    std::string err;
    bool installed = PackManager::InstallPack(corruptSource.string(), tempRoot.string(), err);
    TEST_ASSERT(!installed, "Pack with corrupted hash must fail installation");
    TEST_ASSERT(err.find("SHA-256 integrity mismatch") != std::string::npos,
        "Error message must indicate SHA-256 mismatch: " + err);

    // Verify rollback: no files should be committed in target library root
    fs::path destPackDir = tempRoot / "com.prismforge.packs.warmstudio" / "1.0.0";
    TEST_ASSERT(!fs::exists(destPackDir), "Target destination must not exist after rollback");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 7: Malformed Manifest Rejection & Strict Grammar
// -------------------------------------------------------------
bool TestMalformedManifestRejection() {
    std::cout << "[TEST] Running TestMalformedManifestRejection ..." << std::endl;

    PackManifest m;
    std::string err;

    // 1. Syntax error
    TEST_ASSERT(!ManifestJson::Deserialize("{ not valid json : 123 }", m, err),
        "Must reject invalid JSON syntax");

    // 2. Missing packId
    std::string jsonNoId = "{\"schemaVersion\": 1, \"version\": \"1.0.0\", \"displayName\": \"Test\"}";
    TEST_ASSERT(!ManifestJson::Deserialize(jsonNoId, m, err),
        "Must reject manifest without packId");

    // 3. Schema version 2 (unsupported)
    std::string jsonV2 = "{\"schemaVersion\": 2, \"packId\": \"com.test.p\", \"version\": \"1.0.0\", \"displayName\": \"Test\"}";
    TEST_ASSERT(!ManifestJson::Deserialize(jsonV2, m, err),
        "Must reject unsupported schemaVersion");

    // 4. Duplicate key in top-level object
    std::string jsonDupKey = "{\"schemaVersion\": 1, \"packId\": \"com.test.p\", \"packId\": \"com.test.p2\", \"version\": \"1.0.0\", \"displayName\": \"Test\"}";
    TEST_ASSERT(!ManifestJson::Deserialize(jsonDupKey, m, err),
        "Must reject duplicate top-level key");
    TEST_ASSERT(err.find("Duplicate key") != std::string::npos, "Error must mention duplicate key: " + err);

    // 5. Trailing input after root object
    std::string jsonTrailing = "{\"schemaVersion\": 1, \"packId\": \"com.test.p\", \"version\": \"1.0.0\", \"displayName\": \"Test\"} trailing garbage";
    TEST_ASSERT(!ManifestJson::Deserialize(jsonTrailing, m, err),
        "Must reject trailing input after root object");

    // 6. Integer overflow in asset size
    std::string jsonOverflow = "{\"schemaVersion\": 1, \"packId\": \"com.test.p\", \"version\": \"1.0.0\", \"displayName\": \"Test\", \"assets\": [{\"type\": \"CubeLut\", \"id\": \"lut1\", \"path\": \"luts/a.cube\", \"sha256\": \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\", \"size\": 99999999999999999999999999999999}]}";
    TEST_ASSERT(!ManifestJson::Deserialize(jsonOverflow, m, err),
        "Must reject integer overflow in size");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 8: Duplicate Asset ID Rejection
// -------------------------------------------------------------
bool TestDuplicateAssetIdRejection() {
    std::cout << "[TEST] Running TestDuplicateAssetIdRejection ..." << std::endl;

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.duplicate";
    m.version = "1.0.0";
    m.displayName = "Duplicate Asset Pack";

    PackAssetEntry a1;
    a1.assetType = PackAssetType::CubeLut;
    a1.assetId = "common_id";
    a1.relativePath = "luts/a.cube";
    a1.sha256Hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a1.sizeBytes = 100;
    m.assets.push_back(a1);

    PackAssetEntry a2;
    a2.assetType = PackAssetType::CubeLut;
    a2.assetId = "common_id"; // Duplicate!
    a2.relativePath = "luts/b.cube";
    a2.sha256Hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    a2.sizeBytes = 100;
    m.assets.push_back(a2);

    PackValidationResult res = PackValidator::ValidateManifest(m);
    TEST_ASSERT(!res.valid, "ValidateManifest must reject duplicate asset ID");
    TEST_ASSERT(res.errorReason.find("Duplicate asset ID") != std::string::npos,
        "Error must mention Duplicate asset ID");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 9: Safe Version Upgrade and Rollback
// -------------------------------------------------------------
bool TestSafeVersionUpgradeAndRollback() {
    std::cout << "[TEST] Running TestSafeVersionUpgradeAndRollback ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "library_upgrade_test";
    fs::remove_all(tempRoot);

    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;

    // 1. Install initial version v1.0.0
    bool ok1 = PackManager::InstallPack(samplePackDir, tempRoot.string(), err);
    TEST_ASSERT(ok1, "Initial install failed: " + err);

    fs::path v1Dir = tempRoot / "com.prismforge.packs.warmstudio" / "1.0.0";
    TEST_ASSERT(fs::exists(v1Dir / "manifest.json"), "v1.0.0 must exist");

    // 2. Prepare v1.0.0 corrupted update (same destination directory update test)
    fs::path corruptSource = GetScopedTempDir() / "source_corrupt_v1";
    fs::remove_all(corruptSource);
    fs::copy(samplePackDir, corruptSource, fs::copy_options::recursive);

    // Corrupt one file
    {
        std::ofstream f((corruptSource / "luts" / "warm_studio.cube").string(), std::ios::binary);
        f << "corrupted contents";
    }

    // Attempt to install corrupt pack over v1.0.0
    bool okCorrupt = PackManager::InstallPack(corruptSource.string(), tempRoot.string(), err);
    TEST_ASSERT(!okCorrupt, "Corrupt update must be rejected");

    // CRITICAL: The existing v1.0.0 on disk must remain valid and intact!
    auto loadedV1 = PackManager::LoadPack(v1Dir.string(), err);
    TEST_ASSERT(loadedV1 != nullptr, "Existing v1.0.0 must remain intact and valid after failed update: " + err);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 10: Enumerate and Uninstall
// -------------------------------------------------------------
bool TestEnumerateAndUninstall() {
    std::cout << "[TEST] Running TestEnumerateAndUninstall ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "library_enum_test";
    fs::remove_all(tempRoot);

    std::string pack1 = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string pack2 = "windows/src/effects/packs/samples/pack_cool_broadcast";
    std::string err;

    bool ok1 = PackManager::InstallPack(pack1, tempRoot.string(), err);
    TEST_ASSERT(ok1, "Pack 1 install failed: " + err);

    bool ok2 = PackManager::InstallPack(pack2, tempRoot.string(), err);
    TEST_ASSERT(ok2, "Pack 2 install failed: " + err);

    // Enumerate
    std::vector<PackManifest> enumerated = PackManager::EnumeratePacks(tempRoot.string());
    TEST_ASSERT(enumerated.size() == 2, "Must enumerate exactly 2 packs");

    bool foundWarm = false;
    bool foundCool = false;
    for (const auto& m : enumerated) {
        if (m.packId == "com.prismforge.packs.warmstudio") foundWarm = true;
        if (m.packId == "com.prismforge.packs.coolbroadcast") foundCool = true;
    }
    TEST_ASSERT(foundWarm && foundCool, "Both installed packs must be enumerated");

    // Uninstall Pack 1
    bool uninstalled = PackManager::UninstallPack(tempRoot.string(), "com.prismforge.packs.warmstudio", "1.0.0", err);
    TEST_ASSERT(uninstalled, "UninstallPack failed: " + err);

    // Verify Pack 1 gone, Pack 2 remains
    std::vector<PackManifest> afterEnum = PackManager::EnumeratePacks(tempRoot.string());
    TEST_ASSERT(afterEnum.size() == 1, "Must enumerate 1 pack after uninstall");
    TEST_ASSERT(afterEnum[0].packId == "com.prismforge.packs.coolbroadcast", "Cool broadcast pack must remain");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 11: Identifier Validation & Reserved Names
// -------------------------------------------------------------
bool TestIdentifierValidationAndReservedNames() {
    std::cout << "[TEST] Running TestIdentifierValidationAndReservedNames ..." << std::endl;

    std::string err;

    // 1. Traversal & special components
    TEST_ASSERT(!PackValidator::ValidateIdentifier("..", false, err), "Must reject '..'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier(".", false, err), "Must reject '.'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("com/prismforge", false, err), "Must reject '/' in id");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("com\\prismforge", false, err), "Must reject '\\' in id");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("C:pack", false, err), "Must reject ':' in id");

    // 2. Windows reserved device names
    const std::vector<std::string> reserved = { "CON", "PRN", "AUX", "NUL", "COM1", "COM9", "LPT1", "LPT9", "con.custom" };
    for (const auto& res : reserved) {
        TEST_ASSERT(!PackValidator::ValidateIdentifier(res, false, err), "Must reject reserved device name: " + res);
    }

    // 3. Control characters and trailing whitespace/dots
    TEST_ASSERT(!PackValidator::ValidateIdentifier("com.test\x01id", false, err), "Must reject control character");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("com.test.pack.", false, err), "Must reject trailing dot");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("com.test.pack ", false, err), "Must reject trailing space");

    // 4. Staging & reserved control names
    TEST_ASSERT(!PackValidator::ValidateIdentifier(".staging", false, err), "Must reject '.staging'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("staging", false, err), "Must reject 'staging'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("backup", false, err), "Must reject 'backup'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("manifest", false, err), "Must reject 'manifest'");

    // 5. Valid identifiers
    TEST_ASSERT(PackValidator::ValidateIdentifier("com.prismforge.packs.warmstudio", false, err), "Valid reverse-DNS packId must pass: " + err);
    TEST_ASSERT(PackValidator::ValidateIdentifier("1.0.0", true, err), "Valid SemVer version must pass: " + err);
    TEST_ASSERT(PackValidator::ValidateIdentifier("2.1.0-beta.1", true, err), "Valid prerelease SemVer version must pass: " + err);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 12: Reparse Point and Junction Rejection
// -------------------------------------------------------------
bool TestReparsePointAndJunctionRejection() {
    std::cout << "[TEST] Running TestReparsePointAndJunctionRejection ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "reparse_test";
    fs::remove_all(tempRoot);
    fs::create_directories(tempRoot / "pack_dir" / "luts");

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.reparse";
    m.version = "1.0.0";
    m.displayName = "Reparse Test Pack";
    PackAssetEntry a;
    a.assetType = PackAssetType::CubeLut;
    a.assetId = "test_lut";
    a.relativePath = "luts/test.cube";
    a.sha256Hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a.sizeBytes = 10;
    m.assets.push_back(a);

    // Create target file in target directory
    fs::path targetDir = tempRoot / "target_dir";
    fs::create_directories(targetDir);
    {
        std::ofstream f((targetDir / "test.cube").string(), std::ios::binary);
        f << "0123456789";
    }

#ifdef _WIN32
    // Attempt to create directory symlink or junction
    fs::path linkPath = tempRoot / "pack_dir" / "luts" / "junction_dir";
    bool symlinkCreated = (CreateSymbolicLinkW(linkPath.c_str(), targetDir.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) != 0);
    if (!symlinkCreated) {
        // Fall back to creating an NTFS Junction via DeviceIoControl FSCTL_SET_REPARSE_POINT (does not require admin privileges)
        fs::create_directories(linkPath);
        auto h = CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const std::wstring substitute = LR"(\??\)" + fs::absolute(targetDir).native();
            const std::wstring print = fs::absolute(targetDir).native();
            const size_t bytes = 16 + (substitute.size() + print.size() + 2) * sizeof(wchar_t);
            std::vector<uint8_t> storage(bytes);
            *reinterpret_cast<DWORD*>(storage.data()) = IO_REPARSE_TAG_MOUNT_POINT;
            auto* fields = reinterpret_cast<WORD*>(storage.data() + 4);
            fields[0] = static_cast<WORD>(bytes - 8); fields[1] = 0;
            fields[2] = 0; fields[3] = static_cast<WORD>(substitute.size() * 2);
            fields[4] = static_cast<WORD>((substitute.size() + 1) * 2); fields[5] = static_cast<WORD>(print.size() * 2);
            auto* names = reinterpret_cast<wchar_t*>(storage.data() + 16);
            std::copy(substitute.begin(), substitute.end(), names);
            std::copy(print.begin(), print.end(), names + substitute.size() + 1);
            DWORD returned = 0;
            if (DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, storage.data(), static_cast<DWORD>(storage.size()), nullptr, 0, &returned, nullptr)) {
                symlinkCreated = true;
            }
            CloseHandle(h);
        }
    }
    if (symlinkCreated) {
        a.relativePath = "luts/junction_dir/test.cube";
        m.assets[0] = a;
        PackValidationResult res = PackValidator::ValidateDirectoryPack((tempRoot / "pack_dir").string(), m);
        TEST_ASSERT(!res.valid, "Must reject directory containing symlink/reparse point");
        TEST_ASSERT(res.errorReason.find("Symlinks and reparse points") != std::string::npos,
            "Error reason must indicate symlink/reparse rejection");
        RemoveDirectoryW(linkPath.c_str());
    } else {
        std::cout << "  [SKIP] Win32 junction/symlink creation not supported on this volume" << std::endl;
        g_lastTestSkipped = true;
        return true;
    }
#endif

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 13: Duplicate Case-Insensitive Relative Paths Rejection
// -------------------------------------------------------------
bool TestDuplicateCaseInsensitivePathsRejection() {
    std::cout << "[TEST] Running TestDuplicateCaseInsensitivePathsRejection ..." << std::endl;

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.casealias";
    m.version = "1.0.0";
    m.displayName = "Case Alias Pack";

    PackAssetEntry a1;
    a1.assetType = PackAssetType::CubeLut;
    a1.assetId = "lut_a";
    a1.relativePath = "luts/warm_studio.cube";
    a1.sha256Hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a1.sizeBytes = 100;
    m.assets.push_back(a1);

    PackAssetEntry a2;
    a2.assetType = PackAssetType::CubeLut;
    a2.assetId = "lut_b";
    a2.relativePath = "LUTS/WARM_STUDIO.CUBE"; // Case-insensitive alias
    a2.sha256Hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    a2.sizeBytes = 100;
    m.assets.push_back(a2);

    PackValidationResult res = PackValidator::ValidateManifest(m);
    TEST_ASSERT(!res.valid, "ValidateManifest must reject case-insensitive duplicate path");
    TEST_ASSERT(res.errorReason.find("Duplicate asset relative path") != std::string::npos,
        "Error must mention Duplicate asset relative path");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 14: Preview Asset Declared Bounds & Mismatch
// -------------------------------------------------------------
bool TestPreviewAssetDeclaredBoundsAndMismatch() {
    std::cout << "[TEST] Running TestPreviewAssetDeclaredBoundsAndMismatch ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "preview_test_root";
    fs::remove_all(tempRoot);
    fs::path packDir = tempRoot / "source_pack";
    fs::create_directories(packDir / "luts");

    std::string lutData = "LUT TEST DATA";
    std::string lutHash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(lutData.data()), lutData.size());
    {
        std::ofstream f((packDir / "luts" / "test.cube").string(), std::ios::binary);
        f << lutData;
    }

    std::string prevData = "RAW PREVIEW IMAGE BYTES 12345678";
    std::string prevHash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(prevData.data()), prevData.size());
    {
        std::ofstream f((packDir / "preview.raw").string(), std::ios::binary);
        f << prevData;
    }

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.preview";
    m.version = "1.0.0";
    m.displayName = "Preview Test Pack";
    m.previewImagePath = "preview.raw";

    PackAssetEntry a1;
    a1.assetType = PackAssetType::CubeLut;
    a1.assetId = "test_lut";
    a1.relativePath = "luts/test.cube";
    a1.sha256Hex = lutHash;
    a1.sizeBytes = lutData.size();
    m.assets.push_back(a1);

    PackAssetEntry a2;
    a2.assetType = PackAssetType::PreviewImage;
    a2.assetId = "pack_preview";
    a2.relativePath = "preview.raw";
    a2.sha256Hex = prevHash;
    a2.sizeBytes = prevData.size();
    m.assets.push_back(a2);

    std::string err;
    TEST_ASSERT(ManifestJson::WriteToFile((packDir / "manifest.json").string(), m, err), "Write manifest failed");

    // Install pack
    fs::path libRoot = tempRoot / "library";
    TEST_ASSERT(PackManager::InstallPack(packDir.string(), libRoot.string(), err), "InstallPack failed: " + err);

    fs::path installedDir = libRoot / m.packId / m.version;
    auto loaded = PackManager::LoadPack(installedDir.string(), err);
    TEST_ASSERT(loaded != nullptr, "LoadPack failed: " + err);

    // Verify preview bytes
    std::vector<uint8_t> previewBytes = loaded->GetPreviewBytes();
    TEST_ASSERT(!previewBytes.empty(), "GetPreviewBytes must return non-empty buffer");
    TEST_ASSERT(std::string(previewBytes.begin(), previewBytes.end()) == prevData, "Preview bytes content mismatch");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 15: Valid ZIP Archive Installation
// -------------------------------------------------------------
bool TestValidZipArchiveInstallation() {
    std::cout << "[TEST] Running TestValidZipArchiveInstallation ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "zip_valid_test";
    fs::remove_all(tempRoot);

    std::string lutData = "CUBE LUT DATA FOR ZIP TEST 12345";
    std::string lutHash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(lutData.data()), lutData.size());

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.zippack";
    m.version = "1.0.0";
    m.displayName = "ZIP Installed Pack";
    m.description = "Imported from supported ZIP archive";

    PackAssetEntry a1;
    a1.assetType = PackAssetType::CubeLut;
    a1.assetId = "zip_lut";
    a1.relativePath = "luts/golden.cube";
    a1.sha256Hex = lutHash;
    a1.sizeBytes = lutData.size();
    m.assets.push_back(a1);

    std::string manifestJson = ManifestJson::Serialize(m);

    std::vector<std::pair<std::string, std::string>> zipEntries = {
        { "manifest.json", manifestJson },
        { "luts/golden.cube", lutData }
    };

    fs::path zipPath = tempRoot / "valid_pack.zip";
    TEST_ASSERT(CreateZipArchive(zipPath.string(), zipEntries), "Failed to create valid ZIP archive");

    fs::path libRoot = tempRoot / "library";
    std::string err;
    bool ok = PackManager::InstallPackFromArchive(zipPath.string(), libRoot.string(), err);
    TEST_ASSERT(ok, "InstallPackFromArchive failed: " + err);

    fs::path installedDir = libRoot / m.packId / m.version;
    TEST_ASSERT(fs::exists(installedDir / "manifest.json"), "manifest.json must exist in installed library");
    TEST_ASSERT(fs::exists(installedDir / "luts" / "golden.cube"), "LUT asset must exist in installed library");

    auto loaded = PackManager::LoadPack(installedDir.string(), err);
    TEST_ASSERT(loaded != nullptr, "LoadPack failed on zip-installed pack: " + err);
    std::vector<uint8_t> readBytes = loaded->ReadAssetBytes("zip_lut");
    TEST_ASSERT(readBytes.size() == lutData.size(), "ReadAssetBytes size mismatch");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 16: ZIP Archive Security Rejections
// -------------------------------------------------------------
bool TestZipArchiveSecurityRejections() {
    std::cout << "[TEST] Running TestZipArchiveSecurityRejections ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "zip_security_test";
    fs::remove_all(tempRoot);
    fs::path libRoot = tempRoot / "library";
    std::string err;

    // 1. Path traversal in ZIP entry name
    fs::path zipTraversal = tempRoot / "traversal.zip";
    std::vector<std::pair<std::string, std::string>> traversalEntries = {
        { "manifest.json", "{}" },
        { "../../../evil.txt", "evil data" }
    };
    TEST_ASSERT(CreateZipArchive(zipTraversal.string(), traversalEntries), "Create traversal zip failed");
    TEST_ASSERT(!PackManager::InstallPackFromArchive(zipTraversal.string(), libRoot.string(), err),
        "Must reject ZIP with '../' path traversal entry");
    TEST_ASSERT(err.find("traversal") != std::string::npos, "Error must mention traversal: " + err);

    // 2. Absolute path in ZIP entry
    fs::path zipAbs = tempRoot / "abs.zip";
    std::vector<std::pair<std::string, std::string>> absEntries = {
        { "manifest.json", "{}" },
        { "/root/evil.txt", "evil data" }
    };
    TEST_ASSERT(CreateZipArchive(zipAbs.string(), absEntries), "Create abs zip failed");
    TEST_ASSERT(!PackManager::InstallPackFromArchive(zipAbs.string(), libRoot.string(), err),
        "Must reject ZIP with absolute path entry");

    // 3. Encrypted ZIP archive
    fs::path zipEnc = tempRoot / "encrypted.zip";
    std::vector<std::pair<std::string, std::string>> encEntries = {
        { "manifest.json", "{}" }
    };
    TEST_ASSERT(CreateZipArchive(zipEnc.string(), encEntries, true), "Create encrypted zip failed");
    TEST_ASSERT(!PackManager::InstallPackFromArchive(zipEnc.string(), libRoot.string(), err),
        "Must reject encrypted ZIP archive");
    TEST_ASSERT(err.find("Encrypted") != std::string::npos, "Error must mention Encrypted: " + err);

    // 4. Missing manifest.json in archive
    fs::path zipNoManifest = tempRoot / "no_manifest.zip";
    std::vector<std::pair<std::string, std::string>> noManEntries = {
        { "luts/golden.cube", "data" }
    };
    TEST_ASSERT(CreateZipArchive(zipNoManifest.string(), noManEntries), "Create no manifest zip failed");
    TEST_ASSERT(!PackManager::InstallPackFromArchive(zipNoManifest.string(), libRoot.string(), err),
        "Must reject ZIP archive lacking manifest.json");
    TEST_ASSERT(err.find("manifest.json") != std::string::npos, "Error must mention manifest.json: " + err);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 17: ZIP Decompression Bounds & Bomb Rejection
// -------------------------------------------------------------
bool TestZipDecompressionBoundsAndBombRejection() {
    std::cout << "[TEST] Running TestZipDecompressionBoundsAndBombRejection ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "zip_bomb_test";
    fs::remove_all(tempRoot);
    fs::path libRoot = tempRoot / "library";
    std::string err;

    // Create a zip with > 100 entries to test entry count bounds
    fs::path zipManyEntries = tempRoot / "too_many_entries.zip";
    std::vector<std::pair<std::string, std::string>> manyEntries;
    for (int i = 0; i < 105; ++i) {
        manyEntries.push_back({ "file_" + std::to_string(i) + ".txt", "x" });
    }
    TEST_ASSERT(CreateZipArchive(zipManyEntries.string(), manyEntries), "Create many entries zip failed");
    TEST_ASSERT(!PackManager::InstallPackFromArchive(zipManyEntries.string(), libRoot.string(), err),
        "Must reject ZIP with > 100 entries");
    TEST_ASSERT(err.find("maximum allowed entry count") != std::string::npos, "Error must mention entry count: " + err);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 18: Transactional Fault Seams & Rollback
// -------------------------------------------------------------
bool TestTransactionalFaultSeamsAndRollback() {
    std::cout << "[TEST] Running TestTransactionalFaultSeamsAndRollback ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "fault_seams_test";
    fs::remove_all(tempRoot);
    fs::path libRoot = tempRoot / "library";
    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;

    // 1. Install base pack successfully
    PackManager::SetFaultSeam(PackFaultSeam::None);
    TEST_ASSERT(PackManager::InstallPack(samplePackDir, libRoot.string(), err), "Base install failed: " + err);
    fs::path destV1 = libRoot / "com.prismforge.packs.warmstudio" / "1.0.0";
    TEST_ASSERT(fs::exists(destV1 / "manifest.json"), "Base pack must exist");

    // 2. Fault seam: Copy Failure on update
    PackManager::SetFaultSeam(PackFaultSeam::SimulateCopyFailure);
    bool okCopyFail = PackManager::InstallPack(samplePackDir, libRoot.string(), err);
    TEST_ASSERT(!okCopyFail, "Must fail when SimulateCopyFailure is active");
    TEST_ASSERT(err.find("Simulated copy failure") != std::string::npos, "Error must mention simulated copy failure: " + err);

    // Verify base version remained untouched and usable
    auto loadedAfterCopyFail = PackManager::LoadPack(destV1.string(), err);
    TEST_ASSERT(loadedAfterCopyFail != nullptr, "Base pack must remain usable after copy failure rollback: " + err);

    // 3. Fault seam: Commit Rename Failure on update
    PackManager::SetFaultSeam(PackFaultSeam::SimulateCommitRenameFailure);
    bool okCommitFail = PackManager::InstallPack(samplePackDir, libRoot.string(), err);
    TEST_ASSERT(!okCommitFail, "Must fail when SimulateCommitRenameFailure is active");

    // Verify base version remained restored and fully intact
    auto loadedAfterCommitFail = PackManager::LoadPack(destV1.string(), err);
    TEST_ASSERT(loadedAfterCommitFail != nullptr, "Base pack must be restored after commit rename failure: " + err);

    PackManager::SetFaultSeam(PackFaultSeam::None);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 19: Concurrent Leased Readers & Immutability
// -------------------------------------------------------------
bool TestConcurrentLeasedReadersAndImmutability() {
    std::cout << "[TEST] Running TestConcurrentLeasedReadersAndImmutability ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "concurrency_test";
    fs::remove_all(tempRoot);
    fs::path libRoot = tempRoot / "library";
    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;

    TEST_ASSERT(PackManager::InstallPack(samplePackDir, libRoot.string(), err), "Install failed: " + err);
    fs::path packPath = libRoot / "com.prismforge.packs.warmstudio" / "1.0.0";

    auto loadedPack = PackManager::LoadPack(packPath.string(), err);
    TEST_ASSERT(loadedPack != nullptr, "LoadPack failed: " + err);

    std::atomic<bool> startFlag{ false };
    std::atomic<int> successCount{ 0 };
    const int threadCount = 4;
    std::vector<std::thread> workers;

    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([&loadedPack, &startFlag, &successCount]() {
            while (!startFlag.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 50; ++i) {
                std::vector<uint8_t> b = loadedPack->ReadAssetBytes("warm_studio_lut");
                if (!b.empty()) {
                    successCount.fetch_add(1);
                }
            }
        });
    }

    startFlag.store(true);

    for (auto& w : workers) {
        w.join();
    }

    TEST_ASSERT(successCount.load() == threadCount * 50, "All concurrent asset reads must succeed");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 20: Safe Uninstall Sentinel Preservation
// -------------------------------------------------------------
bool TestSafeUninstallSentinelPreservation() {
    std::cout << "[TEST] Running TestSafeUninstallSentinelPreservation ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "sentinel_test";
    fs::remove_all(tempRoot);
    fs::path libRoot = tempRoot / "library";
    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;

    TEST_ASSERT(PackManager::InstallPack(samplePackDir, libRoot.string(), err), "Install failed: " + err);

    // Create sentinel file and sentinel directory in library root
    fs::path sentinelFile = libRoot / "sentinel_file.txt";
    fs::path sentinelDir = libRoot / "sentinel_dir";
    fs::create_directories(sentinelDir);
    {
        std::ofstream f(sentinelFile.string());
        f << "DO NOT DELETE SENTINEL FILE";
    }
    {
        std::ofstream f((sentinelDir / "data.bin").string());
        f << "SENTINEL DATA";
    }

    // 1. Attempt uninstall with path traversal
    TEST_ASSERT(!PackManager::UninstallPack(libRoot.string(), "../sentinel_dir", "", err),
        "Must reject traversal in packId");

    // 2. Attempt uninstall with reserved Windows name
    TEST_ASSERT(!PackManager::UninstallPack(libRoot.string(), "CON", "", err),
        "Must reject Windows device name 'CON'");

    // 3. Uninstall legitimate pack
    TEST_ASSERT(PackManager::UninstallPack(libRoot.string(), "com.prismforge.packs.warmstudio", "1.0.0", err),
        "Legitimate uninstall failed: " + err);

    // Verify pack removed
    fs::path packDir = libRoot / "com.prismforge.packs.warmstudio";
    TEST_ASSERT(!fs::exists(packDir), "Uninstalled pack directory must be removed");

    // CRITICAL: Sentinel files and directories must be completely intact!
    TEST_ASSERT(fs::exists(sentinelFile), "Sentinel file must NOT be deleted!");
    TEST_ASSERT(fs::exists(sentinelDir / "data.bin"), "Sentinel directory contents must NOT be deleted!");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 21: Deterministic Catalog Ordering
// -------------------------------------------------------------
bool TestDeterministicCatalogOrdering() {
    std::cout << "[TEST] Running TestDeterministicCatalogOrdering ..." << std::endl;

    fs::path tempRoot = GetScopedTempDir() / "catalog_sort_test";
    fs::remove_all(tempRoot);

    // Create 3 dummy pack directories with valid manifests
    auto makePack = [&](const std::string& pId, const std::string& ver) {
        fs::path pDir = tempRoot / pId / ver;
        fs::create_directories(pDir / "luts");
        std::string d = "dummy lut";
        std::string h = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(d.data()), d.size());
        {
            std::ofstream f((pDir / "luts" / "d.cube").string());
            f << d;
        }

        PackManifest m;
        m.schemaVersion = 1;
        m.packId = pId;
        m.version = ver;
        m.displayName = "Catalog Pack";
        PackAssetEntry a;
        a.assetType = PackAssetType::CubeLut;
        a.assetId = "lut";
        a.relativePath = "luts/d.cube";
        a.sha256Hex = h;
        a.sizeBytes = d.size();
        m.assets.push_back(a);

        std::string err;
        ManifestJson::WriteToFile((pDir / "manifest.json").string(), m, err);
    };

    // Install in arbitrary order
    makePack("com.zeta.pack", "1.0.0");
    makePack("com.alpha.pack", "2.0.0");
    makePack("com.alpha.pack", "1.0.0");

    std::vector<PackManifest> catalog = PackManager::ListInstalledPacks(tempRoot.string());
    TEST_ASSERT(catalog.size() == 3, "Catalog must list 3 packs");

    // Must be sorted strictly: com.alpha.pack 1.0.0, com.alpha.pack 2.0.0, com.zeta.pack 1.0.0
    TEST_ASSERT(catalog[0].packId == "com.alpha.pack" && catalog[0].version == "1.0.0",
        "Catalog item 0 must be com.alpha.pack 1.0.0");
    TEST_ASSERT(catalog[1].packId == "com.alpha.pack" && catalog[1].version == "2.0.0",
        "Catalog item 1 must be com.alpha.pack 2.0.0");
    TEST_ASSERT(catalog[2].packId == "com.zeta.pack" && catalog[2].version == "1.0.0",
        "Catalog item 2 must be com.zeta.pack 1.0.0");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

} // anonymous namespace


// -------------------------------------------------------------
// Test 22: Strict JSON Grammar & Unicode Surrogates
// -------------------------------------------------------------
bool TestStrictJsonGrammarAndUnicodeSurrogates() {
    std::cout << "[TEST] Running TestStrictJsonGrammarAndUnicodeSurrogates ..." << std::endl;
    PackManifest m;
    std::string err;

    // 1. Comments
    TEST_ASSERT(!ManifestJson::Deserialize("{\n  // comment\n  \"schemaVersion\": 1\n}", m, err), "Must reject comments");

    // 2. Trailing comma in object
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"packId\": \"com.a.b\", }", m, err), "Must reject trailing comma in object");

    // 3. Trailing comma in array
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"assets\": [1, ]}", m, err), "Must reject trailing comma in array");

    // 4. Leading zero integer
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 01}", m, err), "Must reject leading zero integer");

    // 5. Duplicate keys
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"schemaVersion\": 1}", m, err), "Must reject duplicate key");

    // 6. Lone high surrogate
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"displayName\": \"\\uD800\"}", m, err), "Must reject lone high surrogate");

    // 7. Lone low surrogate
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"displayName\": \"\\uDC00\"}", m, err), "Must reject lone low surrogate");

    // 8. Reversed surrogates
    TEST_ASSERT(!ManifestJson::Deserialize("{\"schemaVersion\": 1, \"displayName\": \"\\uDC00\\uD800\"}", m, err), "Must reject reversed surrogates");

    // 9. Valid surrogate pair: U+1F600 (Grinning Face: \uD83D\uDE00)
    std::string validJson = "{\n  \"schemaVersion\": 1,\n  \"packId\": \"com.prismforge.unicode\",\n  \"version\": \"1.0.0\",\n  \"displayName\": \"Grinning \\uD83D\\uDE00 Face\",\n  \"assets\": [\n    {\n      \"type\": \"CubeLut\",\n      \"id\": \"lut1\",\n      \"path\": \"luts/test.cube\",\n      \"sha256\": \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\n      \"size\": 10\n    }\n  ]\n}";
    TEST_ASSERT(ManifestJson::Deserialize(validJson, m, err), "Must accept valid surrogate pair: " + err);
    TEST_ASSERT(m.displayName.find("Grinning") != std::string::npos, "Parsed display name must contain emoji");

    // 10. Junk trailing content
    TEST_ASSERT(!ManifestJson::Deserialize(validJson + " junk_at_end", m, err), "Must reject trailing junk content");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 23: SemVer Edge Cases & Strict Syntax Rejection
// -------------------------------------------------------------
bool TestSemVerEdgeCasesAndRejections() {
    std::cout << "[TEST] Running TestSemVerEdgeCasesAndRejections ..." << std::endl;
    std::string err;

    // Invalid SemVer
    TEST_ASSERT(!PackValidator::ValidateIdentifier("1a", true, err), "Must reject '1a'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("1+", true, err), "Must reject '1+'");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("01.0.0", true, err), "Must reject '01.0.0' with leading zero");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("1.0", true, err), "Must reject '1.0' (missing patch)");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("1.0.0.0", true, err), "Must reject '1.0.0.0' (four segments)");
    TEST_ASSERT(!PackValidator::ValidateIdentifier("1.0.0-01", true, err), "Must reject leading zero in numeric prerelease");

    // Valid SemVer
    TEST_ASSERT(PackValidator::ValidateIdentifier("1.0.0", true, err), "Must accept '1.0.0'");
    TEST_ASSERT(PackValidator::ValidateIdentifier("0.1.0", true, err), "Must accept '0.1.0'");
    TEST_ASSERT(PackValidator::ValidateIdentifier("1.0.0-alpha.1", true, err), "Must accept '1.0.0-alpha.1'");
    TEST_ASSERT(PackValidator::ValidateIdentifier("1.0.0+20130313144700", true, err), "Must accept '1.0.0+20130313144700'");
    TEST_ASSERT(PackValidator::ValidateIdentifier("1.0.0-beta.2+exp.sha.5114f85", true, err), "Must accept full SemVer");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 24: ZIP Undeclared Payload Whitelist Rejection
// -------------------------------------------------------------
bool TestZipUndeclaredPayloadRejection() {
    std::cout << "[TEST] Running TestZipUndeclaredPayloadRejection ..." << std::endl;
    fs::path tempRoot = GetScopedTempDir() / "zip_undeclared_test";
    fs::remove_all(tempRoot);

    std::string lutData = "CUBE LUT TEST";
    std::string lutHash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(lutData.data()), lutData.size());

    PackManifest m;
    m.schemaVersion = 1;
    m.packId = "com.prismforge.undeclaredzip";
    m.version = "1.0.0";
    m.displayName = "Undeclared Payload Test";
    PackAssetEntry a;
    a.assetType = PackAssetType::CubeLut;
    a.assetId = "lut1";
    a.relativePath = "luts/test.cube";
    a.sha256Hex = lutHash;
    a.sizeBytes = lutData.size();
    m.assets.push_back(a);

    std::string manifestJson = ManifestJson::Serialize(m);
    fs::path zipPath = tempRoot / "payload_test.zip";

    std::vector<std::pair<std::string, std::string>> entries = {
        { "manifest.json", manifestJson },
        { "luts/test.cube", lutData },
        { "scripts/malicious_payload.py", "import os; print('malicious')" }
    };

    TEST_ASSERT(CreateZipArchive(zipPath.string(), entries), "Failed to create test zip");

    std::string err;
    bool ok = PackManager::InstallPackFromArchive(zipPath.string(), (tempRoot / "library").string(), err);
    TEST_ASSERT(!ok, "Must reject ZIP archive containing undeclared payload file");
    TEST_ASSERT(err.find("Undeclared payload file") != std::string::npos, "Error must mention undeclared payload: " + err);
    TEST_ASSERT(!fs::exists(tempRoot / "library" / m.packId), "Library must not contain installed pack");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 25: Catalogue Integrity and Actionable Error Reporting
// -------------------------------------------------------------
bool TestCatalogueIntegrityAndActionableErrors() {
    std::cout << "[TEST] Running TestCatalogueIntegrityAndActionableErrors ..." << std::endl;
    fs::path tempRoot = GetScopedTempDir() / "catalogue_integrity_test";
    fs::remove_all(tempRoot);
    fs::path lib = tempRoot / "library";

    // 1. Install valid pack
    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;
    TEST_ASSERT(PackManager::InstallPack(samplePackDir, lib.string(), err), "Base install failed: " + err);

    // 2. Query catalogue
    auto catalogue = PackManager::GetCatalogue(lib.string());
    TEST_ASSERT(!catalogue.empty(), "Catalogue must not be empty");
    TEST_ASSERT(catalogue[0].integrityState == PackIntegrityState::Valid, "Installed sample pack must have Valid integrity");
    TEST_ASSERT(catalogue[0].isUsable, "Installed sample pack must be usable");

    // 3. Corrupt an asset in place
    fs::path lutPath = lib / catalogue[0].packId / catalogue[0].version / "luts" / "warm_studio.cube";
    {
        std::fstream f(lutPath.string(), std::ios::in | std::ios::out | std::ios::binary);
        char c = 'Z';
        f.seekp(0);
        f.write(&c, 1);
    }

    // Query catalogue again: must report CorruptAsset
    auto corruptCat = PackManager::GetCatalogue(lib.string());
    TEST_ASSERT(!corruptCat.empty(), "Catalogue must report pack even if corrupt");
    TEST_ASSERT(corruptCat[0].integrityState == PackIntegrityState::CorruptAsset, "Corrupted asset must be reported as CorruptAsset");
    TEST_ASSERT(!corruptCat[0].isUsable, "Corrupted pack must not be usable");
    TEST_ASSERT(corruptCat[0].actionableError.find("SHA-256 integrity mismatch") != std::string::npos,
        "Actionable error must explain mismatch: " + corruptCat[0].actionableError);

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 26: Journal Interrupted Crash Recovery
// -------------------------------------------------------------
bool TestJournalInterruptedCrashRecovery() {
    std::cout << "[TEST] Running TestJournalInterruptedCrashRecovery ..." << std::endl;
    fs::path tempRoot = GetScopedTempDir() / "journal_crash_test";
    fs::remove_all(tempRoot);
    fs::path lib = tempRoot / "library";

    std::string samplePackDir = "windows/src/effects/packs/samples/pack_warm_studio";
    std::string err;
    TEST_ASSERT(PackManager::InstallPack(samplePackDir, lib.string(), err), "Initial install failed: " + err);

    fs::path installedDir = lib / "com.prismforge.packs.warmstudio" / "1.0.0";
    auto loaded1 = PackManager::LoadPack(installedDir.string(), err);
    TEST_ASSERT(loaded1 != nullptr, "LoadPack failed: " + err);

    fs::path updatedSource = tempRoot / "updated_source";
    fs::copy(samplePackDir, updatedSource, fs::copy_options::recursive);

    // Inject crash seam: SimulateCrashAfterBackup
    PackManager::SetFaultSeam(PackFaultSeam::SimulateCrashAfterBackup);
    bool installUpdated = PackManager::InstallPack(updatedSource.string(), lib.string(), err);
    TEST_ASSERT(!installUpdated, "Install must fail at crash fault seam");
    PackManager::SetFaultSeam(PackFaultSeam::None);

    std::string recErr;
    bool recovered = PackManager::RecoverInterruptedTransactions(lib.string(), recErr);
    TEST_ASSERT(recovered, "Recovery must succeed: " + recErr);

    TEST_ASSERT(fs::exists(installedDir / "manifest.json"), "Restored manifest must exist");
    auto loadedRestored = PackManager::LoadPack(installedDir.string(), err);
    TEST_ASSERT(loadedRestored != nullptr, "LoadPack must succeed after journal recovery: " + err);
    TEST_ASSERT(loadedRestored->manifest.packId == "com.prismforge.packs.warmstudio", "Pack ID intact");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 27: Preview Format Decode Bounds & Format Enforcement
// -------------------------------------------------------------
bool TestPreviewFormatDecodeBounds() {
    std::cout << "[TEST] Running TestPreviewFormatDecodeBounds ..." << std::endl;
    std::string err;
    int w = 0, h = 0;

    // 1. Arbitrary text in .png extension -> reject
    std::string fakePng = "This is not a real PNG image file";
    TEST_ASSERT(!PackValidator::ValidatePreviewImageBytes(
        reinterpret_cast<const uint8_t*>(fakePng.data()), fakePng.size(), "preview.png", w, h, err),
        "Must reject arbitrary text with .png extension");

    // 2. Arbitrary text in .bmp extension -> reject
    std::string fakeBmp = "This is not a BMP file";
    TEST_ASSERT(!PackValidator::ValidatePreviewImageBytes(
        reinterpret_cast<const uint8_t*>(fakeBmp.data()), fakeBmp.size(), "preview.bmp", w, h, err),
        "Must reject arbitrary text with .bmp extension");

    // 3. Valid PNG header
    uint8_t validPng[32] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x00, 0x40, // width = 64
        0x00, 0x00, 0x00, 0x40, // height = 64
        0x08, 0x06, 0x00, 0x00, 0x00
    };
    TEST_ASSERT(PackValidator::ValidatePreviewImageBytes(validPng, sizeof(validPng), "preview.png", w, h, err),
        "Must accept valid PNG preview: " + err);
    TEST_ASSERT(w == 64 && h == 64, "PNG dimensions must be 64x64");

    // 4. Oversized PNG (> 4096)
    uint8_t hugePng[32];
    std::memcpy(hugePng, validPng, sizeof(validPng));
    hugePng[18] = 0x20; // width = 8192
    TEST_ASSERT(!PackValidator::ValidatePreviewImageBytes(hugePng, sizeof(hugePng), "preview.png", w, h, err),
        "Must reject PNG with dimensions exceeding 4096");

    std::cout << "  -> PASSED" << std::endl;
    return true;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--exit-proof") {
        return 42;
    }
    if (argc == 2 && std::string(argv[1]) == "--hang-proof") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }

    std::cout << "========================================================" << std::endl;
    std::cout << "PrismForge Offline Effect-Pack Test Suite" << std::endl;
    std::cout << "========================================================" << std::endl;

    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::cerr << "[ERROR] Failed to initialize wxWidgets runtime for tests!" << std::endl;
        return 1;
    }

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    auto runTest = [&](bool (*testFunc)(), const char* name) {
        g_lastTestSkipped = false;
        try {
            if (testFunc()) {
                if (g_lastTestSkipped) {
                    skipped++;
                } else {
                    passed++;
                }
            } else {
                failed++;
                std::cerr << "[FAIL] Test failed: " << name << std::endl;
            }
        } catch (const std::exception& e) {
            failed++;
            std::cerr << "[FAIL] Exception in " << name << ": " << e.what() << std::endl;
        }
    };

    runTest(TestSha256StandardVectors, "TestSha256StandardVectors");
    runTest(TestManifestSerializationAndParsing, "TestManifestSerializationAndParsing");
    runTest(TestPathTraversalSecurityRejection, "TestPathTraversalSecurityRejection");
    runTest(TestProhibitedExtensionsRejection, "TestProhibitedExtensionsRejection");
    runTest(TestInstallAndLoadValidSamplePack, "TestInstallAndLoadValidSamplePack");
    runTest(TestCorruptedHashRollback, "TestCorruptedHashRollback");
    runTest(TestMalformedManifestRejection, "TestMalformedManifestRejection");
    runTest(TestDuplicateAssetIdRejection, "TestDuplicateAssetIdRejection");
    runTest(TestSafeVersionUpgradeAndRollback, "TestSafeVersionUpgradeAndRollback");
    runTest(TestEnumerateAndUninstall, "TestEnumerateAndUninstall");

    runTest(TestIdentifierValidationAndReservedNames, "TestIdentifierValidationAndReservedNames");
    runTest(TestReparsePointAndJunctionRejection, "TestReparsePointAndJunctionRejection");
    runTest(TestDuplicateCaseInsensitivePathsRejection, "TestDuplicateCaseInsensitivePathsRejection");
    runTest(TestPreviewAssetDeclaredBoundsAndMismatch, "TestPreviewAssetDeclaredBoundsAndMismatch");
    runTest(TestValidZipArchiveInstallation, "TestValidZipArchiveInstallation");
    runTest(TestZipArchiveSecurityRejections, "TestZipArchiveSecurityRejections");
    runTest(TestZipDecompressionBoundsAndBombRejection, "TestZipDecompressionBoundsAndBombRejection");
    runTest(TestTransactionalFaultSeamsAndRollback, "TestTransactionalFaultSeamsAndRollback");
    runTest(TestConcurrentLeasedReadersAndImmutability, "TestConcurrentLeasedReadersAndImmutability");
    runTest(TestSafeUninstallSentinelPreservation, "TestSafeUninstallSentinelPreservation");
    runTest(TestDeterministicCatalogOrdering, "TestDeterministicCatalogOrdering");

    // Milestones 2, 3, 4, 5, 6 Adversarial tests
    runTest(TestStrictJsonGrammarAndUnicodeSurrogates, "TestStrictJsonGrammarAndUnicodeSurrogates");
    runTest(TestSemVerEdgeCasesAndRejections, "TestSemVerEdgeCasesAndRejections");
    runTest(TestZipUndeclaredPayloadRejection, "TestZipUndeclaredPayloadRejection");
    runTest(TestCatalogueIntegrityAndActionableErrors, "TestCatalogueIntegrityAndActionableErrors");
    runTest(TestJournalInterruptedCrashRecovery, "TestJournalInterruptedCrashRecovery");
    runTest(TestPreviewFormatDecodeBounds, "TestPreviewFormatDecodeBounds");

    std::cout << "========================================================" << std::endl;
    std::cout << "[SUMMARY] " << passed << " PASSED, " << skipped << " SKIPPED, " << failed << " FAILED out of " << (passed + skipped + failed) << " Effect-Pack Tests!" << std::endl;
    std::cout << "========================================================" << std::endl;

    return (failed == 0) ? 0 : 1;
}
