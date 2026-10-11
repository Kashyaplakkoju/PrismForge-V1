#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>

namespace PrismForge::Effects::Gpu {

// Isolated Direct3D 11 device and context wrapper.
// Encapsulates hardware device creation with automatic WARP software rasterizer fallback.
class D3D11Context {
public:
    D3D11Context();
    ~D3D11Context();

    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    // Initializes D3D11 device and context.
    // Tries D3D_DRIVER_TYPE_HARDWARE first; falls back to D3D_DRIVER_TYPE_WARP if no hardware adapter is available.
    bool Initialize(bool preferHardware = true);

    // Tears down D3D11 device and context resources.
    void Release() noexcept;

    ID3D11Device* GetDevice() const noexcept { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const noexcept { return m_context.Get(); }

    const std::string& GetAdapterName() const noexcept { return m_adapterName; }
    size_t GetDedicatedVideoMemoryMB() const noexcept { return m_dedicatedVramMB; }
    bool IsHardwareAdapter() const noexcept { return m_isHardware; }
    D3D_FEATURE_LEVEL GetFeatureLevel() const noexcept { return m_featureLevel; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    std::string m_adapterName;
    size_t m_dedicatedVramMB = 0;
    bool m_isHardware = false;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
};

} // namespace PrismForge::Effects::Gpu
