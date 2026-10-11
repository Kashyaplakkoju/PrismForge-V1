#pragma comment(lib, "wxbase33u.lib")
#include "pack_manager.h"
#include "pack_manifest.h"
#include "pack_validator.h"
#include "sha256.h"
#include "pack_preview_decoder.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <iostream>

#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <wx/init.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "pack_transaction_store.h"

namespace PrismForge::Effects::Packs {

namespace fs = std::filesystem;

struct VerifiedPackSnapshot {
    PackManifest manifest;
    std::unordered_map<std::string, std::vector<uint8_t>> assets;
    std::string previewAssetId;
    std::vector<uint8_t> manifestBytes;
    fs::path root;
};

namespace {

static std::mutex g_packManagerMutex;
static std::mutex g_recoveryErrorMutex;
static std::string g_lastRecoveryError;
static std::atomic<PackFaultSeam> g_currentFaultSeam{ PackFaultSeam::None };
static std::atomic<uint64_t> g_stagingCounter{ 0 };
static std::atomic<PackTransactionCheckpoint> g_checkpoint{nullptr};
static std::atomic<void*> g_checkpointContext{nullptr};
static std::atomic<uint32_t> g_lockTimeout{30000};
static std::atomic<uint32_t> g_lastLockResult{WAIT_FAILED};
static thread_local PackStorage::LibraryOwner* g_owner=nullptr;
void Checkpoint(PackTransactionPhase phase) {auto cb=g_checkpoint.load();if(cb)cb(phase,g_checkpointContext.load());}
struct OwnerScope {PackStorage::LibraryOwner* previous;OwnerScope(PackStorage::LibraryOwner& o):previous(g_owner){g_owner=&o;}~OwnerScope(){g_owner=previous;}};

void SetGlobalRecoveryError(const std::string& err) {
    std::lock_guard<std::mutex> lock(g_recoveryErrorMutex);
    g_lastRecoveryError = err;
}

std::string ToLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path GetCanonicalLibraryPath(const std::string& targetLibraryRoot) {
    // Authority is checked on original spelling before any normalization.
    return fs::u8path(targetLibraryRoot);
}

std::string GetLibraryMutexName(const fs::path& canonicalRoot) {
    std::string norm = PackStorage::PathIdentity(canonicalRoot.lexically_normal());
    std::string hash = Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(norm.data()), norm.size());
    return "Local\\PrismForge_PackLibrary_Lock_" + hash;
}

class ProcessLibraryLock {
public:
    explicit ProcessLibraryLock(const fs::path& canonicalRoot, DWORD timeoutMs = 0) {
#ifdef _WIN32
        std::string mName = GetLibraryMutexName(canonicalRoot);
        std::wstring mutexName(mName.begin(), mName.end());
        m_handle = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (m_handle) {
            DWORD waitRes = WaitForSingleObject(m_handle, timeoutMs?timeoutMs:g_lockTimeout.load());
            g_lastLockResult.store(waitRes);
            if (waitRes == WAIT_OBJECT_0) {
                m_acquired = true;
            } else if (waitRes == WAIT_ABANDONED) {
                m_acquired = true;
                m_wasAbandoned = true;
            }
        }
#else
        m_acquired = true;
#endif
    }
    ~ProcessLibraryLock() {
#ifdef _WIN32
        if (m_handle) {
            if (m_acquired) {
                ReleaseMutex(m_handle);
            }
            CloseHandle(m_handle);
        }
#endif
    }
    bool IsLocked() const { return m_acquired; }
    bool WasAbandoned() const { return m_wasAbandoned; }
private:
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#endif
    bool m_acquired = false;
    bool m_wasAbandoned = false;
};

#ifdef _WIN32
class DirectoryHandleLock {
public:
    explicit DirectoryHandleLock(const fs::path& dirPath) {
        m_handle = CreateFileW(
            PackStorage::NativePath(dirPath).c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr
        );
    }
    ~DirectoryHandleLock() {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }
    bool IsValid() const {
        return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
    }
    bool CheckNoReparse() const {
        if (!IsValid()) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(m_handle, &info)) return false;
        return (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
#endif

std::string MakeUniqueStagingName(const std::string&) {
    std::string error;auto token=PackStorage::RandomHex(16,error);
    if(token.empty())throw std::runtime_error(error);return "tx_"+token;
}

bool SameComponent(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const auto aw = a.lexically_normal().generic_wstring();
    const auto bw = b.lexically_normal().generic_wstring();
    return CompareStringOrdinal(aw.c_str(), -1, bw.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}

bool IsPathStrictlyBeneath(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const auto c = fs::weakly_canonical(child, ec);
    if (ec) return false;
    const auto p = fs::weakly_canonical(parent, ec);
    if (ec || p.empty()) return false;
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || !SameComponent(*ci, *pi)) return false;
    }
    return ci != c.end();
}

struct SourcePathLocks {
#ifdef _WIN32
    std::vector<HANDLE> handles;
    ~SourcePathLocks() { for (auto h : handles) CloseHandle(h); }
#endif
    bool Check(const fs::path& absolute, bool regularFile, std::string& error) {
        std::error_code ec;
        fs::path cur;
        for (const auto& component : absolute) {
            cur /= component;
            if (cur == absolute.root_name()) continue;
#ifdef _WIN32
            auto h = CreateFileW(PackStorage::NativePath(cur).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                error = "Cannot lock original source component: " + cur.u8string(); return false;
            }
            try { handles.push_back(h); }
            catch (...) { CloseHandle(h); throw; }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = "Source reparse points/symlinks are prohibited: " + cur.u8string(); return false;
            }
            const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((!SameComponent(cur, absolute) && !directory) ||
                (SameComponent(cur, absolute) && directory == regularFile)) {
                error = "Source component has the wrong file type: " + cur.u8string(); return false;
            }
#else
            const auto status = fs::symlink_status(cur, ec);
            if (ec || fs::is_symlink(status)) { error = "Invalid/link source component"; return false; }
            if ((cur != absolute && !fs::is_directory(status)) ||
                (cur == absolute && (regularFile ? !fs::is_regular_file(status) : !fs::is_directory(status)))) {
                error = "Source component has the wrong file type"; return false;
            }
#endif
        }
        return true;
    }
};

constexpr uint64_t kMaxAssetBytes = 50ull * 1024 * 1024;
constexpr uint64_t kMaxPreviewBytes = 10ull * 1024 * 1024;
constexpr uint64_t kMaxPackBytes = 150ull * 1024 * 1024;
constexpr uint64_t kMaxManifestBytes = 1024ull * 1024;

bool ReadBounded(const fs::path& path, uint64_t limit, uint64_t expectedSize,
    bool exactSize, std::vector<uint8_t>& bytes, std::string& error) {
#ifdef _WIN32
    const auto h = CreateFileW(PackStorage::NativePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = "Cannot open original source file: " + path.u8string(); return false; }
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } closer{h};
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes &
        (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || !GetFileSizeEx(h, &size) ||
        size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > limit ||
        (exactSize && static_cast<uint64_t>(size.QuadPart) != expectedSize)) {
        error = "Original source size/type exceeds bounds or differs from manifest: " + path.u8string(); return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD got = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset));
        if (!ReadFile(h, bytes.data() + offset, chunk, &got, nullptr) || got != chunk) {
            error = "Incomplete source read: " + path.u8string(); return false;
        }
        offset += got;
    }
    uint8_t extra = 0; DWORD got = 0;
    if (!ReadFile(h, &extra, 1, &got, nullptr) || got != 0) {
        error = "Source grew beyond verified bytes: " + path.u8string(); return false;
    }
