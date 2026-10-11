#include "d3d11_approximate_shadows.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::Relighting::Gpu {

namespace {

bool ValidTextureView(ID3D11View* view, ID3D11Device* device, int width, int height, DXGI_FORMAT format) {
    if(!view || !device || width<=0 || height<=0 || width>8192 || height>8192) return false;
    ComPtr<ID3D11Device> owner; view->GetDevice(&owner); if(owner.Get()!=device) return false;
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture; if(FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    return desc.Width==static_cast<UINT>(width) && desc.Height==static_cast<UINT>(height) && desc.Format==format &&
        desc.MipLevels==1 && desc.ArraySize==1 && desc.SampleDesc.Count==1;
}

const char* g_shadowComputeShader = R"(
Texture2D<float> DepthTex    : register(t0);
Texture2D<float4> NormalsTex : register(t1);
Texture2D<float> MaskTex     : register(t2);

RWTexture2D<float2> OutputShadowUav : register(u0);

cbuffer ShadowCB : register(b0)
{
    float4 Light1Pos;    // (x, y, z, enabled)
    float4 Light1Params; // (intensity, pad, pad, pad)
    float4 Light2Pos;    // (x, y, z, enabled)
    float4 Light2Params; // (intensity, pad, pad, pad)
    float4 ShadowConfig; // (maxSteps, maxDistance, rayElevation, shadowIntensity)
    float4 ExtraConfig;  // (shadowSoftness, aspect, hasMask, hasNormals)
    float4 ScreenParams; // (width, height, enabled, pad)
};

float ComputeLightVisibilityGpu(
    float u, float v,
    float zSurface,
    float3 N,
    float4 lPos,
    float lIntensity,
    float aspect,
    uint width,
    uint height)
{
    if (lPos.w <= 0.5f || lIntensity <= 0.0f || ShadowConfig.w <= 0.0f)
        return 1.0f;

    if (zSurface < 0.0f || zSurface > 1.0f || isnan(zSurface) || isinf(zSurface))
        return 1.0f;

    float lx = clamp(lPos.x, -1.0f, 1.0f);
    float ly = clamp(lPos.y, -1.0f, 1.0f);
    float lz = max(0.1f, lPos.z);

    float deltaX = (lx - u) * aspect;
    float deltaY = ly - v;
    float dist2D = sqrt(deltaX * deltaX + deltaY * deltaY);
    if (dist2D < 1e-4f)
        return 1.0f;

    float dist3D = sqrt(dist2D * dist2D + lz * lz);
    float3 L3 = float3(deltaX, deltaY, lz) / dist3D;
    float dotNL = dot(N, L3);
    if (dotNL <= 0.0f)
    {
        return max(0.0f, 1.0f - ShadowConfig.w);
    }

    float maxDist = min(dist2D * 0.5f, clamp(ShadowConfig.y, 0.05f, 0.50f));
    int steps = (int)clamp(ShadowConfig.x, 4.0f, 32.0f);
    float stepDist = maxDist / float(steps);

    float raySlope = (lz / (dist3D + 0.1f)) + ShadowConfig.z;
    float tStart = stepDist * 0.5f;
    float bias = 0.003f + 0.005f * (1.0f - max(0.0f, N.z));

    float dirX = deltaX / dist2D;
    float dirY = deltaY / dist2D;

    float minVis = 1.0f;

    for (int i = 0; i < steps; ++i)
    {
        float t = tStart + float(i) * stepDist;
        float su = u + (dirX / aspect) * t;
        float sv = v + dirY * t;

        if (su < -1.0f || su > 1.0f || sv < -1.0f || sv > 1.0f)
            continue;

        int px = (int)((su + 1.0f) * 0.5f * float(width - 1) + 0.5f);
        int py = (int)((sv + 1.0f) * 0.5f * float(height - 1) + 0.5f);
        px = clamp(px, 0, (int)width - 1);
        py = clamp(py, 0, (int)height - 1);

        float zSample = DepthTex[uint2(px, py)];
        if (zSample < 0.0f || zSample > 1.0f || isnan(zSample) || isinf(zSample))
            continue;

        float zRay = zSurface - t * raySlope;

        if (zSample < (zRay - bias))
        {
            float deltaZ = (zRay - bias) - zSample;
            if (deltaZ < 0.35f)
            {
                float penumbra = ExtraConfig.x * t + 0.02f;
                float stepVis = saturate(1.0f - (deltaZ / penumbra));
                minVis = min(minVis, stepVis);
                if (minVis <= 0.0f)
                    break;
            }
        }
    }

    float effVis = 1.0f - ShadowConfig.w * (1.0f - minVis);
    return saturate(effVis);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)ScreenParams.x;
    uint height = (uint)ScreenParams.y;
    if (id.x >= width || id.y >= height)
        return;

    // If shadows not enabled
    if (ScreenParams.z < 0.5f)
    {
        OutputShadowUav[id.xy] = float2(1.0f, 1.0f);
        return;
    }

    bool hasMask = ExtraConfig.z > 0.5f;
    if (hasMask)
    {
        float alpha = MaskTex[id.xy];
        if (!isfinite(alpha) || alpha <= 0.0f)
        {
            OutputShadowUav[id.xy] = float2(1.0f, 1.0f);
            return;
        }
    }

    float zSurface = DepthTex[id.xy];
    if (zSurface < 0.0f || zSurface > 1.0f || isnan(zSurface) || isinf(zSurface))
    {
        OutputShadowUav[id.xy] = float2(1.0f, 1.0f);
        return;
    }

    float aspect = ExtraConfig.y;
    float u = 2.0f * ((float(id.x) + 0.5f) / float(width)) - 1.0f;
    float v = 2.0f * ((float(id.y) + 0.5f) / float(height)) - 1.0f;

    float3 N = float3(0.0f, 0.0f, 1.0f);
    bool hasNormals = ExtraConfig.w > 0.5f;
    if (hasNormals)
    {
        float4 rawN = NormalsTex[id.xy];
        float nLenSq = dot(rawN.xyz, rawN.xyz);
        if (isfinite(nLenSq) && nLenSq > 1e-6f)
        {
            N = normalize(rawN.xyz);
        }
    }

    float v1 = ComputeLightVisibilityGpu(u, v, zSurface, N, Light1Pos, Light1Params.x, aspect, width, height);
    float v2 = ComputeLightVisibilityGpu(u, v, zSurface, N, Light2Pos, Light2Params.x, aspect, width, height);

    OutputShadowUav[id.xy] = float2(v1, v2);
}
)";

