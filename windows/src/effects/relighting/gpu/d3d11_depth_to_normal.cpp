#include "d3d11_depth_to_normal.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace PrismForge::Effects::Relighting::Gpu {

static const char* g_csDepthToNormalsSrc = R"(
Texture2D<float> g_DepthTex : register(t0);
RWTexture2D<float4> g_OutputNormal : register(u0);

cbuffer DepthToNormalCB : register(b0) {
    int g_Width;
    int g_Height;
    int g_Format;       // 0 = RelativeDepth, 1 = InverseDepth
    int g_UseSobel;     // 1 = Sobel 3x3, 0 = Central diff
    float g_DepthScale;
    float g_InvalidMin;
    float g_InvalidMax;
    float g_Aspect;
    int g_FlipX;
    int g_FlipY;
    float g_FallbackZ;
    float g_Padding;
};

bool IsValidDepth(float d) {
    if (isnan(d) || isinf(d)) return false;
    return (d >= g_InvalidMin && d <= g_InvalidMax);
}

float SampleValidDepth(int x, int y, float centerVal) {
    x = clamp(x, 0, g_Width - 1);
    y = clamp(y, 0, g_Height - 1);
    float d = g_DepthTex.Load(int3(x, y, 0));
    return IsValidDepth(d) ? d : centerVal;
}

[numthreads(16, 16, 1)]
void CS_DepthToNormals(uint3 dispatchThreadId : SV_DispatchThreadID) {
    int x = (int)dispatchThreadId.x;
    int y = (int)dispatchThreadId.y;
    if (x >= g_Width || y >= g_Height) return;

    float centerDepth = g_DepthTex.Load(int3(x, y, 0));
    if (!IsValidDepth(centerDepth)) {
        g_OutputNormal[int2(x, y)] = float4(0.0f, 0.0f, 1.0f, 1.0f);
        return;
    }

    float dx = 0.0f;
    float dy = 0.0f;

    if (g_UseSobel != 0) {
        float tl = SampleValidDepth(x - 1, y - 1, centerDepth);
        float tc = SampleValidDepth(x,     y - 1, centerDepth);
        float tr = SampleValidDepth(x + 1, y - 1, centerDepth);
        float ml = SampleValidDepth(x - 1, y,     centerDepth);
        float mr = SampleValidDepth(x + 1, y,     centerDepth);
        float bl = SampleValidDepth(x - 1, y + 1, centerDepth);
        float bc = SampleValidDepth(x,     y + 1, centerDepth);
        float br = SampleValidDepth(x + 1, y + 1, centerDepth);

        dx = ((tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl)) * 0.125f;
        dy = ((bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr)) * 0.125f;
    } else {
        float l = SampleValidDepth(x - 1, y, centerDepth);
        float r = SampleValidDepth(x + 1, y, centerDepth);
        float t = SampleValidDepth(x, y - 1, centerDepth);
        float b = SampleValidDepth(x, y + 1, centerDepth);

        dx = (r - l) * 0.5f;
        dy = (b - t) * 0.5f;
    }

    float nx = 0.0f;
    float ny = 0.0f;
    float scaleX = g_DepthScale * (float)g_Width;
    float scaleY = g_DepthScale * (float)g_Height;
    if (g_Format == 0) {
        nx = dx * scaleX;
        ny = dy * scaleY;
    } else {
        nx = -dx * scaleX;
        ny = -dy * scaleY;
    }

    if (g_FlipX != 0) nx = -nx;
    if (g_FlipY != 0) ny = -ny;

    float nz = (g_FallbackZ > 0.0f) ? g_FallbackZ : 1.0f;
    float3 normal = float3(nx, ny, nz);
    float lenSq = dot(normal, normal);
    if (lenSq > 1e-6f && !isnan(lenSq) && !isinf(lenSq)) {
        normal = normalize(normal);
    } else {
        normal = float3(0.0f, 0.0f, 1.0f);
    }

    if (isnan(normal.x) || isnan(normal.y) || isnan(normal.z) ||
        isinf(normal.x) || isinf(normal.y) || isinf(normal.z)) {
        normal = float3(0.0f, 0.0f, 1.0f);
    }

    g_OutputNormal[int2(x, y)] = float4(normal, 1.0f);
}
)";