#else
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || !size || size > limit || (exactSize && size != expectedSize)) {
        error = "Source size outside bounds"; return false;
    }
    std::ifstream file(path, std::ios::binary);
    bytes.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
        file.peek() != std::char_traits<char>::eof() || file.bad()) {
        error = "Incomplete source read"; return false;
    }
#endif
    return true;
}

std::shared_ptr<const VerifiedPackSnapshot> CaptureVerifiedSource(
    const fs::path& input, std::string& error) {
    try {
        std::error_code ec;
        auto result = std::make_shared<VerifiedPackSnapshot>();
        result->root = fs::absolute(input, ec).lexically_normal();
        SourcePathLocks locks;
        if (ec) { error = "Cannot resolve input path: " + ec.message(); return {}; }
        if (!locks.Check(result->root, false, error)) return {};
        const auto manifestPath = result->root / "manifest.json";
        if (!locks.Check(manifestPath, true, error) ||
            !ReadBounded(manifestPath, kMaxManifestBytes, 0, false, result->manifestBytes, error)) return {};
        if (!ManifestJson::Deserialize(std::string(result->manifestBytes.begin(), result->manifestBytes.end()),
            result->manifest, error)) return {};
        const auto valid = PackValidator::ValidateManifest(result->manifest);
        if (!valid.valid) { error = valid.errorReason; return {}; }
        uint64_t total = 0;
        const PackAssetEntry* selectedPreview = nullptr;
        for (const auto& entry : result->manifest.assets) {
            if (!result->manifest.previewImagePath.empty() &&
                SameComponent(fs::u8path(entry.relativePath).lexically_normal(),
                    fs::u8path(result->manifest.previewImagePath).lexically_normal())) {
                if (entry.assetType != PackAssetType::PreviewImage) {
                    error = "Preview path must reference a declared PreviewImage asset"; return {};
                }
                selectedPreview = &entry;
            } else if (result->manifest.previewImagePath.empty() && !selectedPreview &&
                entry.assetType == PackAssetType::PreviewImage) selectedPreview = &entry;
            const uint64_t limit = entry.assetType == PackAssetType::PreviewImage ? kMaxPreviewBytes : kMaxAssetBytes;
            if (!entry.sizeBytes || entry.sizeBytes > limit || entry.sizeBytes > kMaxPackBytes - total) {
                error = "Declared asset/preview/total exceeds snapshot bounds"; return {};
            }
            total += entry.sizeBytes;
        }
        if (!result->manifest.previewImagePath.empty() && !selectedPreview) {
            error = "Preview path is not a declared hashed PreviewImage asset"; return {};
        }
        if (selectedPreview) result->previewAssetId = selectedPreview->assetId;
        for (const auto& entry : result->manifest.assets) {
            const auto path = result->root / fs::u8path(entry.relativePath);
            std::vector<uint8_t> bytes;
            if (!locks.Check(path, true, error)) return {};
            if (!IsPathStrictlyBeneath(path, result->root)) {
                error = "Asset path is not strictly beneath pack root: " + path.u8string();
                return {};
            }
            if (!ReadBounded(path, entry.assetType == PackAssetType::PreviewImage ? kMaxPreviewBytes : kMaxAssetBytes,
                    entry.sizeBytes, true, bytes, error)) return {};
            if (ToLower(Sha256::ComputeHex(bytes.data(), bytes.size())) != ToLower(entry.sha256Hex)) {
                error = "SHA-256 integrity mismatch for: " + entry.relativePath; return {};
            }
            if (entry.assetType == PackAssetType::PreviewImage) {
                const auto decoded = PackPreviewDecoder::DecodePreviewAsset(entry,bytes.data(),bytes.size());
                if(!decoded.success){error="Preview decode failed: "+decoded.errorReason;return {};}
            }
            result->assets.emplace(entry.assetId, std::move(bytes));
        }
        error.clear();
        return result;
    } catch (const std::exception& e) {
        error = std::string("Cannot capture bounded pack snapshot: ") + e.what(); return {};
    }
}

bool WriteSnapshotFile(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;fs::create_directories(path.parent_path(),ec);
    if(ec){error="Cannot create snapshot parent: "+ec.message();return false;}
    SourcePathLocks guards;if(!guards.Check(path.parent_path(),false,error))return false;
    return PackStorage::DurableFile(path,bytes,true,error);
}

