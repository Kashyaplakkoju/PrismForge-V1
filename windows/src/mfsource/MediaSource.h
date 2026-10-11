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

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <wrl/implements.h>

class VirtualCamMediaStream;

enum MFMEDIASOURCE_STATE
{
    MFMEDIASOURCE_STOPPED,
    MFMEDIASOURCE_RUNNING,
    MFMEDIASOURCE_PAUSED
};

class VirtualCamMediaSource : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IMFMediaSourceEx,
    IMFMediaSource,
    IMFMediaEventGenerator,
    IMFGetService,
    IKsControl>
{
public:
    VirtualCamMediaSource();
    ~VirtualCamMediaSource();

    HRESULT RuntimeClassInitialize();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

private:
    MFMEDIASOURCE_STATE                         _state;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue>  _eventQueue;
    Microsoft::WRL::ComPtr<IMFAttributes>       _sourceAttributes;
    Microsoft::WRL::ComPtr<VirtualCamMediaStream> _stream;
};