D3D11DepthToNormal::D3D11DepthToNormal() = default;
D3D11DepthToNormal::~D3D11DepthToNormal() {
    ReleaseResources();
}

void D3D11DepthToNormal::Reset() {
    // Invalidate only published output; preserve complete allocations for the next frame.
    m_hasOutput = false;
}

void D3D11DepthToNormal::ReleaseResources() {
    Reset();
    m_outputSRV.Reset();
    m_outputUAV.Reset();
    m_outputTex.Reset();
    m_currentWidth = 0;
    m_currentHeight = 0;
    m_resourceDevice.Reset();
    m_inputSRV.Reset();
    m_inputTex.Reset();
    m_stagingTex.Reset();
    m_constantBuffer.Reset();
    m_computeShader.Reset();
    m_inputCapacityW = 0;
    m_inputCapacityH = 0;
}

bool D3D11DepthToNormal::EnsureShaders(ID3D11Device* device, std::string* errorDescription) {
    if (m_computeShader && m_constantBuffer) {
        return true;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(
        g_csDepthToNormalsSrc,
        strlen(g_csDepthToNormalsSrc),
        "CS_DepthToNormals",
        nullptr,
        nullptr,
        "CS_DepthToNormals",
        "cs_5_0",
        compileFlags,
        0,
        &csBlob,
        &errBlob
    );

    if (FAILED(hr)) {
        if (errorDescription) {
            *errorDescription = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Failed to compile CS_DepthToNormals";
        }
        return false;
    }

    hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &m_computeShader);
    if (FAILED(hr)) {
        if (errorDescription) *errorDescription = "Failed to create ComputeShader";
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(DepthToNormalCB);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
    if (FAILED(hr)) {
        if (errorDescription) *errorDescription = "Failed to create ConstantBuffer";
        return false;
    }

    return true;
}

bool D3D11DepthToNormal::EnsureTextures(ID3D11Device* device, int width, int height, std::string* errorDescription) {
    using Microsoft::WRL::ComPtr;
    // Allocate into local ownership, and publish complete texture/view sets only.
    if (!m_inputTex || !m_inputSRV || m_inputCapacityW < width || m_inputCapacityH < height) {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R32_FLOAT; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DYNAMIC; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv))) {
            if(errorDescription) *errorDescription="Failed to create complete depth upload resources";
            return false;
        }
        m_inputTex=std::move(texture); m_inputSRV=std::move(srv);
        m_inputCapacityW=width; m_inputCapacityH=height;
    }
    if (m_outputTex && m_outputUAV && m_outputSRV && m_currentWidth==width && m_currentHeight==height) return true;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
        FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv))) {
        if(errorDescription) *errorDescription="Failed to create complete reconstructed normal resources";
        return false;
    }
    m_outputTex=std::move(texture); m_outputUAV=std::move(uav); m_outputSRV=std::move(srv);
    m_stagingTex.Reset(); // Readback dimensions must follow the committed output allocation.
    m_currentWidth=width; m_currentHeight=height;
    return true;
}

