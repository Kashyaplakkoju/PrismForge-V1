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
#include <wrl/client.h>
#include <wrl/implements.h>
#include "MediaSource.h"
#include "Logger.h"

using namespace Microsoft::WRL;

class VirtualCamActivator :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFActivate, IKsControl>
{
    ComPtr<IMFAttributes> _attrs;
    ComPtr<VirtualCamMediaSource> _source;

public:
    HRESULT RuntimeClassInitialize()
    {
        Log("VirtualCamActivator::RuntimeClassInitialize");
        return MFCreateAttributes(&_attrs, 8);
    }

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override
    {
        Log("VirtualCamActivator::ActivateObject called");
        if (!ppv) return E_POINTER;

        if (!_source) {
            _source = Make<VirtualCamMediaSource>();
            if (!_source) return E_OUTOFMEMORY;

            HRESULT hr = _source->RuntimeClassInitialize();
            if (FAILED(hr)) {
                _source.Reset();
                return hr;
            }
        }
        return _source->QueryInterface(riid, ppv);
    }

    STDMETHODIMP ShutdownObject() override
    {
        Log("VirtualCamActivator::ShutdownObject called");
        if (_source) {
            _source->Shutdown();
            _source.Reset();
        }
        return S_OK;
    }

    STDMETHODIMP DetachObject() override
    {
        Log("VirtualCamActivator::DetachObject called");
        _source.Reset();
        return S_OK;
    }

    // ------------------------------------------------------------------
    // IMFAttributes implementation (forwarded)
    // ------------------------------------------------------------------
    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* pValue) override { return _attrs ? _attrs->GetItem(guidKey, pValue) : E_FAIL; }
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) override { return _attrs ? _attrs->GetItemType(guidKey, pType) : E_FAIL; }
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) override { return _attrs ? _attrs->CompareItem(guidKey, Value, pbResult) : E_FAIL; }
    STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) override { return _attrs ? _attrs->Compare(pTheirs, MatchType, pbResult) : E_FAIL; }
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue) override { return _attrs ? _attrs->GetUINT32(guidKey, punValue) : E_FAIL; }
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue) override { return _attrs ? _attrs->GetUINT64(guidKey, punValue) : E_FAIL; }
    STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue) override { return _attrs ? _attrs->GetDouble(guidKey, pfValue) : E_FAIL; }
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue) override { return _attrs ? _attrs->GetGUID(guidKey, pguidValue) : E_FAIL; }
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength) override { return _attrs ? _attrs->GetStringLength(guidKey, pcchLength) : E_FAIL; }
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override { return _attrs ? _attrs->GetString(guidKey, pwszValue, cchBufSize, pcchLength) : E_FAIL; }
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override { return _attrs ? _attrs->GetAllocatedString(guidKey, ppwszValue, pcchLength) : E_FAIL; }
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) override { return _attrs ? _attrs->GetBlobSize(guidKey, pcbBlobSize) : E_FAIL; }
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) override { return _attrs ? _attrs->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize) : E_FAIL; }
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) override { return _attrs ? _attrs->GetAllocatedBlob(guidKey, ppBuf, pcbSize) : E_FAIL; }
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override { return _attrs ? _attrs->GetUnknown(guidKey, riid, ppv) : E_FAIL; }
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override { return _attrs ? _attrs->SetItem(guidKey, Value) : E_FAIL; }
    STDMETHODIMP DeleteItem(REFGUID guidKey) override { return _attrs ? _attrs->DeleteItem(guidKey) : E_FAIL; }
    STDMETHODIMP DeleteAllItems() override { return _attrs ? _attrs->DeleteAllItems() : E_FAIL; }
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override { return _attrs ? _attrs->SetUINT32(guidKey, unValue) : E_FAIL; }
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override { return _attrs ? _attrs->SetUINT64(guidKey, unValue) : E_FAIL; }
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override { return _attrs ? _attrs->SetDouble(guidKey, fValue) : E_FAIL; }
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override { return _attrs ? _attrs->SetGUID(guidKey, guidValue) : E_FAIL; }
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override { return _attrs ? _attrs->SetString(guidKey, wszValue) : E_FAIL; }
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) override { return _attrs ? _attrs->SetBlob(guidKey, pBuf, cbBufSize) : E_FAIL; }
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* pUnknown) override { return _attrs ? _attrs->SetUnknown(guidKey, pUnknown) : E_FAIL; }
    STDMETHODIMP LockStore() override { return _attrs ? _attrs->LockStore() : E_FAIL; }
    STDMETHODIMP UnlockStore() override { return _attrs ? _attrs->UnlockStore() : E_FAIL; }
    STDMETHODIMP GetCount(UINT32* pcItems) override { return _attrs ? _attrs->GetCount(pcItems) : E_FAIL; }
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) override { return _attrs ? _attrs->GetItemByIndex(unIndex, pguidKey, pValue) : E_FAIL; }
    STDMETHODIMP CopyAllItems(IMFAttributes* pDest) override { return _attrs ? _attrs->CopyAllItems(pDest) : E_FAIL; }

    // ------------------------------------------------------------------
    // IKsControl stub
    // ------------------------------------------------------------------
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return E_NOTIMPL; }
};
