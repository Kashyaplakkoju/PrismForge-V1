#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mf_capture.h"
#include "sample_converter.h"
#include "mf_sample_lock.h"
#include <mferror.h>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace PrismForge::Capture {

namespace {

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

std::string SubtypeGuidToString(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) return "NV12";
    if (subtype == MFVideoFormat_YUY2) return "YUY2";
    if (subtype == MFVideoFormat_RGB32) return "RGB32";
    if (subtype == MFVideoFormat_RGB24) return "RGB24";
    if (subtype == MFVideoFormat_MJPG) return "MJPG";
    if (subtype == MFVideoFormat_H264) return "H264";
    return "UNKNOWN";
}

CaptureErrorCategory ClassifyMfError(HRESULT hr) {
    if (hr == E_ACCESSDENIED) {
        return CaptureErrorCategory::PrivacyAccessDenied;
    }
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        return CaptureErrorCategory::Busy;
    }
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED) ||
        hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)) {
        return CaptureErrorCategory::DeviceRemoved;
    }
    if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_NO_MORE_TYPES) {
        return CaptureErrorCategory::UnsupportedFormat;
    }
    return CaptureErrorCategory::InternalFailure;
}

} // namespace

BackendEnumerationResult MfDeviceEnumerator::EnumerateDevices(std::vector<CaptureDevice>& outDevices) {
    BackendEnumerationResult result;
    result.backend = CaptureBackend::MediaFoundation;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        result.success = false;
        result.hr = hr;
        result.errorMessage = "Failed to initialize Media Foundation startup.";
        return result;
    }

    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) {
        MFShutdown();
        result.success = false;
        result.hr = hr;
        result.errorMessage = "Failed to create MF attributes for device enumeration.";
        return result;
    }

    hr = pAttributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(hr)) {
        hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    }

    if (FAILED(hr)) {
        pAttributes->Release();
        MFShutdown();
        result.success = false;
        result.hr = hr;
        result.errorMessage = "MFEnumDeviceSources failed.";
        return result;
    }

    result.success = true;
    result.hr = S_OK;

    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* pDevice = ppDevices[i];
        if (!pDevice) continue;

        WCHAR* pFriendlyName = nullptr;
        UINT32 fnLength = 0;
        pDevice->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pFriendlyName, &fnLength);

        WCHAR* pSymLink = nullptr;
        UINT32 slLength = 0;
        pDevice->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &pSymLink, &slLength);

        std::string friendlyNameStr = WideToUtf8(pFriendlyName);
        std::string symLinkStr = WideToUtf8(pSymLink);

        if (pFriendlyName) CoTaskMemFree(pFriendlyName);
        if (pSymLink) CoTaskMemFree(pSymLink);

        // Check if this source represents PrismForge's virtual camera output
        if (SystemCameraCatalog::IsPrismForgeOutput(symLinkStr) ||
            SystemCameraCatalog::IsPrismForgeOutput(friendlyNameStr)) {
            pDevice->Release();
            continue;
        }

        CaptureDevice dev;
        dev.displayName = friendlyNameStr.empty() ? "Video Capture Device" : friendlyNameStr;
        dev.devicePath = SystemCameraCatalog::NormalizeDevicePath(symLinkStr);

        DeviceRoute route;
        route.backend = CaptureBackend::MediaFoundation;
        route.routeId = symLinkStr;
        route.routeName = "Media Foundation";
        dev.routes.push_back(route);

        outDevices.push_back(dev);
        pDevice->Release();
    }

    if (ppDevices) {
        CoTaskMemFree(ppDevices);
    }
    pAttributes->Release();
    MFShutdown();

    result.deviceCount = outDevices.size();
    return result;
}

