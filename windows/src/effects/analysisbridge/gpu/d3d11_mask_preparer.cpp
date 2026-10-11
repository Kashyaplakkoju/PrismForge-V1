#include "d3d11_mask_preparer.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::AnalysisBridge::Gpu {

namespace {

constexpr int kMaxDimension = 8192;
constexpr uint64_t kMaxPixels = 64ULL * 1024ULL * 1024ULL;

const char* g_maskPreparerShaders = R"(
// -------------------------------------------------------------
// Constant Buffers
// -------------------------------------------------------------
cbuffer ResampleCB : register(b0)
{
    float4 SrcDims;         // (srcW, srcH, 1.0f / srcW, 1.0f / srcH)
    float4 DstDims;         // (dstW, dstH, 1.0f / dstW, 1.0f / dstH)
    float4 ModeParams;      // (mode: 0=Stretch, 1=Letterbox, 2=CoverCrop, pad)
    float4 LetterboxParams; // (boxW, boxH, offsetX, offsetY)
    float4 CoverCropParams; // (cropW, cropH, startCropX, startCropY)
};

cbuffer FeatherCB : register(b1)
{
    float4 FeatherParams;   // (radius, width, height, 1.0f / (2 * radius + 1))
};

cbuffer StabilizeCB : register(b2)
{
    float4 StabParams;      // (alpha, changeThreshold, width, height)
};

// -------------------------------------------------------------
// 1. Bilinear Resample Compute Shader
// -------------------------------------------------------------
Texture2D<float> NativeMask : register(t0);
RWTexture2D<float> ResampleOutput : register(u0);

[numthreads(16, 16, 1)]
void CS_Resample(uint3 id : SV_DispatchThreadID)
{
    uint dstW = (uint)DstDims.x;
    uint dstH = (uint)DstDims.y;
    if (id.x >= dstW || id.y >= dstH)
        return;

    float srcW = SrcDims.x;
    float srcH = SrcDims.y;
    int mode = (int)ModeParams.x;

    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    float w0x = 1.0f, w1x = 0.0f, w0y = 1.0f, w1y = 0.0f;

    if (mode == 0) // Stretch
    {
        float u = (dstW <= 1) ? 0.0f : ((float)id.x / (float)(dstW - 1));
        u = clamp(u, 0.0f, 1.0f);
        float fx = u * (srcW - 1.0f);
        x0 = (int)fx;
        x1 = min(x0 + 1, (int)srcW - 1);
        float dx = fx - (float)x0;
        w0x = 1.0f - dx;
        w1x = dx;

        float v = (dstH <= 1) ? 0.0f : ((float)id.y / (float)(dstH - 1));
        v = clamp(v, 0.0f, 1.0f);
        float fy = v * (srcH - 1.0f);
        y0 = (int)fy;
        y1 = min(y0 + 1, (int)srcH - 1);
        float dy = fy - (float)y0;
        w0y = 1.0f - dy;
        w1y = dy;
    }
    else if (mode == 1) // Letterbox
    {
        float boxW = LetterboxParams.x;
        float boxH = LetterboxParams.y;
        float offsetX = LetterboxParams.z;
        float offsetY = LetterboxParams.w;

        int startX = max(0, (int)offsetX);
        int endX = min((int)dstW, (int)(offsetX + boxW));
        int startY = max(0, (int)offsetY);
        int endY = min((int)dstH, (int)(offsetY + boxH));

        if ((int)id.x < startX || (int)id.x >= endX || (int)id.y < startY || (int)id.y >= endY)
        {
            ResampleOutput[id.xy] = 0.0f;
            return;
        }

        float denomX = max(1.0f, boxW - 1.0f);
        float u = ((float)id.x - offsetX) / denomX;
        u = clamp(u, 0.0f, 1.0f);
        float fx = u * (srcW - 1.0f);
        x0 = (int)fx;
        x1 = min(x0 + 1, (int)srcW - 1);
        float dx = fx - (float)x0;
        w0x = 1.0f - dx;
        w1x = dx;

        float denomY = max(1.0f, boxH - 1.0f);
        float v = ((float)id.y - offsetY) / denomY;
        v = clamp(v, 0.0f, 1.0f);
        float fy = v * (srcH - 1.0f);
        y0 = (int)fy;
        y1 = min(y0 + 1, (int)srcH - 1);
        float dy = fy - (float)y0;
        w0y = 1.0f - dy;
        w1y = dy;
    }
    else // CoverCrop
    {
        float cropW = CoverCropParams.x;
        float cropH = CoverCropParams.y;
        float startCropX = CoverCropParams.z;
        float startCropY = CoverCropParams.w;

        float u = (startCropX + ((float)id.x / (float)dstW) * cropW) / srcW;
        u = clamp(u, 0.0f, 1.0f);
        float fx = u * (srcW - 1.0f);
        x0 = (int)fx;
        x1 = min(x0 + 1, (int)srcW - 1);
        float dx = fx - (float)x0;
        w0x = 1.0f - dx;
        w1x = dx;

        float v = (startCropY + ((float)id.y / (float)dstH) * cropH) / srcH;
        v = clamp(v, 0.0f, 1.0f);
        float fy = v * (srcH - 1.0f);
        y0 = (int)fy;
        y1 = min(y0 + 1, (int)srcH - 1);
        float dy = fy - (float)y0;
        w0y = 1.0f - dy;
        w1y = dy;
    }

    float s00 = NativeMask[uint2((uint)x0, (uint)y0)];
    float s01 = NativeMask[uint2((uint)x1, (uint)y0)];
    float s10 = NativeMask[uint2((uint)x0, (uint)y1)];
    float s11 = NativeMask[uint2((uint)x1, (uint)y1)];

    float interp = (s00 * w0x + s01 * w1x) * w0y + (s10 * w0x + s11 * w1x) * w1y;
    ResampleOutput[id.xy] = clamp(interp, 0.0f, 1.0f);
}

// -------------------------------------------------------------
// 2. Horizontal Box Feathering Compute Shader
// -------------------------------------------------------------
Texture2D<float> HorizInput : register(t0);
RWTexture2D<float> HorizOutput : register(u0);

[numthreads(16, 16, 1)]
void CS_HorizFeather(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)FeatherParams.y;
    uint height = (uint)FeatherParams.z;
    if (id.x >= width || id.y >= height)
        return;

