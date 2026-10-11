#pragma once
#include <mutex>
#include "rtsp/ffmpeg.h"
#include "SharedFrameTransport.h"

class MFSharedMemWriter {
public:
    explicit MFSharedMemWriter(PrismForge::CameraTransport::Names names = {});
    ~MFSharedMemWriter();
    bool Initialize(int width = 1920, int height = 1080);
    // False means no complete frame was published (including bounded IPC contention).
    bool WriteFrame(const AVFrame* frame);
    void SetStreaming(bool streaming);
    void Close();
private:
    bool InitializeLocked(int width, int height);
    bool Convert(const AVFrame* frame);
    bool Publish(PrismForge::CameraTransport::Mapping& mapping);
    bool TryPublishUpgrade();
    PrismForge::CameraTransport::Names _names;
    PrismForge::CameraTransport::Mapping _mapping;
    std::mutex _mtx;
    int _targetWidth = 1920, _targetHeight = 1080;
    AVFrame* _nv12 = nullptr;
    AVFrame* _rgb = nullptr;
    SwsContext* _toRgb = nullptr;
    SwsContext* _toNv12 = nullptr;
    UINT64 _frameCounter = 0;
    bool _usingPrimary = false;
    int _upgradeCheckCounter = 0;
};
