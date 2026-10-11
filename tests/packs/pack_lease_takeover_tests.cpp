#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/packs/sha256.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif
using namespace PrismForge::Effects::Packs;
namespace fs = std::filesystem;
namespace {
int assertions = 0;
struct CapabilitySkip : std::runtime_error { using std::runtime_error::runtime_error; };
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(value.data(), value.size()); out.close();
    Check(!out.fail(), "fixture write failed");
}
PackAssetEntry Asset(const std::string& id, const std::string& path,
    PackAssetType type, const std::string& bytes) {
    return {type, id, path, Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), bytes.size()};
}
PackManifest Fixture(const fs::path& root, const std::string& value, const std::string& preview = "verified preview") {
    PackManifest m;
    m.packId = "com.prismforge.takeover"; m.version = "1.0.0"; m.displayName = "Takeover";
    m.previewImagePath = "images/preview.raw";
    m.assets = {Asset("look", "looks/test.cube", PackAssetType::CubeLut, value),
        Asset("preview", m.previewImagePath, PackAssetType::PreviewImage, preview)};
    Write(root / m.assets[0].relativePath, value);
    Write(root / m.assets[1].relativePath, preview);
    std::string error;
    Check(ManifestJson::WriteToFile((root / "manifest.json").string(), m, error), "fixture manifest write failed");
    return m;
}
std::string Text(const std::vector<uint8_t>& value) { return {value.begin(), value.end()}; }
fs::path Installed(const fs::path& library) { return library / "com.prismforge.takeover" / "1.0.0"; }
std::unique_ptr<LoadedPack> Load(const fs::path& path) {
    std::string error;
    auto result = PackManager::LoadPack(path.string(), error);
    if (!result) throw std::runtime_error("load failed: " + error);
    ++assertions; return result;
}
void Install(const fs::path& source, const fs::path& library) {
    std::string error;
    if (!PackManager::InstallPack(source.string(), library.string(), error)) throw std::runtime_error("install failed: " + error);
    ++assertions;
}
void SaveManifest(const fs::path& root, const PackManifest& manifest) {
    std::string error;
    Check(ManifestJson::WriteToFile((root / "manifest.json").string(), manifest, error), "save manifest failed");
}

void RetainedLifecycle(const fs::path& root) {
    auto first = root / "source-a", second = root / "source-b", library = root / "library";
    Fixture(first, "asset-a", "preview-a"); Fixture(second, "asset-b", "preview-b");
    Install(first, library); auto old = Load(Installed(library));
    const auto* descriptor = old->GetAssetDescriptor("look");
    const auto* verified = old->GetVerifiedManifest();
    Check(descriptor && descriptor->assetType == PackAssetType::CubeLut && verified, "stable descriptors missing");
    Install(second, library); auto latest = Load(Installed(library));
    Check(Text(latest->ReadAssetBytes("look")) == "asset-b", "update did not publish new asset");
    Check(Text(old->ReadAssetBytes("look")) == "asset-a", "old lease changed after update");
    Check(Text(old->GetPreviewBytes()) == "preview-a", "old preview changed after update");
    std::string error;
    Check(PackManager::UninstallPack(library.string(), verified->packId, verified->version, error), "uninstall failed");
    Check(!fs::exists(Installed(library)), "uninstall left version present");
    Check(Text(old->ReadAssetBytes("look")) == "asset-a" && Text(latest->ReadAssetBytes("look")) == "asset-b", "uninstall broke retained leases");
    old->manifest.assets.clear(); old->manifest.previewImagePath = "../../sentinel"; old->packDirectory = "Z:\\untrusted";
    Check(old->GetAssetDescriptor("look") == descriptor && old->GetVerifiedManifest() == verified, "legacy metadata changed owned descriptors");
    Check(Text(old->ReadAssetBytes("look")) == "asset-a" && Text(old->GetPreviewBytes()) == "preview-a", "legacy mutation changed owned bytes");
    auto copy = old->ReadAssetBytes("look"); copy[0] = 'x';
    Check(Text(old->ReadAssetBytes("look")) == "asset-a", "caller mutable bytes changed lease");
    Check(old->ReadAssetBytes("missing").empty(), "unknown asset unexpectedly exists");
}

