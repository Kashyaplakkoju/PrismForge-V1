#include "d3d11_studiolighting.h"
#include "effects/relighting/gpu/d3d11_approximate_shadows.h"

#include <d3dcompiler.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <new>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace PrismForge::Effects::Lighting::Gpu {

namespace {

const char* g_lightingComputeShader = R"(
Texture2D<float4> InputTex   : register(t0);
Texture2D<float>  MaskTex    : register(t1);
Texture2D<float4> NormalsTex : register(t2);
Texture2D<float2> ShadowsTex : register(t3);

RWTexture2D<float4> OutputUav : register(u0);

cbuffer LightingCB : register(b0)
{
    float4 Light1Pos;      // (x, y, z, enabled)
    float4 Light1Color;    // (r, g, b, intensity)
    float4 Light2Pos;      // (x, y, z, enabled)
    float4 Light2Color;    // (r, g, b, intensity)
    float4 GlobalParams;   // (globalStrength, ambientIntensity, fillIntensity, bypass)
    float4 ModeParams;     // (hasMask, hasNormals, subjectOnlyMode, hasShadows)
    float4 ScreenParams;   // (width, height, aspect, pad)
};

float SrgbToLinear(float c)
{
    return (c <= 0.04045f) ? (c / 12.92f) : pow(abs(c + 0.055f) / 1.055f, 2.4f);
}

float3 SrgbToLinear3(float3 c)
{
    return float3(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b));
}

float LinearToSrgb(float L)
{
    L = saturate(L);
    return (L <= 0.0031308f) ? (12.92f * L) : (1.055f * pow(abs(L), 1.0f / 2.4f) - 0.055f);
}

float3 LinearToSrgb3(float3 L)
{
    return float3(LinearToSrgb(L.r), LinearToSrgb(L.g), LinearToSrgb(L.b));
}