    int r = (int)FeatherParams.x;
    if (r <= 0)
    {
        HorizOutput[id.xy] = HorizInput[id.xy];
        return;
    }

    float sum = 0.0f;
    for (int dx = -r; dx <= r; ++dx)
    {
        int sx = clamp((int)id.x + dx, 0, (int)width - 1);
        sum += HorizInput[uint2((uint)sx, id.y)];
    }
    HorizOutput[id.xy] = clamp(sum * FeatherParams.w, 0.0f, 1.0f);
}

// -------------------------------------------------------------
// 3. Vertical Box Feathering Compute Shader
// -------------------------------------------------------------
Texture2D<float> VertInput : register(t0);
RWTexture2D<float> VertOutput : register(u0);

[numthreads(16, 16, 1)]
void CS_VertFeather(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)FeatherParams.y;
    uint height = (uint)FeatherParams.z;
    if (id.x >= width || id.y >= height)
        return;

    int r = (int)FeatherParams.x;
    if (r <= 0)
    {
        VertOutput[id.xy] = VertInput[id.xy];
        return;
    }

    float sum = 0.0f;
    for (int dy = -r; dy <= r; ++dy)
    {
        int sy = clamp((int)id.y + dy, 0, (int)height - 1);
        sum += VertInput[uint2(id.x, (uint)sy)];
    }
    VertOutput[id.xy] = clamp(sum * FeatherParams.w, 0.0f, 1.0f);
}

// -------------------------------------------------------------
// 4. Temporal Stabilization Compute Shader
// -------------------------------------------------------------
Texture2D<float> CurMask : register(t0);
Texture2D<float> PrevMask : register(t1);
RWTexture2D<float> StabOutput : register(u0);

[numthreads(16, 16, 1)]
void CS_TemporalStabilize(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)StabParams.z;
    uint height = (uint)StabParams.w;
    if (id.x >= width || id.y >= height)
        return;

    float rawVal = clamp(CurMask[id.xy], 0.0f, 1.0f);
    float prevVal = PrevMask[id.xy];
    float delta = abs(rawVal - prevVal);

    float alpha = StabParams.x;
    float threshold = StabParams.y;
    float effectiveAlpha = alpha;
    if (threshold > 0.0f && delta > threshold)
    {
        float excess = (delta - threshold) / (1.0f - threshold + 1e-6f);
        effectiveAlpha = alpha + (1.0f - alpha) * clamp(excess, 0.0f, 1.0f);
    }

    float smoothed = prevVal * (1.0f - effectiveAlpha) + rawVal * effectiveAlpha;
    StabOutput[id.xy] = clamp(smoothed, 0.0f, 1.0f);
}
)";

} // namespace

