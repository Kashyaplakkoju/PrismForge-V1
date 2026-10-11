#include <sstream>
#include "pack_validator.h"
#include "sha256.h"
#include "pack_preview_decoder.h"

#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace PrismForge::Effects::Packs {

namespace fs = std::filesystem;

namespace {

std::string ToLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ToUpper(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool IsValidSha256Hex(const std::string& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool IsWindowsReservedDeviceName(const std::string& name) {
    std::string upper = ToUpper(name);
    size_t dotPos = upper.find('.');
    std::string stem = (dotPos != std::string::npos) ? upper.substr(0, dotPos) : upper;

    static const std::unordered_set<std::string> reservedNames = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    return reservedNames.find(stem) != reservedNames.end() || reservedNames.find(upper) != reservedNames.end();
}

bool IsReservedControlName(const std::string& name) {
    std::string lower = ToLower(name);
    static const std::unordered_set<std::string> reserved = {
        ".staging", "staging", "backup", "temp", ".old_tmp", "manifest", "manifest.json", "preview.raw", ".lock"
    };
    return reserved.find(lower) != reserved.end();
}

bool IsReparsePointOrSymlink(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(p, ec)) {
        return true;
    }
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#endif
    return false;
}

bool ValidateSemVerCore(const std::string& semver, std::string& outError) {
    // Format: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
    if (semver.empty()) {
        outError = "SemVer version cannot be empty";
        return false;
    }

    size_t plusPos = semver.find('+');
    std::string buildMetadata;
    std::string withoutBuild = semver;
    if (plusPos != std::string::npos) {
        buildMetadata = semver.substr(plusPos + 1);
        withoutBuild = semver.substr(0, plusPos);
        if (buildMetadata.empty()) {
            outError = "Empty build metadata after '+' in SemVer";
            return false;
        }
        // Validate build identifiers
        std::stringstream ss(buildMetadata);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (part.empty()) { outError = "Empty segment in build metadata"; return false; }
            for (char c : part) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
                    outError = "Invalid character in build metadata: " + std::string(1, c);
                    return false;
                }
            }
        }
    }

    size_t dashPos = withoutBuild.find('-');
    std::string prerelease;
    std::string core = withoutBuild;
    if (dashPos != std::string::npos) {
        prerelease = withoutBuild.substr(dashPos + 1);
        core = withoutBuild.substr(0, dashPos);
        if (prerelease.empty()) {
            outError = "Empty prerelease tag after '-' in SemVer";
            return false;
        }
        // Validate prerelease identifiers
        std::stringstream ss(prerelease);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (part.empty()) { outError = "Empty segment in prerelease identifier"; return false; }
            bool allDigits = true;
            for (char c : part) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
                    outError = "Invalid character in prerelease: " + std::string(1, c);
                    return false;
                }
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    allDigits = false;
                }
            }
            if (allDigits && part.size() > 1 && part[0] == '0') {
                outError = "Leading zero prohibited in numeric prerelease identifier: " + part;
                return false;
            }
        }
    }

    // Now validate core: MAJOR.MINOR.PATCH
    std::stringstream ss(core);
    std::string segment;
    std::vector<std::string> parts;
    while (std::getline(ss, segment, '.')) {
        parts.push_back(segment);
    }

    if (parts.size() != 3) {
        outError = "SemVer core must contain exactly three segments (MAJOR.MINOR.PATCH): " + core;
        return false;
    }

    for (const auto& p : parts) {
        if (p.empty()) {
            outError = "Empty numeric component in SemVer core";
            return false;
        }
        for (char c : p) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                outError = "Non-digit character in SemVer core: '" + std::string(1, c) + "' in " + p;
                return false;
            }
        }
        if (p.size() > 1 && p[0] == '0') {
            outError = "Leading zeros are strictly prohibited in SemVer components: " + p;
            return false;
        }
    }

    return true;
}

} // anonymous namespace

