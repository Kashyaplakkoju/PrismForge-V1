#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "systemcamera.h"
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PrismForge::Capture {

class DShowDeviceEnumerator {
public:
    static BackendEnumerationResult EnumerateDevices(std::vector<CaptureDevice>& outDevices);
    static std::vector<CaptureMode> QueryCapabilities(const std::string& routeId);
};

class GrabberCallbackHandler;

class DShowCaptureBackend : public SystemCameraSource::ICaptureBackend {
public:
    DShowCaptureBackend(
        SystemCameraSource::FrameCallback frameCb,
        SystemCameraSource::StatusCallback statusCb);
    ~DShowCaptureBackend() override;

    bool Open(const DeviceRoute& route, const CaptureMode* requestedMode) override;
    void Cancel() override;
    void Close() override;

    CaptureMode GetDeliveredMode() const override { return m_deliveredMode; }
    CaptureMode GetNativeMode() const override { return m_nativeMode; }
    bool IsWorkerThread() const override;

private:
    void WorkerLoop(const std::string& routeId, const CaptureMode* requestedMode);

    SystemCameraSource::FrameCallback m_frameCallback;
    SystemCameraSource::StatusCallback m_statusCallback;

    std::atomic<bool> m_cancelling{false};
    std::thread m_workerThread;
    std::thread::id m_workerThreadId;

    CaptureMode m_nativeMode{};
    CaptureMode m_deliveredMode{};

    mutable std::mutex m_controlMutex;
    IMediaControl* m_pMediaControl = nullptr; // Protected by m_controlMutex

    std::shared_ptr<GrabberCallbackHandler> m_callbackHandler;
};

} // namespace PrismForge::Capture