struct D3D11MaskPreparer::Impl {
    ComPtr<ID3D11Device> m_resourceDevice;
    int m_historyNativeW=0, m_historyNativeH=0, m_historyMode=-1, m_historyFeather=-1;
    // Compiled Compute Shaders
    ComPtr<ID3D11ComputeShader> m_csResample;
    ComPtr<ID3D11ComputeShader> m_csHorizFeather;
    ComPtr<ID3D11ComputeShader> m_csVertFeather;
    ComPtr<ID3D11ComputeShader> m_csTemporalStabilize;

    // Constant Buffers
    ComPtr<ID3D11Buffer> m_cbResample;
    ComPtr<ID3D11Buffer> m_cbFeather;
    ComPtr<ID3D11Buffer> m_cbStabilize;

    // Native resolution upload textures
    ComPtr<ID3D11Texture2D> m_nativeTex;
    ComPtr<ID3D11ShaderResourceView> m_nativeSRV;
    int m_nativeW = 0;
    int m_nativeH = 0;

    // Target resolution intermediate & output textures
    int m_targetW = 0;
    int m_targetH = 0;

    ComPtr<ID3D11Texture2D> m_resampleTex;
    ComPtr<ID3D11ShaderResourceView> m_resampleSRV;
    ComPtr<ID3D11UnorderedAccessView> m_resampleUAV;

    ComPtr<ID3D11Texture2D> m_featherHorizTex;
    ComPtr<ID3D11ShaderResourceView> m_featherHorizSRV;
    ComPtr<ID3D11UnorderedAccessView> m_featherHorizUAV;

    ComPtr<ID3D11Texture2D> m_featherVertTex;
    ComPtr<ID3D11ShaderResourceView> m_featherVertSRV;
    ComPtr<ID3D11UnorderedAccessView> m_featherVertUAV;

    // Ping-pong temporal history textures
    ComPtr<ID3D11Texture2D> m_historyTex[2];
    ComPtr<ID3D11ShaderResourceView> m_historySRV[2];
    ComPtr<ID3D11UnorderedAccessView> m_historyUAV[2];
    int m_historyIdx = 0;
    bool m_hasHistory = false;

    // Final active output
    ComPtr<ID3D11Texture2D> m_activeOutputTex;
    ComPtr<ID3D11ShaderResourceView> m_activeOutputSRV;

