#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000B
#endif

#include <windows.h>

#ifndef STATUS_WAIT_0
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#endif
#ifndef STATUS_ABANDONED_WAIT_0
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#endif

#include <mfidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <memory>
#include <vector>
#include "FrameServer.h"

class VirtualCamMediaSource;

class VirtualCamMediaStream :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMFMediaStream,
        IMFMediaEventGenerator>
{
public:
    VirtualCamMediaStream();
    ~VirtualCamMediaStream();

    HRESULT RuntimeClassInitialize(VirtualCamMediaSource* pSource, PrismForge::CameraTransport::Names names = {});
    void SetActive(bool active);
    void Shutdown();
    HRESULT SetCurrentMediaTypeOnHandler();
    HRESULT FireStreamStarted(const PROPVARIANT* pvarStartPosition);

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

private:
    HRESULT CreateSupportedMediaTypes(std::vector<Microsoft::WRL::ComPtr<IMFMediaType>>& types);
    HRESULT BuildMediaType(UINT32 width, UINT32 height, UINT32 fpsNumerator, UINT32 fpsDenominator, IMFMediaType** ppType);
    void QueueBlankSample(IUnknown* pToken);
    void BuildStandbyFrame(UINT32 width, UINT32 height);

    Microsoft::WRL::ComPtr<IMFMediaEventQueue>     _eventQueue;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor>    _streamDescriptor;
    Microsoft::WRL::ComPtr<VirtualCamMediaSource>  _source;

    std::unique_ptr<FrameServer> _frameServer;
    std::mutex                   _sampleMutex;
    SharedFrameSnapshot          _frameSnapshot;
    std::vector<BYTE>            _lastFrame;
    std::vector<BYTE>            _standbyFrame;
    UINT64                       _lastFrameIndex;
    bool                         _active;
    UINT32                       _currentWidth;
    UINT32                       _currentHeight;
};