bool PackValidator::ValidateIdentifier(
    const std::string& id,
    bool isVersion,
    std::string& outError)
{
    if (id.empty()) {
        outError = (isVersion ? "Version" : "Pack ID") + std::string(" cannot be empty");
        return false;
    }

    if (id.size() > 128) {
        outError = (isVersion ? "Version" : "Pack ID") + std::string(" exceeds maximum length of 128 characters");
        return false;
    }

    if (id == "." || id == "..") {
        outError = "Identifier cannot be '.' or '..'";
        return false;
    }

    if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.find(':') != std::string::npos) {
        outError = "Identifier cannot contain path separators ('/', '\\', or ':')";
        return false;
    }

    for (unsigned char c : id) {
        if (c < 0x20 || c == 0x7F) {
            outError = "Identifier contains illegal control byte";
            return false;
        }
    }

    if (id.back() == ' ' || id.back() == '.') {
        outError = "Identifier cannot end with a trailing space or dot";
        return false;
    }

    if (IsWindowsReservedDeviceName(id)) {
        outError = "Identifier matches a Windows reserved device name: " + id;
        return false;
    }

    if (IsReservedControlName(id)) {
        outError = "Identifier matches a reserved system or staging name: " + id;
        return false;
    }

    if (!isVersion) {
        // Reverse-DNS style (e.g. "com.prismforge.packs.warmstudio")
        if (id.front() == '.' || id.front() == '-' || id.front() == '_' ||
            id.back() == '-' || id.back() == '_') {
            outError = "packId cannot start or end with '.', '-', or '_'";
            return false;
        }

        if (id.find("..") != std::string::npos) {
            outError = "packId cannot contain consecutive dots ('..')";
            return false;
        }

        int dotCount = 0;
        for (char c : id) {
            if (c == '.') {
                dotCount++;
            } else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                outError = "packId contains invalid character: '" + std::string(1, c) + "'";
                return false;
            }
        }

        if (dotCount < 1) {
            outError = "packId must be a reverse-DNS styled identifier with at least two segments (e.g. 'com.company.pack')";
            return false;
        }
    } else {
        // Strict SemVer 2.0.0
        return ValidateSemVerCore(id, outError);
    }

    return true;
}

namespace {
// Validate the caller's original spelling BEFORE canonicalization: resolving a
// junction first would erase the evidence that this was an unowned alias.
bool ValidateOriginalAbsolute(const fs::path& p, bool directory, std::string& error) {
    error.clear();
    const auto raw=p.native();
    if(raw.size()<3||raw.size()>260||!((raw[0]>=L'A'&&raw[0]<=L'Z')||(raw[0]>=L'a'&&raw[0]<=L'z'))||raw[1]!=L':'||(raw[2]!=L'\\'&&raw[2]!=L'/')) {
        error="Managed path must be an absolute drive-letter path";return false;
    }
    for(size_t i=0;i<raw.size();++i) {
        wchar_t c=raw[i];
        if(c<32||c==127||c==L'*'||c==L'?'||c==L'<'||c==L'>'||c==L'|'||c==L'\"'||(c==L':'&&i!=1)) {
            error="Illegal managed path character";return false;
        }
    }
    fs::path current=p.root_path();
    std::vector<fs::path> parts;
    for(const auto& part:p.relative_path())if(!part.empty())parts.push_back(part);
    for(size_t i=0;i<parts.size();++i) {
        std::string componentError;
        if(!PackValidator::ValidatePathComponent(parts[i].u8string(),componentError)) {error=componentError;return false;}
        current/=parts[i];
        DWORD attrs=GetFileAttributesW(current.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES) {
            DWORD code=GetLastError();
            if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND) {error="Cannot inspect managed path attributes: "+std::to_string(code);return false;}
            continue;
        }
        if(attrs&FILE_ATTRIBUTE_REPARSE_POINT) {error="Managed path contains a reparse point: "+current.u8string();return false;}
        if((directory||i+1<parts.size())&&!(attrs&FILE_ATTRIBUTE_DIRECTORY)) {error="Managed path ancestor/root is not a directory";return false;}
    }
    return true;
}
}
bool PackValidator::ValidateLibraryRoot(const std::string& rootPath,std::string& outError) {
    try { return ValidateOriginalAbsolute(fs::u8path(rootPath),true,outError); }
    catch(const std::exception& e) {outError=std::string("Invalid library root: ")+e.what();return false;}
}