float CalculateSpatialApprox(float u, float v, float aspect, float4 lPos, float4 lCol)
{
    if (lPos.w <= 0.5f || lCol.w <= 0.0f)
        return 0.0f;

    float posX = clamp(lPos.x, -1.0f, 1.0f);
    float posY = clamp(lPos.y, -1.0f, 1.0f);
    float posZ = max(0.1f, lPos.z);

    float deltaX = (posX - u) * aspect;
    float deltaY = posY - v;
    float deltaZ = posZ;

    float dist = sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    if (dist < 1e-5f)
        return 1.0f;

    float lx = deltaX / dist;
    float ly = deltaY / dist;
    float lz = deltaZ / dist;

    float nx = 0.45f * u;
    float ny = 0.35f * v;
    float nz = sqrt(max(0.05f, 1.0f - nx * nx - ny * ny));
    float invLen = rsqrt(nx * nx + ny * ny + nz * nz);
    nx *= invLen;
    ny *= invLen;
    nz *= invLen;

    float dotNL = nx * lx + ny * ly + nz * lz;
    float diffuse = max(0.0f, 0.35f + 0.65f * dotNL);
    float falloff = 1.0f / (1.0f + 0.4f * max(0.0f, dist - posZ));

    return diffuse * falloff;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)ScreenParams.x;
    uint height = (uint)ScreenParams.y;
    if (id.x >= width || id.y >= height)
        return;

    float4 src = InputTex[id.xy];

    // Bypass check
    if (GlobalParams.w > 0.5f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    bool hasMask = ModeParams.x > 0.5f;
    bool hasNormals = ModeParams.y > 0.5f;
    bool subjectOnlyMode = ModeParams.z > 0.5f;

    // Subject-only mode without mask: passthrough
    if (subjectOnlyMode && !hasMask)
    {
        OutputUav[id.xy] = src;
        return;
    }

    float alpha = 1.0f;
    if (hasMask)
    {
        alpha = saturate(MaskTex[id.xy]);
    }

    // Exact background preservation
    if (alpha <= 0.0f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    float aspect = ScreenParams.z;
    float u = 2.0f * ((float(id.x) + 0.5f) / float(width)) - 1.0f;
    float v = 2.0f * ((float(id.y) + 0.5f) / float(height)) - 1.0f;

    float globalStrength = clamp(GlobalParams.x, 0.0f, 2.0f);
    float ambient = clamp(GlobalParams.y, 0.0f, 2.0f);
    float fill = clamp(GlobalParams.z, 0.0f, 2.0f);
    float baseFactor = ambient + fill;

    // Normal calculation
    float3 N = float3(0.0f, 0.0f, 1.0f);
    bool validNormal = false;
    if (hasNormals)
    {
        float4 rawN = NormalsTex[id.xy];
        float nLenSq = dot(rawN.xyz, rawN.xyz);
        if (nLenSq > 1e-6f)
        {
            N = normalize(rawN.xyz);
            validNormal = true;
        }
    }

    // Light 1
    float l1_factor = 0.0f;
    if (Light1Pos.w > 0.5f && Light1Color.w > 0.0f)
    {
        float l1_intensity = clamp(Light1Color.w, 0.0f, 2.0f) * globalStrength;
        if (validNormal)
        {
            float posX = clamp(Light1Pos.x, -1.0f, 1.0f);
            float posY = clamp(Light1Pos.y, -1.0f, 1.0f);
            float posZ = max(0.1f, Light1Pos.z);
            float deltaX = (posX - u) * aspect;
            float deltaY = posY - v;
            float deltaZ = posZ;
            float dist = sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
            if (dist > 1e-5f)
            {
                float3 L = float3(deltaX, deltaY, deltaZ) / dist;
                float dotNL = max(0.0f, dot(N, L));
                float falloff = 1.0f / (1.0f + 0.4f * max(0.0f, dist - posZ));
                l1_factor = dotNL * falloff * l1_intensity;
            }
        }
        else
        {
            l1_factor = CalculateSpatialApprox(u, v, aspect, Light1Pos, Light1Color) * l1_intensity;
        }
    }

    // Light 2
    float l2_factor = 0.0f;
    if (Light2Pos.w > 0.5f && Light2Color.w > 0.0f)
    {
        float l2_intensity = clamp(Light2Color.w, 0.0f, 2.0f) * globalStrength;
        if (validNormal)
        {
            float posX = clamp(Light2Pos.x, -1.0f, 1.0f);
            float posY = clamp(Light2Pos.y, -1.0f, 1.0f);
            float posZ = max(0.1f, Light2Pos.z);
            float deltaX = (posX - u) * aspect;
            float deltaY = posY - v;
            float deltaZ = posZ;
            float dist = sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
            if (dist > 1e-5f)
            {
                float3 L = float3(deltaX, deltaY, deltaZ) / dist;
                float dotNL = max(0.0f, dot(N, L));
                float falloff = 1.0f / (1.0f + 0.4f * max(0.0f, dist - posZ));
                l2_factor = dotNL * falloff * l2_intensity;
            }
        }
        else
        {
            l2_factor = CalculateSpatialApprox(u, v, aspect, Light2Pos, Light2Color) * l2_intensity;
        }
    }

    float2 vis = float2(1.0f, 1.0f);
    if (ModeParams.w > 0.5f)
    {
        vis = ShadowsTex[id.xy];
    }

    float3 mult = float3(baseFactor, baseFactor, baseFactor) +
                  Light1Color.rgb * (l1_factor * vis.x) +
                  Light2Color.rgb * (l2_factor * vis.y);

    float3 linSrc = SrgbToLinear3(src.rgb);
    float3 litLin = linSrc * mult;
    float3 litSrgb = LinearToSrgb3(litLin);

    float3 outRgb = lerp(src.rgb, litSrgb, alpha);
    OutputUav[id.xy] = float4(outRgb, src.a);
}
)";

