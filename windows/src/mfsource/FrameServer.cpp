#include "FrameServer.h"
#include <cstring>
#include <new>

using namespace PrismForge::CameraTransport;

FrameServer::FrameServer(Names names) : _names(std::move(names)) {}

HRESULT FrameServer::Initialize() {
    std::lock_guard<std::mutex> lock(_mtx);
    if (_mapping.header) return S_OK;
    // Select an entire mapping/mutex pair; never mix Global and Local objects.
    if (!_mapping.Open(_names.primaryMapping, _names.primaryMutex, true) &&
        !_mapping.Open(_names.fallbackMapping, _names.fallbackMutex, true)) return E_FAIL;
    // New pagefile mappings are already zeroed. Do not reset an existing producer.
    return S_OK;
}

HRESULT FrameServer::GetLatestFrame(SharedFrameSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(_mtx);
    snapshot.isStreaming = false;
    if (!_mapping.header) return E_FAIL;
    MutexLock sharedLock(_mapping.mutex);
    if (!sharedLock.Owned()) return sharedLock.Status();
    if (sharedLock.Abandoned()) {
        // A producer died inside a publication. Discard possibly partial bytes.
        _mapping.header->isStreaming = 0;
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    const auto* header = _mapping.header;
    if (!header->isStreaming) return E_PENDING;
    DWORD bytes = 0;
    if (header->isStreaming != 1 || !FrameSize(header->width, header->height, bytes) ||
        header->stride != header->width || header->frameSize != bytes || !header->frameIndex)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG publishedAt = 0;
    if (header->reserved[0] == ProtocolTag)
        publishedAt = UINT64(header->reserved[1]) | (UINT64(header->reserved[2]) << 32);
    else if (!header->reserved[0]) {
        // Old producers have no heartbeat: allow only a bounded repeated-frame interval.
        if (header->frameIndex != _lastIndex) _observedAt = now;
        publishedAt = _observedAt;
    } else return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (publishedAt > now) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (now - publishedAt > MaxFrameAgeMs) return E_PENDING;
    snapshot.isStreaming = true;
    if (header->frameIndex == _lastIndex) return E_PENDING;
    try { snapshot.data.resize(bytes); }
    catch (const std::bad_alloc&) { snapshot.isStreaming = false; return E_OUTOFMEMORY; }
    std::memcpy(snapshot.data.data(), header->data, bytes);
    snapshot.width = header->width; snapshot.height = header->height;
    snapshot.frameIndex = header->frameIndex;
    _lastIndex = header->frameIndex;
    return S_OK;
}