bool PackValidator::ValidateRelativePath(
    const std::string& path,
    PackAssetType assetType,
    std::string& outError)
{
    if (path.empty()) {
        outError = "Relative path is empty";
        return false;
    }

    outError.clear();
    if (path.back() == '/' || path.back() == '\\' || path.find("//") != std::string::npos ||
        path.find("\\\\") != std::string::npos || path.find("/\\") != std::string::npos || path.find("\\/") != std::string::npos) {
        outError = "Empty relative path component prohibited"; return false;
    }
    if (path.size() > 260) {
        outError = "Path exceeds maximum length of 260 characters";
        return false;
    }

    if (path.find(':') != std::string::npos) {
        outError = "Path contains drive letter or colon: " + path;
        return false;
    }

    if (path.front() == '/' || path.front() == '\\') {
        outError = "Path is absolute: " + path;
        return false;
    }

    for (unsigned char c : path) {
        if (c < 0x20 || c == 0x7F || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == '"') {
            outError = "Path contains illegal characters: " + path;
            return false;
        }
    }

    fs::path p;
    try {p=fs::u8path(path);}catch(const std::exception&){outError="Invalid UTF8 relative path";return false;}
    for (const auto& part : p) {
        std::string s = part.u8string();
        if (s == ".." || s == ".") {
            outError = "Path traversal component ('" + s + "') prohibited: " + path;
            return false;
        }
        if (s.back() == ' ' || s.back() == '.') {
            outError = "Path component has trailing dot or space: " + s;
            return false;
        }
        if (IsWindowsReservedDeviceName(s)) {
            outError = "Path component is a Windows reserved device name: " + s;
            return false;
        }
    }

    std::string ext = ToLower(p.extension().u8string());
    static const std::unordered_set<std::string> prohibitedExtensions = {
        ".exe", ".dll", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".py",
        ".sh", ".bin", ".sys", ".com", ".scr", ".msi", ".jar"
    };

    if (prohibitedExtensions.find(ext) != prohibitedExtensions.end()) {
        outError = "Prohibited executable/script file extension: " + ext;
        return false;
    }

    switch (assetType) {
        case PackAssetType::CubeLut:
            if (ext != ".cube") {
                outError = "CubeLut asset must have .cube extension: " + path;
                return false;
            }
            break;
        case PackAssetType::TrackedAccessory:
            if (ext != ".json" && ext != ".raw") {
                outError = "TrackedAccessory asset must have .json or .raw extension: " + path;
                return false;
            }
            break;
        case PackAssetType::ScreenOverlay:
            if (ext != ".json" && ext != ".raw" && ext != ".png") {
                outError = "ScreenOverlay asset must have .json, .raw, or .png extension: " + path;
                return false;
            }
            break;
        case PackAssetType::PresetConfig:
            if (ext != ".json") {
                outError = "PresetConfig asset must have .json extension: " + path;
                return false;
            }
            break;
        case PackAssetType::PreviewImage:
            if (ext != ".raw" && ext != ".png" && ext != ".bmp" && ext != ".jpg" && ext != ".jpeg") {
                outError = "PreviewImage asset must have .raw, .png, .bmp, or .jpg extension: " + path;
                return false;
            }
            break;
        default:
            outError = "Unknown asset type for path: " + path;
            return false;
    }

    return true;
}

