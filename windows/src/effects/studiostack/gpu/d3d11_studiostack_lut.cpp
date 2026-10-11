#include "d3d11_studiostack_lut.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::StudioStack::Gpu {

namespace {

const char* g_lutShader = R"(
Texture2D<float4> InputTex : register(t0);
Texture3D<float4> LutTex   : register(t1);
RWTexture2D<float4> OutputUav : register(u0);

cbuffer LutCB : register(b0)
{
    float4 DomainMin; // (min.r, min.g, min.b, 0.0)
    float4 DomainMax; // (max.r, max.g, max.b, 0.0)
    float4 Params;    // (size, intensity, width, height)
};

[numthreads(16, 16, 1)]
void CS_ApplyLut(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Params.z;
    uint height = (uint)Params.w;
    if (id.x >= width || id.y >= height)
        return;

    float4 src = InputTex[id.xy];
    float intensity = Params.y;
    if (intensity == 0.0f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    float3 domMin = DomainMin.rgb;
    float3 domMax = DomainMax.rgb;
    float3 domSpan = domMax - domMin;
    if (domSpan.r <= 0.0f || domSpan.g <= 0.0f || domSpan.b <= 0.0f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    float3 u = saturate((src.rgb - domMin) / domSpan);
    float size = Params.x;
    float3 coord = u * (size - 1.0f);

    int3 i0 = clamp((int3)floor(coord), 0, (int)size - 1);
    int3 i1 = min(i0 + 1, (int)size - 1);
    float3 f = coord - (float3)i0;

    float3 c000 = LutTex.Load(int4(i0.x, i0.y, i0.z, 0)).rgb;
    float3 c100 = LutTex.Load(int4(i1.x, i0.y, i0.z, 0)).rgb;
    float3 c010 = LutTex.Load(int4(i0.x, i1.y, i0.z, 0)).rgb;
    float3 c110 = LutTex.Load(int4(i1.x, i1.y, i0.z, 0)).rgb;
    float3 c001 = LutTex.Load(int4(i0.x, i0.y, i1.z, 0)).rgb;
    float3 c101 = LutTex.Load(int4(i1.x, i0.y, i1.z, 0)).rgb;
    float3 c011 = LutTex.Load(int4(i0.x, i1.y, i1.z, 0)).rgb;
    float3 c111 = LutTex.Load(int4(i1.x, i1.y, i1.z, 0)).rgb;

    float3 c00 = lerp(c000, c100, f.x);
    float3 c10 = lerp(c010, c110, f.x);
    float3 c01 = lerp(c001, c101, f.x);
    float3 c11 = lerp(c011, c111, f.x);

    float3 c0 = lerp(c00, c10, f.y);
    float3 c1 = lerp(c01, c11, f.y);

    float3 sampled = lerp(c0, c1, f.z);
    float3 lutRgb = saturate(sampled);
    float3 outRgb = lerp(src.rgb, lutRgb, intensity);

    OutputUav[id.xy] = float4(floor(saturate(outRgb)*255.0f+0.5f)/255.0f, src.a);
}
)";

struct LutConstantBuffer {
    float domainMin[4];
    float domainMax[4];
    float params[4]; // size, intensity, width, height
};

} // anonymous namespace

struct D3D11StudioStackLut::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;

    ComPtr<ID3D11ComputeShader> m_csApplyLut;
    ComPtr<ID3D11Buffer> m_constantBuffer;

    // Cached 3D Texture for currently uploaded CubeLut
    std::shared_ptr<const CubeLut> m_cachedOwner;
    std::vector<float> m_cachedData;
    int m_cachedSize=0;
    size_t uploadCount=0;
    DesktopColourFault fault=DesktopColourFault::None;
    ComPtr<ID3D11Texture3D> m_lutTexture;
    ComPtr<ID3D11ShaderResourceView> m_lutSRV;

    bool m_initialized = false;

    Impl(ID3D11Device* dev, ID3D11DeviceContext* ctx)
        : m_device(dev), m_context(ctx)
    {

    }

    bool Initialize() {
        if (!m_device || !m_context) return false;
        m_csApplyLut.Reset(); m_constantBuffer.Reset(); m_initialized=false;
        if(fault==DesktopColourFault::ShaderCompile) return false;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(
            g_lutShader, std::strlen(g_lutShader),
            "LutShader", nullptr, nullptr,
            "CS_ApplyLut", "cs_5_0", flags, 0,
            csBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        if(fault==DesktopColourFault::ShaderCreation) return false;
        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
            nullptr, m_csApplyLut.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(LutConstantBuffer);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if(fault==DesktopColourFault::BufferCreation) return false;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf());
        if (FAILED(hr)) return false;

        m_initialized = true;
        return true;
    }