struct alignas(16) LightingShaderCB {
    float Light1Pos[4];     // x, y, z, enabled (1.0f or 0.0f)
    float Light1Color[4];   // r, g, b, intensity
    float Light2Pos[4];     // x, y, z, enabled
    float Light2Color[4];   // r, g, b, intensity
    float GlobalParams[4];  // globalStrength, ambient, fill, bypass
    float ModeParams[4];    // hasMask, hasNormals, subjectOnlyMode, hasShadows
    float ScreenParams[4];  // width, height, aspect, pad
};

} // anonymous namespace

struct D3D11StudioLighting::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11ComputeShader> m_computeShader;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    std::unique_ptr<PrismForge::Effects::Relighting::Gpu::D3D11ApproximateShadows> m_shadowEngine;
    bool m_isDeviceLost = false;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {
        InitShader();
    }

    bool InitShader() {
        if (!m_device) return false;

        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(DEBUG) || defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        HRESULT hr = D3DCompile(
            g_lightingComputeShader,
            std::strlen(g_lightingComputeShader),
            "LightingCS",
            nullptr,
            nullptr,
            "CSMain",
            "cs_5_0",
            flags,
            0,
            &csBlob,
            &errorBlob
        );

        if (FAILED(hr)) {
            return false;
        }

        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(),
            nullptr,
            &m_computeShader
        );
        if (FAILED(hr)) {
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(LightingShaderCB);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = 0;

        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        return SUCCEEDED(hr);
    }
};

D3D11StudioLighting::D3D11StudioLighting(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context)) {
}

D3D11StudioLighting::~D3D11StudioLighting() = default;

D3D11StudioLighting::D3D11StudioLighting(D3D11StudioLighting&&) noexcept = default;
D3D11StudioLighting& D3D11StudioLighting::operator=(D3D11StudioLighting&&) noexcept = default;

bool D3D11StudioLighting::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    if (!newDevice || !newContext) return false;
    m_impl = std::make_unique<Impl>(newDevice, newContext);
    return m_impl->m_computeShader != nullptr;
}

bool D3D11StudioLighting::IsDeviceLost() const {
    if (!m_impl || !m_impl->m_device) return true;
    HRESULT hr = m_impl->m_device->GetDeviceRemovedReason();
    return FAILED(hr);
}