bool PackValidator::ValidatePreviewImageBytes(const uint8_t* data,size_t size,
    const std::string& path,int& width,int& height,std::string& error) {
    return PackPreviewDecoder::ValidateAndGetDimensions(data,size,path,width,height,error);
}

PackValidationResult PackValidator::ValidateManifest(const PackManifest& manifest) {
    if (manifest.schemaVersion != 1) {
        return PackValidationResult::Failure("Unsupported schemaVersion: " + std::to_string(manifest.schemaVersion));
    }

    std::string idErr;
    if (!ValidateIdentifier(manifest.packId, false, idErr)) {
        return PackValidationResult::Failure("Invalid packId: " + idErr);
    }

    std::string verErr;
    if (!ValidateIdentifier(manifest.version, true, verErr)) {
        return PackValidationResult::Failure("Invalid version: " + verErr);
    }

    if (manifest.displayName.empty()) {
        return PackValidationResult::Failure("Manifest displayName cannot be empty");
    }

    if (manifest.displayName.size() > 128) {
        return PackValidationResult::Failure("Manifest displayName exceeds maximum length (128)");
    }

    if (manifest.assets.empty()) {
        return PackValidationResult::Failure("Manifest contains zero assets");
    }

    if (manifest.assets.size() > 50) {
        return PackValidationResult::Failure("Manifest exceeds maximum asset count (50)");
    }

    // Validate preview path if declared
    const PackAssetEntry* previewAsset = nullptr;
    if (!manifest.previewImagePath.empty()) {
        std::string err;
        if (!ValidateRelativePath(manifest.previewImagePath, PackAssetType::PreviewImage, err)) {
            return PackValidationResult::Failure("Invalid previewImagePath: " + err);
        }
        // Must match a declared PreviewImage asset
        for (const auto& a : manifest.assets) {
            if (a.assetType == PackAssetType::PreviewImage &&
                ToLower(fs::u8path(a.relativePath).lexically_normal().u8string()) ==
                ToLower(fs::u8path(manifest.previewImagePath).lexically_normal().u8string())) {
                previewAsset = &a;
                break;
            }
        }
        if (!previewAsset) {
            return PackValidationResult::Failure("Preview path is not a declared hashed PreviewImage asset");
        }
    }

    std::unordered_set<std::string> seenIds;
    std::unordered_set<std::string> seenPaths;
    uint64_t totalPackBytes = 0;

    for (const auto& a : manifest.assets) {
        if (a.assetId.empty()) {
            return PackValidationResult::Failure("Asset ID cannot be empty");
        }

        if (a.assetId.size() > 64) {
            return PackValidationResult::Failure("Asset ID exceeds maximum length (64): " + a.assetId);
        }

        std::string lowerId = ToLower(a.assetId);
        if (seenIds.find(lowerId) != seenIds.end()) {
            return PackValidationResult::Failure("Duplicate asset ID: " + a.assetId);
        }
        seenIds.insert(lowerId);

        std::string pathErr;
        if (!ValidateRelativePath(a.relativePath, a.assetType, pathErr)) {
            return PackValidationResult::Failure("Invalid asset path '" + a.relativePath + "': " + pathErr);
        }

        std::string lowerPath = ToLower(fs::u8path(a.relativePath).lexically_normal().generic_string());
        if (seenPaths.find(lowerPath) != seenPaths.end()) {
            return PackValidationResult::Failure("Duplicate asset relative path: " + a.relativePath);
        }
        seenPaths.insert(lowerPath);

        if (!IsValidSha256Hex(a.sha256Hex)) {
            return PackValidationResult::Failure("Invalid SHA-256 hash for asset '" + a.assetId + "': " + a.sha256Hex);
        }

        if (a.sizeBytes == 0) {
            return PackValidationResult::Failure("Asset '" + a.assetId + "' declared size cannot be 0");
        }

        uint64_t maxAssetLimit = (a.assetType == PackAssetType::PreviewImage) ? (10ULL * 1024 * 1024) : (50ULL * 1024 * 1024);
        if (a.sizeBytes > maxAssetLimit) {
            return PackValidationResult::Failure("Asset '" + a.assetId + "' exceeds maximum size limit");
        }

        totalPackBytes += a.sizeBytes;
    }

    if (totalPackBytes > 150ULL * 1024 * 1024) {
        return PackValidationResult::Failure("Total declared pack size exceeds limit (150 MB)");
    }

    return PackValidationResult::Success();
}