struct alignas(16) ShadowCBData {
    float Light1Pos[4];    // (x, y, z, enabled)
    float Light1Params[4]; // (intensity, pad, pad, pad)
    float Light2Pos[4];    // (x, y, z, enabled)
    float Light2Params[4]; // (intensity, pad, pad, pad)
    float ShadowConfig[4]; // (maxSteps, maxDistance, rayElevation, shadowIntensity)
    float ExtraConfig[4];  // (shadowSoftness, aspect, hasMask, hasNormals)
    float ScreenParams[4]; // (width, height, enabled, pad)
};

} // anonymous namespace

struct D3D11ApproximateShadows::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11ComputeShader> m_computeShader;
    ComPtr<ID3D11Buffer> m_constantBuffer;

    // Managed output texture and views
    ComPtr<ID3D11Texture2D> m_outputTexture;
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    ComPtr<ID3D11ShaderResourceView> m_outputSRV;
    int m_allocatedW = 0;
    int m_allocatedH = 0;

    // Test staging resources
    ComPtr<ID3D11Texture2D> m_depthUploadTex;
    ComPtr<ID3D11ShaderResourceView> m_depthUploadSRV;
    ComPtr<ID3D11Texture2D> m_normalsUploadTex;
    ComPtr<ID3D11ShaderResourceView> m_normalsUploadSRV;
    ComPtr<ID3D11Texture2D> m_maskUploadTex;
    ComPtr<ID3D11ShaderResourceView> m_maskUploadSRV;
    ComPtr<ID3D11Texture2D> m_readbackTex;
    int m_uploadW = 0;
    int m_uploadH = 0;

    GpuShadowFaultSeam m_faultSeam = GpuShadowFaultSeam::None;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {}

    bool CompileShader() {
        if (m_faultSeam == GpuShadowFaultSeam::Shader_Compile) {
            return false;
        }
        if (m_computeShader) {
            return true;
        }

        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(
            g_shadowComputeShader,
            strlen(g_shadowComputeShader),
            "CS_ApproximateGeometryShadows",
            nullptr,
            nullptr,
            "CSMain",
            "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            csBlob.GetAddressOf(),
            errorBlob.GetAddressOf()
        );

        if (FAILED(hr) || !csBlob) {
            return false;
        }

        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(),
            nullptr,
            m_computeShader.GetAddressOf()
        );

        return SUCCEEDED(hr);
    }

    bool EnsureConstantBuffer() {
        if (m_faultSeam == GpuShadowFaultSeam::Cb_Creation) {
            return false;
        }
        if (m_constantBuffer) {
            return true;
        }

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(ShadowCBData);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateBuffer(&desc, nullptr, m_constantBuffer.GetAddressOf());
        return SUCCEEDED(hr);
    }

    bool EnsureOutputResources(int width, int height) {
        if (m_faultSeam == GpuShadowFaultSeam::OutputUav_Creation) {
            return false;
        }
        if (m_outputTexture && m_outputUAV && m_outputSRV && m_allocatedW == width && m_allocatedH == height) {
            return true;
        }

        m_outputUAV.Reset();
        m_outputSRV.Reset();
        m_outputTexture.Reset();
        m_allocatedW = 0;
        m_allocatedH = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_outputTexture.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        hr = m_device->CreateUnorderedAccessView(m_outputTexture.Get(), &uavDesc, m_outputUAV.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = m_device->CreateShaderResourceView(m_outputTexture.Get(), &srvDesc, m_outputSRV.GetAddressOf());
        if (FAILED(hr)) return false;

        m_allocatedW = width;
        m_allocatedH = height;
        return true;
    }
};