GpuLightingOutcome D3D11StudioLighting::Process(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11UnorderedAccessView* outputUAV,
    ID3D11ShaderResourceView* maskSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* depthSRV,
    int width, int height,
    const LightingConfig& config
) {
    if (IsDeviceLost()) {
        return GpuLightingOutcome::DeviceLost;
    }

    if (!inputSRV || !outputUAV || width <= 0 || height <= 0) {
        return GpuLightingOutcome::InvalidInput;
    }

    if (!m_impl->m_computeShader || !m_impl->m_constantBuffer) {
        return GpuLightingOutcome::ShaderError;
    }

    if(!config.bypass && config.shadowConfig.enabled && !depthSRV) return GpuLightingOutcome::NotReady;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    // Evaluate approximate shadows if enabled and depthSRV is available
    ID3D11ShaderResourceView* shadowSRV = nullptr;
    bool hasShadows = false;
    if (config.shadowConfig.enabled && depthSRV) {
        if (!m_impl->m_shadowEngine) {
            m_impl->m_shadowEngine = std::make_unique<PrismForge::Effects::Relighting::Gpu::D3D11ApproximateShadows>(
                m_impl->m_device.Get(), m_impl->m_context.Get()
            );
        }
        auto sOutcome = m_impl->m_shadowEngine->ExecuteShadowPass(
            depthSRV, normalsSRV, maskSRV, width, height, config
        );
        if (sOutcome != PrismForge::Effects::Relighting::Gpu::GpuShadowOutcome::Success) {
            return sOutcome==PrismForge::Effects::Relighting::Gpu::GpuShadowOutcome::DeviceLost ? GpuLightingOutcome::DeviceLost :
                (sOutcome==PrismForge::Effects::Relighting::Gpu::GpuShadowOutcome::NotReady ? GpuLightingOutcome::NotReady : GpuLightingOutcome::InvalidInput);
        }
        if (sOutcome == PrismForge::Effects::Relighting::Gpu::GpuShadowOutcome::Success) {
            shadowSRV = m_impl->m_shadowEngine->GetOutputSRV();
            hasShadows = (shadowSRV != nullptr);
        }
    }

    LightingShaderCB cbData = {};

    // Light 1
    cbData.Light1Pos[0] = std::clamp(config.light1.posX, -1.0f, 1.0f);
    cbData.Light1Pos[1] = std::clamp(config.light1.posY, -1.0f, 1.0f);
    cbData.Light1Pos[2] = std::max(0.1f, config.light1.posZ);
    cbData.Light1Pos[3] = (config.light1.enabled && config.light1.intensity > 0.0f) ? 1.0f : 0.0f;

    float l1_r, l1_g, l1_b;
    config.light1.GetEffectiveLinearColor(l1_r, l1_g, l1_b);
    cbData.Light1Color[0] = std::clamp(l1_r, 0.0f, 1.0f);
    cbData.Light1Color[1] = std::clamp(l1_g, 0.0f, 1.0f);
    cbData.Light1Color[2] = std::clamp(l1_b, 0.0f, 1.0f);
    cbData.Light1Color[3] = std::clamp(config.light1.intensity, 0.0f, 2.0f);

    // Light 2
    cbData.Light2Pos[0] = std::clamp(config.light2.posX, -1.0f, 1.0f);
    cbData.Light2Pos[1] = std::clamp(config.light2.posY, -1.0f, 1.0f);
    cbData.Light2Pos[2] = std::max(0.1f, config.light2.posZ);
    cbData.Light2Pos[3] = (config.light2.enabled && config.light2.intensity > 0.0f) ? 1.0f : 0.0f;

    float l2_r, l2_g, l2_b;
    config.light2.GetEffectiveLinearColor(l2_r, l2_g, l2_b);
    cbData.Light2Color[0] = std::clamp(l2_r, 0.0f, 1.0f);
    cbData.Light2Color[1] = std::clamp(l2_g, 0.0f, 1.0f);
    cbData.Light2Color[2] = std::clamp(l2_b, 0.0f, 1.0f);
    cbData.Light2Color[3] = std::clamp(config.light2.intensity, 0.0f, 2.0f);

    // Global and Mode Parameters
    cbData.GlobalParams[0] = std::clamp(config.globalStrength, 0.0f, 2.0f);
    cbData.GlobalParams[1] = std::clamp(config.ambientIntensity, 0.0f, 2.0f);
    cbData.GlobalParams[2] = std::clamp(config.fillIntensity, 0.0f, 2.0f);
    cbData.GlobalParams[3] = config.bypass ? 1.0f : 0.0f;

    cbData.ModeParams[0] = (maskSRV != nullptr) ? 1.0f : 0.0f;
    if (config.lightingMode == LightingMode::NormalGuided) {
        if (!normalsSRV && !config.bypass) {
            return GpuLightingOutcome::NotReady;
        }
        cbData.ModeParams[1] = normalsSRV ? 1.0f : 0.0f;
    } else {
        cbData.ModeParams[1] = 0.0f; // SpatialApproximation
    }
    cbData.ModeParams[2] = config.subjectOnlyMode ? 1.0f : 0.0f;
    cbData.ModeParams[3] = hasShadows ? 1.0f : 0.0f;

    cbData.ScreenParams[0] = static_cast<float>(width);
    cbData.ScreenParams[1] = static_cast<float>(height);
    cbData.ScreenParams[2] = aspect;
    cbData.ScreenParams[3] = 0.0f;

    // Update Constant Buffer
    m_impl->m_context->UpdateSubresource(m_impl->m_constantBuffer.Get(), 0, nullptr, &cbData, 0, 0);

    // Set Shader & Resources
    m_impl->m_context->CSSetShader(m_impl->m_computeShader.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { m_impl->m_constantBuffer.Get() };
    m_impl->m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[4] = { inputSRV, maskSRV, normalsSRV, shadowSRV };
    m_impl->m_context->CSSetShaderResources(0, 4, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { outputUAV };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // Dispatch Compute Shader
    const UINT numGroupsX = (width + 15) / 16;
    const UINT numGroupsY = (height + 15) / 16;
    m_impl->m_context->Dispatch(numGroupsX, numGroupsY, 1);

    // Clean up bindings to prevent hazard on subsequent passes
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    m_impl->m_context->CSSetShaderResources(0, 4, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

    ID3D11Buffer* nullCBs[1] = { nullptr };
    m_impl->m_context->CSSetConstantBuffers(0, 1, nullCBs);

    m_impl->m_context->CSSetShader(nullptr, nullptr, 0);

    if (config.bypass) {
        return GpuLightingOutcome::Bypass;
    }
    if (config.subjectOnlyMode && !maskSRV) {
        return GpuLightingOutcome::NotReady;
    }

    return GpuLightingOutcome::Success;
}

// ----------------------------------------------------------------------------
// D3D11StudioLightingAdapter Implementation
// ----------------------------------------------------------------------------

struct D3D11StudioLightingAdapter::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::unique_ptr<D3D11StudioLighting> m_gpuOperator;
    std::unique_ptr<StudioLighting> m_cpuFallback;

    // Managed GPU textures
    ComPtr<ID3D11Texture2D> m_inputTex;
    ComPtr<ID3D11ShaderResourceView> m_inputSRV;

    ComPtr<ID3D11Texture2D> m_outputTex;
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;

    ComPtr<ID3D11Texture2D> m_maskTex;
    ComPtr<ID3D11ShaderResourceView> m_maskSRV;

    ComPtr<ID3D11Texture2D> m_normalsTex;
    ComPtr<ID3D11ShaderResourceView> m_normalsSRV;

    ComPtr<ID3D11Texture2D> m_depthTex;
    ComPtr<ID3D11ShaderResourceView> m_depthSRV;

    ComPtr<ID3D11Texture2D> m_readbackTex;

    // Timestamp query objects
    ComPtr<ID3D11Query> m_disjointQuery;
    ComPtr<ID3D11Query> m_startQuery;
    ComPtr<ID3D11Query> m_stopQuery;

    // Host staging buffers
    std::vector<uint8_t> m_uploadRgba;
    std::vector<float> m_normalsRgba;

    AVFrame* m_outputFrame = nullptr;
    int m_allocatedWidth = 0;
    int m_allocatedHeight = 0;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {
        m_gpuOperator = std::make_unique<D3D11StudioLighting>(device, context);
        m_cpuFallback = std::make_unique<StudioLighting>();
        InitQueries();
    }

    ~Impl() {
        if (m_outputFrame) {
            av_frame_free(&m_outputFrame);
        }
    }

    void InitQueries() {
        if (!m_device) return;

        D3D11_QUERY_DESC qDesc = {};
        qDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        m_device->CreateQuery(&qDesc, &m_disjointQuery);

        qDesc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&qDesc, &m_startQuery);
        m_device->CreateQuery(&qDesc, &m_stopQuery);
    }

    bool EnsureTextures(int width, int height) {
        if (m_allocatedWidth == width && m_allocatedHeight == height && m_inputTex) {
            return true;
        }

        m_inputTex.Reset();
        m_inputSRV.Reset();
        m_outputTex.Reset();
        m_outputUAV.Reset();
        m_maskTex.Reset();
        m_maskSRV.Reset();
        m_normalsTex.Reset();
        m_normalsSRV.Reset();
        m_depthTex.Reset();
        m_depthSRV.Reset();
        m_readbackTex.Reset();

        // 1. Input Texture (RGBA8)
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_inputTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_inputTex.Get(), nullptr, &m_inputSRV);
        if (FAILED(hr)) return false;

        // 2. Output Texture (RGBA8 UAV)
        texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_outputTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateUnorderedAccessView(m_outputTex.Get(), nullptr, &m_outputUAV);
        if (FAILED(hr)) return false;

        // 3. Mask Texture (R32_FLOAT)
        D3D11_TEXTURE2D_DESC maskDesc = texDesc;
        maskDesc.Format = DXGI_FORMAT_R32_FLOAT;
        maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&maskDesc, nullptr, &m_maskTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_maskTex.Get(), nullptr, &m_maskSRV);
        if (FAILED(hr)) return false;

        // 4. Normals Texture (R32G32B32A32_FLOAT)
        D3D11_TEXTURE2D_DESC normDesc = texDesc;
        normDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        normDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&normDesc, nullptr, &m_normalsTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_normalsTex.Get(), nullptr, &m_normalsSRV);
        if (FAILED(hr)) return false;

        // 4b. Depth Texture (R32_FLOAT)
        D3D11_TEXTURE2D_DESC depthDesc = texDesc;
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_depthTex.Get(), nullptr, &m_depthSRV);
        if (FAILED(hr)) return false;

        // 5. Readback Staging Texture
        D3D11_TEXTURE2D_DESC stageDesc = texDesc;
        stageDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_device->CreateTexture2D(&stageDesc, nullptr, &m_readbackTex);
        if (FAILED(hr)) return false;

        m_uploadRgba.resize(width * height * 4);
        m_normalsRgba.resize(width * height * 4);

        m_allocatedWidth = width;
        m_allocatedHeight = height;
        return true;
    }

    bool EnsureOutputFrame(int width, int height) {
        if (m_outputFrame) {
            if (m_outputFrame->width == width &&
                m_outputFrame->height == height &&
                m_outputFrame->format == AV_PIX_FMT_BGR24 &&
                av_frame_is_writable(m_outputFrame)) {
                return true;
            }
            av_frame_free(&m_outputFrame);
        }

        m_outputFrame = av_frame_alloc();
        if (!m_outputFrame) return false;

        m_outputFrame->width = width;
        m_outputFrame->height = height;
        m_outputFrame->format = AV_PIX_FMT_BGR24;

        if (av_frame_get_buffer(m_outputFrame, 32) < 0) {
            av_frame_free(&m_outputFrame);
            return false;
        }
        return true;
    }
};