void ConcurrentLifecycle(const fs::path& root) {
    auto first = root / "source-a", second = root / "source-b", library = root / "library";
    const std::string a(65536, 'a'), b(65536, 'b');
    Fixture(first, a, "preview-a"); Fixture(second, b, "preview-b");
    Install(first, library); auto lease = Load(Installed(library));
    std::atomic<bool> started{false}, stop{false}, valid{true};
    std::atomic<unsigned> reads{0}, duringWrites{0}; std::atomic<bool> writing{false};
    std::thread reader([&] {
        started = true;
        while (!stop || reads < 200) {
            if (Text(lease->ReadAssetBytes("look")) != a || Text(lease->GetPreviewBytes()) != "preview-a") valid = false;
            ++reads; if (writing) ++duringWrites;
        }
    });
    struct Join { std::thread& reader; std::atomic<bool>& stop; ~Join() { stop = true; reader.join(); } } join{reader, stop};
    while (!started) std::this_thread::yield();
    for (int i = 0; i < 12; ++i) {
        writing = true;
        Install(second, library); auto newest = Load(Installed(library));
        Check(Text(newest->ReadAssetBytes("look")) == b, "concurrent update bytes mismatch");
        std::string error;
        Check(PackManager::UninstallPack(library.string(), "com.prismforge.takeover", "1.0.0", error), "concurrent uninstall failed");
        Install(first, library); writing = false;
    }
    stop = true;
    while (reads < 200) std::this_thread::yield();
    Check(valid && reads >= 200 && duringWrites > 0, "retained reads failed or did not overlap real writers");
    std::cout << "  Retained concurrent reads=" << reads << ", overlapping writes=" << duringWrites << '\n';
}

void IntegrityAndBounds(const fs::path& root) {
    auto source = root / "source", library = root / "library";
    auto m = Fixture(source, "known good", "typed preview");
    auto lease = Load(source); Write(source / "looks/test.cube", "wrong hash");
    Check(Text(lease->ReadAssetBytes("look")) == "known good", "lease reread corrupted path");
    std::string error;
    Check(!PackManager::LoadPack(source.string(), error), "corrupt asset newly loaded");
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "corrupt source installed");
    Check(!fs::exists(library / ".staging"), "invalid original source created staging");
    Write(source / "looks/test.cube", "known good plus trailing bytes");
    Check(!PackManager::LoadPack(source.string(), error), "appended bytes accepted");
    Write(source / "looks/test.cube", "k");
    Check(!PackManager::LoadPack(source.string(), error), "truncated bytes accepted");
    m.assets[0].sizeBytes = std::numeric_limits<uint64_t>::max(); SaveManifest(source, m);
    Check(!PackManager::LoadPack(source.string(), error), "unchecked huge declared allocation accepted");
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "huge declared source installed");
    Fixture(source, "good");
    { std::ofstream file(source / "looks/test.cube", std::ios::binary | std::ios::trunc); file.seekp(50ull * 1024 * 1024); file.put('x'); }
    Check(!PackManager::LoadPack(source.string(), error), "oversized actual file accepted");
    Write(source / "manifest.json", std::string(1024 * 1024 + 1, ' '));
    Check(!PackManager::LoadPack(source.string(), error), "oversized manifest accepted");
}

void PreviewContracts(const fs::path& root) {
    auto source = root / "source", library = root / "library";
    auto m = Fixture(source, "value", "typed-a");
    m.assets.push_back(Asset("preview-b", "images/b.raw", PackAssetType::PreviewImage, "typed-b"));
    Write(source / "images/b.raw", "typed-b"); m.previewImagePath = "images/b.raw"; SaveManifest(source, m);
    auto lease = Load(source);
    Check(lease->GetPreviewDescriptor() && lease->GetPreviewDescriptor()->assetId == "preview-b", "preview selected first asset instead of explicit path");
    Check(Text(lease->GetPreviewBytes()) == "typed-b", "explicit typed preview mismatch");
    Write(source / "images/b.raw", "tamper!"); std::string error;
    Check(!PackManager::LoadPack(source.string(), error), "preview integrity failure accepted");
    Check(Text(lease->GetPreviewBytes()) == "typed-b", "retained preview reread disk");
    Fixture(source, "value"); m = Fixture(source, "value");
    m.previewImagePath = "images/loose.raw"; Write(source / m.previewImagePath, "unverified"); SaveManifest(source, m);
    Check(!PackManager::LoadPack(source.string(), error), "undeclared preview loaded");
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "undeclared preview installed");
    m.previewImagePath = "images/preview.raw"; m.assets[1].assetType = PackAssetType::ScreenOverlay; SaveManifest(source, m);
    Check(!PackManager::LoadPack(source.string(), error), "untyped preview loaded");
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "untyped preview installed");
    m.assets[1].assetType = PackAssetType::PreviewImage; m.assets[1].sizeBytes = 10ull * 1024 * 1024 + 1; SaveManifest(source, m);
    Check(!PackManager::LoadPack(source.string(), error), "oversized typed preview accepted");
}

void SourceSentinels(const fs::path& root) {
    auto source = root / "source", library = root / "library";
    auto m = Fixture(source, "value");
    const auto sentinel = root / "outside" / "sentinel.raw"; Write(sentinel, "sentinel must survive");
    m.assets[0].relativePath = "../outside/sentinel.raw"; SaveManifest(source, m);
    std::string error;
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "source traversal installed");
    m.assets[0].relativePath = sentinel.string(); SaveManifest(source, m);
    Check(!PackManager::InstallPack(source.string(), library.string(), error), "source absolute asset installed");
    Check(!fs::exists(library), "invalid source mutated library");
    std::ifstream in(sentinel, std::ios::binary); std::string text((std::istreambuf_iterator<char>(in)), {});
    Check(text == "sentinel must survive", "source sentinel changed");
}

