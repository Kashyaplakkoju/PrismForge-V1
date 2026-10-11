#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "systemcamera.h"
#include "mf_async_reader.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PrismForge::Capture {

class MfDeviceEnumerator {
public:
    static BackendEnumerationResult EnumerateDevices(std::vector<CaptureDevice>& outDevices);
    static std::vector<CaptureMode> QueryCapabilities(const std::string& symbolicLink);
};

class MfCaptureBackend : public SystemCameraSource::ICaptureBackend {
public:
    MfCaptureBackend(
        SystemCameraSource::FrameCallback frameCb,
        SystemCameraSource::StatusCallback statusCb);
    ~MfCaptureBackend() override;

    bool Open(const DeviceRoute& route, const CaptureMode* requestedMode) override;
    void Cancel() override;
    void Close() override;

    CaptureMode GetDeliveredMode() const override { return m_deliveredMode; }
    CaptureMode GetNativeMode() const override { return m_nativeMode; }
    bool IsWorkerThread() const override { return std::this_thread::get_id() == m_workerThreadId; }

private:
    void WorkerLoop(const std::string& symbolicLink, const CaptureMode* requestedMode);
    bool NegotiateMediaType(
        IMFSourceReader* pReader,
        const CaptureMode* requestedMode,
        CaptureMode& outNativeMode,
        CaptureMode& outDeliveredMode,
        bool& outIsCompressedMjpeg,
        bool& outIsCompressedH264);

    SystemCameraSource::FrameCallback m_frameCallback;
    SystemCameraSource::StatusCallback m_statusCallback;

    std::atomic<bool> m_cancelling{false};
    std::thread m_workerThread;
    std::thread::id m_workerThreadId;

    LONG m_defaultStride = 0;
    CaptureMode m_nativeMode{};
    CaptureMode m_deliveredMode{};

    AVColorRange m_colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace m_colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic m_colorTrc = AVCOL_TRC_UNSPECIFIED;

    mutable std::mutex m_readerMutex;
    std::shared_ptr<MfReadState> m_readState = std::make_shared<MfReadState>();
    IMFSourceReader* m_pReader = nullptr; // Protected by m_readerMutex
};

} // namespace PrismForge::Capture
