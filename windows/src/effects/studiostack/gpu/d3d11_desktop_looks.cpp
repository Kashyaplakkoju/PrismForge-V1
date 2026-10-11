#include "d3d11_desktop_looks.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::StudioStack::Gpu {

namespace {

// ---------------------------------------------------------------------------
// HLSL Compute Shader
//
// Input:  DXGI_FORMAT_R8G8B8A8_UNORM (full-range SDR, [0,1] float in shader)
// Output: same format, same dimensions
//
// Look modes (Params.x as uint):
//   0 = Original (passthrough — should be short-circuited in C++ before dispatch)
//   1 = Warm  (R+14/255, G+4/255, B-12/255, clamped)
//   2 = Cool  (R-12/255, G+2/255, B+14/255, clamped)
//   3 = Mono  (Rec.601 integer luma: (77*rByte+150*gByte+29*bByte+128)>>8)
//
// strength (Params.y): linear lerp [0,1] between original and graded.
//
// Warm/Cool/Mono formulas match FramePipeline GradeTables / ApplyLook exactly
// via uint8 round-trip, so strength==1 gives bit-exact CPU oracle parity.
// ---------------------------------------------------------------------------
const char* g_desktopLooksShader = R"(
Texture2D<float4>   InputTex  : register(t0);
RWTexture2D<float4> OutputUav : register(u0);

cbuffer LookCB : register(b0)
{
    uint  LookMode;    // 0=Original, 1=Warm, 2=Cool, 3=Mono
    float Strength;    // [0,1]
    uint  Width;
    uint  Height;
};

[numthreads(16, 16, 1)]
void CS_ApplyDesktopLook(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float4 src = InputTex[id.xy];

    if (LookMode == 0u || Strength == 0.0f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    // Reconstruct exact uint8 bytes (UNORM input has values byte/255)
    uint rByte = (uint)(src.r * 255.0f + 0.5f);
    uint gByte = (uint)(src.g * 255.0f + 0.5f);
    uint bByte = (uint)(src.b * 255.0f + 0.5f);

    uint grR = rByte, grG = gByte, grB = bByte;

    if (LookMode == 1u) // Warm
    {
        grR = min(255u, rByte + 14u);
        grG = min(255u, gByte +  4u);
        grB = (bByte >= 12u) ? (bByte - 12u) : 0u;
    }
    else if (LookMode == 2u) // Cool
    {
        grR = (rByte >= 12u) ? (rByte - 12u) : 0u;
        grG = min(255u, gByte +  2u);
        grB = min(255u, bByte + 14u);
    }
    else if (LookMode == 3u) // Mono — Rec.601 integer luma
    {
        // (77*R + 150*G + 29*B + 128) >> 8  — integer arithmetic only
        uint luma = (77u * rByte + 150u * gByte + 29u * bByte + 128u) >> 8u;
        luma = min(luma, 255u);
        grR = grG = grB = luma;
    }

    float strength = clamp(Strength, 0.0f, 1.0f);

    // Lerp in [0,255] integer space, then normalise
    float outR = floor((1.0f-strength)*rByte + strength*grR + 0.5f) / 255.0f;
    float outG = floor((1.0f-strength)*gByte + strength*grG + 0.5f) / 255.0f;
    float outB = floor((1.0f-strength)*bByte + strength*grB + 0.5f) / 255.0f;

    OutputUav[id.xy] = float4(saturate(outR), saturate(outG), saturate(outB), src.a);
}
)";

struct LookConstantBuffer {
    uint32_t lookMode;  // 0-3
    float    strength;  // [0,1]
    uint32_t width;
    uint32_t height;
};

} // anonymous namespace

