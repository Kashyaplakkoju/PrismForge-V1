#include <windows.h>
#include <new>
#include <initguid.h>
#include "Activator.h"
#include "Logger.h"

// Unique CLSID for the PrismForge Virtual Camera Source
DEFINE_GUID(CLSID_PrismForgeSource,
    0x2141c943, 0xdd09, 0x4915, 0xbd, 0x14, 0x26, 0x47, 0x24, 0xdb, 0xa9, 0x55);

static const WCHAR* CLSID_STRING = L"{2141C943-DD09-4915-BD14-264724DBA955}";

static HMODULE g_hModule  = nullptr;
static LONG    g_objCount = 0;

class VCamClassFactory : public IClassFactory
{
    LONG _refCount = 1;

public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_refCount); }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG ref = InterlockedDecrement(&_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override
    {
        Log("VCamClassFactory::CreateInstance called");
        if (pOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv)   return E_POINTER;
        *ppv = nullptr;

        auto activator = Microsoft::WRL::Make<VirtualCamActivator>();
        if (!activator) return E_OUTOFMEMORY;

        HRESULT hr = activator->RuntimeClassInitialize();
        if (FAILED(hr)) return hr;

        InterlockedIncrement(&g_objCount);
        hr = activator->QueryInterface(riid, ppv);
        if (FAILED(hr)) {
            InterlockedDecrement(&g_objCount);
        }
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_objCount);
        else       InterlockedDecrement(&g_objCount);
        return S_OK;
    }
};

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID /*lpReserved*/)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hInstDLL;
        DisableThreadLibraryCalls(hInstDLL);
        Log("prismforge_source.dll loaded");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (rclsid != CLSID_PrismForgeSource)
        return CLASS_E_CLASSNOTAVAILABLE;

    auto factory = new (std::nothrow) VCamClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_objCount == 0) ? S_OK : S_FALSE;
}

static HRESULT RegisterToKey(HKEY rootKey, const WCHAR* dllPath)
{
    HKEY hKey = nullptr;
    WCHAR keyPath[256];

    swprintf_s(keyPath, L"SOFTWARE\\Classes\\CLSID\\%s", CLSID_STRING);
    LSTATUS status = RegCreateKeyExW(rootKey, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    const WCHAR desc[] = L"PrismForge Virtual Camera Source";
    RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)desc, (DWORD)((wcslen(desc) + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);

    swprintf_s(keyPath, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", CLSID_STRING);
    status = RegCreateKeyExW(rootKey, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(WCHAR)));

    const WCHAR threading[] = L"Both";
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)threading, (DWORD)((wcslen(threading) + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);

    return S_OK;
}

STDAPI DllRegisterServer()
{
    WCHAR dllPath[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, dllPath, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());

    // Register to HKCU (always accessible to current user)
    HRESULT hrUser = RegisterToKey(HKEY_CURRENT_USER, dllPath);

    // Also register to HKLM (accessible when run as Administrator)
    HRESULT hrMachine = RegisterToKey(HKEY_LOCAL_MACHINE, dllPath);

    Log("prismforge_source.dll registered (HKCU: 0x%08X, HKLM: 0x%08X)", hrUser, hrMachine);
    return SUCCEEDED(hrMachine) ? hrMachine : hrUser;
}

STDAPI DllUnregisterServer()
{
    WCHAR keyPath[256];
    swprintf_s(keyPath, L"SOFTWARE\\Classes\\CLSID\\%s", CLSID_STRING);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    RegDeleteTreeW(HKEY_CURRENT_USER, keyPath);
    Log("prismforge_source.dll unregistered from registry");
    return S_OK;
}
