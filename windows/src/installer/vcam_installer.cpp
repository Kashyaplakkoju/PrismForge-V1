#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

static const WCHAR* SOURCE_CLSID  = L"{2141C943-DD09-4915-BD14-264724DBA955}";
static const WCHAR* FRIENDLY_NAME = L"PrismForge Virtual Camera";
static bool g_silent = false;

static void ShowMsg(const std::wstring& msg, bool isError = false)
{
    std::wcout << (isError ? L"[ERROR] " : L"[INFO] ") << msg << std::endl;
    if (!g_silent) {
        MessageBoxW(
            NULL,
            msg.c_str(),
            L"PrismForge Virtual Camera Installer",
            (isError ? MB_ICONERROR : MB_ICONINFORMATION) | MB_OK
        );
    }
}

static std::wstring GetExecutableDir()
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring str(path);
    size_t pos = str.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        return str.substr(0, pos + 1);
    return L"";
}

static std::wstring FindFile(const std::wstring& filename, const std::wstring& userPath = L"")
{
    if (!userPath.empty() && GetFileAttributesW(userPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return userPath;

    std::wstring exeDir = GetExecutableDir();
    std::vector<std::wstring> candidates = {
        exeDir + filename,
        exeDir + L"dist\\" + filename,
        exeDir + L"..\\dist\\" + filename,
        exeDir + L"..\\" + filename,
        exeDir + L"..\\windows\\src\\mfsource\\" + filename,
        exeDir + L"..\\windows\\" + filename
    };

    for (const auto& candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return L"";
}

static HRESULT CallDllFunction(const std::wstring& dllPath, const char* funcName)
{
    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    if (!hMod) {
        DWORD err = GetLastError();
        std::wcout << L"Failed to load " << dllPath << L", error: " << err << std::endl;
        return HRESULT_FROM_WIN32(err);
    }

    typedef HRESULT (STDAPICALLTYPE *pfnDllFunc)();
    pfnDllFunc pFunc = (pfnDllFunc)GetProcAddress(hMod, funcName);
    if (!pFunc) {
        DWORD err = GetLastError();
        FreeLibrary(hMod);
        return HRESULT_FROM_WIN32(err);
    }

    HRESULT hr = pFunc();
    FreeLibrary(hMod);
    return hr;
}

static bool RemoveSystemCamera()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(MFStartup(MF_VERSION))) {
            ComPtr<IMFVirtualCamera> virtualCamera;
            hr = MFCreateVirtualCamera(
                MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_System,
                MFVirtualCameraAccess_AllUsers,
                FRIENDLY_NAME,
                SOURCE_CLSID,
                nullptr,
                0,
                &virtualCamera
            );
            if (SUCCEEDED(hr) && virtualCamera) {
                virtualCamera->Remove();
                virtualCamera.Reset();
                std::wcout << L"[OK] Removed persistent system virtual camera." << std::endl;
            }
            MFShutdown();
        }
        CoUninitialize();
    }
    return true;
}

static bool CreateSystemCamera()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;

    if (FAILED(MFStartup(MF_VERSION))) {
        CoUninitialize();
        return false;
    }

    BOOL supported = FALSE;
    hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (FAILED(hr) || !supported) {
        MFShutdown();
        CoUninitialize();
        return false;
    }

    ComPtr<IMFVirtualCamera> virtualCamera;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers,
        FRIENDLY_NAME,
        SOURCE_CLSID,
        nullptr,
        0,
        &virtualCamera
    );

    if (FAILED(hr) || !virtualCamera) {
        MFShutdown();
        CoUninitialize();
        return false;
    }

    hr = virtualCamera->Start(nullptr);
    virtualCamera.Reset();
    MFShutdown();
    CoUninitialize();

    return SUCCEEDED(hr);
}

static int EnsureComRegistered(const std::wstring& mfSourcePath, const std::wstring& softcamPath)
{
    std::wstring mfDll = FindFile(L"prismforge_source.dll", mfSourcePath);
    if (mfDll.empty()) {
        ShowMsg(L"Could not locate prismforge_source.dll.\nPlease make sure it is in the same directory or in 'dist\\'.", true);
        return 1;
    }

    std::wstring dsDll = FindFile(L"prismforge_cam.dll", softcamPath);
    if (!dsDll.empty()) {
        HRESULT hrDs = CallDllFunction(dsDll, "DllRegisterServer");
        if (SUCCEEDED(hrDs)) {
            std::wcout << L"[OK] Registered legacy DirectShow filter (prismforge_cam.dll)" << std::endl;
        }
    }

    HRESULT hrReg = CallDllFunction(mfDll, "DllRegisterServer");
    if (FAILED(hrReg)) {
        wchar_t buf[256];
        swprintf_s(buf, L"Failed to register prismforge_source.dll into HKLM (0x%08X).\nPlease ensure you run as Administrator.", hrReg);
        ShowMsg(buf, true);
        return 1;
    }
    std::wcout << L"[OK] Registered COM Media Source in HKLM: " << SOURCE_CLSID << std::endl;
    return 0;
}

