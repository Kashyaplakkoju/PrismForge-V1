#include "d3d11_studiostack_background.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::StudioStack::Gpu {

namespace {

const char* g_backgroundShaders = R"(
// -------------------------------------------------------------
// Horizontal Box Blur Compute Shader
// -------------------------------------------------------------
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> HorizUav : register(u0);

cbuffer BlurCB : register(b0)
{
    float4 Params;     // (radius, width, height, bypass)
    float4 SolidColor; // (solidR/255, solidG/255, solidB/255, 1.0)
};

[numthreads(16, 16, 1)]
void CS_HorizBlur(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Params.y;
    uint height = (uint)Params.z;
    if (id.x >= width || id.y >= height)
        return;

    int r = (int)Params.x;
    if (r <= 0)
    {
        HorizUav[id.xy] = InputTex[id.xy];
        return;
    }

    int K = 2 * r + 1;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int dx = -r; dx <= r; ++dx)
    {
        int sx = clamp((int)id.x + dx, 0, (int)width - 1);
        sum += InputTex[uint2((uint)sx, id.y)];
    }

    HorizUav[id.xy] = sum / (float)K;
}

// -------------------------------------------------------------
// Vertical Box Blur & Foreground Compositing Compute Shader
// -------------------------------------------------------------
Texture2D<float4> HorizTex      : register(t0);
Texture2D<float4> InputTexFwd   : register(t1);
Texture2D<float>  MaskTex       : register(t2);
SamplerState      LinearSampler : register(s0);

RWTexture2D<float4> OutputUav : register(u0);

[numthreads(16, 16, 1)]
void CS_VertBlurAndComposite(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Params.y;
    uint height = (uint)Params.z;
    if (id.x >= width || id.y >= height)
        return;

    int r = (int)Params.x;
    int K = 2 * r + 1;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int dy = -r; dy <= r; ++dy)
    {
        int sy = clamp((int)id.y + dy, 0, (int)height - 1);
        sum += HorizTex[uint2(id.x, (uint)sy)];
    }

    float4 blurredBg = sum / (float)K;
    float4 fg = InputTexFwd[id.xy];

    float2 uv = (float2(id.xy) + 0.5f) / float2((float)width, (float)height);
    float alpha = saturate(MaskTex.SampleLevel(LinearSampler, uv, 0).r);

    float3 outRgb = fg.rgb * alpha + blurredBg.rgb * (1.0f - alpha);
    OutputUav[id.xy] = float4(outRgb, fg.a);
}

// -------------------------------------------------------------
// Solid Background Compositing Compute Shader
// -------------------------------------------------------------
[numthreads(16, 16, 1)]
void CS_SolidComposite(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Params.y;
    uint height = (uint)Params.z;
    if (id.x >= width || id.y >= height)
        return;

    float4 fg = InputTexFwd[id.xy];
    float2 uv = (float2(id.xy) + 0.5f) / float2((float)width, (float)height);
    float alpha = saturate(MaskTex.SampleLevel(LinearSampler, uv, 0).r);

    float3 outRgb = fg.rgb * alpha + SolidColor.rgb * (1.0f - alpha);
    OutputUav[id.xy] = float4(outRgb, fg.a);
}

// -------------------------------------------------------------
// Image Background Compositing Compute Shader
// -------------------------------------------------------------
Texture2D<float4> BgImageTex : register(t3);

cbuffer ImageParamsCB : register(b1)
{
    float4 ImageParams; // (bgW, bgH, fitMode, 0)
};