// Durable manager-local schema2 records. No journal path text grants authority.
enum class JournalState {Prepared,BackedUp,Published,Committed};
std::string StateToString(JournalState s){switch(s){case JournalState::Prepared:return "PREPARED";case JournalState::BackedUp:return "BACKED_UP";case JournalState::Published:return "PUBLISHED";case JournalState::Committed:return "COMMITTED";}return "";}
bool StringToState(const std::string& s,JournalState& state){if(s=="PREPARED")state=JournalState::Prepared;else if(s=="BACKED_UP")state=JournalState::BackedUp;else if(s=="PUBLISHED")state=JournalState::Published;else if(s=="COMMITTED")state=JournalState::Committed;else return false;return true;}
struct TransactionRecord {
    std::string txId,packId,version,newManifestSha256,oldManifestSha256,newContentSha256,oldContentSha256,ownerToken;
    JournalState state=JournalState::Prepared;bool oldExists=false;uint64_t timestamp=0;
};
constexpr size_t kMaxJournalBytes=65536;
std::string ContentDigest(const VerifiedPackSnapshot& snapshot){
    std::vector<std::pair<std::string,std::string>> entries;
    for(const auto& asset:snapshot.manifest.assets){const auto& b=snapshot.assets.at(asset.assetId);
        entries.push_back({asset.relativePath,std::to_string(b.size())+":"+Sha256::ComputeHex(b.data(),b.size())});}
    std::sort(entries.begin(),entries.end());Sha256 h;
    h.Update(Sha256::ComputeHex(snapshot.manifestBytes.data(),snapshot.manifestBytes.size())+"\n");
    for(const auto& e:entries)h.Update(e.first+"\n"+e.second+"\n");return h.Final();
}
std::string RecordBinding(const TransactionRecord& r){
    return PackStorage::PathIdentity(g_owner->Root())+"\n2\n"+r.txId+"\n"+StateToString(r.state)+"\n"+
        r.packId+"\n"+r.version+"\n"+(r.oldExists?"true":"false")+"\n"+r.newManifestSha256+"\n"+
        r.oldManifestSha256+"\n"+r.newContentSha256+"\n"+r.oldContentSha256+"\n"+std::to_string(r.timestamp)+"\n";
}
bool WriteJournalFileAtomic(const fs::path& stagingRoot,const std::string& txId,const TransactionRecord& rec,std::string& error){
    const auto seam=PackManager::GetFaultSeam();
    if(seam==PackFaultSeam::SimulateJournalWriteFailure ||
        (rec.state==JournalState::BackedUp&&seam==PackFaultSeam::SimulateJournalAfterBackupFailure) ||
        (rec.state==JournalState::Published&&seam==PackFaultSeam::SimulateJournalAfterPublishFailure) ||
        (rec.state==JournalState::Committed&&seam==PackFaultSeam::SimulateJournalAfterCommitFailure)){
        error="Injected durable journal transition failure at "+StateToString(rec.state);return false;}
    std::ostringstream o;o<<"{\n\"schemaVersion\":2,\n\"txId\":\""<<rec.txId<<"\",\n\"state\":\""<<StateToString(rec.state)
        <<"\",\n\"packId\":\""<<rec.packId<<"\",\n\"version\":\""<<rec.version<<"\",\n\"oldExists\":"<<(rec.oldExists?"true":"false")
        <<",\n\"newManifestSha256\":\""<<rec.newManifestSha256<<"\",\n\"oldManifestSha256\":\""<<rec.oldManifestSha256
        <<"\",\n\"newContentSha256\":\""<<rec.newContentSha256<<"\",\n\"oldContentSha256\":\""<<rec.oldContentSha256
        <<"\",\n\"timestamp\":"<<rec.timestamp<<",\n\"ownerToken\":\""<<PackStorage::Mac(g_owner->Key(),RecordBinding(rec))<<"\"\n}\n";
    const auto text=o.str();const auto final=stagingRoot/("journal_"+txId+".json");
    std::string random=PackStorage::RandomHex(8,error);if(random.empty())return false;
    const auto temp=stagingRoot/("tmp_"+txId+"_"+random+".tmp");PackStorage::Handle h;
    if(!PackStorage::DurableFile(temp,std::vector<uint8_t>(text.begin(),text.end()),true,error,&h))return false;
    if(seam==PackFaultSeam::SimulateJournalFlushFailure){error="Injected journal durability failure";return false;}
    if(seam==PackFaultSeam::SimulateJournalRenameFailure){error="Injected atomic journal rename failure";return false;}
    if(!PackStorage::RenameHandle(h.Get(),final,true,error))return false;
    return h.Close(error);
}
bool ReadJournalFileStrict(const fs::path& file,TransactionRecord& r,std::string& error){
    std::vector<uint8_t> bytes;if(!ReadBounded(file,kMaxJournalBytes,0,false,bytes,error))return false;
    std::map<std::string,PackStorage::Scalar> fields;
    if(!PackStorage::ParseRecord(std::string(bytes.begin(),bytes.end()),fields,error))return false;
    using V=PackStorage::Scalar;
    const std::map<std::string,V::Kind> expected={{"schemaVersion",V::Number},{"txId",V::String},{"state",V::String},
        {"packId",V::String},{"version",V::String},{"oldExists",V::Boolean},{"newManifestSha256",V::String},
        {"oldManifestSha256",V::String},{"newContentSha256",V::String},{"oldContentSha256",V::String},{"timestamp",V::Number},{"ownerToken",V::String}};
    if(fields.size()!=expected.size()){error="Journal has missing or unknown fields";return false;}
    for(const auto& e:expected){auto it=fields.find(e.first);if(it==fields.end()||it->second.kind!=e.second){error="Wrong/missing typed journal field: "+e.first;return false;}}
    auto get=[&](const char* n){return fields.at(n).text;};
    if(get("schemaVersion")!="2"){error="Unsupported/unowned legacy journal schema";return false;}
    r.txId=get("txId");r.packId=get("packId");r.version=get("version");r.oldExists=get("oldExists")=="true";
    r.newManifestSha256=get("newManifestSha256");r.oldManifestSha256=get("oldManifestSha256");
    r.newContentSha256=get("newContentSha256");r.oldContentSha256=get("oldContentSha256");r.ownerToken=get("ownerToken");
    r.timestamp=std::stoull(get("timestamp"));
    if(!PackStorage::TxId(r.txId)||file.filename().u8string()!="journal_"+r.txId+".json" ||
       !StringToState(get("state"),r.state)||!r.timestamp ||
       !PackValidator::ValidateIdentifier(r.packId,false,error)||!PackValidator::ValidateIdentifier(r.version,true,error)||
       !PackStorage::Hex64(r.newManifestSha256)||!PackStorage::Hex64(r.newContentSha256)||!PackStorage::Hex64(r.ownerToken)||
       (r.oldExists?(!PackStorage::Hex64(r.oldManifestSha256)||!PackStorage::Hex64(r.oldContentSha256)||r.oldContentSha256==r.newContentSha256):
           (!r.oldManifestSha256.empty()||!r.oldContentSha256.empty()))) {
        if(error.empty())error="Journal identity, filename, state or content hash contract invalid";return false;}
    if(r.ownerToken!=PackStorage::Mac(g_owner->Key(),RecordBinding(r))){error="Journal ownership/content binding invalid";return false;}
    return true;
}
bool SnapshotFromTree(PackStorage::OwnedTree& tree,const fs::path& path,std::shared_ptr<VerifiedPackSnapshot>& out,std::string& error){
    std::unordered_map<std::string,std::vector<uint8_t>> files;if(!tree.Bytes(files,error))return false;
    auto m=files.find("manifest.json");if(m==files.end()){error="Owned pack lacks manifest";return false;}
    out=std::make_shared<VerifiedPackSnapshot>();out->root=path;out->manifestBytes=m->second;
    if(!ManifestJson::Deserialize(std::string(m->second.begin(),m->second.end()),out->manifest,error))return false;
    auto val=PackValidator::ValidateManifest(out->manifest);if(!val.valid){error=val.errorReason;return false;}
    if(files.size()!=out->manifest.assets.size()+1){error="Owned pack includes undeclared/missing files";return false;}
    std::set<std::string> expectedDirectories;
    for(const auto& a:out->manifest.assets){auto parent=fs::u8path(a.relativePath).parent_path();while(!parent.empty()){expectedDirectories.insert(parent.generic_u8string());parent=parent.parent_path();}}
    for(const auto& directory:tree.Directories())if(!expectedDirectories.count(directory)){error="Owned pack includes an undeclared directory";return false;}
    for(const auto& a:out->manifest.assets){auto f=files.find(a.relativePath);
        if(f==files.end()||f->second.size()!=a.sizeBytes||ToLower(Sha256::ComputeHex(f->second.data(),f->second.size()))!=ToLower(a.sha256Hex)){
            error="Owned pack content differs from its complete manifest";return false;}
        if(a.assetType==PackAssetType::PreviewImage){auto d=PackPreviewDecoder::DecodePreviewAsset(a,f->second.data(),f->second.size());
            if(!d.success){error="Owned preview invalid: "+d.errorReason;return false;}
            if(a.relativePath==out->manifest.previewImagePath||(out->manifest.previewImagePath.empty()&&out->previewAssetId.empty()))out->previewAssetId=a.assetId;}
        out->assets.emplace(a.assetId,std::move(f->second));
    }
    return true;
}
struct BoundTree {std::unique_ptr<PackStorage::OwnedTree> tree;std::shared_ptr<VerifiedPackSnapshot> snapshot;enum Kind{Absent,Old,New,Unknown}kind=Absent;};
bool ProbeTree(const fs::path& path,const TransactionRecord& r,BoundTree& b,std::string& error){
    DWORD attr=GetFileAttributesW(PackStorage::NativePath(path).c_str());if(attr==INVALID_FILE_ATTRIBUTES){DWORD e=GetLastError();
        if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return true;error="Cannot inspect transaction object";return false;}
    b.kind=BoundTree::Unknown;b.tree=std::make_unique<PackStorage::OwnedTree>();
    if(!b.tree->Open(path,error)||!SnapshotFromTree(*b.tree,path,b.snapshot,error))return false;
    if(b.snapshot->manifest.packId!=r.packId||b.snapshot->manifest.version!=r.version){error="Transaction object has wrong manifest identity";return false;}
    auto mh=ToLower(Sha256::ComputeHex(b.snapshot->manifestBytes.data(),b.snapshot->manifestBytes.size()));auto ch=ToLower(ContentDigest(*b.snapshot));
    if(mh==r.newManifestSha256&&ch==r.newContentSha256)b.kind=BoundTree::New;
    else if(r.oldExists&&mh==r.oldManifestSha256&&ch==r.oldContentSha256)b.kind=BoundTree::Old;
    else {error="Transaction object has unknown/corrupt expected content";return false;}
    return true;
}
bool RemoveJournal(const fs::path& path,std::string& error){
    PackStorage::Handle h(CreateFileW(PackStorage::NativePath(path).c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!h.Valid()||!PackStorage::Attributes(h.Get(),false,error)){if(error.empty())error="Cannot own journal for deletion";return false;}
    return PackStorage::DeleteHandle(h,error);
}
bool Reconcile(const fs::path& root,const fs::path& journal,TransactionRecord& r,std::string& error){
    if(!g_owner->HoldStaging(false,error)||!g_owner->EnsurePackParent(r.packId,error))return false;
    const auto staging=root/".staging";const auto dest=root/r.packId/r.version;
    BoundTree target,backup,stage;
    if(!ProbeTree(dest,r,target,error)||!ProbeTree(staging/("backup_"+r.txId),r,backup,error)||
       !ProbeTree(staging/("staging_"+r.txId),r,stage,error))return false;
    if((backup.kind!=BoundTree::Absent&&backup.kind!=BoundTree::Old)||
       (stage.kind!=BoundTree::Absent&&stage.kind!=BoundTree::New)) {error="Conflicting transaction object roles; preserving all data";return false;}
    if(target.kind==BoundTree::New){
        // Publication may precede its journal transition. Verify new bytes before
        // committing, and verify every old backup independently before removal.
        if(r.state!=JournalState::Committed){r.state=JournalState::Committed;if(!WriteJournalFileAtomic(staging,r.txId,r,error))return false;}
        Checkpoint(PackTransactionPhase::BeforeCleanup);
        if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCleanupFailure){error="Injected verified transaction cleanup failure";return false;}
        if(backup.tree&&!backup.tree->Remove(error))return false;
        if(stage.tree&&!stage.tree->Remove(error))return false;
        return RemoveJournal(journal,error);
    }
    if(target.kind==BoundTree::Old){
        if(backup.kind!=BoundTree::Absent){error="Duplicate old target/backup conflict; preserving data";return false;}
    } else if(target.kind==BoundTree::Absent){
        if(r.oldExists){
            if(backup.kind!=BoundTree::Old){error="No intact expected old/new version; preserving recoverable state";return false;}
            if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateRollbackFailure){error="Injected restore failure; intact backup preserved";return false;}
            if(!backup.tree->Rename(dest,error))return false;
            std::shared_ptr<VerifiedPackSnapshot> restored;
            if(!SnapshotFromTree(*backup.tree,dest,restored,error)||ToLower(ContentDigest(*restored))!=r.oldContentSha256){if(error.empty())error="Restored content changed; preserving journal";return false;}
        } else if(backup.kind!=BoundTree::Absent){error="First install cannot own a prior backup";return false;}
        else if(r.state==JournalState::Published||r.state==JournalState::Committed){
            error="Published first install is missing its expected new target; preserving transaction";return false;
        }
    } else {error="Unknown target; preserving all transaction objects";return false;}
    // An aborted first install or update restored to the exact old version.
    if(stage.tree&&!stage.tree->Remove(error))return false;
    return RemoveJournal(journal,error);
}
bool RecoverInterruptedTransactionsLocked(const fs::path& root,std::string& error){
    std::error_code ec;if(!fs::exists(root,ec)){if(ec){error=ec.message();return false;}error.clear();SetGlobalRecoveryError("");return true;}
    PackStorage::LibraryOwner local;std::unique_ptr<OwnerScope> scope;
    if(!g_owner){
        if(!local.Open(root,false,error)){
            // Empty/unmanaged trees are read-only; absence of any journal is safe.
            const auto staging=root/".staging";if(!fs::exists(staging,ec)&&!ec){error.clear();SetGlobalRecoveryError("");return true;}
            SetGlobalRecoveryError(error);return false;}
        scope=std::make_unique<OwnerScope>(local);
    }
    const auto staging=root/".staging";if(!fs::exists(staging,ec)){if(ec){error=ec.message();return false;}error.clear();SetGlobalRecoveryError("");return true;}
    if(!g_owner->HoldStaging(false,error)){SetGlobalRecoveryError(error);return false;}
    std::vector<fs::path> journals;fs::directory_iterator it(staging,ec),end;
    if(ec){error="Cannot enumerate transaction journals: "+ec.message();SetGlobalRecoveryError(error);return false;}
    for(;it!=end;it.increment(ec)){
        if(ec){error="Cannot finish journal enumeration";SetGlobalRecoveryError(error);return false;}
        const auto name=it->path().filename().u8string();if(name.rfind("journal_",0)==0&&it->path().extension()==".json")journals.push_back(it->path());
        if(journals.size()>512){error="Too many transaction journals";SetGlobalRecoveryError(error);return false;}
    }
    if(ec){error=ec.message();SetGlobalRecoveryError(error);return false;}
    std::sort(journals.begin(),journals.end());
    for(const auto& path:journals){TransactionRecord rec;
        if(!ReadJournalFileStrict(path,rec,error)||!Reconcile(root,path,rec,error)){
            error="Pack recovery preserved transaction "+path.filename().u8string()+": "+error;SetGlobalRecoveryError(error);return false;}}
    error.clear();SetGlobalRecoveryError("");return true;
}
bool CommitStagedPackLocked(const fs::path& root,const fs::path& stagingPath,const PackManifest& manifest,
    const std::string& txId,const std::vector<uint8_t>&,std::string& error){
    if(!g_owner||!PackStorage::TxId(txId)||stagingPath!=root/".staging"/("staging_"+txId)){
        error="Staged publisher lacks exact held owner/transaction identity";return false;}
    if(!g_owner->HoldStaging(false,error)||!g_owner->EnsurePackParent(manifest.packId,error))return false;
    PackStorage::OwnedTree stage;if(!stage.Open(stagingPath,error))return false;
    std::shared_ptr<VerifiedPackSnapshot> next;if(!SnapshotFromTree(stage,stagingPath,next,error))return false;
    TransactionRecord r;r.txId=txId;r.packId=manifest.packId;r.version=manifest.version;
    r.newManifestSha256=ToLower(Sha256::ComputeHex(next->manifestBytes.data(),next->manifestBytes.size()));r.newContentSha256=ToLower(ContentDigest(*next));
    r.timestamp=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto dest=root/r.packId/r.version;const auto staging=root/".staging";const auto backup=staging/("backup_"+txId);const auto journal=staging/("journal_"+txId+".json");
    std::unique_ptr<PackStorage::OwnedTree> old;DWORD attrs=GetFileAttributesW(PackStorage::NativePath(dest).c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES){old=std::make_unique<PackStorage::OwnedTree>();std::shared_ptr<VerifiedPackSnapshot> prior;
        if(!old->Open(dest,error)||!SnapshotFromTree(*old,dest,prior,error))return false;
        if(prior->manifest.packId!=r.packId||prior->manifest.version!=r.version){error="Existing destination is not this managed pack/version";return false;}
        r.oldExists=true;r.oldManifestSha256=ToLower(Sha256::ComputeHex(prior->manifestBytes.data(),prior->manifestBytes.size()));r.oldContentSha256=ToLower(ContentDigest(*prior));
        if(r.oldContentSha256==r.newContentSha256){if(!stage.Remove(error))return false;error.clear();return true;}
    }else if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND){error="Cannot inspect existing destination";return false;}
    if(!WriteJournalFileAtomic(staging,txId,r,error))return false;Checkpoint(PackTransactionPhase::PreparedJournal);
    if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCrashAfterPrepared){error="Injected interruption after durable PREPARED";return false;}
    if(old){if(!old->Rename(backup,error))return false;
        std::shared_ptr<VerifiedPackSnapshot> movedOld;
        if(!SnapshotFromTree(*old,backup,movedOld,error)||ToLower(ContentDigest(*movedOld))!=r.oldContentSha256){if(error.empty())error="Old content changed during protected displacement";return false;}
        Checkpoint(PackTransactionPhase::BackupMoved);
        r.state=JournalState::BackedUp;if(!WriteJournalFileAtomic(staging,txId,r,error))return false;Checkpoint(PackTransactionPhase::BackupJournal);
        if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCrashAfterBackup){error="Injected interruption after durable BACKED_UP";return false;}}
    if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCommitRenameFailure||PackManager::GetFaultSeam()==PackFaultSeam::SimulateRollbackFailure){
        const std::string primary="Injected publish rename failure";std::string restore;
        if(old&&(PackManager::GetFaultSeam()==PackFaultSeam::SimulateRollbackFailure||!old->Rename(dest,restore))) {
            error=primary+"; restore failed: "+(restore.empty()?"injected restore failure; intact backup preserved":restore);return false;}
        error=primary+"; old version preserved; journal retained for recovery";return false;
    }
    if(!stage.Rename(dest,error)){
        const std::string primary=error;std::string restore;if(old&&!old->Rename(dest,restore))error=primary+"; restore failed: "+restore;return false;}
    std::shared_ptr<VerifiedPackSnapshot> movedNew;
    if(!SnapshotFromTree(stage,dest,movedNew,error)||ToLower(ContentDigest(*movedNew))!=r.newContentSha256){if(error.empty())error="New content changed during protected publication";return false;}
    Checkpoint(PackTransactionPhase::PublishMoved);
    r.state=JournalState::Published;if(!WriteJournalFileAtomic(staging,txId,r,error))return false;Checkpoint(PackTransactionPhase::PublishJournal);
    if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCrashAfterPublish){error="Injected interruption after durable PUBLISHED";return false;}
    r.state=JournalState::Committed;if(!WriteJournalFileAtomic(staging,txId,r,error))return false;Checkpoint(PackTransactionPhase::CommitJournal);
    Checkpoint(PackTransactionPhase::BeforeCleanup);
    if(PackManager::GetFaultSeam()==PackFaultSeam::SimulateCleanupFailure){error="Injected verified transaction cleanup failure";return false;}
    if(old&&!old->Remove(error))return false;if(!RemoveJournal(journal,error))return false;
    Checkpoint(PackTransactionPhase::Complete);error.clear();return true;
}

} // anonymous namespace