static int DoRegister(const std::wstring& mfSourcePath, const std::wstring& softcamPath)
{
    std::wcout << L"=== PrismForge Virtual Camera Setup (Default: Dynamic Mode) ===" << std::endl;

    int res = EnsureComRegistered(mfSourcePath, softcamPath);
    if (res != 0) return res;

    // Clean any persistent system camera so the app manages it dynamically by default
    RemoveSystemCamera();

    ShowMsg(L"PrismForge Virtual Camera has been successfully registered!\n\nDefault Mode: Show only when app is active (Dynamic).\nThe camera will appear in Windows Settings and camera apps whenever PrismForge is open.");
    return 0;
}

static int DoModeAlways(const std::wstring& mfSourcePath, const std::wstring& softcamPath)
{
    std::wcout << L"=== Setting Camera Mode: Always Show in Windows Settings ===" << std::endl;

    int res = EnsureComRegistered(mfSourcePath, softcamPath);
    if (res != 0) return res;

    if (!CreateSystemCamera()) {
        ShowMsg(L"Failed to activate persistent Virtual Camera in Windows.", true);
        return 1;
    }

    ShowMsg(L"PrismForge Virtual Camera is now set to ALWAYS show in Windows Settings.\n(Persistent mode active)");
    return 0;
}

static int DoModeDynamic(const std::wstring& mfSourcePath, const std::wstring& softcamPath)
{
    std::wcout << L"=== Setting Camera Mode: Show Only When App is Active ===" << std::endl;

    EnsureComRegistered(mfSourcePath, softcamPath);
    RemoveSystemCamera();

    ShowMsg(L"PrismForge Virtual Camera is now set to show ONLY when PrismForge is running.\n(Dynamic mode active)");
    return 0;
}

static int DoModeDisabled()
{
    std::wcout << L"=== Setting Camera Mode: Do Not Show in Windows Settings ===" << std::endl;

    RemoveSystemCamera();
    ShowMsg(L"PrismForge Virtual Camera has been removed from Windows Settings.\n(DirectShow only mode active)");
    return 0;
}

static int DoUnregister(const std::wstring& mfSourcePath, const std::wstring& softcamPath)
{
    std::wcout << L"=== PrismForge Virtual Camera Unregistration ===" << std::endl;

    RemoveSystemCamera();

    std::wstring mfDll = FindFile(L"prismforge_source.dll", mfSourcePath);
    if (!mfDll.empty()) {
        CallDllFunction(mfDll, "DllUnregisterServer");
        std::wcout << L"[OK] Unregistered prismforge_source.dll from HKLM." << std::endl;
    }

    std::wstring dsDll = FindFile(L"prismforge_cam.dll", softcamPath);
    if (!dsDll.empty()) {
        CallDllFunction(dsDll, "DllUnregisterServer");
        std::wcout << L"[OK] Unregistered prismforge_cam.dll." << std::endl;
    }

    ShowMsg(L"PrismForge Virtual Camera has been successfully uninstalled from the system.");
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring command = L"register";
    std::wstring mfPath = L"";
    std::wstring dsPath = L"";

    std::vector<std::wstring> cleanArgs;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"/silent" || arg == L"-silent" || arg == L"--silent" || arg == L"/s" || arg == L"-s") {
            g_silent = true;
        } else {
            cleanArgs.push_back(arg);
        }
    }

    if (!cleanArgs.empty()) {
        command = cleanArgs[0];
        if (cleanArgs.size() >= 2) mfPath = cleanArgs[1];
        if (cleanArgs.size() >= 3) dsPath = cleanArgs[2];
    }

    if (command == L"register" || command == L"install") {
        return DoRegister(mfPath, dsPath);
    } else if (command == L"mode-always") {
        return DoModeAlways(mfPath, dsPath);
    } else if (command == L"mode-dynamic") {
        return DoModeDynamic(mfPath, dsPath);
    } else if (command == L"mode-disabled") {
        return DoModeDisabled();
    } else if (command == L"unregister" || command == L"uninstall") {
        return DoUnregister(mfPath, dsPath);
    } else {
        std::wcout << L"Usage:" << std::endl;
        std::wcout << L"  prismforge_vcam_installer.exe register [/silent]" << std::endl;
        std::wcout << L"  prismforge_vcam_installer.exe mode-always [/silent]" << std::endl;
        std::wcout << L"  prismforge_vcam_installer.exe mode-dynamic [/silent]" << std::endl;
        std::wcout << L"  prismforge_vcam_installer.exe mode-disabled [/silent]" << std::endl;
        std::wcout << L"  prismforge_vcam_installer.exe unregister [/silent]" << std::endl;
        return 1;
    }
}