    bool EnsureShaders(ID3D11Device* device, std::string* outError) {
        if (m_csResample && m_csHorizFeather && m_csVertFeather && m_csTemporalStabilize && m_cbResample && m_cbFeather && m_cbStabilize) {
            return true;
        }

        auto compileCS = [&](const char* entry, ComPtr<ID3D11ComputeShader>& outCS) -> bool {
            ComPtr<ID3DBlob> codeBlob;
            ComPtr<ID3DBlob> errorBlob;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            HRESULT hr = D3DCompile(
                g_maskPreparerShaders, std::strlen(g_maskPreparerShaders),
                nullptr, nullptr, nullptr,
                entry, "cs_5_0",
                flags, 0,
                &codeBlob, &errorBlob);

            if (FAILED(hr)) {
                if (outError) {
                    *outError = errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                          : ("D3DCompile failed for " + std::string(entry));
                }
                return false;
            }

            hr = device->CreateComputeShader(
                codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(),
                nullptr, &outCS);
            return SUCCEEDED(hr);
        };

        if (!compileCS("CS_Resample", m_csResample)) return false;
        if (!compileCS("CS_HorizFeather", m_csHorizFeather)) return false;
        if (!compileCS("CS_VertFeather", m_csVertFeather)) return false;
        if (!compileCS("CS_TemporalStabilize", m_csTemporalStabilize)) return false;

        // Constant buffers (16-byte aligned)
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        cbDesc.ByteWidth = sizeof(float) * 20; // 80 bytes -> align up to multiple of 16 (96 bytes)
        cbDesc.ByteWidth = (cbDesc.ByteWidth + 15) & ~15;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_cbResample))) return false;

        cbDesc.ByteWidth = sizeof(float) * 4; // 16 bytes
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_cbFeather))) return false;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_cbStabilize))) return false;

        return true;
    }

    bool EnsureNativeTexture(ID3D11Device* device, int w, int h) {
        if (m_nativeTex && m_nativeSRV && m_nativeW == w && m_nativeH == h) {
            return true;
        }

        m_nativeTex.Reset();
        m_nativeSRV.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_nativeTex))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(device->CreateShaderResourceView(m_nativeTex.Get(), &srvDesc, &m_nativeSRV))) {
            m_nativeTex.Reset();
            return false;
        }

        m_nativeW = w;
        m_nativeH = h;
        return true;
    }

    bool EnsureTargetTextures(ID3D11Device* device, int w, int h, bool needFeather, bool needTemporal, GpuMaskFaultSeam faultSeam) {
        if (faultSeam != GpuMaskFaultSeam::PartialTargetViewCreation &&
            m_resampleTex && m_resampleSRV && m_resampleUAV && m_targetW == w && m_targetH == h &&
            (!needFeather || (m_featherHorizTex && m_featherHorizSRV && m_featherHorizUAV && m_featherVertTex && m_featherVertSRV && m_featherVertUAV)) &&
            (!needTemporal || (m_historyTex[0] && m_historySRV[0] && m_historyUAV[0] && m_historyTex[1] && m_historySRV[1] && m_historyUAV[1])))
        {
            return true;
        }

        if (m_targetW != w || m_targetH != h) {
            m_resampleTex.Reset();
            m_resampleSRV.Reset();
            m_resampleUAV.Reset();

            m_featherHorizTex.Reset();
            m_featherHorizSRV.Reset();
            m_featherHorizUAV.Reset();

            m_featherVertTex.Reset();
            m_featherVertSRV.Reset();
            m_featherVertUAV.Reset();

            m_historyTex[0].Reset(); m_historySRV[0].Reset(); m_historyUAV[0].Reset();
            m_historyTex[1].Reset(); m_historySRV[1].Reset(); m_historyUAV[1].Reset();
            m_hasHistory = false;
            m_historyIdx = 0;
            m_targetW = w;
            m_targetH = h;
        }

        auto createTargetTex = [&](ComPtr<ID3D11Texture2D>& tex,
                                   ComPtr<ID3D11ShaderResourceView>& srv,
                                   ComPtr<ID3D11UnorderedAccessView>& uav) -> bool {
            if (tex && srv && uav && faultSeam != GpuMaskFaultSeam::PartialTargetViewCreation) return true;
            ComPtr<ID3D11Texture2D> candidateTex;
            ComPtr<ID3D11ShaderResourceView> candidateSrv;
            ComPtr<ID3D11UnorderedAccessView> candidateUav;

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(w);
            desc.Height = static_cast<UINT>(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

            if (FAILED(device->CreateTexture2D(&desc, nullptr, &candidateTex))) return false;

            if (faultSeam == GpuMaskFaultSeam::PartialTargetViewCreation) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(candidateTex.Get(), &srvDesc, &candidateSrv))) return false;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = desc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateUnorderedAccessView(candidateTex.Get(), &uavDesc, &candidateUav))) return false;
            tex=std::move(candidateTex); srv=std::move(candidateSrv); uav=std::move(candidateUav);
            return true;
        };

        if (!createTargetTex(m_resampleTex, m_resampleSRV, m_resampleUAV)) return false;

        if (needFeather) {
            if (!createTargetTex(m_featherHorizTex, m_featherHorizSRV, m_featherHorizUAV)) return false;
            if (!createTargetTex(m_featherVertTex, m_featherVertSRV, m_featherVertUAV)) return false;
        }

        if (needTemporal) {
            if (!createTargetTex(m_historyTex[0], m_historySRV[0], m_historyUAV[0])) return false;
            if (!createTargetTex(m_historyTex[1], m_historySRV[1], m_historyUAV[1])) return false;
        }

        return true;
    }
};

D3D11MaskPreparer::D3D11MaskPreparer() : m_impl(std::make_unique<Impl>()) {}
D3D11MaskPreparer::~D3D11MaskPreparer() = default;

D3D11MaskPreparer::D3D11MaskPreparer(D3D11MaskPreparer&&) noexcept = default;
D3D11MaskPreparer& D3D11MaskPreparer::operator=(D3D11MaskPreparer&&) noexcept = default;

void D3D11MaskPreparer::Reset() noexcept {
    if (m_impl) {
        m_impl->m_hasHistory = false;
        m_impl->m_historyIdx = 0;
        m_impl->m_activeOutputTex.Reset();
        m_impl->m_activeOutputSRV.Reset();
    }
}