PackValidationResult PackValidator::ValidateDirectoryPack(
    const std::string& packDirectoryPath,
    const PackManifest& manifest)
{
    fs::path root;
    try {root=fs::u8path(packDirectoryPath);}catch(const std::exception&){return PackValidationResult::Failure("Invalid UTF8 pack directory");}
    std::error_code ec;

    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return PackValidationResult::Failure("Pack directory does not exist or is not a directory: " + packDirectoryPath);
    }

    // Reparse point / junction check on the root itself
    if (IsReparsePointOrSymlink(root)) {
        return PackValidationResult::Failure("Pack directory is a symbolic link or reparse point (junction): " + packDirectoryPath);
    }

    // Check all ancestors of pack directory
    fs::path curRoot;
    for (const auto& comp : root) {
        curRoot /= comp;
        if (curRoot == root.root_name() || curRoot == root.root_directory() || curRoot == root.root_path()) continue;
        if (fs::exists(curRoot, ec) && IsReparsePointOrSymlink(curRoot)) {
            return PackValidationResult::Failure("Pack directory ancestor is a symlink or reparse point: " + curRoot.u8string());
        }
    }

    // 1. Validate manifest structure
    PackValidationResult manifestRes = ValidateManifest(manifest);
    if (!manifestRes.valid) {
        return manifestRes;
    }

    fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec) {
        return PackValidationResult::Failure("Failed to resolve pack root path: " + ec.message());
    }

    // 2. Validate on-disk assets
    for (const auto& a : manifest.assets) {
        fs::path cur = root;
        fs::path relPath=fs::u8path(a.relativePath);

        for (const auto& part : relPath) {
            cur /= part;
            if (IsReparsePointOrSymlink(cur)) {
                return PackValidationResult::Failure("Symlinks and reparse points (junctions) are strictly prohibited: " + cur.u8string());
            }
        }

        fs::path assetPath = cur;

        if (!fs::exists(assetPath, ec) || !fs::is_regular_file(assetPath, ec)) {
            return PackValidationResult::Failure("Asset file missing or not a regular file: " + a.relativePath);
        }

        fs::path canonicalAsset = fs::weakly_canonical(assetPath, ec);
        if (ec || canonicalAsset.u8string().find(canonicalRoot.u8string()) != 0) {
            return PackValidationResult::Failure("Asset path escapes pack root: " + a.relativePath);
        }

        uint64_t actualSize = fs::file_size(assetPath, ec);
        if(ec || actualSize != a.sizeBytes) return PackValidationResult::Failure("Asset size mismatch: " + a.relativePath);
        if(a.assetType == PackAssetType::PreviewImage) {
            if(actualSize == 0 || actualSize > PackPreviewDecoder::kMaxEncodedSizeBytes)
                return PackValidationResult::Failure("Preview exceeds encoded byte limit: " + a.relativePath);
            HANDLE file=CreateFileW(assetPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            if(file==INVALID_HANDLE_VALUE)return PackValidationResult::Failure("Cannot open immutable preview: "+a.relativePath);
            struct FileCloser {HANDLE h;~FileCloser(){CloseHandle(h);}} close{file};
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
            if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||
                (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
                !GetFileSizeEx(file,&size)||size.QuadPart<0||uint64_t(size.QuadPart)!=actualSize)
                return PackValidationResult::Failure("Preview file identity/size changed: "+a.relativePath);
            std::vector<uint8_t> bytes;
            try {bytes.resize(size_t(actualSize));}catch(const std::bad_alloc&){return PackValidationResult::Failure("Preview allocation failed");}
            size_t offset=0;
            while(offset<bytes.size()) {DWORD read=0;DWORD requested=DWORD(std::min<size_t>(bytes.size()-offset,1024*1024));
                if(!ReadFile(file,bytes.data()+offset,requested,&read,nullptr)||read==0)return PackValidationResult::Failure("Incomplete preview read: "+a.relativePath);
                offset+=read;}
            uint8_t extra=0;DWORD read=0;if(!ReadFile(file,&extra,1,&read,nullptr)||read!=0)return PackValidationResult::Failure("Preview grew during read");
            auto decoded=PackPreviewDecoder::DecodePreviewAsset(a,bytes.data(),bytes.size());
            if(!decoded.success)return PackValidationResult::Failure("Preview validation failed for '"+a.relativePath+"': "+decoded.errorReason);
        } else {
            std::string hashError;
            std::string actualHash=Sha256::ComputeFileHexBounded(assetPath.u8string(),a.sizeBytes,50ULL*1024*1024,hashError);
            if(actualHash.empty())return PackValidationResult::Failure("Asset bounded hash failed: "+a.relativePath+": "+hashError);
            if(ToLower(actualHash)!=ToLower(a.sha256Hex))
                return PackValidationResult::Failure("Asset SHA256 mismatch: "+a.relativePath);
        }
    }

    return PackValidationResult::Success();
}