// -------------------------------------------------------------
// LoadedPack Implementation
// -------------------------------------------------------------

const PackManifest* LoadedPack::GetVerifiedManifest() const {
    return snapshot_ ? &snapshot_->manifest : &manifest;
}

const PackAssetEntry* LoadedPack::GetAssetDescriptor(const std::string& assetId) const {
    if (!snapshot_) return nullptr;
    for (const auto& a : snapshot_->manifest.assets) {
        if (a.assetId == assetId) return &a;
    }
    return nullptr;
}

const PackAssetEntry* LoadedPack::GetPreviewDescriptor() const {
    return snapshot_ ? GetAssetDescriptor(snapshot_->previewAssetId) : nullptr;
}

std::string LoadedPack::GetAssetAbsolutePath(const std::string& assetId) const {
    const auto entry = GetAssetDescriptor(assetId);
    if (!entry) return {};
    return (snapshot_->root / fs::u8path(entry->relativePath)).u8string();
}

std::vector<uint8_t> LoadedPack::ReadAssetBytes(const std::string& assetId) const {
    if (!snapshot_) return {};
    const auto it = snapshot_->assets.find(assetId);
    if (it == snapshot_->assets.end()) return {};
    try { return it->second; } catch (const std::bad_alloc&) { return {}; }
}