[numthreads(16, 16, 1)]
void CS_ImageComposite(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Params.y;
    uint height = (uint)Params.z;
    if (id.x >= width || id.y >= height)
        return;

    float4 fg = InputTexFwd[id.xy];
    float2 uv = (float2(id.xy) + 0.5f) / float2((float)width, (float)height);
    float alpha = saturate(MaskTex.SampleLevel(LinearSampler, uv, 0).r);

    float bgW = ImageParams.x;
    float bgH = ImageParams.y;
    int fitMode = (int)ImageParams.z; // 0 = CropFill, 1 = FitLetterbox, 2 = Stretch

    float4 bgPixel = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (bgW > 0.0f && bgH > 0.0f)
    {
        if (fitMode == 0) // CropFill (CoverCrop)
        {
            float scale = max((float)width / bgW, (float)height / bgH);
            float xInBg = ((float)id.x + 0.5f - (float)width * 0.5f) / scale + bgW * 0.5f;
            float yInBg = ((float)id.y + 0.5f - (float)height * 0.5f) / scale + bgH * 0.5f;
            float2 bgUv = float2(xInBg / bgW, yInBg / bgH);
            bgPixel = BgImageTex.SampleLevel(LinearSampler, saturate(bgUv), 0);
        }
        else if (fitMode == 1) // FitLetterbox
        {
            float scale = min((float)width / bgW, (float)height / bgH);
            float xInBg = ((float)id.x + 0.5f - (float)width * 0.5f) / scale + bgW * 0.5f;
            float yInBg = ((float)id.y + 0.5f - (float)height * 0.5f) / scale + bgH * 0.5f;
            if (xInBg >= 0.0f && xInBg <= bgW && yInBg >= 0.0f && yInBg <= bgH)
            {
                float2 bgUv = float2(xInBg / bgW, yInBg / bgH);
                bgPixel = BgImageTex.SampleLevel(LinearSampler, saturate(bgUv), 0);
            }
            else
            {
                bgPixel = float4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
        else // Stretch
        {
            bgPixel = BgImageTex.SampleLevel(LinearSampler, uv, 0);
        }
    }

    float3 outRgb = fg.rgb * alpha + bgPixel.rgb * (1.0f - alpha);
    OutputUav[id.xy] = float4(outRgb, fg.a);
}
)";

struct BackgroundConstantBuffer {
    float params[4];     // (radius, width, height, bypass)
    float solidColor[4]; // (r, g, b, 1.0)
};

struct ImageConstantBuffer {
    float imageParams[4]; // (bgW, bgH, fitMode, 0)
};

} // anonymous namespace

