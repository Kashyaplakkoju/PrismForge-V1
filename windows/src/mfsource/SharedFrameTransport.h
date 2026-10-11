#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#define PRISMFORGE_SHARED_MEM_NAME L"Global\\PrismForge_SharedMem"
#define PRISMFORGE_MUTEX_NAME L"Global\\PrismForge_Mutex"
#define PRISMFORGE_MAX_WIDTH 3840
#define PRISMFORGE_MAX_HEIGHT 2160
#define PRISMFORGE_MAX_FRAME_SIZE (PRISMFORGE_MAX_WIDTH * PRISMFORGE_MAX_HEIGHT * 3 / 2)

// Keep the existing on-wire layout and mapping size: deployed readers use this ABI.
#pragma pack(push, 1)
struct SharedMemHeader {
    UINT32 width, height, stride, frameSize;
    UINT64 frameIndex;
    UINT32 isStreaming;
    UINT32 reserved[3];
    UINT8 data[1]; // tightly packed limited-range BT.709 NV12
};
#pragma pack(pop)
static_assert(offsetof(SharedMemHeader, data) == 40 && sizeof(SharedMemHeader) == 41,
    "Shared camera transport ABI changed");

namespace PrismForge::CameraTransport {
inline constexpr DWORD MappingSize = sizeof(SharedMemHeader) + PRISMFORGE_MAX_FRAME_SIZE;
inline constexpr UINT32 ProtocolTag = 0x31534650; // PFS1, in the previously reserved words
inline constexpr ULONGLONG MaxFrameAgeMs = 1500;

inline bool FrameSize(UINT32 width, UINT32 height, DWORD& bytes) {
    bytes = 0;
    if (!width || !height || (width & 1) || (height & 1) ||
        width > PRISMFORGE_MAX_WIDTH || height > PRISMFORGE_MAX_HEIGHT) return false;
    bytes = width * height * 3 / 2; // bounded above, so no overflow
    return true;
}

struct Rectangle { int width, height, x, y; };
inline Rectangle Fit(UINT32 sourceW, UINT32 sourceH, UINT32 targetW, UINT32 targetH) {
    const double scale = (double(targetW) / sourceW < double(targetH) / sourceH)
        ? double(targetW) / sourceW : double(targetH) / sourceH;
    const int width = (int(sourceW * scale) & ~1) < 2 ? 2 : (int(sourceW * scale) & ~1);
    const int height = (int(sourceH * scale) & ~1) < 2 ? 2 : (int(sourceH * scale) & ~1);
    return {width, height, ((int(targetW) - width) / 2) & ~1, ((int(targetH) - height) / 2) & ~1};
}

struct Names {
    std::wstring primaryMapping = PRISMFORGE_SHARED_MEM_NAME;
    std::wstring primaryMutex = PRISMFORGE_MUTEX_NAME;
    std::wstring fallbackMapping = L"Local\\PrismForge_SharedMem";
    std::wstring fallbackMutex = L"Local\\PrismForge_Mutex";
};

class MutexLock {
public:
    explicit MutexLock(HANDLE handle, DWORD timeout = 5) : _handle(handle) {
        if (!_handle) { _error = ERROR_INVALID_HANDLE; return; }
        _result = WaitForSingleObject(_handle, timeout);
        if (_result == WAIT_FAILED) { _error = GetLastError(); if (!_error) _error = ERROR_GEN_FAILURE; }
    }
    ~MutexLock() { if (Owned()) ReleaseMutex(_handle); }
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
    bool Owned() const { return _result == WAIT_OBJECT_0 || _result == WAIT_ABANDONED; }
    bool Abandoned() const { return _result == WAIT_ABANDONED; }
    HRESULT Status() const {
        if (Owned()) return S_OK;
        return _result == WAIT_TIMEOUT ? E_PENDING : HRESULT_FROM_WIN32(_error);
    }
private:
    HANDLE _handle;
    DWORD _result = WAIT_FAILED;
    DWORD _error = ERROR_INVALID_HANDLE;
};

class Mapping {
public:
    Mapping() = default;
    ~Mapping() { Close(); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    void Swap(Mapping& other) noexcept {
        std::swap(map, other.map); std::swap(mutex, other.mutex); std::swap(header, other.header);
    }
    bool Open(const std::wstring& mappingName, const std::wstring& mutexName, bool create) {
        Close();
        if (mappingName.empty() || mutexName.empty() || mappingName == mutexName) return false;
        SECURITY_DESCRIPTOR descriptor;
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE)) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName.c_str());
        if (!map && create) map = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes,
            PAGE_READWRITE, 0, MappingSize, mappingName.c_str());
        if (!map) return false;
        // A fixed requested length rejects an existing undersized mapping.
        header = static_cast<SharedMemHeader*>(MapViewOfFile(map,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, MappingSize));
        if (header) {
            mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
            if (!mutex && create) mutex = CreateMutexW(&attributes, FALSE, mutexName.c_str());
        }
        if (!header || !mutex) { Close(); return false; }
        return true;
    }
    void Close() noexcept {
        if (header) UnmapViewOfFile(header);
        if (map) CloseHandle(map);
        if (mutex) CloseHandle(mutex);
        header = nullptr; map = nullptr; mutex = nullptr;
    }
    HANDLE map = nullptr;
    HANDLE mutex = nullptr;
    SharedMemHeader* header = nullptr;
};
}