void D3D11MaskPreparer::ReleaseResources() noexcept {
    if (m_impl) {
        m_impl->m_csResample.Reset(); m_impl->m_csHorizFeather.Reset();
        m_impl->m_csVertFeather.Reset(); m_impl->m_csTemporalStabilize.Reset();
        m_impl->m_cbResample.Reset(); m_impl->m_cbFeather.Reset(); m_impl->m_cbStabilize.Reset();
        m_impl->m_resourceDevice.Reset();
        m_impl->m_nativeTex.Reset();
        m_impl->m_nativeSRV.Reset();
        m_impl->m_nativeW = 0;
        m_impl->m_nativeH = 0;

        m_impl->m_resampleTex.Reset();
        m_impl->m_resampleSRV.Reset();
        m_impl->m_resampleUAV.Reset();

        m_impl->m_featherHorizTex.Reset();
        m_impl->m_featherHorizSRV.Reset();
        m_impl->m_featherHorizUAV.Reset();

        m_impl->m_featherVertTex.Reset();
        m_impl->m_featherVertSRV.Reset();
        m_impl->m_featherVertUAV.Reset();

        m_impl->m_historyTex[0].Reset(); m_impl->m_historySRV[0].Reset(); m_impl->m_historyUAV[0].Reset();
        m_impl->m_historyTex[1].Reset(); m_impl->m_historySRV[1].Reset(); m_impl->m_historyUAV[1].Reset();
        m_impl->m_hasHistory = false;
        m_impl->m_historyIdx = 0;
        m_impl->m_targetW = 0;
        m_impl->m_targetH = 0;

        m_impl->m_activeOutputTex.Reset();
        m_impl->m_activeOutputSRV.Reset();
    }
}

ID3D11ShaderResourceView* D3D11MaskPreparer::GetOutputSRV() const noexcept {
    return m_impl ? m_impl->m_activeOutputSRV.Get() : nullptr;
}

ID3D11Texture2D* D3D11MaskPreparer::GetOutputTexture() const noexcept {
    return m_impl ? m_impl->m_activeOutputTex.Get() : nullptr;
}

int D3D11MaskPreparer::GetWidth() const noexcept {
    return m_impl ? m_impl->m_targetW : 0;
}

int D3D11MaskPreparer::GetHeight() const noexcept {
    return m_impl ? m_impl->m_targetH : 0;
}

bool D3D11MaskPreparer::HasOutput() const noexcept {
    return m_impl && m_impl->m_activeOutputSRV != nullptr;
}