struct D3D11StudioStackBackground::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;

    ComPtr<ID3D11ComputeShader> m_csHorizBlur;
    ComPtr<ID3D11ComputeShader> m_csVertBlurAndComposite;
    ComPtr<ID3D11ComputeShader> m_csSolidComposite;
    ComPtr<ID3D11ComputeShader> m_csImageComposite;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    ComPtr<ID3D11Buffer> m_imageConstantBuffer;
    ComPtr<ID3D11SamplerState> m_linearSampler;

    // Intermediate horizontal buffer
    ComPtr<ID3D11Texture2D> m_horizTex;
    ComPtr<ID3D11ShaderResourceView> m_horizSRV;
    ComPtr<ID3D11UnorderedAccessView> m_horizUAV;
    int m_horizW = 0;
    int m_horizH = 0;

    bool m_initialized = false;

    Impl(ID3D11Device* dev, ID3D11DeviceContext* ctx)
        : m_device(dev), m_context(ctx)
    {
        Initialize();
    }

    bool Initialize() {
        if (!m_device || !m_context) {
            return false;
        }

        // Compile shaders
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> csHorizBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(
            g_backgroundShaders, std::strlen(g_backgroundShaders),
            "BackgroundShaders", nullptr, nullptr,
            "CS_HorizBlur", "cs_5_0", flags, 0,
            csHorizBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        ComPtr<ID3DBlob> csVertBlob;
        hr = D3DCompile(
            g_backgroundShaders, std::strlen(g_backgroundShaders),
            "BackgroundShaders", nullptr, nullptr,
            "CS_VertBlurAndComposite", "cs_5_0", flags, 0,
            csVertBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        ComPtr<ID3DBlob> csSolidBlob;
        hr = D3DCompile(
            g_backgroundShaders, std::strlen(g_backgroundShaders),
            "BackgroundShaders", nullptr, nullptr,
            "CS_SolidComposite", "cs_5_0", flags, 0,
            csSolidBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        ComPtr<ID3DBlob> csImageBlob;
        hr = D3DCompile(
            g_backgroundShaders, std::strlen(g_backgroundShaders),
            "BackgroundShaders", nullptr, nullptr,
            "CS_ImageComposite", "cs_5_0", flags, 0,
            csImageBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = m_device->CreateComputeShader(
            csHorizBlob->GetBufferPointer(), csHorizBlob->GetBufferSize(),
            nullptr, m_csHorizBlur.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = m_device->CreateComputeShader(
            csVertBlob->GetBufferPointer(), csVertBlob->GetBufferSize(),
            nullptr, m_csVertBlurAndComposite.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = m_device->CreateComputeShader(
            csSolidBlob->GetBufferPointer(), csSolidBlob->GetBufferSize(),
            nullptr, m_csSolidComposite.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = m_device->CreateComputeShader(
            csImageBlob->GetBufferPointer(), csImageBlob->GetBufferSize(),
            nullptr, m_csImageComposite.GetAddressOf());
        if (FAILED(hr)) return false;

        // Create constant buffers
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(BackgroundConstantBuffer);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC imgCbDesc{};
        imgCbDesc.ByteWidth = sizeof(ImageConstantBuffer);
        imgCbDesc.Usage = D3D11_USAGE_DYNAMIC;
        imgCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        imgCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&imgCbDesc, nullptr, m_imageConstantBuffer.GetAddressOf());
        if (FAILED(hr)) return false;

        // Create bilinear clamp sampler
        D3D11_SAMPLER_DESC sampDesc{};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampDesc.MinLOD = 0;
        sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&sampDesc, m_linearSampler.GetAddressOf());
        if (FAILED(hr)) return false;

        m_initialized = true;
        return true;
    }

    bool EnsureHorizBuffer(int w, int h) {
        if (m_horizTex && m_horizW == w && m_horizH == h) {
            return true;
        }

        m_horizTex.Reset();
        m_horizSRV.Reset();
        m_horizUAV.Reset();
        m_horizW = 0;
        m_horizH = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_horizTex.GetAddressOf());
        if (FAILED(hr)) {
            m_horizTex.Reset();
            return false;
        }

        hr = m_device->CreateShaderResourceView(m_horizTex.Get(), nullptr, m_horizSRV.GetAddressOf());
        if (FAILED(hr)) {
            m_horizTex.Reset();
            m_horizSRV.Reset();
            return false;
        }

        hr = m_device->CreateUnorderedAccessView(m_horizTex.Get(), nullptr, m_horizUAV.GetAddressOf());
        if (FAILED(hr)) {
            m_horizTex.Reset();
            m_horizSRV.Reset();
            m_horizUAV.Reset();
            return false;
        }

        m_horizW = w;
        m_horizH = h;
        return true;
    }

    GpuBackgroundOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        ID3D11ShaderResourceView* maskSRV,
        int width, int height,
        const BackgroundSettings& settings,
        ID3D11ShaderResourceView* bgImageSRV,
        int bgImageW,
        int bgImageH,
        int fitMode,
        bool isImageMode)
    {
        if (IsDeviceLost()) {
            return GpuBackgroundOutcome::DeviceLost;
        }

        if (!isImageMode && settings.mode == BackgroundMode::Original) {
            return GpuBackgroundOutcome::Bypass;
        }

        if (!inputSRV || !outputUAV || width <= 0 || height <= 0) {
            return GpuBackgroundOutcome::InvalidInput;
        }

        // Active background replacement requires a valid mask
        if (!maskSRV) {
            return GpuBackgroundOutcome::NotReady;
        }

        if (!m_initialized) {
            if (!Initialize()) {
                return GpuBackgroundOutcome::ShaderError;
            }
        }

        if (isImageMode) {
            if (!bgImageSRV || bgImageW <= 0 || bgImageH <= 0) {
                return GpuBackgroundOutcome::InvalidInput;
            }

            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr)) return GpuBackgroundOutcome::ShaderError;

            auto* cb = static_cast<BackgroundConstantBuffer*>(mapped.pData);
            cb->params[0] = 0.0f;
            cb->params[1] = static_cast<float>(width);
            cb->params[2] = static_cast<float>(height);
            cb->params[3] = 0.0f;
            m_context->Unmap(m_constantBuffer.Get(), 0);

            // Update image constant buffer
            D3D11_MAPPED_SUBRESOURCE imgMapped{};
            hr = m_context->Map(m_imageConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &imgMapped);
            if (FAILED(hr)) return GpuBackgroundOutcome::ShaderError;

            auto* icb = static_cast<ImageConstantBuffer*>(imgMapped.pData);
            icb->imageParams[0] = static_cast<float>(bgImageW);
            icb->imageParams[1] = static_cast<float>(bgImageH);
            icb->imageParams[2] = static_cast<float>(fitMode);
            icb->imageParams[3] = 0.0f;
            m_context->Unmap(m_imageConstantBuffer.Get(), 0);

            UINT dispatchX = (width + 15) / 16;
            UINT dispatchY = (height + 15) / 16;

            m_context->CSSetShader(m_csImageComposite.Get(), nullptr, 0);
            ID3D11Buffer* cbs[] = { m_constantBuffer.Get(), m_imageConstantBuffer.Get() };
            m_context->CSSetConstantBuffers(0, 2, cbs);

            ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
            m_context->CSSetSamplers(0, 1, samplers);

            ID3D11ShaderResourceView* srvs[] = { nullptr, inputSRV, maskSRV, bgImageSRV };
            m_context->CSSetShaderResources(0, 4, srvs);

            ID3D11UnorderedAccessView* uavs[] = { outputUAV };
            m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

            m_context->Dispatch(dispatchX, dispatchY, 1);

            // Unbind all
            ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
            ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
            m_context->CSSetShaderResources(0, 4, nullSRVs);
            m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
            ID3D11Buffer* nullCBs[2] = { nullptr, nullptr };
            m_context->CSSetConstantBuffers(0, 2, nullCBs);
            ID3D11SamplerState* nullSamplers[1] = { nullptr };
            m_context->CSSetSamplers(0, 1, nullSamplers);
            m_context->CSSetShader(nullptr, nullptr, 0);

            return GpuBackgroundOutcome::Success;
        }

        if (settings.mode == BackgroundMode::Blur) {
            const int radius = std::clamp(settings.blurRadius, 0, 64);
            if (radius == 0) {
                return GpuBackgroundOutcome::Bypass;
            }

            if (!EnsureHorizBuffer(width, height)) {
                return GpuBackgroundOutcome::ShaderError;
            }

            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr)) return GpuBackgroundOutcome::ShaderError;

            auto* cb = static_cast<BackgroundConstantBuffer*>(mapped.pData);
            cb->params[0] = static_cast<float>(radius);
            cb->params[1] = static_cast<float>(width);
            cb->params[2] = static_cast<float>(height);
            cb->params[3] = 0.0f; // not bypassed

            cb->solidColor[0] = static_cast<float>(settings.solidR) / 255.0f;
            cb->solidColor[1] = static_cast<float>(settings.solidG) / 255.0f;
            cb->solidColor[2] = static_cast<float>(settings.solidB) / 255.0f;
            cb->solidColor[3] = 1.0f;
            m_context->Unmap(m_constantBuffer.Get(), 0);

            UINT dispatchX = (width + 15) / 16;
            UINT dispatchY = (height + 15) / 16;

            // -------------------------------------------------------------
            // Pass 1: Horizontal Blur (InputTex -> HorizUav)
            // -------------------------------------------------------------
            m_context->CSSetShader(m_csHorizBlur.Get(), nullptr, 0);
            ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
            m_context->CSSetConstantBuffers(0, 1, cbs);

            ID3D11ShaderResourceView* srvsPass1[] = { inputSRV };
            m_context->CSSetShaderResources(0, 1, srvsPass1);

            ID3D11UnorderedAccessView* uavsPass1[] = { m_horizUAV.Get() };
            m_context->CSSetUnorderedAccessViews(0, 1, uavsPass1, nullptr);

            m_context->Dispatch(dispatchX, dispatchY, 1);

            // Unbind Pass 1 resources to prevent hazards
            ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
            ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
            m_context->CSSetShaderResources(0, 4, nullSRVs);
            m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

            // -------------------------------------------------------------
            // Pass 2: Vertical Blur & Composite (HorizTex + InputTex + Mask -> OutputUav)
            // -------------------------------------------------------------
            m_context->CSSetShader(m_csVertBlurAndComposite.Get(), nullptr, 0);
            m_context->CSSetConstantBuffers(0, 1, cbs);

            ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
            m_context->CSSetSamplers(0, 1, samplers);

            ID3D11ShaderResourceView* srvsPass2[] = { m_horizSRV.Get(), inputSRV, maskSRV };
            m_context->CSSetShaderResources(0, 3, srvsPass2);

            ID3D11UnorderedAccessView* uavsPass2[] = { outputUAV };
            m_context->CSSetUnorderedAccessViews(0, 1, uavsPass2, nullptr);

            m_context->Dispatch(dispatchX, dispatchY, 1);

            // Unbind all
            m_context->CSSetShaderResources(0, 4, nullSRVs);
            m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
            ID3D11Buffer* nullCBs[2] = { nullptr, nullptr };
            m_context->CSSetConstantBuffers(0, 2, nullCBs);
            ID3D11SamplerState* nullSamplers[1] = { nullptr };
            m_context->CSSetSamplers(0, 1, nullSamplers);
            m_context->CSSetShader(nullptr, nullptr, 0);

            return GpuBackgroundOutcome::Success;
        } else if (settings.mode == BackgroundMode::Solid) {
            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr)) return GpuBackgroundOutcome::ShaderError;

            auto* cb = static_cast<BackgroundConstantBuffer*>(mapped.pData);
            cb->params[0] = 0.0f;
            cb->params[1] = static_cast<float>(width);
            cb->params[2] = static_cast<float>(height);
            cb->params[3] = 0.0f;

            cb->solidColor[0] = static_cast<float>(settings.solidR) / 255.0f;
            cb->solidColor[1] = static_cast<float>(settings.solidG) / 255.0f;
            cb->solidColor[2] = static_cast<float>(settings.solidB) / 255.0f;
            cb->solidColor[3] = 1.0f;
            m_context->Unmap(m_constantBuffer.Get(), 0);

            UINT dispatchX = (width + 15) / 16;
            UINT dispatchY = (height + 15) / 16;

            // Dispatch Solid Composite (InputTex + Mask -> OutputUav)
            m_context->CSSetShader(m_csSolidComposite.Get(), nullptr, 0);
            ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
            m_context->CSSetConstantBuffers(0, 1, cbs);

            ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
            m_context->CSSetSamplers(0, 1, samplers);

            ID3D11ShaderResourceView* srvs[] = { nullptr, inputSRV, maskSRV };
            m_context->CSSetShaderResources(0, 3, srvs);

            ID3D11UnorderedAccessView* uavs[] = { outputUAV };
            m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

            m_context->Dispatch(dispatchX, dispatchY, 1);

            // Unbind all
            ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
            ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
            m_context->CSSetShaderResources(0, 4, nullSRVs);
            m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
            ID3D11Buffer* nullCBs[2] = { nullptr, nullptr };
            m_context->CSSetConstantBuffers(0, 2, nullCBs);
            ID3D11SamplerState* nullSamplers[1] = { nullptr };
            m_context->CSSetSamplers(0, 1, nullSamplers);
            m_context->CSSetShader(nullptr, nullptr, 0);

            return GpuBackgroundOutcome::Success;
        }

        return GpuBackgroundOutcome::Bypass;
    }

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
        m_csHorizBlur.Reset();
        m_csVertBlurAndComposite.Reset();
        m_csSolidComposite.Reset();
        m_csImageComposite.Reset();
        m_constantBuffer.Reset();
        m_imageConstantBuffer.Reset();
        m_linearSampler.Reset();
        m_horizTex.Reset();
        m_horizSRV.Reset();
        m_horizUAV.Reset();
        m_horizW = 0;
        m_horizH = 0;
        m_initialized = false;

        m_device = newDevice;
        m_context = newContext;
        if (m_device && m_context) {
            return Initialize();
        }
        return true;
    }

    bool IsDeviceLost() const {
        if (!m_device) return true;
        HRESULT hr = m_device->GetDeviceRemovedReason();
        return (hr != S_OK);
    }
};

