#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dshow_capture.h"
#include "sample_converter.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <numeric>
#include <cmath>
#include <climits>

namespace PrismForge::Capture {

namespace {

// Standard DirectShow SampleGrabber definitions
static const IID IID_ISampleGrabber = { 0x6B652FFF, 0x11FE, 0x4fce, { 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };
static const IID IID_ISampleGrabberCB = { 0x0579154A, 0x2B53, 0x4994, { 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85 } };
static const CLSID CLSID_SampleGrabber = { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
static const CLSID CLSID_NullRenderer = { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

// DirectShow media subtype GUIDs
static const GUID MEDIASUBTYPE_NV12_DEF = { 0x3231564E, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MEDIASUBTYPE_YUY2_DEF = { 0x32595559, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MEDIASUBTYPE_MJPG_DEF = { 0x47504A4D, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MEDIASUBTYPE_H264_DEF = { 0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

struct ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE *pBuffer, long lBufferSize) = 0;
};

struct ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize, long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *pCallback, long WhichMethodToCallback) = 0;
};

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 1) return "";
    std::string str(sizeNeeded - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (sizeNeeded <= 1) return L"";
    std::wstring wstr(sizeNeeded - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], sizeNeeded);
    return wstr;
}

void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(static_cast<PVOID>(mt.pbFormat));
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* pmt) {
    if (pmt != nullptr) {
        FreeMediaType(*pmt);
        CoTaskMemFree(pmt);
    }
}

std::string DShowSubtypeToString(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_NV12_DEF) return "NV12";
    if (subtype == MEDIASUBTYPE_YUY2_DEF) return "YUY2";
    if (subtype == MEDIASUBTYPE_RGB32) return "RGB32";
    if (subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    if (subtype == MEDIASUBTYPE_MJPG_DEF) return "MJPG";
    if (subtype == MEDIASUBTYPE_H264_DEF) return "H264";
    return "UNKNOWN";
}

GUID StringToDShowSubtype(const std::string& fmt) {
    if (fmt == "NV12") return MEDIASUBTYPE_NV12_DEF;
    if (fmt == "YUY2") return MEDIASUBTYPE_YUY2_DEF;
    if (fmt == "RGB32") return MEDIASUBTYPE_RGB32;
    if (fmt == "RGB24") return MEDIASUBTYPE_RGB24;
    if (fmt == "MJPG") return MEDIASUBTYPE_MJPG_DEF;
    if (fmt == "H264") return MEDIASUBTYPE_H264_DEF;
    return GUID_NULL;
}

void RationalFpsFromAvgTime(REFERENCE_TIME avgTime, int& outNum, int& outDen) {
    if (avgTime <= 0) {
        outNum = 0;
        outDen = 1;
        return;
    }

    int64_t num = 10000000;
    int64_t den = avgTime;
    int64_t g = std::gcd(num, den);
    num /= g; den /= g;
    if (num > INT_MAX || den > INT_MAX) { outNum = 0; outDen = 1; return; }
    outNum = static_cast<int>(num);
    outDen = static_cast<int>(den);
}

CaptureErrorCategory ClassifyDShowError(HRESULT hr) {
    if (hr == E_ACCESSDENIED) {
        return CaptureErrorCategory::PrivacyAccessDenied;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_BUSY) || hr == static_cast<HRESULT>(0x800704C7L) ||
        hr == VFW_E_CANNOT_CONNECT) {
        return CaptureErrorCategory::Busy;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED) || hr == static_cast<HRESULT>(0x8007048FL) ||
        hr == VFW_E_NOT_CONNECTED) {
        return CaptureErrorCategory::DeviceRemoved;
    }
    if (hr == VFW_E_INVALIDMEDIATYPE || hr == VFW_E_TYPE_NOT_ACCEPTED) {
        return CaptureErrorCategory::UnsupportedFormat;
    }
    return CaptureErrorCategory::InternalFailure;
}