    bool UploadLutTexture(const CubeLut* lut, std::shared_ptr<const CubeLut> owner) {
        if (!lut) return false;
        if(owner && owner==m_cachedOwner && m_lutTexture && m_lutSRV) return true;

        const int size = lut->GetSize();
        if (size < 2 || size > 65) return false;

        const RgbFloat domMin = lut->GetDomainMin();
        const RgbFloat domMax = lut->GetDomainMax();
        const float spanR = domMax.r - domMin.r;
        const float spanG = domMax.g - domMin.g;
        const float spanB = domMax.b - domMin.b;

        // Sample exact lattice nodes:
        // In CubeLut, index(r, g, b) = r + size * (g + size * b).
        // For D3D11 3D Texture: X is r, Y is g, Z is b.
        // SysMemPitch = size * sizeof(float4)
        // SysMemSlicePitch = size * size * sizeof(float4)
        std::vector<float> texData(static_cast<size_t>(size) * size * size * 4);

        for (int k = 0; k < size; ++k) {
            const float uB = (size > 1) ? (static_cast<float>(k) / static_cast<float>(size - 1)) : 0.0f;
            const float sampleB = domMin.b + uB * spanB;

            for (int j = 0; j < size; ++j) {
                const float uG = (size > 1) ? (static_cast<float>(j) / static_cast<float>(size - 1)) : 0.0f;
                const float sampleG = domMin.g + uG * spanG;

                for (int i = 0; i < size; ++i) {
                    const float uR = (size > 1) ? (static_cast<float>(i) / static_cast<float>(size - 1)) : 0.0f;
                    const float sampleR = domMin.r + uR * spanR;

                    RgbFloat sampled = lut->Sample(sampleR, sampleG, sampleB);

                    const size_t offset = (static_cast<size_t>(k) * size * size + static_cast<size_t>(j) * size + i) * 4;
                    texData[offset + 0] = sampled.r;
                    texData[offset + 1] = sampled.g;
                    texData[offset + 2] = sampled.b;
                    texData[offset + 3] = 1.0f;
                }
            }
        }

        if (m_lutTexture && m_lutSRV && m_cachedSize==size && m_cachedData==texData) {
            m_cachedOwner=std::move(owner);
            return true;
        }
        ComPtr<ID3D11Texture3D> replacementTex;
        ComPtr<ID3D11ShaderResourceView> replacementSrv;
        D3D11_TEXTURE3D_DESC desc{};
        desc.Width = size;
        desc.Height = size;
        desc.Depth = size;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = texData.data();
        subData.SysMemPitch = size * 4 * sizeof(float);
        subData.SysMemSlicePitch = size * size * 4 * sizeof(float);

        if(fault==DesktopColourFault::LutTexture) return false;
        HRESULT hr = m_device->CreateTexture3D(&desc, &subData, replacementTex.GetAddressOf());
        if (FAILED(hr)) return false;

        if(fault==DesktopColourFault::LutView) return false;
        hr = m_device->CreateShaderResourceView(replacementTex.Get(), nullptr, replacementSrv.GetAddressOf());
        if (FAILED(hr)) return false;

        m_lutTexture=std::move(replacementTex); m_lutSRV=std::move(replacementSrv);
        m_cachedOwner=std::move(owner); m_cachedData=std::move(texData); m_cachedSize=size; ++uploadCount;
        return true;
    }

    GpuLutOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        int width, int height,
        const CubeLut* lut,
        float intensity, std::shared_ptr<const CubeLut> owner={})
    {
        if (IsDeviceLost() || fault==DesktopColourFault::DeviceRemoved) return GpuLutOutcome::DeviceLost;
        if (!std::isfinite(intensity) || intensity < 0 || intensity > 1 ||
            !ValidateDesktopColourViews(m_device.Get(),m_context.Get(),inputSRV,outputUAV,width,height))
            return GpuLutOutcome::InvalidInput;
        if (!lut || intensity==0) return GpuLutOutcome::Bypass;

        if (!m_initialized) {
            if (!Initialize()) {
                return GpuLutOutcome::ShaderError;
            }
        }

        bool uploaded=false;
        try { uploaded=UploadLutTexture(lut,std::move(owner)); } catch(...) { return GpuLutOutcome::ShaderError; }
        if (!uploaded) {
            return GpuLutOutcome::ShaderError;
        }

        // Map and update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(fault==DesktopColourFault::Map) return GpuLutOutcome::ShaderError;
        HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return GpuLutOutcome::ShaderError;

        auto* cb = static_cast<LutConstantBuffer*>(mapped.pData);
        RgbFloat dMin = lut->GetDomainMin();
        RgbFloat dMax = lut->GetDomainMax();

        cb->domainMin[0] = dMin.r;
        cb->domainMin[1] = dMin.g;
        cb->domainMin[2] = dMin.b;
        cb->domainMin[3] = 0.0f;

        cb->domainMax[0] = dMax.r;
        cb->domainMax[1] = dMax.g;
        cb->domainMax[2] = dMax.b;
        cb->domainMax[3] = 0.0f;

        cb->params[0] = static_cast<float>(lut->GetSize());
        cb->params[1] = std::clamp(intensity, 0.0f, 1.0f);
        cb->params[2] = static_cast<float>(width);
        cb->params[3] = static_cast<float>(height);

        m_context->Unmap(m_constantBuffer.Get(), 0);

        if(fault==DesktopColourFault::Dispatch) return GpuLutOutcome::ShaderError;
        // Dispatch
        UINT dispatchX = (width + 15) / 16;
        UINT dispatchY = (height + 15) / 16;

        m_context->CSSetShader(m_csApplyLut.Get(), nullptr, 0);

        ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView* srvs[] = { inputSRV, m_lutSRV.Get() };
        m_context->CSSetShaderResources(0, 2, srvs);

        ID3D11UnorderedAccessView* uavs[] = { outputUAV };
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        m_context->Dispatch(dispatchX, dispatchY, 1);

        // Unbind all resources to prevent hazard
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        ID3D11Buffer* nullCBs[1] = { nullptr };

        m_context->CSSetShaderResources(0, 2, nullSRVs);
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, nullCBs);
        m_context->CSSetShader(nullptr, nullptr, 0);

        return IsDeviceLost() ? GpuLutOutcome::DeviceLost : GpuLutOutcome::Success;
    }

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
        m_csApplyLut.Reset();
        m_constantBuffer.Reset();
        m_lutTexture.Reset();
        m_lutSRV.Reset();
        m_cachedOwner.reset(); m_cachedData.clear(); m_cachedSize=0;
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

D3D11StudioStackLut::D3D11StudioStackLut(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context))
{
}

D3D11StudioStackLut::~D3D11StudioStackLut() = default;
D3D11StudioStackLut::D3D11StudioStackLut(D3D11StudioStackLut&&) noexcept = default;
D3D11StudioStackLut& D3D11StudioStackLut::operator=(D3D11StudioStackLut&&) noexcept = default;

GpuLutOutcome D3D11StudioStackLut::Process(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11UnorderedAccessView* outputUAV,
    int width, int height,
    const CubeLut* lut,
    float intensity)
{
    return m_impl ? m_impl->Process(inputSRV, outputUAV, width, height, lut, intensity)
                  : GpuLutOutcome::NotReady;
}

GpuLutOutcome D3D11StudioStackLut::Process(ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output,
    int width, int height, std::shared_ptr<const CubeLut> lut, float intensity) {
    auto ptr=lut.get();
    return m_impl ? m_impl->Process(input,output,width,height,ptr,intensity,std::move(lut)) : GpuLutOutcome::NotReady;
}
void D3D11StudioStackLut::SetFault(DesktopColourFault fault) {
    if(!m_impl) return;
    m_impl->fault=fault;
    if(fault==DesktopColourFault::ShaderCompile || fault==DesktopColourFault::ShaderCreation || fault==DesktopColourFault::BufferCreation) {
        m_impl->m_initialized=false; m_impl->m_csApplyLut.Reset(); m_impl->m_constantBuffer.Reset();
    }
    if(fault==DesktopColourFault::LutTexture || fault==DesktopColourFault::LutView) {
        // A creation seam must exercise creation, including when identical data
        // was previously cached. Cached GPU views cannot bypass this test seam.
        m_impl->m_cachedOwner.reset();m_impl->m_lutTexture.Reset();m_impl->m_lutSRV.Reset();
    }
}
size_t D3D11StudioStackLut::GetUploadCount() const { return m_impl ? m_impl->uploadCount : 0; }
size_t D3D11StudioStackLut::GetCachedTextureCount() const { return m_impl && m_impl->m_lutTexture ? 1 : 0; }

bool D3D11StudioStackLut::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    return m_impl ? m_impl->ResetDevice(newDevice, newContext) : false;
}

bool D3D11StudioStackLut::IsDeviceLost() const {
    return m_impl ? m_impl->IsDeviceLost() : true;
}

} // namespace PrismForge::Effects::StudioStack::Gpu