std::vector<uint8_t> LoadedPack::GetPreviewBytes() const {
    return snapshot_ ? ReadAssetBytes(snapshot_->previewAssetId) : std::vector<uint8_t>{};
}

bool LoadedPack::GetPreviewDimensions(int& outWidth, int& outHeight) const {
    outWidth = 0;
    outHeight = 0;
    if (!snapshot_ || snapshot_->previewAssetId.empty()) return false;
    const auto desc = GetPreviewDescriptor();
    if (!desc) return false;
    auto bytes = GetPreviewBytes();
    if (bytes.empty()) return false;
    std::string err;
    return PackValidator::ValidatePreviewImageBytes(bytes.data(), bytes.size(), desc->relativePath, outWidth, outHeight, err);
}

// -------------------------------------------------------------
// PackManager Implementation
// -------------------------------------------------------------

void PackManager::SetFaultSeam(PackFaultSeam seam) {
    g_currentFaultSeam.store(seam);
}

PackFaultSeam PackManager::GetFaultSeam() {
    return g_currentFaultSeam.load();
}

std::string PackManager::GetLastRecoveryError() {
    std::lock_guard<std::mutex> lock(g_recoveryErrorMutex);
    return g_lastRecoveryError;
}

bool PackManager::HasRecoveryError() {
    std::lock_guard<std::mutex> lock(g_recoveryErrorMutex);
    return !g_lastRecoveryError.empty();
}

void PackManager::ClearRecoveryError() {
    std::lock_guard<std::mutex> lock(g_recoveryErrorMutex);
    g_lastRecoveryError.clear();
}

bool PackManager::RecoverInterruptedTransactions(const std::string& targetLibraryRoot, std::string& outError) try {
    std::lock_guard<std::mutex> lock(g_packManagerMutex);
    std::string valErr;
    if (!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),valErr) || !PackValidator::ValidateLibraryRoot(targetLibraryRoot, valErr)) {
        outError = "Invalid library root for recovery: " + valErr;
        SetGlobalRecoveryError(outError);
        return false;
    }
    fs::path canonicalRoot = GetCanonicalLibraryPath(targetLibraryRoot);
    ProcessLibraryLock procLock(canonicalRoot);
    if (!procLock.IsLocked()) {
        outError = "Failed to acquire library lock for recovery";
        SetGlobalRecoveryError(outError);
        return false;
    }
    return RecoverInterruptedTransactionsLocked(canonicalRoot, outError);
}
catch(const std::exception& ex){outError=std::string("Pack operation failed safely: ")+ex.what();return false;}
catch(...){outError="Pack operation failed safely with an unexpected error";return false;}

void PackManager::CleanOrphanStaging(const std::string& targetLibraryRoot) {
    // Prefixes and age never grant deletion authority. Recovery cleans only
    // content-bound objects referenced by a valid owned durable transaction.
    std::string error;RecoverInterruptedTransactions(targetLibraryRoot,error);
}

bool PackManager::InstallPack(
    const std::string& sourcePackDir,
    const std::string& targetLibraryRoot,
    std::string& outError)
