#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <string>

enum class VCamMode
{
    Dynamic = 0,   // Show only when app is active (Default)
    Always = 1,    // Always show in Windows Settings (Persistent)
    Disabled = 2   // Do not show in Windows Settings at all (DirectShow only)
};

class VCamDeviceManager
{
public:
    static VCamDeviceManager& Instance();

    void Initialize();
    void Shutdown();

    VCamMode GetCurrentMode() const { return _currentMode; }
    bool SetMode(VCamMode mode, HWND parentHwnd = nullptr);

    bool IsSessionCameraActive() const { return _sessionCamera != nullptr; }

private:
    VCamDeviceManager();
    ~VCamDeviceManager();

    bool StartSessionCamera();
    void StopSessionCamera();

    bool RunInstaller(const std::wstring& argument, HWND parentHwnd);
    std::wstring GetInstallerPath() const;

    VCamMode _currentMode;
    bool _mfStarted;
    Microsoft::WRL::ComPtr<IMFVirtualCamera> _sessionCamera;
};