std::vector<CaptureMode> MfDeviceEnumerator::QueryCapabilities(const std::string& symbolicLink) {
    std::vector<CaptureMode> modes;
    if (symbolicLink.empty()) return modes;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return modes;

    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) {
        MFShutdown();
        return modes;
    }

    std::wstring wlink = Utf8ToWide(symbolicLink);
    pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    pAttributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wlink.c_str());

    IMFMediaSource* pSource = nullptr;
    hr = MFCreateDeviceSource(pAttributes, &pSource);
    pAttributes->Release();

    if (FAILED(hr) || !pSource) {
        MFShutdown();
        return modes;
    }

    IMFPresentationDescriptor* pPD = nullptr;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (SUCCEEDED(hr) && pPD) {
        BOOL fSelected = FALSE;
        IMFStreamDescriptor* pSD = nullptr;
        hr = pPD->GetStreamDescriptorByIndex(0, &fSelected, &pSD);
        if (SUCCEEDED(hr) && pSD) {
            IMFMediaTypeHandler* pHandler = nullptr;
            hr = pSD->GetMediaTypeHandler(&pHandler);
            if (SUCCEEDED(hr) && pHandler) {
                DWORD count = 0;
                pHandler->GetMediaTypeCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IMFMediaType* pType = nullptr;
                    if (SUCCEEDED(pHandler->GetMediaTypeByIndex(i, &pType)) && pType) {
                        GUID majorType = GUID_NULL;
                        pType->GetMajorType(&majorType);
                        if (majorType == MFMediaType_Video) {
                            CaptureMode mode;
                            UINT32 w = 0, h = 0;
                            MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h);
                            mode.width = static_cast<int>(w);
                            mode.height = static_cast<int>(h);

                            UINT32 num = 0, den = 1;
                            MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, &num, &den);
                            mode.fpsNumerator = static_cast<int>(num);
                            mode.fpsDenominator = static_cast<int>(den > 0 ? den : 1);

                            GUID subtype = GUID_NULL;
                            pType->GetGUID(MF_MT_SUBTYPE, &subtype);
                            mode.pixelFormat = SubtypeGuidToString(subtype);

                            UINT32 interlace = MFVideoInterlace_Unknown;
                            pType->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);
                            mode.interlaced = (interlace != MFVideoInterlace_Progressive && interlace != MFVideoInterlace_Unknown);

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
                        pType->Release();
                    }
                }
                pHandler->Release();
            }
            pSD->Release();
        }
        pPD->Release();
    }

    pSource->Shutdown();
    pSource->Release();
    MFShutdown();

    return modes;
}

MfCaptureBackend::MfCaptureBackend(
    SystemCameraSource::FrameCallback frameCb,
    SystemCameraSource::StatusCallback statusCb)
    : m_frameCallback(std::move(frameCb))
    , m_statusCallback(std::move(statusCb))
{
}

MfCaptureBackend::~MfCaptureBackend() {
    Close();
}

bool MfCaptureBackend::Open(const DeviceRoute& route, const CaptureMode* requestedMode) {
    if (m_workerThread.joinable()) {
        Close();
    }

    m_cancelling.store(false);
    m_defaultStride = 0;
    m_colorRange = AVCOL_RANGE_UNSPECIFIED;
    m_colorSpace = AVCOL_SPC_UNSPECIFIED;
    m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    m_colorTrc = AVCOL_TRC_UNSPECIFIED;
    m_readState = std::make_shared<MfReadState>();
    const std::string symLink = route.routeId;
    std::unique_ptr<CaptureMode> reqModeCopy;
    if (requestedMode) {
        reqModeCopy = std::make_unique<CaptureMode>(*requestedMode);
    }

    m_workerThread = std::thread([this, symLink, reqMode = std::move(reqModeCopy)]() {
        m_workerThreadId = std::this_thread::get_id();
        WorkerLoop(symLink, reqMode.get());
    });

    return true;
}

void MfCaptureBackend::Cancel() {
    m_cancelling.store(true);
    m_readState->Cancel();
}