try {
    std::lock_guard<std::mutex> lock(g_packManagerMutex);
    outError.clear();

    if (!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),outError) || !PackValidator::ValidateLibraryRoot(targetLibraryRoot, outError)) {
        return false;
    }
    fs::path canonicalRoot = GetCanonicalLibraryPath(targetLibraryRoot);
    ProcessLibraryLock procLock(canonicalRoot);
    if (!procLock.IsLocked()) {
        outError = "Failed to acquire library lock for installation";
        return false;
    }

    PackStorage::LibraryOwner owner;
    if(!owner.Open(canonicalRoot,true,outError)||!owner.HoldStaging(true,outError))return false;
    OwnerScope ownerScope(owner);
    Checkpoint(PackTransactionPhase::LockAcquired);
    std::string recErr;
    if (!RecoverInterruptedTransactionsLocked(canonicalRoot, recErr)) {
        outError = "Recovery failed before installation: " + recErr;
        return false;
    }

    const auto source = CaptureVerifiedSource(fs::u8path(sourcePackDir), outError);
    if (!source) {
        if (outError.empty()) outError = "CaptureVerifiedSource returned null";
        return false;
    }
    const auto& manifest = source->manifest;
    std::error_code ec;

    if (GetFaultSeam() == PackFaultSeam::SimulateValidationFailure) {
        outError = "Simulated validation failure via fault seam";
        return false;
    }

    std::string txId = MakeUniqueStagingName("tx_" + manifest.packId);
    fs::path stagingRoot = canonicalRoot / ".staging" / ("staging_" + txId);

    std::string descErr;
    if (!PackValidator::ValidateOwnedDescendant(canonicalRoot, stagingRoot, descErr)) {
        outError = "Staging directory validation failed: " + descErr;
        return false;
    }

    fs::create_directories(stagingRoot, ec);
    if (ec) {
        outError = "Failed to create staging directory: " + ec.message();
        return false;
    }

    auto stageGuards=std::make_unique<SourcePathLocks>();
    if(!stageGuards->Check(stagingRoot,false,outError))return false;

    auto rollback = [&]() {
        // Keep failed unjournaled staging rather than deleting a path/name lookalike.
        // A committed transaction is reconciled from its owned journal on next call.
    };

    if (!WriteSnapshotFile(stagingRoot / "manifest.json", source->manifestBytes, outError)) {
        rollback(); return false;
    }
    for (const auto& a : manifest.assets) {
        if (GetFaultSeam() == PackFaultSeam::SimulateCopyFailure) {
            rollback(); outError = "Simulated copy failure via fault seam"; return false;
        }
        if (!WriteSnapshotFile(stagingRoot / fs::u8path(a.relativePath), source->assets.at(a.assetId), outError)) {
            rollback(); return false;
        }
    }

    PackValidationResult stageVal = PackValidator::ValidateDirectoryPack(stagingRoot.u8string(), manifest);
    if (!stageVal.valid) {
        rollback();
        outError = "Staged pack verification failed: " + stageVal.errorReason;
        return false;
    }

    stageGuards.reset();
    if (!CommitStagedPackLocked(canonicalRoot, stagingRoot, manifest, txId, source->manifestBytes, outError)) {
        rollback();
        return false;
    }

    return true;
}
catch(const std::exception& ex){outError=std::string("Pack operation failed safely: ")+ex.what();return false;}
catch(...){outError="Pack operation failed safely with an unexpected error";return false;}

bool PackManager::InstallPackFromArchive(
    const std::string& archivePath,
    const std::string& targetLibraryRoot,
    std::string& outError)