bool ValidateDesktopColourViews(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output, int width, int height) noexcept
{
    if (!device || !context || !input || !output || width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return false;
    ComPtr<ID3D11Device> ctxDevice, inDevice, outDevice;
    context->GetDevice(&ctxDevice); input->GetDevice(&inDevice); output->GetDevice(&outDevice);
    if (ctxDevice.Get()!=device || inDevice.Get()!=device || outDevice.Get()!=device ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; input->GetDesc(&sd);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; output->GetDesc(&ud);
    if (sd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM || ud.Format!=sd.Format ||
        sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || ud.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2D ||
        sd.Texture2D.MostDetailedMip!=0 || sd.Texture2D.MipLevels!=1 || ud.Texture2D.MipSlice!=0) return false;
    ComPtr<ID3D11Resource> ir,orr; input->GetResource(&ir); output->GetResource(&orr);
    if (ir.Get()==orr.Get()) return false;
    ComPtr<ID3D11Texture2D> it,ot;
    if (FAILED(ir.As(&it)) || FAILED(orr.As(&ot))) return false;
    D3D11_TEXTURE2D_DESC id{},od{}; it->GetDesc(&id);ot->GetDesc(&od);
    return id.Width==static_cast<UINT>(width) && id.Height==static_cast<UINT>(height) &&
        od.Width==id.Width && od.Height==id.Height && id.SampleDesc.Count==1 && od.SampleDesc.Count==1 &&
        id.ArraySize==1 && od.ArraySize==1 && id.MipLevels==1 && od.MipLevels==1;
}

// ---------------------------------------------------------------------------
// D3D11DesktopLooks::Impl
// ---------------------------------------------------------------------------

struct D3D11DesktopLooks::Impl {
    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;

    ComPtr<ID3D11ComputeShader> m_cs;
    ComPtr<ID3D11Buffer>        m_constantBuffer;

    bool m_initialized = false;
    DesktopColourFault fault = DesktopColourFault::None;

    Impl(ID3D11Device* dev, ID3D11DeviceContext* ctx)
        : m_device(dev), m_context(ctx)
    {

    }

    bool Initialize() {
        if (!m_device || !m_context) return false;
        m_cs.Reset(); m_constantBuffer.Reset(); m_initialized=false;
        if (fault==DesktopColourFault::ShaderCompile) return false;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(
            g_desktopLooksShader, std::strlen(g_desktopLooksShader),
            "DesktopLooksShader", nullptr, nullptr,
            "CS_ApplyDesktopLook", "cs_5_0", flags, 0,
            csBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr)) return false;

        if (fault==DesktopColourFault::ShaderCreation) return false;
        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
            nullptr, m_cs.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth      = sizeof(LookConstantBuffer);
        cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (fault==DesktopColourFault::BufferCreation) return false;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf());
        if (FAILED(hr)) return false;

        m_initialized = true;
        return true;
    }

    GpuDesktopLooksOutcome Process(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11UnorderedAccessView* outputUAV,
        int width, int height,
        DesktopLook look,
        float strength)
    {
        if (IsDeviceLost() || fault==DesktopColourFault::DeviceRemoved) return GpuDesktopLooksOutcome::DeviceLost;
        if (!std::isfinite(strength) || strength < 0 || strength > 1 || static_cast<unsigned>(look)>3u ||
            !ValidateDesktopColourViews(m_device.Get(),m_context.Get(),inputSRV,outputUAV,width,height))
            return GpuDesktopLooksOutcome::InvalidInput;
        if (look==DesktopLook::Original || strength==0) return GpuDesktopLooksOutcome::Bypass;

        if (!m_initialized) {
            if (!Initialize()) return GpuDesktopLooksOutcome::ShaderError;
        }

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (fault==DesktopColourFault::Map) return GpuDesktopLooksOutcome::ShaderError;
        HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return GpuDesktopLooksOutcome::ShaderError;

        auto* cb = static_cast<LookConstantBuffer*>(mapped.pData);
        cb->lookMode = static_cast<uint32_t>(look);
        cb->strength = std::clamp(strength, 0.0f, 1.0f);
        cb->width    = static_cast<uint32_t>(width);
        cb->height   = static_cast<uint32_t>(height);
        m_context->Unmap(m_constantBuffer.Get(), 0);

        if (fault==DesktopColourFault::Dispatch) return GpuDesktopLooksOutcome::ShaderError;
        // Dispatch
        UINT dispatchX = (static_cast<UINT>(width)  + 15u) / 16u;
        UINT dispatchY = (static_cast<UINT>(height) + 15u) / 16u;

        m_context->CSSetShader(m_cs.Get(), nullptr, 0);

        ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView* srvs[] = { inputSRV };
        m_context->CSSetShaderResources(0, 1, srvs);

        ID3D11UnorderedAccessView* uavs[] = { outputUAV };
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        m_context->Dispatch(dispatchX, dispatchY, 1);

        // Unbind all to prevent hazard
        ID3D11ShaderResourceView*  nullSRVs[1] = { nullptr };
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        ID3D11Buffer*              nullCBs[1]  = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSRVs);
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, nullCBs);
        m_context->CSSetShader(nullptr, nullptr, 0);

        return IsDeviceLost() ? GpuDesktopLooksOutcome::DeviceLost : GpuDesktopLooksOutcome::Success;
    }

    bool ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
        m_cs.Reset();
        m_constantBuffer.Reset();
        m_initialized = false;

        m_device  = newDevice;
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

// ---------------------------------------------------------------------------
// Public API forwarding
// ---------------------------------------------------------------------------

D3D11DesktopLooks::D3D11DesktopLooks(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context))
{
}

D3D11DesktopLooks::~D3D11DesktopLooks() = default;
D3D11DesktopLooks::D3D11DesktopLooks(D3D11DesktopLooks&&) noexcept = default;
D3D11DesktopLooks& D3D11DesktopLooks::operator=(D3D11DesktopLooks&&) noexcept = default;

GpuDesktopLooksOutcome D3D11DesktopLooks::Process(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11UnorderedAccessView* outputUAV,
    int width, int height,
    DesktopLook look,
    float strength)
{
    return m_impl
        ? m_impl->Process(inputSRV, outputUAV, width, height, look, strength)
        : GpuDesktopLooksOutcome::NotReady;
}

void D3D11DesktopLooks::SetFault(DesktopColourFault fault) {
    if (!m_impl) return;
    m_impl->fault=fault;
    if (fault==DesktopColourFault::ShaderCompile || fault==DesktopColourFault::ShaderCreation ||
        fault==DesktopColourFault::BufferCreation) {
        m_impl->m_cs.Reset(); m_impl->m_constantBuffer.Reset(); m_impl->m_initialized=false;
    }
}

bool D3D11DesktopLooks::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    return m_impl ? m_impl->ResetDevice(newDevice, newContext) : false;
}

bool D3D11DesktopLooks::IsDeviceLost() const {
    return m_impl ? m_impl->IsDeviceLost() : true;
}

} // namespace PrismForge::Effects::StudioStack::Gpu
