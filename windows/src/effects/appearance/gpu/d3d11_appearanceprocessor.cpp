#include "d3d11_appearanceprocessor.h"

#include <d3dcompiler.h>
#include <chrono>
#include <cstddef>
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

namespace PrismForge::Effects::Appearance::Gpu {

namespace {

const char* g_appearanceComputeShader = R"(
Texture2D<float4> InputTex          : register(t0);
Texture2D<float>  SkinMaskTex       : register(t1);
Texture2D<float>  ProtectionMaskTex : register(t2);

RWTexture2D<float4> OutputUav : register(u0);

cbuffer AppearanceCB : register(b0)
{
    float4 Params;     // (smoothingStrength, detailPreservation, sharpenStrength, bypass)
    float4 ModeParams; // (hasSkinMask, hasProtectionMask, width, height)
    float4 KernelParams; // (finite touchupBoost, boosted sigmaRangeSq2, reserved, reserved)
    float4 KernelOffsets[2]; // Seven integer-valued offsets, packed four per register
    float4 SpatialWeights[13]; // Forty-nine Gaussian weights, packed four per register
};

float CalcLuminance(float3 c)
{
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)ModeParams.z;
    uint height = (uint)ModeParams.w;
    if (id.x >= width || id.y >= height)
        return;

    float4 src = InputTex[id.xy];

    // Bypass check
    if (Params.w > 0.5f)
    {
        OutputUav[id.xy] = src;
        return;
    }

    bool hasSkinMask = ModeParams.x > 0.5f;
    bool hasProtectionMask = ModeParams.y > 0.5f;

    // Absent skin mask -> passthrough
    if (!hasSkinMask)
    {
        OutputUav[id.xy] = src;
        return;
    }

    float skinAlpha = saturate(SkinMaskTex[id.xy]);
    float protAlpha = hasProtectionMask ? saturate(ProtectionMaskTex[id.xy]) : 0.0f;

    float smoothing = clamp(Params.x, 0.0f, 1.0f);
    float detailPreserve = clamp(Params.y, 0.0f, 1.0f);
    float sharpen = clamp(Params.z, 0.0f, 1.0f);

    float boost = KernelParams.x;
    float boostedSmoothing = smoothing > 0.0f ? smoothing + boost * (1.0f - smoothing) : 0.0f;
    float effSmoothing = skinAlpha * (1.0f - protAlpha) * boostedSmoothing;
    float effSharpen = skinAlpha * (1.0f - protAlpha) * sharpen;

    if (protAlpha >= 0.999f || (effSmoothing <= 1e-4f && effSharpen <= 1e-4f))
    {
        OutputUav[id.xy] = src;
        return;
    }

    float3 c0 = src.rgb;
    float y0 = CalcLuminance(c0);

    // Keep the original constants on the zero-boost route so the unrolled
    // legacy spatial exponentials remain compiler-folded constants.
    const float legacySigmaSpatialSq2 = 2.0f * 2.5f * 2.5f;
    const float legacySigmaRangeSq2 = 2.0f * (18.0f / 255.0f) * (18.0f / 255.0f);
    float sigmaRangeSq2 = boost > 0.0f ? KernelParams.y : legacySigmaRangeSq2;

    float sumW = 0.0f;
    float3 sumC = float3(0.0f, 0.0f, 0.0f);
    float sumY = 0.0f;

    [unroll]
    for (int dy = -3; dy <= 3; ++dy)
    {
        int iy = dy + 3;
        int offsetY = boost > 0.0f ? (int)KernelOffsets[iy / 4][iy % 4] : dy;
        int ny = clamp((int)id.y + offsetY, 0, (int)height - 1);
        [unroll]
        for (int dx = -3; dx <= 3; ++dx)
        {
            int ix = dx + 3;
            int offsetX = boost > 0.0f ? (int)KernelOffsets[ix / 4][ix % 4] : dx;
            int nx = clamp((int)id.x + offsetX, 0, (int)width - 1);
            float3 nc = InputTex[uint2(nx, ny)].rgb;
            float nyLum = CalcLuminance(nc);

            float dLum = nyLum - y0;

            int weightIndex = iy * 7 + ix;
            float ws = boost > 0.0f ? SpatialWeights[weightIndex / 4][weightIndex % 4] :
                exp(-float(dx * dx + dy * dy) / legacySigmaSpatialSq2);
            float wr = exp(-(dLum * dLum) / sigmaRangeSq2);
            float neighborWeight = 1.0f;
            if (boost > 0.0f) {
                float eligible = SkinMaskTex[uint2(nx, ny)];
                eligible = isfinite(eligible) ? saturate(eligible) : 0.0f;
                float protectedValue = hasProtectionMask ? ProtectionMaskTex[uint2(nx, ny)] : 0.0f;
                protectedValue = isfinite(protectedValue) ? saturate(protectedValue) : 0.0f;
                neighborWeight = eligible * (1.0f - protectedValue);
            }
            float w = ws * wr * neighborWeight;

            sumW += w;
            sumC += w * nc;
            sumY += w * nyLum;
        }
    }