try {
    std::lock_guard<std::mutex> lock(g_packManagerMutex);
    outError.clear();

    if (!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),outError) || !PackValidator::ValidateLibraryRoot(targetLibraryRoot, outError)) {
        return false;
    }
    fs::path canonicalRoot = GetCanonicalLibraryPath(targetLibraryRoot);
    ProcessLibraryLock procLock(canonicalRoot);
    if (!procLock.IsLocked()) {
        outError = "Failed to acquire library lock for archive installation";
        return false;
    }

    PackStorage::LibraryOwner owner;
    if(!owner.Open(canonicalRoot,true,outError)||!owner.HoldStaging(true,outError))return false;
    OwnerScope ownerScope(owner);
    Checkpoint(PackTransactionPhase::LockAcquired);
    std::string recErr;
    if (!RecoverInterruptedTransactionsLocked(canonicalRoot, recErr)) {
        outError = "Recovery failed before archive installation: " + recErr;
        return false;
    }

    fs::path archPath=fs::u8path(archivePath);
    std::error_code ec;
    if (!fs::exists(archPath, ec) || !fs::is_regular_file(archPath, ec)) {
        outError = "Archive file does not exist or is not a regular file: " + archivePath;
        return false;
    }

    uintmax_t archiveSize = fs::file_size(archPath, ec);
    if (ec || archiveSize > 150 * 1024 * 1024) {
        outError = "Archive file exceeds maximum allowed size (150 MB)";
        return false;
    }

    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        outError = "Failed to initialize wxWidgets runtime for ZIP decoding";
        return false;
    }

    wxFileInputStream fileStream(wxString::FromUTF8(archivePath.c_str()));
    if (!fileStream.IsOk()) {
        outError = "Failed to open archive file stream: " + archivePath;
        return false;
    }

    wxZipInputStream zipStream(fileStream);
    if (!zipStream.IsOk()) {
        outError = "Invalid or corrupt ZIP archive header: " + archivePath;
        return false;
    }

    std::string txId = MakeUniqueStagingName("tx_zip");
    fs::path stagingRoot = canonicalRoot / ".staging" / ("staging_" + txId);
    fs::create_directories(stagingRoot, ec);
    if (ec) {
        outError = "Failed to create staging directory for ZIP extraction: " + ec.message();
        return false;
    }

    auto stageGuards=std::make_unique<SourcePathLocks>();
    if(!stageGuards->Check(stagingRoot,false,outError))return false;

    auto rollback = [&]() {
        // Keep failed unjournaled staging rather than deleting a path/name lookalike.
        // A committed transaction is reconciled from its owned journal on next call.
    };

    size_t entryCount = 0;
    uint64_t totalExpandedBytes = 0;
    std::unordered_set<std::string> seenPaths;

    std::unique_ptr<wxZipEntry> entry;
    while ((entry.reset(zipStream.GetNextEntry())), entry != nullptr) {
        entryCount++;
        if (entryCount > 100) {
            rollback();
            outError = "Archive exceeds maximum allowed entry count (100)";
            return false;
        }

        if ((entry->GetFlags() & wxZIP_ENCRYPTED) != 0) {
            rollback();
            outError = "Encrypted ZIP archives are not supported";
            return false;
        }

        if ((entry->GetMode() & 0170000) == 0120000) {
            rollback();
            outError = "Symbolic link entries in ZIP archives are prohibited";
            return false;
        }

        wxString internalName = entry->GetInternalName();
        std::string rawName = std::string(internalName.utf8_str());
        std::replace(rawName.begin(), rawName.end(), '\\', '/');

        if (rawName.empty()) {
            rollback();
            outError = "ZIP entry has empty name";
            return false;
        }

        if (rawName.front() == '/' || rawName.find(':') != std::string::npos) {
            rollback();
            outError = "Absolute path or drive letter in ZIP entry prohibited: " + rawName;
            return false;
        }

        fs::path entryPath=fs::u8path(rawName);
        for (const auto& part : entryPath) {
            std::string s = part.u8string();
            if(!PackValidator::ValidatePathComponent(s,outError)) {
                outError="Unsafe ZIP entry component before extraction: "+outError;return false;
            }
            if (s == ".." || s == ".") {
                rollback();
                outError = "Path traversal component ('" + s + "') prohibited in ZIP entry: " + rawName;
                return false;
            }
            if (s.back() == ' ' || s.back() == '.') {
                rollback();
                outError = "ZIP entry component has trailing space or dot: " + s;
                return false;
            }
        }

        std::string lowerNorm = ToLower(entryPath.generic_u8string());
        if (seenPaths.find(lowerNorm) != seenPaths.end()) {
            rollback();
            outError = "Duplicate entry or case-insensitive collision in ZIP archive: " + rawName;
            return false;
        }
        seenPaths.insert(lowerNorm);

        fs::path destPath = stagingRoot / entryPath;

        if (entry->IsDir()) {
            fs::create_directories(destPath, ec);
            if (ec) {
                rollback();
                outError = "Failed to create directory in staging: " + destPath.u8string();
                return false;
            }
            if(!stageGuards->Check(destPath,false,outError))return false;
        } else {
            ssize_t declaredSize = entry->GetSize();
            if (declaredSize > 50 * 1024 * 1024) {
                rollback();
                outError = "ZIP entry exceeds maximum single file size (50 MB): " + rawName;
                return false;
            }

            fs::create_directories(destPath.parent_path(), ec);
            if(ec||!stageGuards->Check(destPath.parent_path(),false,outError)) {
                if(outError.empty())outError="Cannot create ZIP parent: "+ec.message();return false;
            }

            std::ofstream outFile(destPath, std::ios::binary);
            if (!outFile.is_open()) {
                rollback();
                outError = "Failed to create file in staging: " + destPath.u8string();
                return false;
            }

            char buffer[65536];
            uint64_t fileBytes = 0;
            while (zipStream.CanRead()) {
                zipStream.Read(buffer, sizeof(buffer));
                size_t bytesRead = zipStream.LastRead();
                if (bytesRead == 0) break;

                fileBytes += bytesRead;
                totalExpandedBytes += bytesRead;

                if (fileBytes > 50 * 1024 * 1024) {
                    outFile.close();
                    rollback();
                    outError = "Decompressed file exceeds 50 MB bound: " + rawName;
                    return false;
                }

                if (totalExpandedBytes > 150 * 1024 * 1024) {
                    outFile.close();
                    rollback();
                    outError = "Total decompressed size exceeds 150 MB bound";
                    return false;
                }

                outFile.write(buffer, bytesRead);
            }
            outFile.flush();
            if(!outFile){outError="Incomplete ZIP output write/flush: "+rawName;return false;}
            outFile.close();
            if(outFile.fail()){outError="Cannot close ZIP output: "+rawName;return false;}
            PackStorage::Handle durable(CreateFileW(PackStorage::NativePath(destPath).c_str(),GENERIC_WRITE,
                FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!durable.Valid()||!PackStorage::Attributes(durable.Get(),false,outError)||!FlushFileBuffers(durable.Get())) {
                if(outError.empty())outError=PackStorage::WinError("Cannot durably flush ZIP output");return false;
            }
            if(!durable.Close(outError))return false;
            if(zipStream.GetLastError()!=wxSTREAM_NO_ERROR && zipStream.GetLastError()!=wxSTREAM_EOF) {
                outError="ZIP entry decompression/CRC failure: "+rawName;return false;
            }

            if (declaredSize >= 0 && fileBytes != static_cast<uint64_t>(declaredSize)) {
                rollback();
                outError = "ZIP entry decompression size mismatch for: " + rawName;
                return false;
            }
        }
    }

    if(zipStream.GetLastError()!=wxSTREAM_NO_ERROR && zipStream.GetLastError()!=wxSTREAM_EOF) {
        outError="ZIP archive ended with a stream/CRC failure";return false;
    }
    fs::path manifestPath = stagingRoot / "manifest.json";
    if (!fs::exists(manifestPath, ec)) {
        rollback();
        outError = "Missing manifest.json in root of extracted ZIP archive";
        return false;
    }

    PackManifest manifest;
    if (!ManifestJson::ReadFromFile(manifestPath.u8string(), manifest, outError)) {
        rollback();
        return false;
    }

    std::unordered_set<std::string> declaredFilePaths;
    declaredFilePaths.insert(ToLower("manifest.json"));
    if (!manifest.previewImagePath.empty()) {
        declaredFilePaths.insert(ToLower(fs::u8path(manifest.previewImagePath).generic_u8string()));
    }
    for (const auto& a : manifest.assets) {
        declaredFilePaths.insert(ToLower(fs::u8path(a.relativePath).generic_u8string()));
    }

    for (const auto& dirEntry : fs::recursive_directory_iterator(stagingRoot, ec)) {
        if (dirEntry.is_regular_file()) {
            std::string relPath = fs::relative(dirEntry.path(), stagingRoot).generic_u8string();
            if (declaredFilePaths.find(ToLower(relPath)) == declaredFilePaths.end()) {
                rollback();
                outError = "Undeclared payload file prohibited in archive: " + relPath;
                return false;
            }
        }
    }

    PackValidationResult stageVal = PackValidator::ValidateDirectoryPack(stagingRoot.u8string(), manifest);
    if (!stageVal.valid) {
        rollback();
        outError = "Extracted pack verification failed: " + stageVal.errorReason;
        return false;
    }

    std::vector<uint8_t> manifestBytes;
    std::string mErr;
    if(!ReadBounded(manifestPath, kMaxManifestBytes, 0, false, manifestBytes, mErr)) {
        outError="Cannot capture extracted manifest: "+mErr;return false;
    }

    stageGuards.reset();
    if (!CommitStagedPackLocked(canonicalRoot, stagingRoot, manifest, txId, manifestBytes, outError)) {
        rollback();
        return false;
    }

    return true;
}
catch(const std::exception& ex){outError=std::string("Pack operation failed safely: ")+ex.what();return false;}
catch(...){outError="Pack operation failed safely with an unexpected error";return false;}

std::unique_ptr<LoadedPack> PackManager::LoadPack(
    const std::string& packDirectory,
    std::string& outError)
{
    outError.clear();
    fs::path p=fs::u8path(packDirectory);
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        outError = "Pack directory does not exist: " + packDirectory;
        return nullptr;
    }

    fs::path manifestPath = p / "manifest.json";
    if (!fs::exists(manifestPath, ec)) {
        outError = "Missing manifest.json in pack directory: " + packDirectory;
        return nullptr;
    }

    const auto source = CaptureVerifiedSource(p, outError);
    if (!source) return nullptr;

    auto loaded = std::make_unique<LoadedPack>();
    loaded->manifest = source->manifest;
    loaded->packDirectory = source->root.u8string();
    loaded->snapshot_ = source;

    return loaded;
}

std::vector<PackManifest> PackManager::EnumeratePacks(const std::string& targetLibraryRoot) {
    std::vector<PackManifest> results;
    std::string valErr;
    if (!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),valErr) || !PackValidator::ValidateLibraryRoot(targetLibraryRoot, valErr)) {
        return results;
    }
    fs::path root = GetCanonicalLibraryPath(targetLibraryRoot);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return results;
    }

    for (const auto& packIdEntry : fs::directory_iterator(root, ec)) {
        if (!packIdEntry.is_directory()) continue;
        std::string packIdName = packIdEntry.path().filename().u8string();
        if (packIdName == ".staging" || packIdName.empty() || packIdName[0] == '.') continue;

        std::string idErr;
        if (!PackValidator::ValidateIdentifier(packIdName, false, idErr)) {
            continue;
        }

        for (const auto& versionEntry : fs::directory_iterator(packIdEntry.path(), ec)) {
            if (!versionEntry.is_directory()) continue;
            std::string verName = versionEntry.path().filename().u8string();
            std::string verErr;
            if (!PackValidator::ValidateIdentifier(verName, true, verErr)) {
                continue;
            }

            std::string descErr;
            if (!PackValidator::ValidateOwnedDescendant(root, versionEntry.path(), descErr)) {
                continue;
            }

            fs::path manifestPath = versionEntry.path() / "manifest.json";
            if (fs::exists(manifestPath, ec)) {
                PackManifest m;
                std::string err;
                if (ManifestJson::ReadFromFile(manifestPath.u8string(), m, err) &&
                    m.IsValidBasic() &&
                    m.packId == packIdName &&
                    m.version == verName) {
                    PackValidationResult v = PackValidator::ValidateDirectoryPack(versionEntry.path().u8string(), m);
                    if (v.valid) {
                        results.push_back(m);
                    }
                }
            }
        }
    }

    std::sort(results.begin(), results.end(), [](const PackManifest& a, const PackManifest& b) {
        if (a.packId != b.packId) return a.packId < b.packId;
        return a.version < b.version;
    });

    return results;
}