D3D11ApproximateShadows::D3D11ApproximateShadows(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context)) {}

D3D11ApproximateShadows::~D3D11ApproximateShadows() = default;
D3D11ApproximateShadows::D3D11ApproximateShadows(D3D11ApproximateShadows&&) noexcept = default;
D3D11ApproximateShadows& D3D11ApproximateShadows::operator=(D3D11ApproximateShadows&&) noexcept = default;

void D3D11ApproximateShadows::SetFaultSeam(GpuShadowFaultSeam seam) {
    m_impl->m_faultSeam = seam;
}

void D3D11ApproximateShadows::ReleaseResources() {
    m_impl->m_outputUAV.Reset();
    m_impl->m_outputSRV.Reset();
    m_impl->m_outputTexture.Reset();
    m_impl->m_allocatedW = 0;
    m_impl->m_allocatedH = 0;

    m_impl->m_depthUploadTex.Reset();
    m_impl->m_depthUploadSRV.Reset();
    m_impl->m_normalsUploadTex.Reset();
    m_impl->m_normalsUploadSRV.Reset();
    m_impl->m_maskUploadTex.Reset();
    m_impl->m_maskUploadSRV.Reset();
    m_impl->m_readbackTex.Reset();
    m_impl->m_uploadW = 0;
    m_impl->m_uploadH = 0;
}

