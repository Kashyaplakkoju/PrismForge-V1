#include "d3d11_normal_preparer.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::AnalysisBridge::Gpu {

namespace {

const char* g_normalResampleShader = R"(
Texture2D<float4> SrcNormalsTex : register(t0);
RWTexture2D<float4> DstNormalsUav : register(u0);

cbuffer ResampleParams : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint TargetWidth;
    uint TargetHeight;
    uint ResizeMode; // 0 = Stretch, 1 = Letterbox, 2 = CoverCrop
    float ScaleX;
    float ScaleY;
    float OffsetX;
    float OffsetY;
    float3 Pad;
};

[numthreads(16, 16, 1)]
void CS_ResampleNormals(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= TargetWidth || id.y >= TargetHeight) return;

    float u, v;
    if (ResizeMode == 0) // Stretch
    {
        u = (float(id.x) + 0.5f) / float(TargetWidth);
        v = (float(id.y) + 0.5f) / float(TargetHeight);
    }
    else // Letterbox / CoverCrop
    {
        u = ((float(id.x) + 0.5f) - OffsetX) / ScaleX;
        v = ((float(id.y) + 0.5f) - OffsetY) / ScaleY;
    }

    float srcX = u * float(SrcWidth) - 0.5f;
    float srcY = v * float(SrcHeight) - 0.5f;

    int x0 = clamp((int)floor(srcX), 0, (int)SrcWidth - 1);
    int y0 = clamp((int)floor(srcY), 0, (int)SrcHeight - 1);
    int x1 = clamp((int)floor(srcX) + 1, 0, (int)SrcWidth - 1);
    int y1 = clamp((int)floor(srcY) + 1, 0, (int)SrcHeight - 1);

    float fx = frac(srcX);
    float fy = frac(srcY);

    float4 n00 = SrcNormalsTex.Load(int3(x0, y0, 0));
    float4 n10 = SrcNormalsTex.Load(int3(x1, y0, 0));
    float4 n01 = SrcNormalsTex.Load(int3(x0, y1, 0));
    float4 n11 = SrcNormalsTex.Load(int3(x1, y1, 0));

    float3 interp = lerp(lerp(n00.xyz, n10.xyz, fx), lerp(n01.xyz, n11.xyz, fx), fy);

    float lenSq = dot(interp, interp);
    float3 norm;
    if (isfinite(lenSq) && lenSq > 1e-6f)
    {
        norm = interp * rsqrt(lenSq);
    }
    else
    {
        norm = float3(0.0f, 0.0f, 1.0f);
    }

    DstNormalsUav[id.xy] = float4(norm, 1.0f);
}
)";

struct alignas(16) ResampleCBData {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t targetWidth;
    uint32_t targetHeight;
    uint32_t resizeMode;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float pad[3];
};

} // anonymous namespace

struct D3D11NormalPreparer::Impl {
    ComPtr<ID3D11Device> m_resourceDevice;
    // Native source texture (dynamic upload, R32G32B32A32_FLOAT)
    ComPtr<ID3D11Texture2D> m_nativeTex;
    ComPtr<ID3D11ShaderResourceView> m_nativeSRV;
    int m_nativeW = 0;
    int m_nativeH = 0;

    // Resampled target normal texture (R32G32B32A32_FLOAT)
    ComPtr<ID3D11Texture2D> m_targetTex;
    ComPtr<ID3D11ShaderResourceView> m_targetSRV;
    ComPtr<ID3D11UnorderedAccessView> m_targetUAV;
    int m_targetW = 0;
    int m_targetH = 0;

    // Constant buffer & Shaders
    ComPtr<ID3D11Buffer> m_paramsCB;
    ComPtr<ID3D11ComputeShader> m_csResample;

    // Active output pointers (null if unready/failed)
    ID3D11ShaderResourceView* m_activeOutputSRV = nullptr;
    ID3D11Texture2D* m_activeOutputTex = nullptr;

    void ReleaseResources() {
        m_resourceDevice.Reset();
        m_activeOutputSRV = nullptr;
        m_activeOutputTex = nullptr;
        m_nativeSRV.Reset();
        m_nativeTex.Reset();
        m_nativeW = 0;
        m_nativeH = 0;
        m_targetSRV.Reset();
        m_targetUAV.Reset();
        m_targetTex.Reset();
        m_targetW = 0;
        m_targetH = 0;
        m_paramsCB.Reset();
        m_csResample.Reset();
    }

