#include "VCamDeviceManager.h"
#include "../settings.h"
#include "../logger.h"
#include <shellapi.h>
#include <vector>

VCamDeviceManager& VCamDeviceManager::Instance()
{
    static VCamDeviceManager s_instance;
    return s_instance;
}

VCamDeviceManager::VCamDeviceManager()
    : _currentMode(VCamMode::Dynamic)
    , _mfStarted(false)
{
}

VCamDeviceManager::~VCamDeviceManager()
{
    Shutdown();
}

void VCamDeviceManager::Initialize()
{
    int savedMode = Settings::Get("VIRTUAL_CAMERA_MODE");
    if (savedMode < 0 || savedMode > 2) {
        savedMode = 0; // Default: Dynamic (Mode 0)
    }
    _currentMode = (VCamMode)savedMode;

    if (_currentMode == VCamMode::Dynamic) {
        StartSessionCamera();
    }
}

void VCamDeviceManager::Shutdown()
{
    StopSessionCamera();

    if (_mfStarted) {
        MFShutdown();
        CoUninitialize();
        _mfStarted = false;
    }
}

static void EnsureUserComRegistered()
{
    HKEY hTest = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{2141C943-DD09-4915-BD14-264724DBA955}\\InprocServer32", 0, KEY_READ, &hTest) == ERROR_SUCCESS) {
        RegCloseKey(hTest);
        return; // Already registered
    }

    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);

    std::wstring candidates[] = {
        dir + L"prismforge_source.dll",
        dir + L"dist\\prismforge_source.dll",
        dir + L"..\\dist\\prismforge_source.dll",
        dir + L"..\\windows\\src\\mfsource\\prismforge_source.dll"
    };

    std::wstring dllPath;
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            dllPath = c;
            break;
        }
    }
    if (dllPath.empty()) return;

    HKEY hKey = nullptr;
    WCHAR keyPath[256];
    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\{2141C943-DD09-4915-BD14-264724DBA955}");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        const WCHAR desc[] = L"PrismForge Virtual Camera Source";
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)desc, (DWORD)((wcslen(desc) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }

    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\{2141C943-DD09-4915-BD14-264724DBA955}\\InprocServer32");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)dllPath.c_str(), (DWORD)((dllPath.length() + 1) * sizeof(WCHAR)));
        const WCHAR threading[] = L"Both";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)threading, (DWORD)((wcslen(threading) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

bool VCamDeviceManager::StartSessionCamera()
{
    if (_sessionCamera)
        return true;

    logger << "[CAM] Registering independent user source" << std::endl;
    EnsureUserComRegistered();

    if (!_mfStarted) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        logger << "[CAM] Starting Media Foundation" << std::endl;
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) return false;
        _mfStarted = true;
    }

    BOOL supported = FALSE;
    logger << "[CAM] Checking virtual-camera support" << std::endl;
    HRESULT hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (FAILED(hr) || !supported)
        return false;

    const WCHAR* SOURCE_CLSID = L"{2141C943-DD09-4915-BD14-264724DBA955}";
    const WCHAR* FRIENDLY_NAME = L"PrismForge Virtual Camera";

    logger << "[CAM] Creating session camera" << std::endl;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        FRIENDLY_NAME,
        SOURCE_CLSID,
        nullptr,
        0,
        &_sessionCamera
    );

    if (FAILED(hr) || !_sessionCamera) {
        _sessionCamera.Reset();
        return false;
    }

    logger << "[CAM] Starting session camera" << std::endl;
    hr = _sessionCamera->Start(nullptr);
    logger << "[CAM] Session camera Start result: " << hr << std::endl;
    if (FAILED(hr)) {
        _sessionCamera->Shutdown();
        _sessionCamera.Reset();
        return false;
    }

    return true;
}

void VCamDeviceManager::StopSessionCamera()
{
    if (_sessionCamera) {
        _sessionCamera->Stop();
        _sessionCamera->Shutdown();
        _sessionCamera.Reset();
    }
}

std::wstring VCamDeviceManager::GetInstallerPath() const
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring str(path);
    size_t pos = str.find_last_of(L"\\/");
    std::wstring exeDir = (pos != std::wstring::npos) ? str.substr(0, pos + 1) : L"";

    std::vector<std::wstring> candidates = {
        exeDir + L"prismforge_vcam_installer.exe",
        exeDir + L"dist\\prismforge_vcam_installer.exe",
        exeDir + L"..\\dist\\prismforge_vcam_installer.exe",
        exeDir + L"..\\prismforge_vcam_installer.exe",
        exeDir + L"src\\installer\\prismforge_vcam_installer.exe"
    };

    for (const auto& candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return L"";
}

bool VCamDeviceManager::RunInstaller(const std::wstring& argument, HWND parentHwnd)
{
    std::wstring installer = GetInstallerPath();
    if (installer.empty()) return false;

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = installer.c_str();
    sei.lpParameters = argument.c_str();
    sei.hwnd = parentHwnd;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei))
        return false;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return (exitCode == 0);
    }
    return true;
}

bool VCamDeviceManager::SetMode(VCamMode mode, HWND parentHwnd)
{
    if (mode == _currentMode)
        return true;

    VCamMode oldMode = _currentMode;
    _currentMode = mode;
    Settings::Set("VIRTUAL_CAMERA_MODE", (int)mode);
    Settings::Save();

    if (mode == VCamMode::Dynamic) {
        if (oldMode == VCamMode::Always) {
            RunInstaller(L"mode-dynamic", parentHwnd);
        }
        return StartSessionCamera();
    }
    else if (mode == VCamMode::Always) {
        StopSessionCamera();
        return RunInstaller(L"mode-always", parentHwnd);
    }
    else if (mode == VCamMode::Disabled) {
        StopSessionCamera();
        if (oldMode == VCamMode::Always) {
            RunInstaller(L"mode-disabled", parentHwnd);
        }
        return true;
    }

    return false;
}