bool PackValidator::ValidatePathComponent(
    const std::string& component,
    std::string& outError)
{
    if (component.empty()) {
        outError = "Path component cannot be empty";
        return false;
    }
    if (component.size() > 128) {
        outError = "Path component exceeds maximum length";
        return false;
    }
    if (component == "." || component == "..") {
        outError = "Path component cannot be '.' or '..'";
        return false;
    }
    if (component.find('/') != std::string::npos || component.find('\\') != std::string::npos || component.find(':') != std::string::npos) {
        outError = "Path component cannot contain path separators";
        return false;
    }
    for (unsigned char c : component) {
        if (c < 0x20 || c == 0x7F) {
            outError = "Path component contains illegal control byte";
            return false;
        }
    }
    if (component.back() == ' ' || component.back() == '.') {
        outError = "Path component cannot end with a trailing space or dot";
        return false;
    }
    if (IsWindowsReservedDeviceName(component)) {
        outError = "Path component matches a Windows reserved device name: " + component;
        return false;
    }
    return true;
}

bool PackValidator::ValidateOwnedDescendant(const fs::path& root,const fs::path& candidate,std::string& outError) {
    try {
        if(!ValidateOriginalAbsolute(root,true,outError)||!ValidateOriginalAbsolute(candidate,false,outError))return false;
        auto normRoot=root.lexically_normal(),normCandidate=candidate.lexically_normal();
        // A trailing separator is not an ownership component.
        if(normRoot.filename().empty()&&normRoot!=normRoot.root_path())normRoot=normRoot.parent_path();
        if(normCandidate.filename().empty()&&normCandidate!=normCandidate.root_path())normCandidate=normCandidate.parent_path();
        auto ci=normCandidate.begin();
        for(auto ri=normRoot.begin();ri!=normRoot.end();++ri,++ci) {
            if(ci==normCandidate.end()||CompareStringOrdinal(ri->c_str(),-1,ci->c_str(),-1,TRUE)!=CSTR_EQUAL) {
                outError="Candidate escapes managed root";return false;
            }
        }
        if(ci==normCandidate.end()){outError="Candidate cannot equal managed root";return false;}
        outError.clear();return true;
    }catch(const std::exception& e){outError=std::string("Invalid owned descendant: ")+e.what();return false;}
}

} // namespace PrismForge::Effects::Packs
