#include "MediaStream.h"
#include "MediaSource.h"
#include "Logger.h"
#include <mfapi.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")

VirtualCamMediaStream::VirtualCamMediaStream()
    : _active(false)
    , _lastFrameIndex(0)
    , _currentWidth(1920)
    , _currentHeight(1080)
{
}

VirtualCamMediaStream::~VirtualCamMediaStream()
{
}

HRESULT VirtualCamMediaStream::RuntimeClassInitialize(VirtualCamMediaSource* pSource, PrismForge::CameraTransport::Names names)
{
    try {
        _source = pSource;

        HRESULT hr = MFCreateEventQueue(&_eventQueue);
        if (FAILED(hr)) {
            Log("MediaStream::RuntimeClassInitialize MFCreateEventQueue failed: 0x%08X", hr);
            return hr;
        }

        std::vector<Microsoft::WRL::ComPtr<IMFMediaType>> mediaTypes;
        hr = CreateSupportedMediaTypes(mediaTypes);
        if (FAILED(hr)) return hr;

        std::vector<IMFMediaType*> rawTypes;
        for (auto& t : mediaTypes)
            rawTypes.push_back(t.Get());

        hr = MFCreateStreamDescriptor(0, (DWORD)rawTypes.size(), rawTypes.data(), &_streamDescriptor);
        if (FAILED(hr)) {
            Log("MediaStream::RuntimeClassInitialize MFCreateStreamDescriptor failed: 0x%08X", hr);
            return hr;
        }

        hr = SetCurrentMediaTypeOnHandler();
        if (FAILED(hr)) return hr;

        // Initialize standby screen
        BuildStandbyFrame(_currentWidth, _currentHeight);

        // Initialize the shared memory frame reader
        _frameServer = std::make_unique<FrameServer>(std::move(names));
        hr = _frameServer->Initialize();
        if (FAILED(hr)) return hr;

        Log("VirtualCamMediaStream::RuntimeClassInitialize completed successfully");
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT VirtualCamMediaStream::BuildMediaType(
    UINT32 width, UINT32 height, UINT32 fpsNumerator, UINT32 fpsDenominator, IMFMediaType** ppType)
{
    const UINT32 STRIDE = width;
    const UINT32 SAMPLE_SIZE = width * height * 3 / 2;

    Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
    HRESULT hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr)) return hr;

    hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, fpsNumerator, fpsDenominator);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_SAMPLE_SIZE, SAMPLE_SIZE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, STRIDE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (FAILED(hr)) return hr;

    *ppType = mediaType.Detach();
    return S_OK;
}

HRESULT VirtualCamMediaStream::CreateSupportedMediaTypes(std::vector<Microsoft::WRL::ComPtr<IMFMediaType>>& types)
{
    // List highest-quality formats first so that apps negotiate the best available.
    // Each resolution is offered at both 60fps and 30fps so the consuming app
    // (Google Meet, OBS, Windows Camera, etc.) can pick whichever it supports.

    // 1920x1080
    Microsoft::WRL::ComPtr<IMFMediaType> fhd60, fhd30;
    HRESULT hr = BuildMediaType(1920, 1080, 60, 1, &fhd60); if (SUCCEEDED(hr)) types.push_back(fhd60);
             hr = BuildMediaType(1920, 1080, 30, 1, &fhd30); if (SUCCEEDED(hr)) types.push_back(fhd30);

    // 1280x720
    Microsoft::WRL::ComPtr<IMFMediaType> hd60, hd30;
    hr = BuildMediaType(1280, 720, 60, 1, &hd60); if (SUCCEEDED(hr)) types.push_back(hd60);
    hr = BuildMediaType(1280, 720, 30, 1, &hd30); if (SUCCEEDED(hr)) types.push_back(hd30);

    // 3840x2160 (4K) - 30fps only; most pipelines cannot sustain 4K 60fps over RTSP
    Microsoft::WRL::ComPtr<IMFMediaType> uhd30;
    hr = BuildMediaType(3840, 2160, 30, 1, &uhd30); if (SUCCEEDED(hr)) types.push_back(uhd30);

    // 640x480 (SD)
    Microsoft::WRL::ComPtr<IMFMediaType> sd60, sd30;
    hr = BuildMediaType(640, 480, 60, 1, &sd60); if (SUCCEEDED(hr)) types.push_back(sd60);
    hr = BuildMediaType(640, 480, 30, 1, &sd30); if (SUCCEEDED(hr)) types.push_back(sd30);

    return types.empty() ? E_FAIL : S_OK;
}

HRESULT VirtualCamMediaStream::SetCurrentMediaTypeOnHandler()
{
    Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
    HRESULT hr = _streamDescriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
    hr = handler->GetMediaTypeByIndex(0, &mediaType);
    if (FAILED(hr)) return hr;

    return handler->SetCurrentMediaType(mediaType.Get());
}