IPin* FindCapturePin(IBaseFilter* pFilter) {
    if (!pFilter) return nullptr;
    IEnumPins* pEnum = nullptr;
    if (FAILED(pFilter->EnumPins(&pEnum)) || !pEnum) return nullptr;

    IPin* pPin = nullptr;
    while (pEnum->Next(1, &pPin, nullptr) == S_OK) {
        PIN_DIRECTION dir;
        pPin->QueryDirection(&dir);
        if (dir == PINDIR_OUTPUT) {
            pEnum->Release();
            return pPin;
        }
        pPin->Release();
    }
    pEnum->Release();
    return nullptr;
}

} // namespace

class GrabberCallbackHandler : public ISampleGrabberCB {
public:
    GrabberCallbackHandler(
        SystemCameraSource::FrameCallback frameCb,
        SystemCameraSource::StatusCallback statusCb)
        : m_frameCallback(std::move(frameCb))
        , m_statusCallback(std::move(statusCb))
    {
        m_ownedFrame = av_frame_alloc();
    }

    ~GrabberCallbackHandler() {
        if (m_ownedFrame) {
            av_frame_free(&m_ownedFrame);
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_refCount;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = --m_refCount;
        return count;
    }

    STDMETHODIMP SampleCB(double, IMediaSample *) override {
        return S_OK;
    }

    STDMETHODIMP BufferCB(double SampleTime, BYTE *pBuffer, long lBufferSize) override {
        if (m_cancelling.load() || !pBuffer || lBufferSize <= 0) {
            return S_OK;
        }

        const int width = m_mode.width;
        const int height = m_mode.height;
        if (width <= 0 || width > 16384 || height <= 0 || height > 16384 || !std::isfinite(SampleTime) || std::abs(SampleTime) > 9.0e11) {
            return S_OK;
        }

        m_deliveryThreadId.store(std::this_thread::get_id());

        bool converted = false;
        AVFrame* frameToDeliver = nullptr;
        const int64_t pts = static_cast<int64_t>(SampleTime * 10000000.0);

        if (m_mode.pixelFormat == "MJPG" && m_mjpegDecoder) {
            frameToDeliver = m_mjpegDecoder->DecodePacket(pBuffer, lBufferSize, pts);
            converted = (frameToDeliver != nullptr);
        } else if (m_mode.pixelFormat == "H264" && m_h264Decoder) {
            frameToDeliver = m_h264Decoder->DecodePacket(pBuffer, lBufferSize, pts);
            converted = (frameToDeliver != nullptr);
        } else if (m_mode.pixelFormat == "NV12") {
            converted = SampleConverter::ConvertNv12ToFrame(
                pBuffer, lBufferSize,
                width, height,
                width, width,
                m_ownedFrame,
                m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
            frameToDeliver = m_ownedFrame;
        } else if (m_mode.pixelFormat == "YUY2") {
            converted = SampleConverter::ConvertYuy2ToFrame(
                pBuffer, lBufferSize,
                width, height,
                width * 2,
                m_ownedFrame,
                m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
            frameToDeliver = m_ownedFrame;
        } else if (m_mode.pixelFormat == "RGB32") {
            converted = SampleConverter::ConvertRgb32ToFrame(
                pBuffer, lBufferSize,
                width, height,
                width * 4, m_bottomUp,
                m_ownedFrame,
                m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
            frameToDeliver = m_ownedFrame;
        } else if (m_mode.pixelFormat == "RGB24") {
            const int stride = (width * 3 + 3) & ~3;
            converted = SampleConverter::ConvertRgb24ToFrame(
                pBuffer, lBufferSize,
                width, height,
                stride, m_bottomUp,
                m_ownedFrame,
                m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
            frameToDeliver = m_ownedFrame;
        }

        if (converted && frameToDeliver && m_frameCallback && !m_cancelling.load()) {
            frameToDeliver->pts = pts;
            frameToDeliver->pkt_dts = pts;
            frameToDeliver->time_base = AVRational{1, 10000000};

            try {
                m_frameCallback(frameToDeliver);
                m_framesCaptured++;
            } catch (const std::exception& ex) {
                m_droppedFrames++;
                if (m_statusCallback) {
                    CaptureStatus s;
                    s.state = CaptureState::Running;
                    s.errorMessage = std::string("Exception in frame callback: ") + ex.what();
                    try { m_statusCallback(s); } catch (...) {}
                }
            } catch (...) {
                m_droppedFrames++;
                if (m_statusCallback) {
                    CaptureStatus s;
                    s.state = CaptureState::Running;
                    s.errorMessage = "Unknown exception in frame callback.";
                    try { m_statusCallback(s); } catch (...) {}
                }
            }
        } else {
            m_droppedFrames++;
        }

        m_deliveryThreadId.store(std::thread::id());
        return S_OK;
    }

    void SetFormatConfig(
        const CaptureMode& mode, bool bottomUp,
        AVColorRange range = AVCOL_RANGE_UNSPECIFIED,
        AVColorSpace space = AVCOL_SPC_UNSPECIFIED,
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED,
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED)
    {
        m_mode = mode;
        m_bottomUp = bottomUp;
        m_colorRange = range;
        m_colorSpace = space;
        m_colorPrimaries = primaries;
        m_colorTrc = trc;

        if (m_mode.pixelFormat == "MJPG") {
            m_mjpegDecoder = std::make_unique<CompressedFrameDecoder>(AV_CODEC_ID_MJPEG);
            m_mjpegDecoder->Initialize(mode.width, mode.height);
        } else if (m_mode.pixelFormat == "H264") {
            m_h264Decoder = std::make_unique<CompressedFrameDecoder>(AV_CODEC_ID_H264);
            m_h264Decoder->Initialize(mode.width, mode.height);
        }
    }

    void Cancel() {
        m_cancelling.store(true);
    }

    bool IsDeliveryThread() const {
        return std::this_thread::get_id() == m_deliveryThreadId.load();
    }

    uint64_t GetCapturedCount() const { return m_framesCaptured; }
    uint64_t GetDroppedCount() const { return m_droppedFrames; }

private:
    std::atomic<ULONG> m_refCount{1};
    std::atomic<bool> m_cancelling{false};
    std::atomic<std::thread::id> m_deliveryThreadId{std::thread::id()};

    SystemCameraSource::FrameCallback m_frameCallback;
    SystemCameraSource::StatusCallback m_statusCallback;

    CaptureMode m_mode{};
    bool m_bottomUp = false;
    AVColorRange m_colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace m_colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic m_colorTrc = AVCOL_TRC_UNSPECIFIED;

    AVFrame* m_ownedFrame = nullptr;
    std::unique_ptr<CompressedFrameDecoder> m_mjpegDecoder;
    std::unique_ptr<CompressedFrameDecoder> m_h264Decoder;
    uint64_t m_framesCaptured = 0;
    uint64_t m_droppedFrames = 0;
};

BackendEnumerationResult DShowDeviceEnumerator::EnumerateDevices(std::vector<CaptureDevice>& outDevices) {
    BackendEnumerationResult result;
    result.backend = CaptureBackend::DirectShow;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);

    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(
        CLSID_SystemDeviceEnum,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ICreateDevEnum,
        reinterpret_cast<void**>(&pDevEnum));

    if (FAILED(hr) || !pDevEnum) {
        if (comInit) CoUninitialize();
        result.success = false;
        result.hr = hr;
        result.errorMessage = "Failed to create CLSID_SystemDeviceEnum.";
        return result;
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (FAILED(hr)) {
        result.success = false; result.hr = hr;
        result.errorMessage = "DirectShow capture category enumeration failed.";
        if (pEnum) pEnum->Release();
        if (comInit) CoUninitialize();
        return result;
    }
    if (hr == S_FALSE || !pEnum) {
        if (comInit) CoUninitialize();
        result.success = true;
        result.hr = S_OK;
        result.deviceCount = 0;
        return result;
    }

    result.success = true;
    result.hr = S_OK;

    IMoniker* pMoniker = nullptr;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
        IPropertyBag* pPropBag = nullptr;
        hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, reinterpret_cast<void**>(&pPropBag));

        std::string friendlyNameStr;
        std::string devicePathStr;
        std::string clsidStr;

        if (SUCCEEDED(hr) && pPropBag) {
            VARIANT varName;
            VariantInit(&varName);
            hr = pPropBag->Read(L"FriendlyName", &varName, 0);
            if (SUCCEEDED(hr) && varName.vt == VT_BSTR && varName.bstrVal) {
                friendlyNameStr = WideToUtf8(varName.bstrVal);
            }
            VariantClear(&varName);

            VARIANT varPath;
            VariantInit(&varPath);
            hr = pPropBag->Read(L"DevicePath", &varPath, 0);
            if (SUCCEEDED(hr) && varPath.vt == VT_BSTR && varPath.bstrVal) {
                devicePathStr = WideToUtf8(varPath.bstrVal);
            }
            VariantClear(&varPath);

            VARIANT varClsid;
            VariantInit(&varClsid);
            hr = pPropBag->Read(L"CLSID", &varClsid, 0);
            if (SUCCEEDED(hr) && varClsid.vt == VT_BSTR && varClsid.bstrVal) {
                clsidStr = WideToUtf8(varClsid.bstrVal);
            }
            VariantClear(&varClsid);

            pPropBag->Release();
        }

        IBindCtx* pBindCtx = nullptr;
        CreateBindCtx(0, &pBindCtx);
        LPOLESTR pDisplayName = nullptr;
        std::string monikerDisplayName;
        if (pBindCtx && SUCCEEDED(pMoniker->GetDisplayName(pBindCtx, nullptr, &pDisplayName)) && pDisplayName) {
            monikerDisplayName = WideToUtf8(pDisplayName);
            CoTaskMemFree(pDisplayName);
        }
        if (pBindCtx) pBindCtx->Release();

        // Check if this source represents PrismForge's virtual camera output (softcam D732B8C1-03D7-4D59-AF8E-4CB72B58B940)
        if (SystemCameraCatalog::IsPrismForgeOutput(devicePathStr) ||
            SystemCameraCatalog::IsPrismForgeOutput(monikerDisplayName) ||
            SystemCameraCatalog::IsPrismForgeOutput(clsidStr) ||
            SystemCameraCatalog::IsPrismForgeOutput(friendlyNameStr)) {
            pMoniker->Release();
            continue;
        }

        CaptureDevice dev;
        dev.displayName = friendlyNameStr.empty() ? "DirectShow Video Device" : friendlyNameStr;

        std::string rawPathForNorm = !devicePathStr.empty() ? devicePathStr : monikerDisplayName;
        dev.devicePath = SystemCameraCatalog::NormalizeDevicePath(rawPathForNorm);

        DeviceRoute route;
        route.backend = CaptureBackend::DirectShow;
        route.routeId = !monikerDisplayName.empty() ? monikerDisplayName : devicePathStr;
        route.routeName = "DirectShow";
        dev.routes.push_back(route);

        outDevices.push_back(dev);
        pMoniker->Release();
    }

    pEnum->Release();
    if (comInit) CoUninitialize();

    result.deviceCount = outDevices.size();
    return result;
}

std::vector<CaptureMode> DShowDeviceEnumerator::QueryCapabilities(const std::string& routeId) {
    std::vector<CaptureMode> modes;
    if (routeId.empty()) return modes;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);

    IBindCtx* pBindCtx = nullptr;
    CreateBindCtx(0, &pBindCtx);
    if (!pBindCtx) {
        if (comInit) CoUninitialize();
        return modes;
    }

    std::wstring wroute = Utf8ToWide(routeId);
    ULONG chEaten = 0;
    IMoniker* pMoniker = nullptr;
    hr = MkParseDisplayName(pBindCtx, wroute.c_str(), &chEaten, &pMoniker);
    pBindCtx->Release();

    if (FAILED(hr) || !pMoniker) {
        if (comInit) CoUninitialize();
        return modes;
    }

    IBaseFilter* pCapFilter = nullptr;
    hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&pCapFilter));
    pMoniker->Release();