GpuMaskOutcome D3D11MaskPreparer::PrepareMask(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const MaskBuffer& srcMask,
    int targetWidth,
    int targetHeight,
    const MaskPreparationConfig& config,
    GpuMaskFaultSeam faultSeam,
    HRESULT customFaultHr,
    std::string* outErrorMessage)
{
    if (m_impl) {
        m_impl->m_activeOutputTex.Reset();
        m_impl->m_activeOutputSRV.Reset();
    }

    if (faultSeam == GpuMaskFaultSeam::DeviceRemoved) {
        if (outErrorMessage) *outErrorMessage = "Simulated device removal fault";
        return GpuMaskOutcome::DeviceLost;
    }

    if (!device || !context) {
        if (outErrorMessage) *outErrorMessage = "Device or context is null";
        return GpuMaskOutcome::InvalidArgs;
    }

    if (!m_impl) m_impl=std::make_unique<Impl>();
    ComPtr<ID3D11Device> contextDevice; context->GetDevice(&contextDevice);
    if(contextDevice.Get()!=device || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return GpuMaskOutcome::InvalidArgs;
    if(m_impl->m_resourceDevice.Get()!=device) { ReleaseResources(); m_impl->m_resourceDevice=device; }
    if(FAILED(device->GetDeviceRemovedReason())) return GpuMaskOutcome::DeviceLost;
    // All exit paths after dispatch clear compute bindings, including failed later stages.
    struct BindingCleanup {
        ID3D11DeviceContext* context;
        ~BindingCleanup() {
            ID3D11ShaderResourceView* srvs[2]={}; ID3D11UnorderedAccessView* uavs[1]={}; ID3D11Buffer* cbs[3]={};
            context->CSSetShaderResources(0,2,srvs); context->CSSetUnorderedAccessViews(0,1,uavs,nullptr);
            context->CSSetConstantBuffers(0,3,cbs); context->CSSetShader(nullptr,nullptr,0);
        }
    } cleanup{context};
    // 1. Validate inputs
    if (!srcMask.IsValid() || targetWidth <= 0 || targetHeight <= 0 ||
        targetWidth > kMaxDimension || targetHeight > kMaxDimension)
    {
        if (outErrorMessage) *outErrorMessage = "Invalid source mask or target dimensions";
        return GpuMaskOutcome::InvalidArgs;
    }

    const uint64_t targetPixels64 = static_cast<uint64_t>(targetWidth) * targetHeight;
    if (targetPixels64 > kMaxPixels) {
        if (outErrorMessage) *outErrorMessage = "Target dimensions exceed maximum pixel bound";
        return GpuMaskOutcome::InvalidArgs;
    }

    int modeVal = static_cast<int>(config.resizeMode);
    if (modeVal < 0 || modeVal > 2) {
        if (outErrorMessage) *outErrorMessage = "Invalid resize mode";
        return GpuMaskOutcome::InvalidArgs;
    }

    if (!std::isfinite(config.featherRadius) || config.featherRadius < 0.0f) {
        if (outErrorMessage) *outErrorMessage = "Nonfinite or negative feather radius";
        return GpuMaskOutcome::InvalidArgs;
    }

    if (faultSeam == GpuMaskFaultSeam::ShaderCompile) {
        if (outErrorMessage) *outErrorMessage = "Simulated shader compilation failure";
        return GpuMaskOutcome::ShaderError;
    }

    // 2. Ensure Shaders & Constant Buffers
    std::string shaderErr;
    if (!m_impl->EnsureShaders(device, &shaderErr)) {
        if (outErrorMessage) *outErrorMessage = shaderErr.empty() ? "Shader compilation failed" : shaderErr;
        return GpuMaskOutcome::ShaderError;
    }

    float clampedRadius = std::min(5.0f, config.featherRadius);
    int featherR = (clampedRadius >= 1.0f) ? std::min(5, static_cast<int>(std::round(clampedRadius))) : 0;
    bool needFeather = (featherR > 0);
    bool needTemporal = config.enableTemporalStabilization;
    if(!needTemporal || m_impl->m_historyNativeW!=srcMask.width || m_impl->m_historyNativeH!=srcMask.height ||
        m_impl->m_historyMode!=modeVal || m_impl->m_historyFeather!=featherR) m_impl->m_hasHistory=false;
    m_impl->m_historyNativeW=srcMask.width; m_impl->m_historyNativeH=srcMask.height;
    m_impl->m_historyMode=modeVal; m_impl->m_historyFeather=featherR;

    if (faultSeam == GpuMaskFaultSeam::TargetAlloc) {
        if (outErrorMessage) *outErrorMessage = "Simulated target texture allocation failure";
        return GpuMaskOutcome::AllocationFailed;
    }

    // 3. Ensure Textures
    if (!m_impl->EnsureNativeTexture(device, srcMask.width, srcMask.height) ||
        !m_impl->EnsureTargetTextures(device, targetWidth, targetHeight, needFeather, needTemporal, faultSeam))
    {
        if (outErrorMessage) *outErrorMessage = "Failed to allocate GPU mask textures";
        return GpuMaskOutcome::AllocationFailed;
    }

    if (faultSeam == GpuMaskFaultSeam::NativeUpload_Map) {
        if (outErrorMessage) *outErrorMessage = "Simulated native mask Map failure";
        return GpuMaskOutcome::InjectedFault;
    }

    // 4. Upload native small mask to dynamic native texture (row by row to handle stride)
    D3D11_MAPPED_SUBRESOURCE nativeMap{};
    HRESULT hr = context->Map(m_impl->m_nativeTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &nativeMap);
    if (FAILED(hr)) {
        if (outErrorMessage) *outErrorMessage = "Failed to Map native mask texture";
        return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            ? GpuMaskOutcome::DeviceLost : GpuMaskOutcome::AllocationFailed;
    }

    const int srcW = srcMask.width;
    const int srcH = srcMask.height;
    const int srcStride = srcMask.strideFloats;
    const float* srcData = srcMask.data.data();

    for (int y = 0; y < srcH; ++y) {
        const float* srcRow = srcData + static_cast<size_t>(y) * srcStride;
        float* dstRow = reinterpret_cast<float*>(static_cast<uint8_t*>(nativeMap.pData) + y * nativeMap.RowPitch);
        for (int x = 0; x < srcW; ++x) {
            float v = srcRow[x];
            dstRow[x] = (!std::isfinite(v) || v <= 0.0f) ? 0.0f : ((v >= 1.0f) ? 1.0f : v);
        }
    }
    context->Unmap(m_impl->m_nativeTex.Get(), 0);

    // 5. Pass 1: Bilinear Resample
    if (faultSeam == GpuMaskFaultSeam::ResampleDispatch) {
        if (outErrorMessage) *outErrorMessage = "Simulated resample compute dispatch failure";
        return GpuMaskOutcome::DispatchFailed;
    }

    {
        D3D11_MAPPED_SUBRESOURCE cbMap{};
        if(faultSeam == GpuMaskFaultSeam::ResampleConstants_Map || FAILED(context->Map(m_impl->m_cbResample.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&cbMap))) {
            if(outErrorMessage) *outErrorMessage="Mask constant buffer Map failed";
            m_impl->m_hasHistory=false;
            return GpuMaskOutcome::DispatchFailed;
        }
        {
            float* cb = static_cast<float*>(cbMap.pData);
            // SrcDims
            cb[0] = static_cast<float>(srcW);
            cb[1] = static_cast<float>(srcH);
            cb[2] = 1.0f / static_cast<float>(srcW);
            cb[3] = 1.0f / static_cast<float>(srcH);

            // DstDims
            cb[4] = static_cast<float>(targetWidth);
            cb[5] = static_cast<float>(targetHeight);
            cb[6] = 1.0f / static_cast<float>(targetWidth);
            cb[7] = 1.0f / static_cast<float>(targetHeight);

            // ModeParams
            cb[8] = static_cast<float>(modeVal);
            cb[9] = 0.0f; cb[10] = 0.0f; cb[11] = 0.0f;

            // LetterboxParams
            float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
            float dstAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);
            float boxW, boxH, offX, offY;
            if (srcAspect > dstAspect) {
                boxW = static_cast<float>(targetWidth);
                boxH = boxW / srcAspect;
                offX = 0.0f;
                offY = (targetHeight - boxH) * 0.5f;
            } else {
                boxH = static_cast<float>(targetHeight);
                boxW = boxH * srcAspect;
                offX = (targetWidth - boxW) * 0.5f;
                offY = 0.0f;
            }
            cb[12] = boxW;
            cb[13] = boxH;
            cb[14] = offX;
            cb[15] = offY;

            // CoverCropParams
            float scale = (srcAspect > dstAspect) ? (static_cast<float>(targetHeight) / srcH)
                                                  : (static_cast<float>(targetWidth) / srcW);
            float cropW = targetWidth / scale;
            float cropH = targetHeight / scale;
            cb[16] = cropW;
            cb[17] = cropH;
            cb[18] = (srcW - cropW) * 0.5f; // startCropX
            cb[19] = (srcH - cropH) * 0.5f; // startCropY

            context->Unmap(m_impl->m_cbResample.Get(), 0);
        }

        ID3D11Buffer* cbs[] = { m_impl->m_cbResample.Get() };
        context->CSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView* srvs[] = { m_impl->m_nativeSRV.Get() };
        context->CSSetShaderResources(0, 1, srvs);

        ID3D11UnorderedAccessView* uavs[] = { m_impl->m_resampleUAV.Get() };
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        context->CSSetShader(m_impl->m_csResample.Get(), nullptr, 0);
        context->Dispatch((targetWidth + 15) / 16, (targetHeight + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSRV);
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    }

    ID3D11Texture2D* curOutputTex = m_impl->m_resampleTex.Get();
    ID3D11ShaderResourceView* curOutputSRV = m_impl->m_resampleSRV.Get();

    // 6. Pass 2: Separable Box Feathering (if featherR > 0)
    if (needFeather) {
        if (faultSeam == GpuMaskFaultSeam::FeatherDispatch) {
            if (outErrorMessage) *outErrorMessage = "Simulated feather compute dispatch failure";
            return GpuMaskOutcome::DispatchFailed;
        }

        // Update FeatherCB
        D3D11_MAPPED_SUBRESOURCE cbMap{};
        if(faultSeam == GpuMaskFaultSeam::FeatherConstants_Map || FAILED(context->Map(m_impl->m_cbFeather.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&cbMap))) {
            if(outErrorMessage) *outErrorMessage="Mask constant buffer Map failed";
            m_impl->m_hasHistory=false;
            return GpuMaskOutcome::DispatchFailed;
        }
        {
            float* cb = static_cast<float*>(cbMap.pData);
            cb[0] = static_cast<float>(featherR);
            cb[1] = static_cast<float>(targetWidth);
            cb[2] = static_cast<float>(targetHeight);
            cb[3] = 1.0f / static_cast<float>(2 * featherR + 1);
            context->Unmap(m_impl->m_cbFeather.Get(), 0);
        }

        ID3D11Buffer* cbs[] = { nullptr, m_impl->m_cbFeather.Get() };
        context->CSSetConstantBuffers(0, 2, cbs);

        // Horizontal Pass
        ID3D11ShaderResourceView* hSRVs[] = { curOutputSRV };
        ID3D11UnorderedAccessView* hUAVs[] = { m_impl->m_featherHorizUAV.Get() };
        context->CSSetShaderResources(0, 1, hSRVs);
        context->CSSetUnorderedAccessViews(0, 1, hUAVs, nullptr);
        context->CSSetShader(m_impl->m_csHorizFeather.Get(), nullptr, 0);
        context->Dispatch((targetWidth + 15) / 16, (targetHeight + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSRV);
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        // Vertical Pass
        ID3D11ShaderResourceView* vSRVs[] = { m_impl->m_featherHorizSRV.Get() };
        ID3D11UnorderedAccessView* vUAVs[] = { m_impl->m_featherVertUAV.Get() };
        context->CSSetShaderResources(0, 1, vSRVs);
        context->CSSetUnorderedAccessViews(0, 1, vUAVs, nullptr);
        context->CSSetShader(m_impl->m_csVertFeather.Get(), nullptr, 0);
        context->Dispatch((targetWidth + 15) / 16, (targetHeight + 15) / 16, 1);

        context->CSSetShaderResources(0, 1, nullSRV);
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        curOutputTex = m_impl->m_featherVertTex.Get();
        curOutputSRV = m_impl->m_featherVertSRV.Get();
    }

    // 7. Pass 3: Temporal Stabilization (if enabled)
    if (needTemporal) {
        if (faultSeam == GpuMaskFaultSeam::TemporalDispatch) {
            if (outErrorMessage) *outErrorMessage = "Simulated temporal stabilization compute dispatch failure";
            return GpuMaskOutcome::DispatchFailed;
        }

        if (!m_impl->m_hasHistory) {
            // First frame with temporal enabled: initialize history buffer
            context->CopyResource(m_impl->m_historyTex[0].Get(), curOutputTex);
            m_impl->m_hasHistory = true;
            m_impl->m_historyIdx = 0;
            curOutputTex = m_impl->m_historyTex[0].Get();
            curOutputSRV = m_impl->m_historySRV[0].Get();
        } else {
            int prevIdx = m_impl->m_historyIdx;
            int nextIdx = 1 - prevIdx;

            D3D11_MAPPED_SUBRESOURCE cbMap{};
            if(faultSeam == GpuMaskFaultSeam::TemporalConstants_Map || FAILED(context->Map(m_impl->m_cbStabilize.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&cbMap))) {
                if(outErrorMessage) *outErrorMessage="Mask constant buffer Map failed";
                m_impl->m_hasHistory=false;
                return GpuMaskOutcome::DispatchFailed;
            }
            {
                float* cb = static_cast<float*>(cbMap.pData);
                cb[0] = 0.35f; // alpha (matches default MaskStabilizer)
                cb[1] = 0.15f; // changeThreshold (matches default MaskStabilizer)
                cb[2] = static_cast<float>(targetWidth);
                cb[3] = static_cast<float>(targetHeight);
                context->Unmap(m_impl->m_cbStabilize.Get(), 0);
            }

            ID3D11Buffer* cbs[] = { nullptr, nullptr, m_impl->m_cbStabilize.Get() };
            context->CSSetConstantBuffers(0, 3, cbs);

            ID3D11ShaderResourceView* stabSRVs[] = { curOutputSRV, m_impl->m_historySRV[prevIdx].Get() };
            ID3D11UnorderedAccessView* stabUAVs[] = { m_impl->m_historyUAV[nextIdx].Get() };
            context->CSSetShaderResources(0, 2, stabSRVs);
            context->CSSetUnorderedAccessViews(0, 1, stabUAVs, nullptr);
            context->CSSetShader(m_impl->m_csTemporalStabilize.Get(), nullptr, 0);
            context->Dispatch((targetWidth + 15) / 16, (targetHeight + 15) / 16, 1);

            ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
            ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
            context->CSSetShaderResources(0, 2, nullSRV);
            context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

            m_impl->m_historyIdx = nextIdx;
            curOutputTex = m_impl->m_historyTex[nextIdx].Get();
            curOutputSRV = m_impl->m_historySRV[nextIdx].Get();
        }
    }

    if(FAILED(device->GetDeviceRemovedReason())) { m_impl->m_hasHistory=false; return GpuMaskOutcome::DeviceLost; }
    m_impl->m_activeOutputTex = curOutputTex;
    m_impl->m_activeOutputSRV = curOutputSRV;

    return GpuMaskOutcome::Success;
}

bool D3D11MaskPreparer::ReadbackPixels(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::vector<float>& outPixels,
    int& outW,
    int& outH)
{
    if (!device || !context || !m_impl || !m_impl->m_activeOutputTex) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    m_impl->m_activeOutputTex->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &stagingTex))) {
        return false;
    }

    context->CopyResource(stagingTex.Get(), m_impl->m_activeOutputTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    outW = static_cast<int>(desc.Width);
    outH = static_cast<int>(desc.Height);
    outPixels.resize(static_cast<size_t>(outW) * outH);

    for (int y = 0; y < outH; ++y) {
        const float* srcRow = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        std::memcpy(outPixels.data() + static_cast<size_t>(y) * outW, srcRow, outW * sizeof(float));
    }

    context->Unmap(stagingTex.Get(), 0);
    return true;
}

} // namespace PrismForge::Effects::AnalysisBridge::Gpu