void VirtualCamMediaStream::BuildStandbyFrame(UINT32 width, UINT32 height)
{
    size_t totalBytes = width * height * 3 / 2;
    _standbyFrame.resize(totalBytes);

    // Fill background: dark slate grey (Y = 32, U = 128, V = 128)
    BYTE* yPlane = _standbyFrame.data();
    BYTE* uvPlane = _standbyFrame.data() + (width * height);
    memset(yPlane, 32, width * height);
    memset(uvPlane, 128, (width * height) / 2);

    // Draw an elegant center badge (PrismForge standby card)
    UINT32 cardW = (width * 3) / 8;
    UINT32 cardH = (height * 2) / 8;
    UINT32 startX = (width - cardW) / 2;
    UINT32 startY = (height - cardH) / 2;

    for (UINT32 y = startY; y < startY + cardH; ++y) {
        for (UINT32 x = startX; x < startX + cardW; ++x) {
            // Card surface: darker card with lighter border
            bool isBorder = (x == startX || x == startX + cardW - 1 ||
                             y == startY || y == startY + cardH - 1 ||
                             x == startX + 1 || x == startX + cardW - 2 ||
                             y == startY + 1 || y == startY + cardH - 2);

            if (isBorder) {
                // Vibrant cyan border (Y = 160, U = 190, V = 80)
                yPlane[y * width + x] = 160;
                UINT32 uvIdx = ((y / 2) * (width / 2) + (x / 2)) * 2;
                uvPlane[uvIdx] = 190;
                uvPlane[uvIdx + 1] = 80;
            } else {
                // Card interior (Y = 48)
                yPlane[y * width + x] = 48;
            }
        }
    }
}

void VirtualCamMediaStream::SetActive(bool active)
{
    std::lock_guard<std::mutex> lock(_sampleMutex);
    if (!active) { _lastFrame.clear(); _frameSnapshot = {}; }
    _active = active;
    Log("VirtualCamMediaStream::SetActive(%d)", active ? 1 : 0);
}

HRESULT VirtualCamMediaStream::FireStreamStarted(const PROPVARIANT* pvarStartPosition)
{
    return _eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, pvarStartPosition);
}

STDMETHODIMP VirtualCamMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if (!ppMediaSource) return E_POINTER;
    return _source.CopyTo(ppMediaSource);
}

STDMETHODIMP VirtualCamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (!ppStreamDescriptor) return E_POINTER;
    *ppStreamDescriptor = _streamDescriptor.Get();
    if (_streamDescriptor) _streamDescriptor->AddRef();
    return S_OK;
}

static void ScaleNV12(
    const BYTE* src, UINT32 srcW, UINT32 srcH,
    BYTE* dst, UINT32 dstW, UINT32 dstH)
{
    // Both layouts were validated as tight even NV12 before this call.
    const auto fitted = PrismForge::CameraTransport::Fit(srcW, srcH, dstW, dstH);
    memset(dst, 16, dstW * dstH);
    memset(dst + dstW * dstH, 128, dstW * dstH / 2);
    for (int y = 0; y < fitted.height; ++y) {
        const UINT32 sy = UINT32(y) * srcH / fitted.height;
        const BYTE* row = src + sy * srcW;
        BYTE* output = dst + (fitted.y + y) * dstW + fitted.x;
        for (int x = 0; x < fitted.width; ++x) output[x] = row[UINT32(x) * srcW / fitted.width];
    }
    const BYTE* srcUV = src + srcW * srcH;
    BYTE* dstUV = dst + dstW * dstH;
    for (int y = 0; y < fitted.height / 2; ++y) {
        const UINT32 sy = UINT32(y) * (srcH / 2) / (fitted.height / 2);
        const BYTE* row = srcUV + sy * srcW;
        BYTE* output = dstUV + (fitted.y / 2 + y) * dstW + fitted.x;
        for (int x = 0; x < fitted.width; x += 2) {
            const UINT32 sx = (UINT32(x) * srcW / fitted.width) & ~1U;
            output[x] = row[sx]; output[x + 1] = row[sx + 1];
        }
    }
}