GpuDepthToNormalOutcome D3D11DepthToNormal::ReconstructNormals(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const DepthBuffer& depth,
    const DepthToNormalConfig& config,
    GpuDepthToNormalFaultSeam faultSeam,
    HRESULT* outHresult,
    std::string* errorDescription
) {
    Reset();
    if (!device || !context || !depth.IsValid() || !config.IsValid()) {
        Reset();
        if (outHresult) *outHresult = E_INVALIDARG;
        if (errorDescription) *errorDescription = "Invalid device, context, or DepthBuffer";
        return GpuDepthToNormalOutcome::InvalidInput;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get()!=device || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        if(outHresult) *outHresult=E_INVALIDARG;
        if(errorDescription) *errorDescription="Mismatched device/context or deferred context";
        return GpuDepthToNormalOutcome::InvalidInput;
    }
    if(m_resourceDevice.Get()!=device) { ReleaseResources(); m_resourceDevice=device; }
    const HRESULT deviceReason=device->GetDeviceRemovedReason();
    if(FAILED(deviceReason)) {
        if(outHresult) *outHresult=deviceReason;
        if(errorDescription) *errorDescription="Depth reconstruction device lost";
        return GpuDepthToNormalOutcome::DeviceLoss;
    }

    if (faultSeam == GpuDepthToNormalFaultSeam::DeviceRemoved) {
        Reset();
        if (outHresult) *outHresult = DXGI_ERROR_DEVICE_REMOVED;
        if (errorDescription) *errorDescription = "Injected DeviceRemoved fault";
        return GpuDepthToNormalOutcome::DeviceLoss;
    }

    if (faultSeam == GpuDepthToNormalFaultSeam::ShaderCompile) {
        Reset();
        if (outHresult) *outHresult = E_FAIL;
        if (errorDescription) *errorDescription = "Injected ShaderCompile fault";
        return GpuDepthToNormalOutcome::ShaderFailed;
    }

    if (!EnsureShaders(device, errorDescription)) {
        Reset();
        if (outHresult) *outHresult = E_FAIL;
        return GpuDepthToNormalOutcome::ShaderFailed;
    }

    if (faultSeam == GpuDepthToNormalFaultSeam::OutputAlloc) {
        Reset();
        if (outHresult) *outHresult = E_OUTOFMEMORY;
        if (errorDescription) *errorDescription = "Injected OutputAlloc fault";
        return GpuDepthToNormalOutcome::AllocationFailed;
    }

    if (!EnsureTextures(device, depth.width, depth.height, errorDescription)) {
        Reset();
        if (outHresult) *outHresult = E_OUTOFMEMORY;
        return GpuDepthToNormalOutcome::AllocationFailed;
    }

    if (faultSeam == GpuDepthToNormalFaultSeam::DepthUpload_Map) {
        Reset();
        if (outHresult) *outHresult = E_FAIL;
        if (errorDescription) *errorDescription = "Injected DepthUpload_Map fault";
        return GpuDepthToNormalOutcome::AllocationFailed;
    }

    // Upload depth buffer row by row to dynamic input texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(m_inputTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        Reset();
        if (outHresult) *outHresult = hr;
        if (errorDescription) *errorDescription = "Failed to map input depth texture";
        return GpuDepthToNormalOutcome::AllocationFailed;
    }

    // Encode invalid confidence as an invalid depth sample before native upload.
    // This makes center and neighbour validity identical to the CPU reference.
    for (int y=0; y<depth.height; ++y) {
        const size_t sourceRow=static_cast<size_t>(y)*depth.strideFloats;
        float* destination=reinterpret_cast<float*>(static_cast<uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch);
        for(int x=0; x<depth.width; ++x) {
            const float confidence=depth.confidence.empty()?1.0f:depth.confidence[sourceRow+x];
            destination[x]=(std::isfinite(confidence) && confidence>=config.minConfidence && confidence<=1.0f)
                ? depth.data[sourceRow+x] : std::numeric_limits<float>::quiet_NaN();
        }
    }
    context->Unmap(m_inputTex.Get(), 0);

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    hr = context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB);
    if (FAILED(hr)) {
        Reset();
        if (outHresult) *outHresult = hr;
        if (errorDescription) *errorDescription = "Failed to map ConstantBuffer";
        return GpuDepthToNormalOutcome::AllocationFailed;
    }

    DepthToNormalCB* cb = reinterpret_cast<DepthToNormalCB*>(mappedCB.pData);
    cb->width = depth.width;
    cb->height = depth.height;
    cb->format = static_cast<int>(config.format);
    cb->useSobel = config.useSobel ? 1 : 0;
    cb->depthScale = config.depthScale;
    cb->invalidMin = config.invalidDepthMin;
    cb->invalidMax = config.invalidDepthMax;
    cb->aspect = static_cast<float>(depth.width) / static_cast<float>(depth.height);
    cb->flipX = config.flipX ? 1 : 0;
    cb->flipY = config.flipY ? 1 : 0;
    cb->fallbackZ = (config.fallbackZ > 0.0f) ? config.fallbackZ : 1.0f;
    cb->padding = 0.0f;
    context->Unmap(m_constantBuffer.Get(), 0);

    if (faultSeam == GpuDepthToNormalFaultSeam::Dispatch) {
        Reset();
        if (outHresult) *outHresult = E_FAIL;
        if (errorDescription) *errorDescription = "Injected Dispatch fault";
        return GpuDepthToNormalOutcome::DispatchFailed;
    }

    // Bind pipeline state
    context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = { m_inputSRV.Get() };
    context->CSSetShaderResources(0, 1, srvs);

    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get() };
    UINT initCounts[] = { 0 };
    context->CSSetUnorderedAccessViews(0, 1, uavs, initCounts);

    // Dispatch 16x16 thread groups
    UINT dispatchX = (depth.width + 15) / 16;
    UINT dispatchY = (depth.height + 15) / 16;
    context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind resources to prevent hazards
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    context->CSSetShaderResources(0, 1, nullSRV);

    ID3D11Buffer* nullCB[] = { nullptr };
    context->CSSetConstantBuffers(0, 1, nullCB);
    context->CSSetShader(nullptr, nullptr, 0);

    hr=device->GetDeviceRemovedReason();
    if(FAILED(hr)) { Reset(); if(outHresult) *outHresult=hr; return GpuDepthToNormalOutcome::DeviceLoss; }
    m_hasOutput=true;
    if (outHresult) *outHresult = S_OK;
    return GpuDepthToNormalOutcome::Success;
}