bool D3D11ApproximateShadows::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    ReleaseResources();
    m_impl->m_computeShader.Reset();
    m_impl->m_constantBuffer.Reset();
    m_impl->m_device = newDevice;
    m_impl->m_context = newContext;
    return (newDevice != nullptr && newContext != nullptr);
}

bool D3D11ApproximateShadows::IsDeviceLost() const {
    if (!m_impl || !m_impl->m_device) return true;
    HRESULT reason = m_impl->m_device->GetDeviceRemovedReason();
    return FAILED(reason);
}

ID3D11ShaderResourceView* D3D11ApproximateShadows::GetOutputSRV() const {
    return m_impl->m_outputSRV.Get();
}

ID3D11Texture2D* D3D11ApproximateShadows::GetOutputTexture() const {
    return m_impl->m_outputTexture.Get();
}

bool D3D11ApproximateShadows::HasOutput() const {
    return (m_impl->m_outputSRV != nullptr);
}

GpuShadowOutcome D3D11ApproximateShadows::Process(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* maskSRV,
    ID3D11UnorderedAccessView* outputUAV,
    int width, int height,
    const PrismForge::Effects::Lighting::LightingConfig& config
) {
    if (IsDeviceLost() || m_impl->m_faultSeam == GpuShadowFaultSeam::Device_Removed) {
        return GpuShadowOutcome::DeviceLost;
    }

    if (width <= 0 || height <= 0 || width>8192 || height>8192 || !outputUAV) {
        return GpuShadowOutcome::InvalidInput;
    }

    const auto& shadowCfg = config.shadowConfig;
    if (!shadowCfg.enabled) return GpuShadowOutcome::Bypass;
    if(!depthSRV) return GpuShadowOutcome::NotReady;
    if(!ValidTextureView(depthSRV,m_impl->m_device.Get(),width,height,DXGI_FORMAT_R32_FLOAT) ||
        !ValidTextureView(outputUAV,m_impl->m_device.Get(),width,height,DXGI_FORMAT_R32G32_FLOAT) ||
        (normalsSRV && !ValidTextureView(normalsSRV,m_impl->m_device.Get(),width,height,DXGI_FORMAT_R32G32B32A32_FLOAT)) ||
        (maskSRV && !ValidTextureView(maskSRV,m_impl->m_device.Get(),width,height,DXGI_FORMAT_R32_FLOAT)) ||
        !std::isfinite(shadowCfg.maxDistance) || !std::isfinite(shadowCfg.rayElevation) ||
        !std::isfinite(shadowCfg.shadowIntensity) || !std::isfinite(shadowCfg.shadowSoftness) ||
        !std::isfinite(config.light1.posX) || !std::isfinite(config.light1.posY) || !std::isfinite(config.light1.posZ) || !std::isfinite(config.light1.intensity) ||
        !std::isfinite(config.light2.posX) || !std::isfinite(config.light2.posY) || !std::isfinite(config.light2.posZ) || !std::isfinite(config.light2.intensity)) return GpuShadowOutcome::InvalidInput;

    if (!m_impl->CompileShader()) {
        return GpuShadowOutcome::ShaderError;
    }

    if (!m_impl->EnsureConstantBuffer()) {
        return GpuShadowOutcome::ResourceCreationFailed;
    }

    // Populate constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_impl->m_context->Map(m_impl->m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        return GpuShadowOutcome::ResourceCreationFailed;
    }

    ShadowCBData* cb = static_cast<ShadowCBData*>(mapped.pData);
    cb->Light1Pos[0] = config.light1.posX;
    cb->Light1Pos[1] = config.light1.posY;
    cb->Light1Pos[2] = config.light1.posZ;
    cb->Light1Pos[3] = (config.light1.enabled && config.light1.intensity > 0.0f) ? 1.0f : 0.0f;

    cb->Light1Params[0] = config.light1.intensity;
    cb->Light1Params[1] = 0.0f;
    cb->Light1Params[2] = 0.0f;
    cb->Light1Params[3] = 0.0f;

    cb->Light2Pos[0] = config.light2.posX;
    cb->Light2Pos[1] = config.light2.posY;
    cb->Light2Pos[2] = config.light2.posZ;
    cb->Light2Pos[3] = (config.light2.enabled && config.light2.intensity > 0.0f) ? 1.0f : 0.0f;

    cb->Light2Params[0] = config.light2.intensity;
    cb->Light2Params[1] = 0.0f;
    cb->Light2Params[2] = 0.0f;
    cb->Light2Params[3] = 0.0f;

    cb->ShadowConfig[0] = static_cast<float>(shadowCfg.maxSteps);
    cb->ShadowConfig[1] = shadowCfg.maxDistance;
    cb->ShadowConfig[2] = shadowCfg.rayElevation;
    cb->ShadowConfig[3] = shadowCfg.shadowIntensity;

    cb->ExtraConfig[0] = shadowCfg.shadowSoftness;
    cb->ExtraConfig[1] = static_cast<float>(width) / static_cast<float>(height);
    cb->ExtraConfig[2] = (maskSRV != nullptr) ? 1.0f : 0.0f;
    cb->ExtraConfig[3] = (normalsSRV != nullptr) ? 1.0f : 0.0f;

    cb->ScreenParams[0] = static_cast<float>(width);
    cb->ScreenParams[1] = static_cast<float>(height);
    cb->ScreenParams[2] = shadowCfg.enabled ? 1.0f : 0.0f;
    cb->ScreenParams[3] = 0.0f;

    m_impl->m_context->Unmap(m_impl->m_constantBuffer.Get(), 0);

    if (m_impl->m_faultSeam == GpuShadowFaultSeam::Dispatch) {
        return GpuShadowOutcome::ShaderError;
    }

    // Bind resources
    ID3D11ShaderResourceView* srvs[3] = { depthSRV, normalsSRV, maskSRV };
    m_impl->m_context->CSSetShaderResources(0, 3, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { outputUAV };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    ID3D11Buffer* cbs[1] = { m_impl->m_constantBuffer.Get() };
    m_impl->m_context->CSSetConstantBuffers(0, 1, cbs);

    m_impl->m_context->CSSetShader(m_impl->m_computeShader.Get(), nullptr, 0);

    // Dispatch
    UINT dispatchX = (static_cast<UINT>(width) + 15) / 16;
    UINT dispatchY = (static_cast<UINT>(height) + 15) / 16;
    m_impl->m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind resources to prevent hazards
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    m_impl->m_context->CSSetShaderResources(0, 3, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

    ID3D11Buffer* nullCBs[1] = { nullptr };
    m_impl->m_context->CSSetConstantBuffers(0, 1, nullCBs);

    m_impl->m_context->CSSetShader(nullptr, nullptr, 0);

    return GpuShadowOutcome::Success;
}

GpuShadowOutcome D3D11ApproximateShadows::ExecuteShadowPass(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* maskSRV,
    int width, int height,
    const PrismForge::Effects::Lighting::LightingConfig& config
) {
    if(width<=0 || height<=0 || width>8192 || height>8192) {
        m_impl->m_outputUAV.Reset(); m_impl->m_outputSRV.Reset(); m_impl->m_outputTexture.Reset();
        return GpuShadowOutcome::InvalidInput;
    }
    if (!m_impl->EnsureOutputResources(width, height)) {
        m_impl->m_outputUAV.Reset();
        m_impl->m_outputSRV.Reset();
        m_impl->m_outputTexture.Reset();
        return GpuShadowOutcome::ResourceCreationFailed;
    }

    GpuShadowOutcome outcome = Process(
        depthSRV, normalsSRV, maskSRV,
        m_impl->m_outputUAV.Get(),
        width, height, config
    );

    if (outcome != GpuShadowOutcome::Success) {
        m_impl->m_outputUAV.Reset();
        m_impl->m_outputSRV.Reset();
        m_impl->m_outputTexture.Reset();
    }

    return outcome;
}

bool D3D11ApproximateShadows::UploadDepthAndCompute(
    const DepthBuffer& depth,
    const NormalBuffer* normals,
    const PrismForge::Effects::Lighting::AlphaMask* mask,
    const PrismForge::Effects::Lighting::LightingConfig& config,
    ShadowMapBuffer& outShadows
) {
    outShadows=ShadowMapBuffer{};
    m_impl->m_outputUAV.Reset(); m_impl->m_outputSRV.Reset(); m_impl->m_outputTexture.Reset();
    if (IsDeviceLost() || !depth.IsValid() || depth.format!=0 || depth.data.empty() ||
        (normals && (!normals->IsValid() || normals->width!=depth.width || normals->height!=depth.height)) ||
        (mask && (!mask->data || mask->width!=depth.width || mask->height!=depth.height || mask->strideFloats<depth.width))) {
        return false;
    }

    const int width = depth.width;
    const int height = depth.height;

    // 1. Upload Depth texture
    if (!m_impl->m_depthUploadTex || !m_impl->m_depthUploadSRV || !m_impl->m_normalsUploadTex || !m_impl->m_normalsUploadSRV || !m_impl->m_maskUploadTex || !m_impl->m_maskUploadSRV || !m_impl->m_readbackTex || m_impl->m_uploadW != width || m_impl->m_uploadH != height) {
        m_impl->m_depthUploadSRV.Reset();
        m_impl->m_depthUploadTex.Reset();
        m_impl->m_normalsUploadSRV.Reset();
        m_impl->m_normalsUploadTex.Reset();
        m_impl->m_maskUploadSRV.Reset();
        m_impl->m_maskUploadTex.Reset();
        m_impl->m_readbackTex.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_impl->m_device->CreateTexture2D(&desc, nullptr, m_impl->m_depthUploadTex.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = m_impl->m_device->CreateShaderResourceView(m_impl->m_depthUploadTex.Get(), &srvDesc, m_impl->m_depthUploadSRV.GetAddressOf());
        if (FAILED(hr)) return false;

        // Normals texture (R32G32B32A32_FLOAT)
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        hr = m_impl->m_device->CreateTexture2D(&desc, nullptr, m_impl->m_normalsUploadTex.GetAddressOf());
        if (FAILED(hr)) return false;
        srvDesc.Format = desc.Format;
        hr = m_impl->m_device->CreateShaderResourceView(m_impl->m_normalsUploadTex.Get(), &srvDesc, m_impl->m_normalsUploadSRV.GetAddressOf());
        if (FAILED(hr)) return false;

        // Mask texture (R32_FLOAT)
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        hr = m_impl->m_device->CreateTexture2D(&desc, nullptr, m_impl->m_maskUploadTex.GetAddressOf());
        if (FAILED(hr)) return false;
        srvDesc.Format = desc.Format;
        hr = m_impl->m_device->CreateShaderResourceView(m_impl->m_maskUploadTex.Get(), &srvDesc, m_impl->m_maskUploadSRV.GetAddressOf());
        if (FAILED(hr)) return false;

        // Readback texture (R32G32_FLOAT, STAGING)
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_impl->m_device->CreateTexture2D(&desc, nullptr, m_impl->m_readbackTex.GetAddressOf());
        if (FAILED(hr)) return false;

        m_impl->m_uploadW = width;
        m_impl->m_uploadH = height;
    }

    // Map and copy Depth
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_impl->m_context->Map(m_impl->m_depthUploadTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    float* dstDepth = static_cast<float*>(mapped.pData);
    size_t dstStrideFloats = mapped.RowPitch / sizeof(float);
    for (int y = 0; y < height; ++y) {
        const size_t row=static_cast<size_t>(y)*depth.strideFloats;
        for(int x=0;x<width;++x) {
            const float confidence=depth.confidence.empty()?1.0f:depth.confidence[row+x];
            dstDepth[static_cast<size_t>(y)*dstStrideFloats+x]=(std::isfinite(confidence) && confidence>=.1f && confidence<=1.0f)
                ?depth.data[row+x]:std::numeric_limits<float>::quiet_NaN();
        }
    }
    m_impl->m_context->Unmap(m_impl->m_depthUploadTex.Get(), 0);

    // Map and copy Normals if provided
    ID3D11ShaderResourceView* normalsSRV = nullptr;
    if (normals != nullptr && normals->IsValid()) {
        hr = m_impl->m_context->Map(m_impl->m_normalsUploadTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if(FAILED(hr)) return false;
        {
            float* dstNorm = static_cast<float*>(mapped.pData);
            size_t dstNormStrideFloats = mapped.RowPitch / sizeof(float);
            for (int y = 0; y < height; ++y) {
                const float* srcNorm = normals->data.data() + y * normals->strideFloats;
                for (int x = 0; x < width; ++x) {
                    dstNorm[y * dstNormStrideFloats + 4 * x + 0] = srcNorm[3 * x + 0];
                    dstNorm[y * dstNormStrideFloats + 4 * x + 1] = srcNorm[3 * x + 1];
                    dstNorm[y * dstNormStrideFloats + 4 * x + 2] = srcNorm[3 * x + 2];
                    dstNorm[y * dstNormStrideFloats + 4 * x + 3] = 1.0f;
                }
            }
            m_impl->m_context->Unmap(m_impl->m_normalsUploadTex.Get(), 0);
            normalsSRV = m_impl->m_normalsUploadSRV.Get();
        }
    }

    // Map and copy Mask if provided
    ID3D11ShaderResourceView* maskSRV = nullptr;
    if (mask != nullptr && mask->data != nullptr) {
        hr = m_impl->m_context->Map(m_impl->m_maskUploadTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if(FAILED(hr)) return false;
        {
            float* dstMask = static_cast<float*>(mapped.pData);
            size_t dstMaskStrideFloats = mapped.RowPitch / sizeof(float);
            for (int y = 0; y < height; ++y) {
                std::memcpy(dstMask + y * dstMaskStrideFloats, mask->data + y * mask->strideFloats, width * sizeof(float));
            }
            m_impl->m_context->Unmap(m_impl->m_maskUploadTex.Get(), 0);
            maskSRV = m_impl->m_maskUploadSRV.Get();
        }
    }

    // Execute shadow pass
    GpuShadowOutcome outcome = ExecuteShadowPass(
        m_impl->m_depthUploadSRV.Get(),
        normalsSRV,
        maskSRV,
        width, height, config
    );

    if (outcome != GpuShadowOutcome::Success) {
        return false;
    }

    // Copy to readback texture
    m_impl->m_context->CopyResource(m_impl->m_readbackTex.Get(), m_impl->m_outputTexture.Get());

    ShadowMapBuffer candidate;
    candidate.width=width; candidate.height=height; candidate.strideFloats=width*2;
    try { candidate.data.resize(static_cast<size_t>(width)*height*2); } catch (...) { return false; }
    // Map readback
    hr = m_impl->m_context->Map(m_impl->m_readbackTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const float* srcShadow = static_cast<const float*>(mapped.pData);
    size_t srcStrideFloats = mapped.RowPitch / sizeof(float);
    for (int y = 0; y < height; ++y) {
        std::memcpy(candidate.data.data() + static_cast<size_t>(y)*candidate.strideFloats, srcShadow + y * srcStrideFloats, width * 2 * sizeof(float));
    }

    m_impl->m_context->Unmap(m_impl->m_readbackTex.Get(), 0);
    outShadows=std::move(candidate);
    return true;
}

} // namespace PrismForge::Effects::Relighting::Gpu