void MfCaptureBackend::Close() {
    Cancel();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool MfCaptureBackend::NegotiateMediaType(
    IMFSourceReader* pReader,
    const CaptureMode* requestedMode,
    CaptureMode& outNativeMode,
    CaptureMode& outDeliveredMode,
    bool& outIsCompressedMjpeg,
    bool& outIsCompressedH264)
{
    outIsCompressedMjpeg = false;
    outIsCompressedH264 = false;

    IMFMediaType* pBestNativeType = nullptr;
    CaptureMode bestNativeMode;
    double bestScore = -1.0;

    DWORD mediaTypeIndex = 0;
    while (true) {
        IMFMediaType* pNativeType = nullptr;
        HRESULT hr = pReader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            mediaTypeIndex++,
            &pNativeType);
        if (FAILED(hr) || !pNativeType) break;

        GUID subtype = GUID_NULL;
        pNativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pNativeType, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(pNativeType, MF_MT_FRAME_RATE, &num, &den);

        CaptureMode candidate;
        candidate.width = static_cast<int>(w);
        candidate.height = static_cast<int>(h);
        candidate.fpsNumerator = static_cast<int>(num);
        candidate.fpsDenominator = static_cast<int>(den > 0 ? den : 1);
        candidate.pixelFormat = SubtypeGuidToString(subtype);
        UINT32 interlace = MFVideoInterlace_Unknown;
        pNativeType->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);
        candidate.interlaced = interlace != MFVideoInterlace_Progressive && interlace != MFVideoInterlace_Unknown;

        double score = 0.0;
        if (requestedMode) {
            if (candidate.width == requestedMode->width && candidate.height == requestedMode->height) {
                score += 1000.0;
            } else if (candidate.width > 0 && requestedMode->width > 0) {
                double diff = std::abs(candidate.width - requestedMode->width);
                score += (std::max)(0.0, 500.0 - diff);
            }

            if (candidate.Fps() > 0 && requestedMode->Fps() > 0) {
                double fpsDiff = std::abs(candidate.Fps() - requestedMode->Fps());
                score += (std::max)(0.0, 300.0 - fpsDiff * 10.0);
            }

            if (!requestedMode->pixelFormat.empty() && candidate.pixelFormat == requestedMode->pixelFormat) {
                score += 200.0;
            }
        } else {
            // Default ranking: 1080p -> 720p -> 30fps -> raw formats preferred
            if (candidate.width == 1920 && candidate.height == 1080) score += 500.0;
            else if (candidate.width == 1280 && candidate.height == 720) score += 400.0;
            else score += (std::min)(300.0, candidate.width * 0.1);

            if (candidate.Fps() >= 29.0 && candidate.Fps() <= 61.0) score += 100.0;
            if (candidate.pixelFormat == "NV12") score += 50.0;
            else if (candidate.pixelFormat == "YUY2") score += 40.0;
            else if (candidate.pixelFormat == "RGB24" || candidate.pixelFormat == "RGB32") score += 30.0;
            else if (candidate.pixelFormat == "MJPG") score += 20.0;
        }

        if (candidate.width <= 0 || candidate.width > 16384 || candidate.height <= 0 || candidate.height > 16384 ||
            candidate.pixelFormat == "UNKNOWN" || candidate.interlaced ||
            (candidate.pixelFormat == "NV12" && (candidate.width % 2 || candidate.height % 2)) ||
            (candidate.pixelFormat == "YUY2" && candidate.width % 2)) {
            pNativeType->Release();
            continue;
        }
        if (score > bestScore) {
            bestScore = score;
            if (pBestNativeType) pBestNativeType->Release();
            pBestNativeType = pNativeType;
            bestNativeMode = candidate;
        } else {
            pNativeType->Release();
        }
    }

    if (!pBestNativeType) {
        return false;
    }

    outNativeMode = bestNativeMode;

    // Check for unsupported HDR in native mode
    UINT32 transFunc = 0;
    if (SUCCEEDED(pBestNativeType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transFunc))) {
        if (transFunc == MFVideoTransFunc_2084 || transFunc == MFVideoTransFunc_HLG) {
            pBestNativeType->Release();
            return false; // Reject HDR modes
        }
    }

    // Attempt to set selected native media type
    HRESULT hr = pReader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        nullptr,
        pBestNativeType);

    pBestNativeType->Release();

    if (FAILED(hr)) {
        return false;
    }

    // Read back delivered media type and exact color metadata
    IMFMediaType* pCurrentType = nullptr;
    hr = pReader->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        &pCurrentType);

    if (FAILED(hr) || !pCurrentType) {
        return false; // A guessed delivered layout is unsafe. Allow route fallback.
    }

    GUID currentSubtype = GUID_NULL;
    pCurrentType->GetGUID(MF_MT_SUBTYPE, &currentSubtype);
    UINT32 curW = 0, curH = 0;
    MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &curW, &curH);
    UINT32 curNum = 0, curDen = 1;
    MFGetAttributeRatio(pCurrentType, MF_MT_FRAME_RATE, &curNum, &curDen);

    outDeliveredMode.width = static_cast<int>(curW);
    outDeliveredMode.height = static_cast<int>(curH);
    outDeliveredMode.fpsNumerator = static_cast<int>(curNum);
    outDeliveredMode.fpsDenominator = static_cast<int>(curDen > 0 ? curDen : 1);
    outDeliveredMode.pixelFormat = SubtypeGuidToString(currentSubtype);

    UINT32 strideBits = 0;
    if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits))) {
        m_defaultStride = static_cast<LONG>(strideBits);
    } else if (currentSubtype != MFVideoFormat_MJPG && currentSubtype != MFVideoFormat_H264) {
        MFGetStrideForBitmapInfoHeader(currentSubtype.Data1, curW, &m_defaultStride);
    }
    UINT32 deliveredInterlace = MFVideoInterlace_Unknown;
    pCurrentType->GetUINT32(MF_MT_INTERLACE_MODE, &deliveredInterlace);
    outDeliveredMode.interlaced = deliveredInterlace != MFVideoInterlace_Unknown && deliveredInterlace != MFVideoInterlace_Progressive;
    UINT32 deliveredTransfer = 0;
    pCurrentType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &deliveredTransfer);
    if (!curW || curW > 16384 || !curH || curH > 16384 || outDeliveredMode.pixelFormat == "UNKNOWN" ||
        outDeliveredMode.interlaced ||
        (outDeliveredMode.pixelFormat == "NV12" && (curW % 2 || curH % 2)) ||
        (outDeliveredMode.pixelFormat == "YUY2" && curW % 2) || deliveredTransfer == MFVideoTransFunc_2084 || deliveredTransfer == MFVideoTransFunc_HLG) {
        pCurrentType->Release(); return false;
    }

    // Extract color metadata honestly without unconditional BT.709
    UINT32 matrix = 0;
    if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
        if (matrix == MFVideoTransferMatrix_BT709) m_colorSpace = AVCOL_SPC_BT709;
        else if (matrix == MFVideoTransferMatrix_BT601) m_colorSpace = AVCOL_SPC_BT470BG;
        else m_colorSpace = AVCOL_SPC_UNSPECIFIED;
    } else {
        m_colorSpace = AVCOL_SPC_UNSPECIFIED;
    }

    UINT32 primaries = 0;
    if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries))) {
        if (primaries == MFVideoPrimaries_BT709) m_colorPrimaries = AVCOL_PRI_BT709;
        else if (primaries == MFVideoPrimaries_BT470_2_SysM || primaries == MFVideoPrimaries_BT470_2_SysBG) m_colorPrimaries = AVCOL_PRI_BT470BG;
        else m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    } else {
        m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    }

    UINT32 transfer = 0;
    if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer))) {
        if (transfer == MFVideoTransFunc_709) m_colorTrc = AVCOL_TRC_BT709;
        else m_colorTrc = AVCOL_TRC_UNSPECIFIED;
    } else {
        m_colorTrc = AVCOL_TRC_UNSPECIFIED;
    }

    UINT32 nominalRange = 0;
    if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange))) {
        if (nominalRange == MFNominalRange_16_235) m_colorRange = AVCOL_RANGE_MPEG;
        else if (nominalRange == MFNominalRange_0_255) m_colorRange = AVCOL_RANGE_JPEG;
        else m_colorRange = AVCOL_RANGE_UNSPECIFIED;
    } else {
        m_colorRange = (outDeliveredMode.pixelFormat == "RGB24" || outDeliveredMode.pixelFormat == "RGB32" || outDeliveredMode.pixelFormat == "MJPG")
            ? AVCOL_RANGE_JPEG
            : AVCOL_RANGE_MPEG;
    }

    if (currentSubtype == MFVideoFormat_MJPG) {
        outIsCompressedMjpeg = true;
    } else if (currentSubtype == MFVideoFormat_H264) {
        outIsCompressedH264 = true;
    }

    pCurrentType->Release();
    return true;
}