    float invW = 1.0f / max(1e-5f, sumW);
    float3 cSmooth = sumC * invW;
    float ySmooth = sumY * invW;

    float3 cDetail = c0 - cSmooth;
    float3 cRestored = cSmooth + detailPreserve * cDetail;

    float3 cBlend = lerp(c0, cRestored, effSmoothing);

    if (sharpen > 1e-4f && skinAlpha > 1e-4f)
    {
        float yBlend = CalcLuminance(cBlend);
        float hp = yBlend - ySmooth;
        float hpClamped = clamp(hp, -12.0f / 255.0f, 12.0f / 255.0f);
        float boost = sharpen * hpClamped * skinAlpha * (1.0f - 0.5f * protAlpha);
        cBlend = saturate(cBlend + boost);
    }

    OutputUav[id.xy] = float4(cBlend, src.a);
}
)";

struct alignas(16) AppearanceShaderCB {
    float Params[4];     // smoothingStrength, detailPreservation, sharpenStrength, bypass
    float ModeParams[4]; // hasSkinMask, hasProtectionMask, width, height
    float KernelParams[4]; // finite touchupBoost, boosted sigmaRangeSq2, reserved, reserved
    float KernelOffsets[2][4]; // HLSL float4[2], not an HLSL scalar array
    float SpatialWeights[13][4]; // HLSL float4[13], final three entries are padding
};
static_assert(sizeof(AppearanceShaderCB) == 288, "Appearance constant buffer size mismatch");
static_assert(alignof(AppearanceShaderCB) == 16, "Appearance constant buffer alignment mismatch");
static_assert(offsetof(AppearanceShaderCB, KernelParams) == 32, "Kernel parameters offset mismatch");
static_assert(offsetof(AppearanceShaderCB, KernelOffsets) == 48, "Kernel offsets offset mismatch");
static_assert(offsetof(AppearanceShaderCB, SpatialWeights) == 80, "Spatial weights offset mismatch");

} // anonymous namespace

struct D3D11AppearanceProcessor::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11ComputeShader> m_computeShader;
    ComPtr<ID3D11Buffer> m_constantBuffer;

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
            g_appearanceComputeShader,
            std::strlen(g_appearanceComputeShader),
            "AppearanceCS",
            nullptr,
            nullptr,
            "CSMain",
            "cs_5_0",
            flags,
            0,
            &csBlob,
            &errorBlob
        );

        if (FAILED(hr)) return false;

        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(),
            nullptr,
            &m_computeShader
        );
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(AppearanceShaderCB);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        return SUCCEEDED(hr);
    }
};

D3D11AppearanceProcessor::D3D11AppearanceProcessor(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context)) {
}

D3D11AppearanceProcessor::~D3D11AppearanceProcessor() = default;

D3D11AppearanceProcessor::D3D11AppearanceProcessor(D3D11AppearanceProcessor&&) noexcept = default;
D3D11AppearanceProcessor& D3D11AppearanceProcessor::operator=(D3D11AppearanceProcessor&&) noexcept = default;

bool D3D11AppearanceProcessor::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    if (!newDevice || !newContext) return false;
    m_impl = std::make_unique<Impl>(newDevice, newContext);
    return m_impl->m_computeShader != nullptr;
}

bool D3D11AppearanceProcessor::IsDeviceLost() const {
    if (!m_impl || !m_impl->m_device) return true;
    HRESULT hr = m_impl->m_device->GetDeviceRemovedReason();
    return FAILED(hr);
}