    bool EnsureShaders(ID3D11Device* device, std::string* outError) {
        if (m_csResample && m_paramsCB) return true;

        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;

        HRESULT hr = D3DCompile(
            g_normalResampleShader, std::strlen(g_normalResampleShader),
            "NormalResampleCS", nullptr, nullptr,
            "CS_ResampleNormals", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &csBlob, &errorBlob
        );
        if (FAILED(hr)) {
            if (outError && errorBlob) {
                *outError = static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            return false;
        }

        hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &m_csResample);
        if (FAILED(hr)) {
            if (outError) *outError = "Failed to create normal resample compute shader";
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(ResampleCBData);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&cbDesc, nullptr, &m_paramsCB);
        if (FAILED(hr)) {
            if (outError) *outError = "Failed to create normal resample constant buffer";
            return false;
        }

        return true;
    }

    bool EnsureNativeTexture(ID3D11Device* device, int w, int h) {
        if (m_nativeTex && m_nativeSRV && m_nativeW == w && m_nativeH == h) return true;

        m_nativeSRV.Reset();
        m_nativeTex.Reset();
        m_nativeW = 0;
        m_nativeH = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_nativeTex);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(m_nativeTex.Get(), &srvDesc, &m_nativeSRV);
        if (FAILED(hr)) return false;

        m_nativeW = w;
        m_nativeH = h;
        return true;
    }

    bool EnsureTargetTexture(ID3D11Device* device, int w, int h) {
        if (m_targetTex && m_targetSRV && m_targetUAV && m_targetW == w && m_targetH == h) return true;

        m_targetSRV.Reset();
        m_targetUAV.Reset();
        m_targetTex.Reset();
        m_targetW = 0;
        m_targetH = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_targetTex);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(m_targetTex.Get(), &srvDesc, &m_targetSRV);
        if (FAILED(hr)) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        hr = device->CreateUnorderedAccessView(m_targetTex.Get(), &uavDesc, &m_targetUAV);
        if (FAILED(hr)) return false;

        m_targetW = w;
        m_targetH = h;
        return true;
    }
};

D3D11NormalPreparer::D3D11NormalPreparer()
    : m_impl(std::make_unique<Impl>()) {
}

D3D11NormalPreparer::~D3D11NormalPreparer() = default;
D3D11NormalPreparer::D3D11NormalPreparer(D3D11NormalPreparer&&) noexcept = default;
D3D11NormalPreparer& D3D11NormalPreparer::operator=(D3D11NormalPreparer&&) noexcept = default;

void D3D11NormalPreparer::Reset() noexcept {
    if (m_impl) {
        m_impl->m_activeOutputSRV = nullptr;
        m_impl->m_activeOutputTex = nullptr;
    }
}

void D3D11NormalPreparer::ReleaseResources() noexcept {
    if (m_impl) {
        m_impl->ReleaseResources();
    }
}