    if (FAILED(hr) || !pCapFilter) {
        if (comInit) CoUninitialize();
        return modes;
    }

    IPin* pPin = FindCapturePin(pCapFilter);
    if (pPin) {
        IAMStreamConfig* pConfig = nullptr;
        if (SUCCEEDED(pPin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&pConfig))) && pConfig) {
            int count = 0, size = 0;
            if (SUCCEEDED(pConfig->GetNumberOfCapabilities(&count, &size)) && size >= sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
                std::vector<BYTE> capsBuf(size);
                for (int i = 0; i < count; ++i) {
                    AM_MEDIA_TYPE* pmt = nullptr;
                    if (SUCCEEDED(pConfig->GetStreamCaps(i, &pmt, capsBuf.data())) && pmt) {
                        if (pmt->majortype == MEDIATYPE_Video) {
                            CaptureMode mode;
                            mode.pixelFormat = DShowSubtypeToString(pmt->subtype);

                            if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                                auto* pVih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
                                mode.width = pVih->bmiHeader.biWidth;
                                mode.height = std::abs(pVih->bmiHeader.biHeight);
                                RationalFpsFromAvgTime(pVih->AvgTimePerFrame, mode.fpsNumerator, mode.fpsDenominator);
                            } else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                                auto* pVih2 = reinterpret_cast<VIDEOINFOHEADER2*>(pmt->pbFormat);
                                mode.width = pVih2->bmiHeader.biWidth;
                                mode.height = std::abs(pVih2->bmiHeader.biHeight);
                                RationalFpsFromAvgTime(pVih2->AvgTimePerFrame, mode.fpsNumerator, mode.fpsDenominator);
                                mode.interlaced = (pVih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
                            }

                            if (mode.width > 0 && mode.height > 0) {
                                bool duplicate = false;
                                for (const auto& existing : modes) {
                                    if (existing == mode) {
                                        duplicate = true;
                                        break;
                                    }
                                }
                                if (!duplicate) {
                                    modes.push_back(mode);
                                }
                            }
                        }
                        DeleteMediaType(pmt);
                    }
                }
            }
            pConfig->Release();
        }
        pPin->Release();
    }

    pCapFilter->Release();
    if (comInit) CoUninitialize();

    return modes;
}

