#include "d3d11_context.h"
#include <vector>

namespace PrismForge::Effects::Gpu {

D3D11Context::D3D11Context() = default;
D3D11Context::~D3D11Context() = default;

bool D3D11Context::Initialize(bool preferHardware) {
    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    UINT numFeatureLevels = ARRAYSIZE(featureLevels);

    HRESULT hr = E_FAIL;
    D3D_DRIVER_TYPE driverType = preferHardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;

    hr = D3D11CreateDevice(
        nullptr,
        driverType,
        nullptr,
        createFlags,
        featureLevels,
        numFeatureLevels,
        D3D11_SDK_VERSION,
        &m_device,
        &m_featureLevel,
        &m_context
    );

    // If hardware failed or debug layer failed, retry without debug flags or with WARP
    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            createFlags,
            featureLevels,
            numFeatureLevels,
            D3D11_SDK_VERSION,
            &m_device,
            &m_featureLevel,
            &m_context
        );
    }

    if (FAILED(hr) && preferHardware) {
        // Fallback to WARP
        driverType = D3D_DRIVER_TYPE_WARP;
        hr = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            createFlags,
            featureLevels,
            numFeatureLevels,
            D3D11_SDK_VERSION,
            &m_device,
            &m_featureLevel,
            &m_context
        );
    }

    if (FAILED(hr)) {
        return false;
    }

    m_isHardware = (driverType == D3D_DRIVER_TYPE_HARDWARE);

    // Query adapter details
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                // Convert wide string to std::string
                char nameBuf[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuf, sizeof(nameBuf) - 1, nullptr, nullptr);
                m_adapterName = nameBuf;
                m_dedicatedVramMB = desc.DedicatedVideoMemory / (1024 * 1024);
            }
        }
    }

    if (m_adapterName.empty()) {
        m_adapterName = m_isHardware ? "Direct3D 11 Hardware Device" : "Microsoft Basic Render Driver (WARP)";
    }

    return true;
}

void D3D11Context::Release() noexcept {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
        m_context.Reset();
    }
    m_device.Reset();
    m_adapterName.clear();
    m_dedicatedVramMB = 0;
    m_isHardware = false;
    m_featureLevel = D3D_FEATURE_LEVEL_11_0;
}

} // namespace PrismForge::Effects::Gpu