GpuNormalOutcome D3D11NormalPreparer::PrepareNormals(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const NormalBuffer& srcNormals,
    int targetWidth,
    int targetHeight,
    const NormalPreparationConfig& config,
    GpuNormalFaultSeam faultSeam,
    HRESULT customFaultHr,
    std::string* outErrorMessage
) {
    if (m_impl) {
        m_impl->m_activeOutputSRV = nullptr;
        m_impl->m_activeOutputTex = nullptr;
    }

    if (!device || !context) {
        if (outErrorMessage) *outErrorMessage = "Null D3D11 device or context";
        return GpuNormalOutcome::InvalidArgs;
    }

    if (faultSeam == GpuNormalFaultSeam::DeviceRemoved) {
        if (outErrorMessage) *outErrorMessage = "Simulated device removed fault";
        return GpuNormalOutcome::DeviceLost;
    }

    HRESULT devStatus = device->GetDeviceRemovedReason();
    if (FAILED(devStatus)) {
        if (outErrorMessage) *outErrorMessage = "Device lost before normal preparation";
        return GpuNormalOutcome::DeviceLost;
    }

    if(!m_impl) m_impl=std::make_unique<Impl>();
    ComPtr<ID3D11Device> contextDevice; context->GetDevice(&contextDevice);
    if(contextDevice.Get()!=device || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return GpuNormalOutcome::InvalidArgs;
    if(m_impl->m_resourceDevice.Get()!=device) { m_impl->ReleaseResources(); m_impl->m_resourceDevice=device; }
    if (!srcNormals.IsValid() || static_cast<int>(config.resizeMode)<0 || static_cast<int>(config.resizeMode)>2) {
        if (outErrorMessage) *outErrorMessage = "Invalid source NormalBuffer";
        return GpuNormalOutcome::InvalidArgs;
    }

    if (targetWidth <= 0 || targetHeight <= 0 ||
        targetWidth > NormalBuffer::kMaxDimension || targetHeight > NormalBuffer::kMaxDimension) {
        if (outErrorMessage) *outErrorMessage = "Target dimensions out of bounds";
        return GpuNormalOutcome::InvalidArgs;
    }

    // 1. Shaders & Constant Buffer
    if (faultSeam == GpuNormalFaultSeam::ShaderCompile) {
        if (outErrorMessage) *outErrorMessage = "Simulated shader compile fault";
        return GpuNormalOutcome::InjectedFault;
    }
    if (!m_impl->EnsureShaders(device, outErrorMessage)) {
        return GpuNormalOutcome::ShaderError;
    }

    // 2. Native source texture allocation & upload
    if (!m_impl->EnsureNativeTexture(device, srcNormals.width, srcNormals.height)) {
        if (outErrorMessage) *outErrorMessage = "Failed to allocate native normal texture";
        return GpuNormalOutcome::AllocationFailed;
    }

    if (faultSeam == GpuNormalFaultSeam::NativeUpload_Map) {
        if (outErrorMessage) {
            std::stringstream ss;
            ss << "Simulated fault at native normal map boundary (HRESULT 0x"
               << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (uint32_t)customFaultHr << ")";
            *outErrorMessage = ss.str();
        }
        return GpuNormalOutcome::InjectedFault;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(m_impl->m_nativeTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        if (outErrorMessage) *outErrorMessage = "Failed to map native normal texture";
        return GpuNormalOutcome::DispatchFailed;
    }

    const int srcW = srcNormals.width;
    const int srcH = srcNormals.height;
    const size_t srcStride = static_cast<size_t>(srcNormals.strideFloats);
    const float* srcData = srcNormals.data.data();

    for (int y = 0; y < srcH; ++y) {
        const float* srcRow = srcData + static_cast<size_t>(y) * srcStride;
        float* dstRow = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        for (int x = 0; x < srcW; ++x) {
            float nx = srcRow[3 * x + 0];
            float ny = srcRow[3 * x + 1];
            float nz = srcRow[3 * x + 2];
            if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)) {
                nx = 0.0f; ny = 0.0f; nz = 1.0f;
            }
            dstRow[4 * x + 0] = nx;
            dstRow[4 * x + 1] = ny;
            dstRow[4 * x + 2] = nz;
            dstRow[4 * x + 3] = 1.0f;
        }
    }
    context->Unmap(m_impl->m_nativeTex.Get(), 0);

    // 3. Target texture allocation
    if (faultSeam == GpuNormalFaultSeam::TargetAlloc) {
        if (outErrorMessage) *outErrorMessage = "Simulated target normal texture allocation fault";
        return GpuNormalOutcome::InjectedFault;
    }
    if (!m_impl->EnsureTargetTexture(device, targetWidth, targetHeight)) {
        if (outErrorMessage) *outErrorMessage = "Failed to allocate target normal texture";
        return GpuNormalOutcome::AllocationFailed;
    }

    // 4. Update CB and Dispatch Resample CS
    if (faultSeam == GpuNormalFaultSeam::ResampleDispatch) {
        if (outErrorMessage) *outErrorMessage = "Simulated normal resample dispatch fault";
        return GpuNormalOutcome::InjectedFault;
    }

    ResampleCBData cb = {};
    cb.srcWidth = static_cast<uint32_t>(srcW);
    cb.srcHeight = static_cast<uint32_t>(srcH);
    cb.targetWidth = static_cast<uint32_t>(targetWidth);
    cb.targetHeight = static_cast<uint32_t>(targetHeight);
    cb.resizeMode = static_cast<uint32_t>(config.resizeMode);

    const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    const float dstAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);

    if (config.resizeMode == MaskResizeMode::Stretch) {
        cb.scaleX = static_cast<float>(targetWidth);
        cb.scaleY = static_cast<float>(targetHeight);
        cb.offsetX = 0.0f;
        cb.offsetY = 0.0f;
    } else if (config.resizeMode == MaskResizeMode::Letterbox) {
        if (srcAspect > dstAspect) {
            cb.scaleX = static_cast<float>(targetWidth);
            cb.scaleY = static_cast<float>(targetWidth) / srcAspect;
            cb.offsetX = 0.0f;
            cb.offsetY = (static_cast<float>(targetHeight) - cb.scaleY) * 0.5f;
        } else {
            cb.scaleY = static_cast<float>(targetHeight);
            cb.scaleX = static_cast<float>(targetHeight) * srcAspect;
            cb.offsetX = (static_cast<float>(targetWidth) - cb.scaleX) * 0.5f;
            cb.offsetY = 0.0f;
        }
    } else if (config.resizeMode == MaskResizeMode::CoverCrop) {
        if (srcAspect > dstAspect) {
            cb.scaleY = static_cast<float>(targetHeight);
            cb.scaleX = static_cast<float>(targetHeight) * srcAspect;
            cb.offsetX = (static_cast<float>(targetWidth) - cb.scaleX) * 0.5f;
            cb.offsetY = 0.0f;
        } else {
            cb.scaleX = static_cast<float>(targetWidth);
            cb.scaleY = static_cast<float>(targetWidth) / srcAspect;
            cb.offsetX = 0.0f;
            cb.offsetY = (static_cast<float>(targetHeight) - cb.scaleY) * 0.5f;
        }
    }

    context->UpdateSubresource(m_impl->m_paramsCB.Get(), 0, nullptr, &cb, 0, 0);

    ID3D11Buffer* cbs[1] = { m_impl->m_paramsCB.Get() };
    context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[1] = { m_impl->m_nativeSRV.Get() };
    context->CSSetShaderResources(0, 1, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { m_impl->m_targetUAV.Get() };
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    context->CSSetShader(m_impl->m_csResample.Get(), nullptr, 0);

    const UINT groupsX = (static_cast<UINT>(targetWidth) + 15) / 16;
    const UINT groupsY = (static_cast<UINT>(targetHeight) + 15) / 16;
    context->Dispatch(groupsX, groupsY, 1);

    // Unbind resources to prevent hazards
    ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
    context->CSSetShaderResources(0, 1, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

    ID3D11Buffer* nullCBs[1] = { nullptr };
    context->CSSetConstantBuffers(0, 1, nullCBs);

    context->CSSetShader(nullptr, nullptr, 0);

    if(FAILED(device->GetDeviceRemovedReason())) return GpuNormalOutcome::DeviceLost;
    // Transactional success commit
    m_impl->m_activeOutputSRV = m_impl->m_targetSRV.Get();
    m_impl->m_activeOutputTex = m_impl->m_targetTex.Get();

    return GpuNormalOutcome::Success;
}

ID3D11ShaderResourceView* D3D11NormalPreparer::GetOutputSRV() const noexcept {
    return m_impl ? m_impl->m_activeOutputSRV : nullptr;
}

ID3D11Texture2D* D3D11NormalPreparer::GetOutputTexture() const noexcept {
    return m_impl ? m_impl->m_activeOutputTex : nullptr;
}

int D3D11NormalPreparer::GetWidth() const noexcept {
    return m_impl ? m_impl->m_targetW : 0;
}

int D3D11NormalPreparer::GetHeight() const noexcept {
    return m_impl ? m_impl->m_targetH : 0;
}

bool D3D11NormalPreparer::HasOutput() const noexcept {
    return m_impl && (m_impl->m_activeOutputSRV != nullptr);
}

bool D3D11NormalPreparer::ReadbackPixels(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::vector<float>& outPixels,
    int& outW,
    int& outH
) {
    if (!device || !context || !HasOutput()) {
        return false;
    }

    const int w = m_impl->m_targetW;
    const int h = m_impl->m_targetH;

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = static_cast<UINT>(w);
    stagingDesc.Height = static_cast<UINT>(h);
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> stagingTex;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) return false;

    context->CopyResource(stagingTex.Get(), m_impl->m_activeOutputTex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    outPixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        const float* srcRow = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        float* dstRow = outPixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4 * sizeof(float));
    }

    context->Unmap(stagingTex.Get(), 0);
    outW = w;
    outH = h;
    return true;
}

} // namespace PrismForge::Effects::AnalysisBridge::Gpu
