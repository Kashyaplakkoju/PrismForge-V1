#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "../appearanceprocessor.h"

namespace PrismForge::Effects::Appearance::Gpu {

enum class GpuAppearanceOutcome : int {
    Success = 0,
    Bypass = 1,
    NotReady = 2,
    InvalidInput = 3,
    UnsupportedFormat = 4,
    DeviceLost = 5,
    ShaderError = 6
};

/// <summary>
/// Texture-Native D3D11 Appearance Processing Engine.
/// Executes Compute Shader on caller-provided ID3D11Device and context.
/// </summary>
class D3D11AppearanceProcessor {
public:
    D3D11AppearanceProcessor(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11AppearanceProcessor();

    D3D11AppearanceProcessor(const D3D11AppearanceProcessor&) = delete;
    D3D11AppearanceProcessor& operator=(const D3D11AppearanceProcessor&) = delete;
    D3D11AppearanceProcessor(D3D11AppearanceProcessor&&) noexcept;
    D3D11AppearanceProcessor& operator=(D3D11AppearanceProcessor&&) noexcept;

    GpuAppearanceOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* skinMaskSRV,
        ID3D11ShaderResourceView* protectionMaskSRV,
        int width, int height,
        const AppearanceConfig& config
    );

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);
    bool IsDeviceLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// <summary>
/// Standalone Test Adapter & CPU Fallback Wrapper for Appearance Processing.
/// </summary>
class D3D11AppearanceProcessorAdapter {
public:
    struct TimingStats {
        double uploadMs = 0.0;
        double dispatchGpuMs = 0.0;
        double readbackMs = 0.0;
        double totalRoundtripMs = 0.0;
        bool fallbackUsed = false;
    };

    D3D11AppearanceProcessorAdapter(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11AppearanceProcessorAdapter();

    AVFrame* ProcessFrame(
        const AVFrame* inputFrame,
        const SkinMask* skinMask,
        const ProtectionMask* protectionMask,
        const AppearanceConfig& config,
        AppearanceOutcome& outcome,
        TimingStats* outTiming = nullptr
    );

    D3D11AppearanceProcessor* GetGpuOperator();
    AppearanceProcessor* GetCpuFallback();

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects::Appearance::Gpu