bool D3D11DepthToNormal::ReadbackNormals(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    AnalysisBridge::NormalBuffer& outNormals
) const {
    outNormals = AnalysisBridge::NormalBuffer{};
    if (!device || !context || device!=m_resourceDevice.Get() || !HasOutput() || !m_outputTex || m_currentWidth <= 0 || m_currentHeight <= 0) {
        return false;
    }

    if (!m_stagingTex) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_currentWidth;
        desc.Height = m_currentHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_stagingTex);
        if (FAILED(hr)) return false;
    }

    AnalysisBridge::NormalBuffer candidate;
    candidate.width=m_currentWidth; candidate.height=m_currentHeight; candidate.strideFloats=m_currentWidth*3;
    try { candidate.data.resize(static_cast<size_t>(candidate.strideFloats)*candidate.height); }
    catch (...) { return false; }
    context->CopyResource(m_stagingTex.Get(), m_outputTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(m_stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < m_currentHeight; ++y) {
        const float* float4Row = reinterpret_cast<const float*>(srcRow + y * mapped.RowPitch);
        float* dstRow = candidate.data.data() + static_cast<size_t>(y)*candidate.strideFloats;
        for (int x = 0; x < m_currentWidth; ++x) {
            dstRow[3 * x + 0] = float4Row[4 * x + 0];
            dstRow[3 * x + 1] = float4Row[4 * x + 1];
            dstRow[3 * x + 2] = float4Row[4 * x + 2];
        }
    }

    context->Unmap(m_stagingTex.Get(), 0);
    outNormals=std::move(candidate);
    return true;
}

} // namespace PrismForge::Effects::Relighting::Gpu