#ifdef _WIN32
bool Junction(const fs::path& link, const fs::path& target, DWORD& error) {
    fs::create_directories(link);
    auto h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    const std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
    const std::wstring print = fs::absolute(target).native();
    const size_t bytes = 16 + (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<uint8_t> storage(bytes);
    // REPARSE_DATA_BUFFER mount-point header (8-byte generic + 8-byte offsets/lengths).
    *reinterpret_cast<DWORD*>(storage.data()) = IO_REPARSE_TAG_MOUNT_POINT;
    auto* fields = reinterpret_cast<WORD*>(storage.data() + 4);
    fields[0] = static_cast<WORD>(bytes - 8); fields[1] = 0;
    fields[2] = 0; fields[3] = static_cast<WORD>(substitute.size() * 2);
    fields[4] = static_cast<WORD>((substitute.size() + 1) * 2); fields[5] = static_cast<WORD>(print.size() * 2);
    auto* names = reinterpret_cast<wchar_t*>(storage.data() + 16);
    std::copy(substitute.begin(), substitute.end(), names);
    std::copy(print.begin(), print.end(), names + substitute.size() + 1);
    DWORD returned = 0;
    const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, storage.data(), static_cast<DWORD>(storage.size()), nullptr, 0, &returned, nullptr) != FALSE;
    error = ok ? ERROR_SUCCESS : GetLastError(); CloseHandle(h); return ok;
}
#endif
void JunctionSource(const fs::path& root) {
#ifdef _WIN32
    const auto source = root / "source", outside = root / "outside", library = root / "library";
    auto m = Fixture(source, "value"); Write(outside / "test.cube", "value"); Write(outside / "sentinel.txt", "untouched");
    DWORD error = 0;
    if (!Junction(source / "nested", outside, error)) {
        if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
            throw CapabilitySkip("junction capability unavailable, error=" + std::to_string(error));
        }
        throw std::runtime_error("junction fixture failed, error=" + std::to_string(error));
    }
    m.assets[0].relativePath = "nested/test.cube"; SaveManifest(source, m);
    std::string why;
    Check(!PackManager::InstallPack(source.string(), library.string(), why), "nested original source junction stripped and installed");
    Check(why.find("reparse") != std::string::npos, "junction rejection did not identify source path");
    Check(!PackManager::LoadPack(source.string(), why), "junction-backed source snapshot loaded");
    Check(!fs::exists(library), "junction rejection created staging");
    // A root alias and an ancestor alias must also be rejected.
    Check(Junction(root / "alias", source, error), "root junction create failed");
    Check(!PackManager::InstallPack((root / "alias").string(), library.string(), why), "source root junction installed");
    Check(Junction(root / "parent-alias", root, error), "ancestor junction create failed");
    Check(!PackManager::LoadPack((root / "parent-alias" / "source").string(), why), "ancestor source junction loaded");
    Check(fs::file_size(outside / "sentinel.txt") == 9, "junction sentinel changed");
    // Remove only the three explicit junction entries, never recursively enumerate through them.
    Check(RemoveDirectoryW((source / "nested").c_str()) != FALSE, "nested junction unlink failed");
    Check(RemoveDirectoryW((root / "alias").c_str()) != FALSE, "root junction unlink failed");
    Check(RemoveDirectoryW((root / "parent-alias").c_str()) != FALSE, "ancestor junction unlink failed");
#else
    throw CapabilitySkip("Windows junction capability unavailable");
#endif
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--exit-proof") return 42;
    if (argc == 2 && std::string(argv[1]) == "--hang-proof") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
    if (argc != 2) { std::cerr << "Expected unique run fixture root\n"; return 2; }
    const auto root = fs::absolute(argv[1]);
    if (fs::exists(root)) { std::cerr << "Fixture root must be fresh\n"; return 2; }
    fs::create_directories(root);
    struct Group { const char* name; void (*run)(const fs::path&); } groups[] = {
        {"retained_update_uninstall_and_immutable_metadata", RetainedLifecycle},
        {"concurrent_retained_readers_during_real_updates_and_uninstall", ConcurrentLifecycle},
        {"complete_hash_size_and_allocation_bounds", IntegrityAndBounds},
        {"typed_explicit_preview_integrity_and_leases", PreviewContracts},
        {"original_source_traversal_and_sentinel", SourceSentinels},
        {"nested_root_and_ancestor_original_source_junctions", JunctionSource}};
    int failures = 0, skipped = 0;
    for (const auto& group : groups) {
        try { group.run(root / group.name); std::cout << "[PASS] " << group.name << '\n'; }
        catch (const CapabilitySkip& error) { ++skipped; std::cout << "[SKIP] " << group.name << ": " << error.what() << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << group.name << ": " << error.what() << '\n'; }
    }
    std::cout << "groups=6 passed=" << (6 - failures - skipped) << " skipped=" << skipped << " failures=" << failures << " assertions=" << assertions << '\n';
    return failures ? 1 : 0;
}