STDMETHODIMP VirtualCamMediaStream::RequestSample(IUnknown* pToken)
{
    std::lock_guard<std::mutex> lock(_sampleMutex);
    try {
        if (!_active)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;

        // Detect the current negotiated output resolution
        UINT32 targetW = _currentWidth;
        UINT32 targetH = _currentHeight;

        Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
        if (_streamDescriptor && SUCCEEDED(_streamDescriptor->GetMediaTypeHandler(&handler))) {
            Microsoft::WRL::ComPtr<IMFMediaType> currentType;
            if (SUCCEEDED(handler->GetCurrentMediaType(&currentType)) && currentType) {
                GUID subtype{};
                if (FAILED(currentType->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12)
                    return MF_E_INVALIDMEDIATYPE;
                UINT32 nominalRange = MFNominalRange_16_235, matrix = MFVideoTransferMatrix_BT709;
                currentType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange);
                currentType->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
                if (nominalRange != MFNominalRange_16_235 || matrix != MFVideoTransferMatrix_BT709)
                    return MF_E_INVALIDMEDIATYPE;
                UINT32 w = 0, h = 0;
                DWORD bytes = 0;
                if (FAILED(MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &w, &h)) ||
                    !PrismForge::CameraTransport::FrameSize(w, h, bytes)) return MF_E_INVALIDMEDIATYPE;
                targetW = w; targetH = h;
            }
        }

        DWORD expectedSampleSize = 0;
        if (!PrismForge::CameraTransport::FrameSize(targetW, targetH, expectedSampleSize))
            return MF_E_INVALIDMEDIATYPE;

        const bool dimensionsChanged = targetW != _currentWidth || targetH != _currentHeight;
        if (dimensionsChanged) {
            _currentWidth = targetW;
            _currentHeight = targetH;
            BuildStandbyFrame(targetW, targetH);
        }

        HRESULT hr = _frameServer->GetLatestFrame(_frameSnapshot);
        const bool isStreaming = _frameSnapshot.isStreaming;

        if ((hr == S_OK || (hr == E_PENDING && dimensionsChanged)) && !_frameSnapshot.data.empty() && isStreaming) {
            const BYTE* srcData = _frameSnapshot.data.data();
            const DWORD srcLength = static_cast<DWORD>(_frameSnapshot.data.size());
            const UINT32 srcW = _frameSnapshot.width;
            const UINT32 srcH = _frameSnapshot.height;

            _lastFrame.resize(expectedSampleSize);
            if (srcW == targetW && srcH == targetH && srcLength >= expectedSampleSize) {
                memcpy(_lastFrame.data(), srcData, expectedSampleSize);
            } else {
                ScaleNV12(srcData, srcW, srcH, _lastFrame.data(), targetW, targetH);
            }
            _lastFrameIndex = _frameSnapshot.frameIndex;
        }

        const BYTE* frameData = nullptr;
        if (isStreaming && !_lastFrame.empty() && _lastFrame.size() == expectedSampleSize) {
            frameData = _lastFrame.data();
        } else {
            if (_standbyFrame.size() != expectedSampleSize) {
                BuildStandbyFrame(targetW, targetH);
            }
            frameData = _standbyFrame.data();
        }

        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(expectedSampleSize, &buffer);
        if (FAILED(hr)) return hr;

        BYTE* dst = nullptr;
        hr = buffer->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) return hr;

        memcpy(dst, frameData, expectedSampleSize);

        hr = buffer->Unlock();
        if (FAILED(hr)) return hr;
        hr = buffer->SetCurrentLength(expectedSampleSize);
        if (FAILED(hr)) return hr;
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return hr;
        // Read the frame rate from the currently negotiated media type so that
        // SetSampleDuration is accurate for both 30fps and 60fps consumers.
        // Falls back to 30fps (333333 units) if the type cannot be read.
        LONGLONG sampleDuration = 333333; // default: 30fps in 100-ns units
        {
            Microsoft::WRL::ComPtr<IMFMediaTypeHandler> durHandler;
            if (_streamDescriptor && SUCCEEDED(_streamDescriptor->GetMediaTypeHandler(&durHandler))) {
                Microsoft::WRL::ComPtr<IMFMediaType> curType;
                if (SUCCEEDED(durHandler->GetCurrentMediaType(&curType)) && curType) {
                    UINT32 fpsNum = 0, fpsDen = 1;
                    if (SUCCEEDED(MFGetAttributeRatio(curType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen))
                        && fpsNum > 0 && fpsDen > 0) {
                        // duration = 10,000,000 * fpsDen / fpsNum  (100-nanosecond units)
                        sampleDuration = (LONGLONG)10000000 * fpsDen / fpsNum;
                    }
                }
            }
        }

        hr = sample->SetSampleTime(MFGetSystemTime());
        if (FAILED(hr)) return hr;
        hr = sample->SetSampleDuration(sampleDuration);
        if (FAILED(hr)) return hr;

        if (pToken) {
            hr = sample->SetUnknown(MFSampleExtension_Token, pToken);
            if (FAILED(hr)) return hr;
        }

        return _eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

void VirtualCamMediaStream::QueueBlankSample(IUnknown* pToken)
{
    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (SUCCEEDED(MFCreateSample(&sample))) {
        if (pToken)
            sample->SetUnknown(MFSampleExtension_Token, pToken);
        _eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
}

void VirtualCamMediaStream::Shutdown()
{
    std::lock_guard<std::mutex> lock(_sampleMutex);
    _active = false;
    _lastFrame.clear(); _frameSnapshot = {};
    Log("VirtualCamMediaStream::Shutdown called");
    if (_eventQueue)
        _eventQueue->Shutdown();
}

STDMETHODIMP VirtualCamMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    return _eventQueue ? _eventQueue->GetEvent(dwFlags, ppEvent) : MF_E_SHUTDOWN;
}

STDMETHODIMP VirtualCamMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    return _eventQueue ? _eventQueue->BeginGetEvent(pCallback, punkState) : MF_E_SHUTDOWN;
}

STDMETHODIMP VirtualCamMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    return _eventQueue ? _eventQueue->EndGetEvent(pResult, ppEvent) : MF_E_SHUTDOWN;
}

STDMETHODIMP VirtualCamMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    return _eventQueue ? _eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue) : MF_E_SHUTDOWN;
}