void MfCaptureBackend::WorkerLoop(const std::string& symbolicLink, const CaptureMode* requestedMode) {
    const HRESULT comHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT startupHr = SUCCEEDED(comHr) ? MFStartup(MF_VERSION) : comHr;
    CaptureStatus status;
    if (FAILED(startupHr)) {
        status.state = CaptureState::Error;
        status.hr = startupHr;
        status.errorCategory = CaptureErrorCategory::InternalFailure;
        status.errorMessage = "Camera COM/Media Foundation initialization failed.";
        if (m_statusCallback) m_statusCallback(status);
        if (SUCCEEDED(comHr)) CoUninitialize();
        return;
    }
    status.state = CaptureState::Starting;
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    IMFAttributes* pAttrs = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttrs, 2);
    if (FAILED(hr)) {
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = CaptureErrorCategory::InternalFailure;
        status.errorMessage = "Failed to allocate MF attributes.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        MFShutdown();
        CoUninitialize();
        return;
    }

    std::wstring wlink = Utf8ToWide(symbolicLink);
    pAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    pAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wlink.c_str());

    IMFMediaSource* pSource = nullptr;
    hr = MFCreateDeviceSource(pAttrs, &pSource);
    pAttrs->Release();

    if (FAILED(hr) || !pSource) {
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyMfError(hr);
        status.errorMessage = "MFCreateDeviceSource failed to activate capture device.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        MFShutdown();
        CoUninitialize();
        return;
    }

    IMFAttributes* pReaderAttrs = nullptr;
    hr = MFCreateAttributes(&pReaderAttrs, 4);
    if (SUCCEEDED(hr)) {
        auto* callback = new MfReaderCallback(m_readState);
        hr = pReaderAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback);
        callback->Release();
        pReaderAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    IMFSourceReader* pReader = nullptr;
    if (SUCCEEDED(hr) && pReaderAttrs) hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, &pReader);
    if (pReaderAttrs) pReaderAttrs->Release();

    if (FAILED(hr) || !pReader) {
        pSource->Shutdown();
        pSource->Release();
        status.state = CaptureState::Error;
        status.hr = hr;
        status.errorCategory = ClassifyMfError(hr);
        status.errorMessage = "Failed to create MFSourceReader.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        MFShutdown();
        CoUninitialize();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_readerMutex);
        m_pReader = pReader;
    }

    bool isMjpeg = false;
    bool isH264 = false;
    if (!NegotiateMediaType(pReader, requestedMode, m_nativeMode, m_deliveredMode, isMjpeg, isH264)) {
        {
            std::lock_guard<std::mutex> lock(m_readerMutex);
            m_pReader = nullptr;
        }
        pReader->Release();
        pSource->Shutdown();
        pSource->Release();
        status.state = CaptureState::Error;
        status.hr = MF_E_INVALIDMEDIATYPE;
        status.errorCategory = CaptureErrorCategory::UnsupportedFormat;
        status.errorMessage = "Failed to negotiate compatible media type with capture device.";
        if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
        MFShutdown();
        CoUninitialize();
        return;
    }

    status.state = CaptureState::Running;
    status.nativeMode = m_nativeMode;
    status.deliveredMode = m_deliveredMode;
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    std::unique_ptr<CompressedFrameDecoder> mjpegDecoder;
    if (isMjpeg) {
        mjpegDecoder = std::make_unique<CompressedFrameDecoder>(AV_CODEC_ID_MJPEG);
        mjpegDecoder->Initialize(m_deliveredMode.width, m_deliveredMode.height);
    }

    std::unique_ptr<CompressedFrameDecoder> h264Decoder;
    if (isH264) {
        h264Decoder = std::make_unique<CompressedFrameDecoder>(AV_CODEC_ID_H264);
        h264Decoder->Initialize(m_deliveredMode.width, m_deliveredMode.height);
    }

    AVFrame* ownedFrame = av_frame_alloc();
    uint64_t capturedCount = 0;
    uint64_t droppedCount = 0;

    while (!m_cancelling.load()) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* pSample = nullptr;

        hr = pReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, nullptr, nullptr, nullptr, nullptr);
        if (SUCCEEDED(hr) && !m_readState->Take(hr, flags, timestamp, pSample)) break;

        if (m_cancelling.load()) {
            if (pSample) pSample->Release();
            break;
        }

        if (FAILED(hr)) {
            if (pSample) pSample->Release();
            status.state = CaptureState::Error;
            status.hr = hr;
            status.errorCategory = ClassifyMfError(hr);
            status.errorMessage = "ReadSample failed.";
            if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
            break;
        }

        if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
            if (pSample) pSample->Release();
            status.state = CaptureState::Error;
            status.errorCategory = CaptureErrorCategory::UnsupportedFormat;
            status.errorMessage = "Camera media type changed; reconnect to renegotiate the sample layout.";
            if (m_statusCallback) m_statusCallback(status);
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample) pSample->Release();
            status.state = CaptureState::Disconnected;
            if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
            break;
        }

        if (!pSample) {
            continue;
        }

        IMFMediaBuffer* pBuffer = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuffer);
        if (FAILED(hr) || !pBuffer) {
            hr = pSample->GetBufferByIndex(0, &pBuffer);
        }

        if (SUCCEEDED(hr) && pBuffer) {
            {
                MfSampleLock lock;
                if (lock.Lock(pBuffer, m_deliveredMode, m_defaultStride, isMjpeg || isH264)) {
                    const BYTE* pData = lock.view.data;
                    const size_t currentLen = lock.view.size;
                    const LONG pitch2D = lock.view.pitch;

                    bool converted = false;
                    AVFrame* frameToDeliver = nullptr;

                    if (isMjpeg && mjpegDecoder) {
                        frameToDeliver = mjpegDecoder->DecodePacket(pData, currentLen, timestamp);
                        converted = (frameToDeliver != nullptr);
                    } else if (isH264 && h264Decoder) {
                        frameToDeliver = h264Decoder->DecodePacket(pData, currentLen, timestamp);
                        converted = (frameToDeliver != nullptr);
                    } else if (m_deliveredMode.pixelFormat == "NV12") {
                        int pY = (pitch2D != 0) ? pitch2D : m_deliveredMode.width;
                        int pUV = pY;
                        converted = SampleConverter::ConvertNv12ToFrame(
                            pData, currentLen,
                            m_deliveredMode.width, m_deliveredMode.height,
                            pY, pUV,
                            ownedFrame,
                            m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
                        frameToDeliver = ownedFrame;
                    } else if (m_deliveredMode.pixelFormat == "YUY2") {
                        int p = (pitch2D != 0) ? pitch2D : (m_deliveredMode.width * 2);
                        converted = SampleConverter::ConvertYuy2ToFrame(
                            pData, currentLen,
                            m_deliveredMode.width, m_deliveredMode.height,
                            p,
                            ownedFrame,
                            m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
                        frameToDeliver = ownedFrame;
                    } else if (m_deliveredMode.pixelFormat == "RGB32") {
                        int p = (pitch2D != 0) ? pitch2D : (m_deliveredMode.width * 4);
                        converted = SampleConverter::ConvertRgb32ToFrame(
                            pData, currentLen,
                            m_deliveredMode.width, m_deliveredMode.height,
                            p, false,
                            ownedFrame,
                            m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
                        frameToDeliver = ownedFrame;
                    } else if (m_deliveredMode.pixelFormat == "RGB24") {
                        int p = (pitch2D != 0) ? pitch2D : (m_deliveredMode.width * 3);
                        converted = SampleConverter::ConvertRgb24ToFrame(
                            pData, currentLen,
                            m_deliveredMode.width, m_deliveredMode.height,
                            p, false,
                            ownedFrame,
                            m_colorRange, m_colorSpace, m_colorPrimaries, m_colorTrc);
                        frameToDeliver = ownedFrame;
                    }

                    lock.Unlock(); // All decoded/copied pixels now belong to our AVFrame.

                    if (converted && frameToDeliver && m_frameCallback && !m_cancelling.load()) {
                        frameToDeliver->pts = timestamp;
                        frameToDeliver->pkt_dts = timestamp;
                        frameToDeliver->time_base = AVRational{1, 10000000}; // 100ns units

                        try {
                            m_frameCallback(frameToDeliver);
                            capturedCount++;
                        } catch (const std::exception& ex) {
                            droppedCount++;
                            status.errorMessage = std::string("Exception in frame callback: ") + ex.what();
                            if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
                        } catch (...) {
                            droppedCount++;
                            status.errorMessage = "Unknown exception in frame callback.";
                            if (m_statusCallback) { try { m_statusCallback(status); } catch (...) {} }
                        }
                    } else {
                        droppedCount++;
                    }
                }
            } // Release a matching 2D/linear lock before releasing the buffer.
            pBuffer->Release();
        }
        pSample->Release();
    }

    if (ownedFrame) {
        av_frame_free(&ownedFrame);
    }

    {
        std::lock_guard<std::mutex> lock(m_readerMutex);
        m_pReader = nullptr;
    }

    pReader->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
    pReader->Release();

    pSource->Shutdown();
    pSource->Release();

    status.state = m_cancelling.load() ? CaptureState::Idle : status.state;
    status.framesCaptured = capturedCount;
    status.droppedFrames = droppedCount;
    if (m_statusCallback) {
        try { m_statusCallback(status); } catch (...) {}
    }

    MFShutdown();
    CoUninitialize();
}

} // namespace PrismForge::Capture