DShowCaptureBackend::DShowCaptureBackend(
    SystemCameraSource::FrameCallback frameCb,
    SystemCameraSource::StatusCallback statusCb)
    : m_frameCallback(std::move(frameCb))
    , m_statusCallback(std::move(statusCb))
{
}

DShowCaptureBackend::~DShowCaptureBackend() {
    Close();
}

bool DShowCaptureBackend::IsWorkerThread() const {
    if (std::this_thread::get_id() == m_workerThreadId) {
        return true;
    }
    if (m_callbackHandler && m_callbackHandler->IsDeliveryThread()) {
        return true;
    }
    return false;
}

bool DShowCaptureBackend::Open(const DeviceRoute& route, const CaptureMode* requestedMode) {
    if (m_workerThread.joinable()) {
        Close();
    }

    m_cancelling.store(false);
    const std::string routeId = route.routeId;
    std::unique_ptr<CaptureMode> reqModeCopy;
    if (requestedMode) {
        reqModeCopy = std::make_unique<CaptureMode>(*requestedMode);
    }

    m_workerThread = std::thread([this, routeId, reqMode = std::move(reqModeCopy)]() {
        m_workerThreadId = std::this_thread::get_id();
        WorkerLoop(routeId, reqMode.get());
    });

    return true;
}