D3D11StudioStackBackground::D3D11StudioStackBackground(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context))
{
}

D3D11StudioStackBackground::~D3D11StudioStackBackground() = default;
D3D11StudioStackBackground::D3D11StudioStackBackground(D3D11StudioStackBackground&&) noexcept = default;
D3D11StudioStackBackground& D3D11StudioStackBackground::operator=(D3D11StudioStackBackground&&) noexcept = default;

GpuBackgroundOutcome D3D11StudioStackBackground::Process(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11UnorderedAccessView* outputUAV,
    ID3D11ShaderResourceView* maskSRV,
    int width, int height,
    const BackgroundSettings& settings,
    ID3D11ShaderResourceView* bgImageSRV,
    int bgImageW,
    int bgImageH,
    int fitMode,
    bool isImageMode)
{
    return m_impl ? m_impl->Process(inputSRV, outputUAV, maskSRV, width, height, settings,
                                    bgImageSRV, bgImageW, bgImageH, fitMode, isImageMode)
                  : GpuBackgroundOutcome::NotReady;
}

bool D3D11StudioStackBackground::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    return m_impl ? m_impl->ResetDevice(newDevice, newContext) : false;
}

bool D3D11StudioStackBackground::IsDeviceLost() const {
    return m_impl ? m_impl->IsDeviceLost() : true;
}

} // namespace PrismForge::Effects::StudioStack::Gpu
