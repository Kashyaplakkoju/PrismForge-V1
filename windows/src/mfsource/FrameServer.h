#pragma once
#include "SharedFrameTransport.h"
#include <mutex>
#include <vector>

struct SharedFrameSnapshot {
    std::vector<BYTE> data;
    UINT32 width = 0, height = 0;
    UINT64 frameIndex = 0;
    bool isStreaming = false;
};

class FrameServer {
public:
    explicit FrameServer(PrismForge::CameraTransport::Names names = {});
    HRESULT Initialize();
    // On S_OK the caller owns pixels and matching metadata, independent of future writes.
    // On E_PENDING a repeated active frame may be reused; errors always mark standby.
    HRESULT GetLatestFrame(SharedFrameSnapshot& snapshot);
private:
    PrismForge::CameraTransport::Names _names;
    PrismForge::CameraTransport::Mapping _mapping;
    std::mutex _mtx;
    UINT64 _lastIndex = 0;
    ULONGLONG _observedAt = 0;
};