GpuAppearanceOutcome D3D11AppearanceProcessor::Process(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11UnorderedAccessView* outputUAV,
    ID3D11ShaderResourceView* skinMaskSRV,
    ID3D11ShaderResourceView* protectionMaskSRV,
    int width, int height,
    const AppearanceConfig& config
) {
    if (IsDeviceLost()) {
        return GpuAppearanceOutcome::DeviceLost;
    }

    if (!inputSRV || !outputUAV || width <= 0 || height <= 0) {
        return GpuAppearanceOutcome::InvalidInput;
    }

    if (!m_impl->m_computeShader || !m_impl->m_constantBuffer) {
        return GpuAppearanceOutcome::ShaderError;
    }

    AppearanceShaderCB cbData = {};
    cbData.Params[0] = std::isfinite(config.smoothingStrength)
        ? std::clamp(config.smoothingStrength, 0.0f, 1.0f) : 0.0f;
    cbData.Params[1] = std::isfinite(config.detailPreservation)
        ? std::clamp(config.detailPreservation, 0.0f, 1.0f) : 1.0f;
    cbData.Params[2] = std::isfinite(config.sharpenStrength)
        ? std::clamp(config.sharpenStrength, 0.0f, 1.0f) : 0.0f;
    cbData.Params[3] = config.bypass ? 1.0f : 0.0f;

    cbData.ModeParams[0] = (skinMaskSRV != nullptr) ? 1.0f : 0.0f;
    cbData.ModeParams[1] = (protectionMaskSRV != nullptr) ? 1.0f : 0.0f;
    cbData.ModeParams[2] = static_cast<float>(width);
    cbData.ModeParams[3] = static_cast<float>(height);
    cbData.KernelParams[0] = std::isfinite(config.touchupBoost)
        ? std::clamp(config.touchupBoost, 0.0f, 1.0f) : 0.0f;
    const float boost = cbData.KernelParams[0];
    if (boost > 0.0f) {
        // Geometry and spatial weights depend only on the frame configuration.
        // Prepare them once here instead of evaluating forty-nine spatial
        // exponentials for every eligible face pixel in the compute shader.
        const float rangeSigma = (18.0f + 18.0f * boost) / 255.0f;
        cbData.KernelParams[1] = 2.0f * rangeSigma * rangeSigma;
        const float spatialSigma = 2.5f + 3.5f * boost;
        const float spatialSigmaSq2 = 2.0f * spatialSigma * spatialSigma;
        const int wideOffsets[7] = {-9, -5, -2, 0, 2, 5, 9};
        int offsets[7];
        for (int i = 0; i < 7; ++i) {
            const int legacyOffset = i - 3;
            const float magnitude = static_cast<float>(std::abs(legacyOffset)) + boost *
                static_cast<float>(std::abs(wideOffsets[i]) - std::abs(legacyOffset));
            offsets[i] = (i < 3 ? -1 : 1) * static_cast<int>(magnitude + 0.5f);
            cbData.KernelOffsets[i / 4][i % 4] = static_cast<float>(offsets[i]);
        }
        for (int iy = 0; iy < 7; ++iy) {
            for (int ix = 0; ix < 7; ++ix) {
                const int index = iy * 7 + ix;
                cbData.SpatialWeights[index / 4][index % 4] = std::exp(
                    -static_cast<float>(offsets[ix] * offsets[ix] + offsets[iy] * offsets[iy]) /
                    spatialSigmaSq2);
            }
        }
    }

    m_impl->m_context->UpdateSubresource(m_impl->m_constantBuffer.Get(), 0, nullptr, &cbData, 0, 0);

    m_impl->m_context->CSSetShader(m_impl->m_computeShader.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { m_impl->m_constantBuffer.Get() };
    m_impl->m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[3] = { inputSRV, skinMaskSRV, protectionMaskSRV };
    m_impl->m_context->CSSetShaderResources(0, 3, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { outputUAV };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    const UINT numGroupsX = (width + 15) / 16;
    const UINT numGroupsY = (height + 15) / 16;
    m_impl->m_context->Dispatch(numGroupsX, numGroupsY, 1);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    m_impl->m_context->CSSetShaderResources(0, 3, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_impl->m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

    ID3D11Buffer* nullCBs[1] = { nullptr };
    m_impl->m_context->CSSetConstantBuffers(0, 1, nullCBs);

    m_impl->m_context->CSSetShader(nullptr, nullptr, 0);

    if (config.bypass) {
        return GpuAppearanceOutcome::Bypass;
    }
    if (!skinMaskSRV) {
        return GpuAppearanceOutcome::NotReady;
    }

    return GpuAppearanceOutcome::Success;
}

// ----------------------------------------------------------------------------
// D3D11AppearanceProcessorAdapter Implementation
// ----------------------------------------------------------------------------

struct D3D11AppearanceProcessorAdapter::Impl {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::unique_ptr<D3D11AppearanceProcessor> m_gpuOperator;
    std::unique_ptr<AppearanceProcessor> m_cpuFallback;

    ComPtr<ID3D11Texture2D> m_inputTex;
    ComPtr<ID3D11ShaderResourceView> m_inputSRV;

    ComPtr<ID3D11Texture2D> m_outputTex;
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;

    ComPtr<ID3D11Texture2D> m_skinMaskTex;
    ComPtr<ID3D11ShaderResourceView> m_skinMaskSRV;

    ComPtr<ID3D11Texture2D> m_protectionMaskTex;
    ComPtr<ID3D11ShaderResourceView> m_protectionMaskSRV;

    ComPtr<ID3D11Texture2D> m_readbackTex;

    ComPtr<ID3D11Query> m_disjointQuery;
    ComPtr<ID3D11Query> m_startQuery;
    ComPtr<ID3D11Query> m_stopQuery;

    std::vector<uint8_t> m_uploadRgba;
    AVFrame* m_outputFrame = nullptr;
    int m_allocatedWidth = 0;
    int m_allocatedHeight = 0;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {
        m_gpuOperator = std::make_unique<D3D11AppearanceProcessor>(device, context);
        m_cpuFallback = std::make_unique<AppearanceProcessor>();
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
        m_skinMaskTex.Reset();
        m_skinMaskSRV.Reset();
        m_protectionMaskTex.Reset();
        m_protectionMaskSRV.Reset();
        m_readbackTex.Reset();

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

        texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_outputTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateUnorderedAccessView(m_outputTex.Get(), nullptr, &m_outputUAV);
        if (FAILED(hr)) return false;

        D3D11_TEXTURE2D_DESC maskDesc = texDesc;
        maskDesc.Format = DXGI_FORMAT_R32_FLOAT;
        maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&maskDesc, nullptr, &m_skinMaskTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_skinMaskTex.Get(), nullptr, &m_skinMaskSRV);
        if (FAILED(hr)) return false;

        hr = m_device->CreateTexture2D(&maskDesc, nullptr, &m_protectionMaskTex);
        if (FAILED(hr)) return false;
        hr = m_device->CreateShaderResourceView(m_protectionMaskTex.Get(), nullptr, &m_protectionMaskSRV);
        if (FAILED(hr)) return false;

        D3D11_TEXTURE2D_DESC stageDesc = texDesc;
        stageDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_device->CreateTexture2D(&stageDesc, nullptr, &m_readbackTex);
        if (FAILED(hr)) return false;

        m_uploadRgba.resize(width * height * 4);
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

D3D11AppearanceProcessorAdapter::D3D11AppearanceProcessorAdapter(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_impl(std::make_unique<Impl>(device, context)) {
}

D3D11AppearanceProcessorAdapter::~D3D11AppearanceProcessorAdapter() = default;

D3D11AppearanceProcessor* D3D11AppearanceProcessorAdapter::GetGpuOperator() {
    return m_impl->m_gpuOperator.get();
}

AppearanceProcessor* D3D11AppearanceProcessorAdapter::GetCpuFallback() {
    return m_impl->m_cpuFallback.get();
}

bool D3D11AppearanceProcessorAdapter::ResetDevice(ID3D11Device* newDevice, ID3D11DeviceContext* newContext) {
    if (!newDevice || !newContext) return false;
    m_impl = std::make_unique<Impl>(newDevice, newContext);
    return true;
}

AVFrame* D3D11AppearanceProcessorAdapter::ProcessFrame(
    const AVFrame* inputFrame,
    const SkinMask* skinMask,
    const ProtectionMask* protectionMask,
    const AppearanceConfig& config,
    AppearanceOutcome& outcome,
    TimingStats* outTiming
) {
    TimingStats stats = {};
    const auto tTotalStart = std::chrono::high_resolution_clock::now();

    if (!m_impl->m_device || !m_impl->m_context || m_impl->m_gpuOperator->IsDeviceLost()) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, skinMask, protectionMask, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    if (!inputFrame || !inputFrame->data[0] || inputFrame->width <= 0 || inputFrame->height <= 0) {
        outcome = AppearanceOutcome::InvalidInput;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        outcome = AppearanceOutcome::UnsupportedFormat;
        return nullptr;
    }

    const int width = inputFrame->width;
    const int height = inputFrame->height;
    const int srcLinesize = inputFrame->linesize[0];

    if (!m_impl->EnsureTextures(width, height) || !m_impl->EnsureOutputFrame(width, height)) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, skinMask, protectionMask, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    // 1. Upload Staging
    const auto tUploadStart = std::chrono::high_resolution_clock::now();
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

    ID3D11ShaderResourceView* activeSkinSRV = nullptr;
    if (skinMask && skinMask->data) {
        if (skinMask->width != width || skinMask->height != height) {
            outcome = AppearanceOutcome::InvalidInput;
            return nullptr;
        }
        m_impl->m_context->UpdateSubresource(m_impl->m_skinMaskTex.Get(), 0, nullptr, skinMask->data, skinMask->strideFloats * sizeof(float), 0);
        activeSkinSRV = m_impl->m_skinMaskSRV.Get();
    }

    ID3D11ShaderResourceView* activeProtSRV = nullptr;
    if (protectionMask && protectionMask->data) {
        if (protectionMask->width != width || protectionMask->height != height) {
            outcome = AppearanceOutcome::InvalidInput;
            return nullptr;
        }
        m_impl->m_context->UpdateSubresource(m_impl->m_protectionMaskTex.Get(), 0, nullptr, protectionMask->data, protectionMask->strideFloats * sizeof(float), 0);
        activeProtSRV = m_impl->m_protectionMaskSRV.Get();
    }

    const auto tUploadEnd = std::chrono::high_resolution_clock::now();
    stats.uploadMs = std::chrono::duration<double, std::milli>(tUploadEnd - tUploadStart).count();

    // 2. Dispatch
    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        m_impl->m_context->Begin(m_impl->m_disjointQuery.Get());
        m_impl->m_context->End(m_impl->m_startQuery.Get());
    }

    const auto tDispatchStart = std::chrono::high_resolution_clock::now();

    GpuAppearanceOutcome gpuOutcome = m_impl->m_gpuOperator->Process(
        m_impl->m_inputSRV.Get(),
        m_impl->m_outputUAV.Get(),
        activeSkinSRV,
        activeProtSRV,
        width, height,
        config
    );

    const auto tDispatchEnd = std::chrono::high_resolution_clock::now();

    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        m_impl->m_context->End(m_impl->m_stopQuery.Get());
        m_impl->m_context->End(m_impl->m_disjointQuery.Get());
    }

    if (gpuOutcome == GpuAppearanceOutcome::DeviceLost || gpuOutcome == GpuAppearanceOutcome::ShaderError) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, skinMask, protectionMask, config, outcome);
        const auto tTotalEnd = std::chrono::high_resolution_clock::now();
        stats.totalRoundtripMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (outTiming) *outTiming = stats;
        return res;
    }

    // 3. Readback
    const auto tReadbackStart = std::chrono::high_resolution_clock::now();
    m_impl->m_context->CopyResource(m_impl->m_readbackTex.Get(), m_impl->m_outputTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_impl->m_context->Map(m_impl->m_readbackTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stats.fallbackUsed = true;
        AVFrame* res = m_impl->m_cpuFallback->ProcessFrame(inputFrame, skinMask, protectionMask, config, outcome);
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

    bool queryResolved = false;
    if (m_impl->m_disjointQuery && m_impl->m_startQuery && m_impl->m_stopQuery) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
        UINT64 startTime = 0, stopTime = 0;
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
        outcome = AppearanceOutcome::Bypass;
    } else if (!skinMask || !skinMask->data) {
        outcome = AppearanceOutcome::NotReady;
    } else {
        outcome = AppearanceOutcome::Success;
    }

    if (outTiming) {
        *outTiming = stats;
    }

    return outFrame;
}

} // namespace PrismForge::Effects::Appearance::Gpu