std::vector<PackCatalogueEntry> PackManager::GetCatalogue(const std::string& targetLibraryRoot) {
    std::vector<PackCatalogueEntry> catalogue;
    std::string valErr;
    if (!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),valErr) || !PackValidator::ValidateLibraryRoot(targetLibraryRoot, valErr)) {
        return catalogue;
    }
    fs::path root = GetCanonicalLibraryPath(targetLibraryRoot);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return catalogue;
    }

    for (const auto& packIdEntry : fs::directory_iterator(root, ec)) {
        if (!packIdEntry.is_directory()) continue;
        std::string packIdName = packIdEntry.path().filename().u8string();
        if (packIdName == ".staging" || packIdName.empty() || packIdName[0] == '.') continue;

        std::string idErr;
        if (!PackValidator::ValidateIdentifier(packIdName, false, idErr)) {
            continue;
        }

        for (const auto& versionEntry : fs::directory_iterator(packIdEntry.path(), ec)) {
            if (!versionEntry.is_directory()) continue;
            std::string verName = versionEntry.path().filename().u8string();
            std::string verErr;
            if (!PackValidator::ValidateIdentifier(verName, true, verErr)) {
                continue;
            }

            std::string descErr;
            if (!PackValidator::ValidateOwnedDescendant(root, versionEntry.path(), descErr)) {
                continue;
            }

            PackCatalogueEntry catEntry;
            catEntry.packId = packIdName;
            catEntry.version = verName;

            fs::path manifestPath = versionEntry.path() / "manifest.json";
            if (!fs::exists(manifestPath, ec)) {
                catEntry.integrityState = PackIntegrityState::MissingFile;
                catEntry.actionableError = "Missing manifest.json in pack directory";
                catEntry.isUsable = false;
                catalogue.push_back(catEntry);
                continue;
            }

            PackManifest m;
            std::string mErr;
            if (!ManifestJson::ReadFromFile(manifestPath.u8string(), m, mErr)) {
                catEntry.integrityState = PackIntegrityState::CorruptManifest;
                catEntry.actionableError = "Corrupt manifest: " + mErr;
                catEntry.isUsable = false;
                catalogue.push_back(catEntry);
                continue;
            }

            catEntry.displayName = m.displayName;
            catEntry.description = m.description;
            catEntry.author = m.author;
            catEntry.assets = m.assets;

            if (m.packId != packIdName || m.version != verName) {
                catEntry.integrityState = PackIntegrityState::SecurityViolation;
                catEntry.actionableError = "Installed directory identity mismatch with manifest";
                catEntry.isUsable = false;
                catalogue.push_back(catEntry);
                continue;
            }

            std::string captureError;
            auto loaded=CaptureVerifiedSource(versionEntry.path(),captureError);
            if(!loaded){catEntry.integrityState=PackIntegrityState::CorruptAsset;
                catEntry.actionableError=captureError;catEntry.isUsable=false;catalogue.push_back(catEntry);continue;}
            catEntry.previewAssetId=loaded->previewAssetId;
            catEntry.integrityState=PackIntegrityState::Valid;catEntry.isUsable=true;
            catalogue.push_back(catEntry);
        }
    }

    std::sort(catalogue.begin(), catalogue.end(), [](const PackCatalogueEntry& a, const PackCatalogueEntry& b) {
        if (a.packId != b.packId) return a.packId < b.packId;
        return a.version < b.version;
    });

    return catalogue;
}

bool PackManager::UninstallPack(const std::string& targetLibraryRoot,const std::string& packId,
    const std::string& version,std::string& error) try {
    std::lock_guard<std::mutex> lock(g_packManagerMutex);error.clear();
    if(!PackStorage::OriginalAbsolute(fs::u8path(targetLibraryRoot),error)||
       !PackValidator::ValidateLibraryRoot(targetLibraryRoot,error)||
       !PackValidator::ValidateIdentifier(packId,false,error)||
       (!version.empty()&&!PackValidator::ValidateIdentifier(version,true,error)))return false;
    const auto root=GetCanonicalLibraryPath(targetLibraryRoot);ProcessLibraryLock process(root);
    if(!process.IsLocked()){error="Cannot acquire library lock for uninstall";return false;}
    PackStorage::LibraryOwner owner;if(!owner.Open(root,false,error))return false;OwnerScope scope(owner);
    Checkpoint(PackTransactionPhase::LockAcquired);
    if(!RecoverInterruptedTransactionsLocked(root,error))return false;
    if(!owner.HoldDirectory(root/packId,false,error))return false;
    std::vector<std::pair<fs::path,std::unique_ptr<PackStorage::OwnedTree>>> verified;
    auto capture=[&](const fs::path& path,const std::string& expectedVersion)->bool {
        auto tree=std::make_unique<PackStorage::OwnedTree>();std::shared_ptr<VerifiedPackSnapshot> snap;
        if(!tree->Open(path,error)||!SnapshotFromTree(*tree,path,snap,error))return false;
        if(snap->manifest.packId!=packId||snap->manifest.version!=expectedVersion){error="Uninstall manifest identity mismatch";return false;}
        verified.emplace_back(path,std::move(tree));return true;
    };
    if(!version.empty()){if(!capture(root/packId/version,version))return false;}
    else {
        std::error_code ec;fs::directory_iterator it(root/packId,ec),end;
        if(ec){error="Cannot enumerate pack for uninstall: "+ec.message();return false;}
        bool foreign=false;
        for(;it!=end;it.increment(ec)){
            if(ec){error="Cannot finish uninstall enumeration";return false;}
            const auto name=it->path().filename().u8string();std::string invalid;
            if(!PackValidator::ValidateIdentifier(name,true,invalid)){foreign=true;continue;}
            // Missing/malformed/wrong identity is never deletion authority.
            if(!capture(it->path(),name))return false;
        }
        if(ec){error=ec.message();return false;}
        if(foreign){error="Unowned entries exist beside installed versions; nothing removed";return false;}
    }
    if(verified.empty()){error="No fully verified installed versions to uninstall";return false;}
    for(auto& entry:verified)if(!entry.second->Remove(error))return false;
    error.clear();return true;
}
catch(const std::exception& ex){error=std::string("Pack operation failed safely: ")+ex.what();return false;}
catch(...){error="Pack operation failed safely with an unexpected error";return false;}

void PackManager::SetTransactionCheckpoint(PackTransactionCheckpoint cb,void* context) {
    g_checkpointContext.store(context);g_checkpoint.store(cb);
}
void PackManager::SetLockTimeoutForTests(uint32_t ms){g_lockTimeout.store(ms?ms:1);}
uint32_t PackManager::GetLastProcessLockResult(){return g_lastLockResult.load();}

} // namespace PrismForge::Effects::Packs