D3D11StudioLightingAdapter::D3D11StudioLightingAdapter(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context)) {
}

D3D11StudioLightingAdapter::~D3D11StudioLightingAdapter() = default;

D3D11StudioLighting* D3D11StudioLightingAdapter::GetGpuOperator() {
    return m_impl->m_gpuOperator.get();
}

StudioLighting* D3D11StudioLightingAdapter::GetCpuFallback() {
    return m_impl->m_cpuFallback.get();
}

bool D3D11StudioLightingAdapter::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    if (!newDevice || !newContext) return false;
    m_impl = std::make_unique<Impl>(newDevice, newContext);
    return true;
}

AVFrame* D3D11StudioLightingAdapter::ProcessFrame(
    const AVFrame* inputFrame,
    const AlphaMask* mask,
    const NormalField* normals,
    const DepthField* depth,
    const LightingConfig& config,
    LightingOutcome& outcome,
    TimingStats* outTiming
) {
    TimingStats stats = {};
    const auto tTotalStart = std::chrono::high_resolution_clock::now();

    // Check device validity or fallback
    if (!m_impl->m_device || !m_impl->m_context || m_impl->m_gpuOperator->IsDeviceLost()) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, mask, normals, depth, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    if (!inputFrame || !inputFrame->data[0] || inputFrame->width <= 0 || inputFrame->height <= 0) {
        outcome = LightingOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        outcome = LightingOutcome::UnsupportedFormat;
        return nullptr;
    }

    const int width = inputFrame->width;
    const int height = inputFrame->height;
    const int srcLinesize = inputFrame->linesize[0];

    // Ensure GPU resources
    if (!m_impl->EnsureTextures(width, height) || !m_impl->EnsureOutputFrame(width, height)) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, mask, normals, depth, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    // 1. Staging Upload Phase
    const auto tUploadStart = std::chrono::high_resolution_clock::now();

    // Convert BGR24 input to RGBA8 staging
    uint8_t* uploadPtr = m_impl->m_uploadRgba.data();
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
        uint8_t* dstRow = uploadPtr + y * (width * 4);
        for (int x = 0; x < width; ++x) {
            dstRow[4 * x + 0] = srcRow[3 * x + 2]; // R
            dstRow[4 * x + 1] = srcRow[3 * x + 1]; // G
            dstRow[4 * x + 2] = srcRow[3 * x + 0]; // B
            dstRow[4 * x + 3] = 255;                // A
        }
    }
    m_impl->m_context->UpdateSubresource(m_impl->m_inputTex.Get(), 0, nullptr, uploadPtr, width * 4, 0);

    // Upload Mask if present
    ID3D11ShaderResourceView* activeMaskSRV = nullptr;
    if (mask && mask->data) {
        if (mask->width != width || mask->height != height) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
        m_impl->m_context->UpdateSubresource(m_impl->m_maskTex.Get(), 0, nullptr, mask->data, mask->strideFloats * sizeof(float), 0);
        activeMaskSRV = m_impl->m_maskSRV.Get();
    }

    // Upload Normals if present
    ID3D11ShaderResourceView* activeNormalsSRV = nullptr;
    if (normals && normals->data) {
        if (normals->width != width || normals->height != height) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
        float* normRgba = m_impl->m_normalsRgba.data();
        for (int y = 0; y < height; ++y) {
            const float* srcNorm = normals->data + y * normals->strideFloats;
            float* dstNorm = normRgba + y * (width * 4);
            for (int x = 0; x < width; ++x) {
                dstNorm[4 * x + 0] = srcNorm[3 * x + 0];
                dstNorm[4 * x + 1] = srcNorm[3 * x + 1];
                dstNorm[4 * x + 2] = srcNorm[3 * x + 2];
                dstNorm[4 * x + 3] = 0.0f;
            }
        }
        m_impl->m_context->UpdateSubresource(m_impl->m_normalsTex.Get(), 0, nullptr, normRgba, width * 4 * sizeof(float), 0);
        activeNormalsSRV = m_impl->m_normalsSRV.Get();
    }

    // Upload Depth if present
    ID3D11ShaderResourceView* activeDepthSRV = nullptr;
    if (depth && depth->data) {
        if (depth->width != width || depth->height != height) {
            outcome = LightingOutcome::InvalidInput;
            return nullptr;
        }
        m_impl->m_context->UpdateSubresource(m_impl->m_depthTex.Get(), 0, nullptr, depth->data, depth->strideFloats * sizeof(float), 0);
        activeDepthSRV = m_impl->m_depthSRV.Get();
    }

    const auto tUploadEnd = std::chrono::high_resolution_clock::now();
    stats.uploadMs = std::chrono::duration<double, std::milli>(tUploadEnd - tUploadStart).count();

    // 2. GPU Dispatch Phase with Timestamp Queries
    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        m_impl->m_context->Begin(m_impl->m_disjointQuery.Get());
        m_impl->m_context->End(m_impl->m_startQuery.Get());
    }

    const auto tDispatchStart = std::chrono::high_resolution_clock::now();

    GpuLightingOutcome gpuOutcome = m_impl->m_gpuOperator->Process(
        m_impl->m_inputSRV.Get(),
        m_impl->m_outputUAV.Get(),
        activeMaskSRV,
        activeNormalsSRV,
        activeDepthSRV,
        width, height,
        config
    );

    const auto tDispatchEnd = std::chrono::high_resolution_clock::now();

    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        m_impl->m_context->End(m_impl->m_stopQuery.Get());
        m_impl->m_context->End(m_impl->m_disjointQuery.Get());
    }

    if (gpuOutcome == GpuLightingOutcome::DeviceLost || gpuOutcome == GpuLightingOutcome::ShaderError) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, mask, normals, depth, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    if (gpuOutcome == GpuLightingOutcome::NotReady) {
        AVFrame* outFrame = m_impl->m_outputFrame;
        av_frame_copy_props(outFrame, inputFrame);
        outFrame->width = width;
        outFrame->height = height;
        outFrame->format = AV_PIX_FMT_BGR24;
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcRow = inputFrame->data[0] + y * srcLinesize;
            uint8_t* dstRow = outFrame->data[0] + y * outFrame->linesize[0];
            std::memcpy(dstRow, srcRow, width * 3);
        }
        outcome = LightingOutcome::NotReady;
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return outFrame;
    }

    // 3. Staging Readback Phase
    const auto tReadbackStart = std::chrono::high_resolution_clock::now();

    m_impl->m_context->CopyResource(m_impl->m_readbackTex.Get(), m_impl->m_outputTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_impl->m_context->Map(m_impl->m_readbackTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, mask, normals, depth, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    AVFrame* outFrame = m_impl->m_outputFrame;
    av_frame_copy_props(outFrame, inputFrame);
    outFrame->width = width;
    outFrame->height = height;
    outFrame->format = AV_PIX_FMT_BGR24;

    const int dstLinesize = outFrame->linesize[0];
    const uint8_t* mappedBytes = static_cast<const uint8_t*>(mapped.pData);

    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRgba = mappedBytes + y * mapped.RowPitch;
        uint8_t* dstBgr = outFrame->data[0] + y * dstLinesize;
        for (int x = 0; x < width; ++x) {
            dstBgr[3 * x + 0] = srcRgba[4 * x + 2]; // B
            dstBgr[3 * x + 1] = srcRgba[4 * x + 1]; // G
            dstBgr[3 * x + 2] = srcRgba[4 * x + 0]; // R
        }
    }

    m_impl->m_context->Unmap(m_impl->m_readbackTex.Get(), 0);

    const auto tReadbackEnd = std::chrono::high_resolution_clock::now();
    stats.readbackMs = std::chrono::duration<double, std::milli>(tReadbackEnd - tReadbackStart).count();

    // Resolve GPU timestamp queries if available
    bool queryResolved = false;
    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
        UINT64 startTime = 0, stopTime = 0;

        // Wait bounded for query results
        for (int i = 0; i < 1000; ++i) {
            if (m_impl->m_context->GetData(m_impl->m_disjointQuery.Get(), &disjointData, sizeof(disjointData), 0) == S_OK &&
                m_impl->m_context->GetData(m_impl->m_startQuery.Get(), &startTime, sizeof(startTime), 0) == S_OK &&
                m_impl->m_context->GetData(m_impl->m_stopQuery.Get(), &stopTime, sizeof(stopTime), 0) == S_OK) {
                if (!disjointData.Disjoint && disjointData.Frequency > 0) {
                    stats.dispatchGpuMs = static_cast<double>(stopTime - startTime) * 1000.0 / static_cast<double>(disjointData.Frequency);
                    queryResolved = true;
                }
                break;
            }
        }
    }

    if (!queryResolved) {
        stats.dispatchGpuMs = std::chrono::duration<double, std::milli>(tDispatchEnd - tDispatchStart).count();
    }

    const auto tTotalEnd = std::chrono::high_resolution_clock::now();
    stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();

    if (config.bypass) {
        outcome = LightingOutcome::Bypass;
    } else if (config.subjectOnlyMode && (!mask || !mask->data)) {
        outcome = LightingOutcome::NotReady;
    } else {
        outcome = LightingOutcome::Success;
    }

    if (outTiming) {
        *outTiming = stats;
    }

    return outFrame;
}

} // namespace PrismForge::Effects::Lighting::Gpu