void DShowCaptureBackend::Cancel() {
    // Shutdown is performed by the management worker, never a graph delivery thread.
    m_cancelling.store(true);
}

void DShowCaptureBackend::Close() {
    Cancel();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void DShowCaptureBackend::WorkerLoop(const std::string& routeId, const CaptureMode* requestedMode) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);

    CaptureStatus status;
    status.state = CaptureState::Starting;
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    IBindCtx* pBindCtx = nullptr;
    CreateBindCtx(0, &pBindCtx);
    if (!pBindCtx) {
        status.state = CaptureState::Error;
        status.errorCategory = CaptureErrorCategory::InternalFailure;
        status.errorMessage = "Failed to create bind context.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    std::wstring wroute = Utf8ToWide(routeId);
    ULONG chEaten = 0;
    IMoniker* pMoniker = nullptr;
    hr = MkParseDisplayName(pBindCtx, wroute.c_str(), &chEaten, &pMoniker);
    pBindCtx->Release();

    if (FAILED(hr) || !pMoniker) {
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyDShowError(hr);
        status.errorMessage = "Failed to parse DirectShow moniker route.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    IBaseFilter* pCapFilter = nullptr;
    hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&pCapFilter));
    pMoniker->Release();

    if (FAILED(hr) || !pCapFilter) {
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyDShowError(hr);
        status.errorMessage = "Failed to bind DirectShow capture filter.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    IGraphBuilder* pGraph = nullptr;
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));

    ICaptureGraphBuilder2* pBuild = nullptr;
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, reinterpret_cast<void**>(&pBuild));

    if (FAILED(hr) || !pGraph || !pBuild) {
        if (pGraph) pGraph->Release();
        if (pBuild) pBuild->Release();
        pCapFilter->Release();
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = CaptureErrorCategory::InternalFailure;
        status.errorMessage = "Failed to create DirectShow FilterGraph or CaptureGraphBuilder2.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    pBuild->SetFiltergraph(pGraph);
    pGraph->AddFilter(pCapFilter, L"Video Capture");

    // Negotiate mode on capture pin
    IPin* pPin = FindCapturePin(pCapFilter);
    CaptureMode selectedMode;
    if (pPin) {
        IAMStreamConfig* pConfig = nullptr;
        if (SUCCEEDED(pPin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&pConfig))) && pConfig) {
            int count = 0, size = 0;
            if (SUCCEEDED(pConfig->GetNumberOfCapabilities(&count, &size)) && size >= sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
                std::vector<BYTE> capsBuf(size);
                AM_MEDIA_TYPE* pBestMt = nullptr;
                double bestScore = -1.0;

                for (int i = 0; i < count; ++i) {
                    AM_MEDIA_TYPE* pmt = nullptr;
                    if (SUCCEEDED(pConfig->GetStreamCaps(i, &pmt, capsBuf.data())) && pmt) {
                        if (pmt->majortype == MEDIATYPE_Video && pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
                            int w = vih->bmiHeader.biWidth;
                            int h = std::abs(vih->bmiHeader.biHeight);
                            int num = 0, den = 1;
                            RationalFpsFromAvgTime(vih->AvgTimePerFrame, num, den);
                            std::string fmt = DShowSubtypeToString(pmt->subtype);

                            double score = 0.0;
                            if (requestedMode) {
                                if (w == requestedMode->width && h == requestedMode->height) score += 1000.0;
                                else score += (std::max)(0.0, 500.0 - std::abs(w - requestedMode->width));

                                double candidateFps = den > 0 ? (double)num / den : 0.0;
                                if (candidateFps > 0 && requestedMode->Fps() > 0) {
                                    score += (std::max)(0.0, 300.0 - std::abs(candidateFps - requestedMode->Fps()) * 10.0);
                                }
                                if (!requestedMode->pixelFormat.empty() && fmt == requestedMode->pixelFormat) {
                                    score += 200.0;
                                }
                            } else {
                                if (w == 1920 && h == 1080) score += 500.0;
                                else if (w == 1280 && h == 720) score += 400.0;
                                else score += (std::min)(300.0, w * 0.1);

                                if (fmt == "NV12") score += 50.0;
                                else if (fmt == "YUY2") score += 40.0;
                                else if (fmt == "RGB32" || fmt == "RGB24") score += 30.0;
                                else if (fmt == "MJPG") score += 20.0;
                            }

                            if (score > bestScore) {
                                bestScore = score;
                                if (pBestMt) DeleteMediaType(pBestMt);
                                pBestMt = pmt;
                                selectedMode.width = w;
                                selectedMode.height = h;
                                selectedMode.fpsNumerator = num;
                                selectedMode.fpsDenominator = den;
                                selectedMode.pixelFormat = fmt;
                            } else {
                                DeleteMediaType(pmt);
                            }
                        } else {
                            DeleteMediaType(pmt);
                        }
                    }
                }

                if (pBestMt) {
                    pConfig->SetFormat(pBestMt);
                    DeleteMediaType(pBestMt);
                }
            }
            pConfig->Release();
        }
        pPin->Release();
    }

    m_nativeMode = selectedMode;

    // Create Sample Grabber and Null Renderer
    IBaseFilter* pGrabberFilter = nullptr;
    ISampleGrabber* pGrabber = nullptr;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, reinterpret_cast<void**>(&pGrabberFilter));
    if (SUCCEEDED(hr) && pGrabberFilter) {
        pGrabberFilter->QueryInterface(IID_ISampleGrabber, reinterpret_cast<void**>(&pGrabber));
    }

    IBaseFilter* pNullRenderer = nullptr;
    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, reinterpret_cast<void**>(&pNullRenderer));

    if (!pGrabberFilter || !pGrabber || !pNullRenderer) {
        if (pGrabber) pGrabber->Release();
        if (pGrabberFilter) pGrabberFilter->Release();
        if (pNullRenderer) pNullRenderer->Release();
        pBuild->Release();
        pGraph->Release();
        pCapFilter->Release();
        status.state = CaptureState::Error;
        status.errorCategory = CaptureErrorCategory::InternalFailure;
        status.errorMessage = "Failed to create DirectShow SampleGrabber or NullRenderer.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    pGraph->AddFilter(pGrabberFilter, L"Sample Grabber");
    pGraph->AddFilter(pNullRenderer, L"Null Renderer");

    // Configure grabber media type
    AM_MEDIA_TYPE mtGrab;
    ZeroMemory(&mtGrab, sizeof(AM_MEDIA_TYPE));
    mtGrab.majortype = MEDIATYPE_Video;
    if (!selectedMode.pixelFormat.empty() && selectedMode.pixelFormat != "UNKNOWN") {
        mtGrab.subtype = StringToDShowSubtype(selectedMode.pixelFormat);
    }
    pGrabber->SetMediaType(&mtGrab);
    pGrabber->SetBufferSamples(FALSE);
    pGrabber->SetOneShot(FALSE);

    m_callbackHandler = std::make_shared<GrabberCallbackHandler>(m_frameCallback, m_statusCallback);
    pGrabber->SetCallback(m_callbackHandler.get(), 1); // 1 = BufferCB

    // Connect capture stream
    hr = pBuild->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCapFilter, pGrabberFilter, pNullRenderer);
    if (FAILED(hr)) {
        pGrabber->Release();
        pGrabberFilter->Release();
        pNullRenderer->Release();
        pBuild->Release();
        pGraph->Release();
        pCapFilter->Release();
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyDShowError(hr);
        status.errorMessage = "DirectShow RenderStream failed to connect capture pipeline.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    // Determine connected media type and bottom-up orientation
    AM_MEDIA_TYPE connMt;
    ZeroMemory(&connMt, sizeof(AM_MEDIA_TYPE));
    bool bottomUp = false;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTrc = AVCOL_TRC_UNSPECIFIED;

    if (SUCCEEDED(pGrabber->GetConnectedMediaType(&connMt))) {
        m_deliveredMode.pixelFormat = DShowSubtypeToString(connMt.subtype);
        if (connMt.formattype == FORMAT_VideoInfo && connMt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connMt.pbFormat);
            m_deliveredMode.width = vih->bmiHeader.biWidth;
            m_deliveredMode.height = std::abs(vih->bmiHeader.biHeight);
            bottomUp = (vih->bmiHeader.biHeight > 0);
            RationalFpsFromAvgTime(vih->AvgTimePerFrame, m_deliveredMode.fpsNumerator, m_deliveredMode.fpsDenominator);
        } else if (connMt.formattype == FORMAT_VideoInfo2 && connMt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
            auto* vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(connMt.pbFormat);
            m_deliveredMode.width = vih2->bmiHeader.biWidth;
            m_deliveredMode.height = std::abs(vih2->bmiHeader.biHeight);
            bottomUp = (vih2->bmiHeader.biHeight > 0);
            RationalFpsFromAvgTime(vih2->AvgTimePerFrame, m_deliveredMode.fpsNumerator, m_deliveredMode.fpsDenominator);
            m_deliveredMode.interlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
        }

        colorRange = (m_deliveredMode.pixelFormat == "RGB24" || m_deliveredMode.pixelFormat == "RGB32" || m_deliveredMode.pixelFormat == "MJPG")
            ? AVCOL_RANGE_JPEG
            : AVCOL_RANGE_MPEG;

        FreeMediaType(connMt);
    } else {
        m_deliveredMode = m_nativeMode;
    }

    if (m_deliveredMode.width <= 0 || m_deliveredMode.width > 16384 || m_deliveredMode.height <= 0 ||
        m_deliveredMode.height > 16384 || m_deliveredMode.pixelFormat.empty() || m_deliveredMode.pixelFormat == "UNKNOWN" ||
        m_deliveredMode.interlaced || (m_deliveredMode.pixelFormat == "NV12" && (m_deliveredMode.width % 2 || m_deliveredMode.height % 2)) ||
        (m_deliveredMode.pixelFormat == "YUY2" && m_deliveredMode.width % 2)) {
        pGrabber->SetCallback(nullptr, 0);
        pGrabber->Release(); pGrabberFilter->Release(); pNullRenderer->Release();
        pBuild->Release(); pGraph->Release(); pCapFilter->Release();
        status.state = CaptureState::Error; status.errorCategory = CaptureErrorCategory::UnsupportedFormat;
        status.errorMessage = "DirectShow negotiated an unsupported sample layout.";
        if (m_statusCallback) m_statusCallback(status);
        if (comInit) CoUninitialize();
        return;
    }
    m_callbackHandler->SetFormatConfig(m_deliveredMode, bottomUp, colorRange, colorSpace, colorPrimaries, colorTrc);

    IMediaControl* pControl = nullptr;
    pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_pMediaControl = pControl;
    }

    IMediaEvent* pEvent = nullptr;
    pGraph->QueryInterface(IID_IMediaEvent, reinterpret_cast<void**>(&pEvent));

    hr = pControl ? pControl->Run() : E_FAIL;
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            m_pMediaControl = nullptr;
        }
        if (pControl) pControl->Release();
        if (pEvent) pEvent->Release();
        pGrabber->Release();
        pGrabberFilter->Release();
        pNullRenderer->Release();
        pBuild->Release();
        pGraph->Release();
        pCapFilter->Release();
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyDShowError(hr);
        status.errorMessage = "Failed to run DirectShow media graph.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        if (comInit) CoUninitialize();
        return;
    }

    status.state = CaptureState::Running;
    status.nativeMode = m_nativeMode;
    status.deliveredMode = m_deliveredMode;
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    // Capture running loop
    while (!m_cancelling.load()) {
        if (pEvent) {
            long evCode = 0;
            LONG_PTR p1 = 0, p2 = 0;
            if (pEvent->GetEvent(&evCode, &p1, &p2, 100) == S_OK) {
                pEvent->FreeEventParams(evCode, p1, p2);
                if (evCode == EC_DEVICE_LOST) {
                    status.state = CaptureState::Disconnected;
                    status.errorCategory = CaptureErrorCategory::DeviceRemoved;
                    status.errorMessage = "DirectShow EC_DEVICE_LOST";
                    if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
                    break;
                }
                if (evCode == EC_ERRORABORT) {
                    status.state = CaptureState::Error;
                    status.errorCategory = CaptureErrorCategory::InternalFailure;
                    status.errorMessage = "DirectShow EC_ERRORABORT";
                    if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
                    break;
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    if (m_callbackHandler) {
        m_callbackHandler->Cancel();
    }

    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (pControl) {
            pControl->Stop();
        }
        m_pMediaControl = nullptr;
    }

    status.state = m_cancelling.load() ? CaptureState::Idle : status.state;
    if (m_callbackHandler) {
        status.framesCaptured = m_callbackHandler->GetCapturedCount();
        status.droppedFrames = m_callbackHandler->GetDroppedCount();
    }
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    if (pEvent) pEvent->Release();
    if (pControl) pControl->Release();
    pGrabber->SetCallback(nullptr, 0);
    pGrabber->Release();
    pGrabberFilter->Release();
    pNullRenderer->Release();
    pBuild->Release();
    pGraph->Release();
    pCapFilter->Release();

    if (comInit) CoUninitialize();
}

} // namespace PrismForge::Capture
