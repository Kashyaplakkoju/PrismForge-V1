#include "d3d11_studiostack_overlay.h"
#include "windows/src/effects/studiostack/studio_stack.h"
#include "windows/src/effects/overlays/overlay_validation.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <mutex>
using Microsoft::WRL::ComPtr;
namespace PrismForge::Effects::StudioStack::Gpu {
namespace {
const char* g_overlayShader = R"(
// -------------------------------------------------------------
// PrismForge Texture-Native Tracked Overlays & Accessories Shader
// -------------------------------------------------------------
Texture2D<float4> InputTex      : register(t0);
Texture2D<float4> HatTex        : register(t1);
Texture2D<float4> GlassesTex    : register(t2);
Texture2D<float4> FrameTex      : register(t3);
Texture2D<float>  PersonMaskTex : register(t4);
RWTexture2D<float4> OutputUav   : register(u0);

struct LayerGPUParams {
    float4 Transform;  // (posX, posY, scale, rotation)
    float4 AnchorSize; // (anchorX, anchorY, assetW, assetH)
    float4 Control;    // (enabled, opacity, alphaContract, placement)
    float4 Geometry;   // scaleY
};

cbuffer OverlayCB : register(b0)
{
    float4 Viewport;      // (width, height, 0, 0)
    LayerGPUParams Hat;
    LayerGPUParams Glasses;
    LayerGPUParams Frame;
};

[numthreads(16, 16, 1)]
void CS_CompositeOverlays(uint3 id : SV_DispatchThreadID)
{
    uint width = (uint)Viewport.x;
    uint height = (uint)Viewport.y;
    if (id.x >= width || id.y >= height)
        return;

    float4 color = InputTex[id.xy];

    // 1. Hat Layer
    if (Hat.Control.x > 0.5f) {
        float cx = Hat.Transform.x;
        float cy = Hat.Transform.y;
        float scale = Hat.Transform.z;
        float scaleY = Hat.Geometry.x;
        float angle = Hat.Transform.w;
        float ax = Hat.AnchorSize.x;
        float ay = Hat.AnchorSize.y;
        float assetW = Hat.AnchorSize.z;
        float assetH = Hat.AnchorSize.w;
        float opacity = Hat.Control.y;
        int contract = (int)Hat.Control.z;
        int placement = (int)Hat.Control.w;

        if (scale > 0.001f && opacity > 0.001f) {
            float cosA = cos(angle);
            float sinA = sin(angle);
            float invScale = 1.0f / scale;
            float m00 = cosA * invScale;
            float m01 = sinA * invScale;
            float m10 = -sinA / scaleY;
            float m11 = cosA / scaleY;

            float dx = (float)id.x - cx;
            float dy = (float)id.y - cy;
            float u = ax + (m00 * dx + m01 * dy);
            float v = ay + (m10 * dx + m11 * dy);

            if (u >= 0.0f && u < assetW - 1.0f && v >= 0.0f && v < assetH - 1.0f) {
                int u0 = (int)floor(u);
                int v0 = (int)floor(v);
                float s = u - (float)u0;
                float t = v - (float)v0;

                float4 p00 = HatTex.Load(int3(u0, v0, 0));
                float4 p10 = HatTex.Load(int3(u0 + 1, v0, 0));
                float4 p01 = HatTex.Load(int3(u0, v0 + 1, 0));
                float4 p11 = HatTex.Load(int3(u0 + 1, v0 + 1, 0));

                float4 src = (1.0f - s) * (1.0f - t) * p00 +
                             s * (1.0f - t) * p10 +
                             (1.0f - s) * t * p01 +
                             s * t * p11;

                float effAlpha = src.a * opacity;
                float maskMod = 1.0f;
                if (placement == 1) { // BehindSubject
                    float maskVal = PersonMaskTex.Load(int3(id.xy, 0));
                    maskMod = 1.0f - saturate(maskVal);
                    effAlpha *= maskMod;
                }

                if (effAlpha > 0.001f) {
                    float invAlpha = 1.0f - effAlpha;
                    if (contract == 0) { // Straight
                        color.rgb = src.rgb * effAlpha + color.rgb * invAlpha;
                    } else { // Premultiplied
                        float premulFactor = opacity * maskMod;
                        color.rgb = src.rgb * premulFactor + color.rgb * invAlpha;
                    }
                }
            }
        }
    }

    // 2. Glasses Layer
    if (Glasses.Control.x > 0.5f) {
        float cx = Glasses.Transform.x;
        float cy = Glasses.Transform.y;
        float scale = Glasses.Transform.z;
        float scaleY = Glasses.Geometry.x;
        float angle = Glasses.Transform.w;
        float ax = Glasses.AnchorSize.x;
        float ay = Glasses.AnchorSize.y;
        float assetW = Glasses.AnchorSize.z;
        float assetH = Glasses.AnchorSize.w;
        float opacity = Glasses.Control.y;
        int contract = (int)Glasses.Control.z;
        int placement = (int)Glasses.Control.w;

        if (scale > 0.001f && opacity > 0.001f) {
            float cosA = cos(angle);
            float sinA = sin(angle);
            float invScale = 1.0f / scale;
            float m00 = cosA * invScale;
            float m01 = sinA * invScale;
            float m10 = -sinA / scaleY;
            float m11 = cosA / scaleY;

            float dx = (float)id.x - cx;
            float dy = (float)id.y - cy;
            float u = ax + (m00 * dx + m01 * dy);
            float v = ay + (m10 * dx + m11 * dy);

            if (u >= 0.0f && u < assetW - 1.0f && v >= 0.0f && v < assetH - 1.0f) {
                int u0 = (int)floor(u);
                int v0 = (int)floor(v);
                float s = u - (float)u0;
                float t = v - (float)v0;

                float4 p00 = GlassesTex.Load(int3(u0, v0, 0));
                float4 p10 = GlassesTex.Load(int3(u0 + 1, v0, 0));
                float4 p01 = GlassesTex.Load(int3(u0, v0 + 1, 0));
                float4 p11 = GlassesTex.Load(int3(u0 + 1, v0 + 1, 0));

                float4 src = (1.0f - s) * (1.0f - t) * p00 +
                             s * (1.0f - t) * p10 +
                             (1.0f - s) * t * p01 +
                             s * t * p11;

                float effAlpha = src.a * opacity;
                float maskMod = 1.0f;
                if (placement == 1) { // BehindSubject
                    float maskVal = PersonMaskTex.Load(int3(id.xy, 0));
                    maskMod = 1.0f - saturate(maskVal);
                    effAlpha *= maskMod;
                }

                if (effAlpha > 0.001f) {
                    float invAlpha = 1.0f - effAlpha;
                    if (contract == 0) { // Straight
                        color.rgb = src.rgb * effAlpha + color.rgb * invAlpha;
                    } else { // Premultiplied
                        float premulFactor = opacity * maskMod;
                        color.rgb = src.rgb * premulFactor + color.rgb * invAlpha;
                    }
                }
            }
        }
    }

    // 3. Screen Frame Layer
    if (Frame.Control.x > 0.5f) {
        float assetW = Frame.AnchorSize.z;
        float assetH = Frame.AnchorSize.w;
        float opacity = Frame.Control.y;

        if (opacity > 0.001f) {
            float scaleU = (assetW - 1.0f) / max(1.0f, (float)width - 1.0f);
            float scaleV = (assetH - 1.0f) / max(1.0f, (float)height - 1.0f);

            float u = (float)id.x * scaleU;
            float v = (float)id.y * scaleV;

            int u0 = clamp((int)floor(u), 0, (int)assetW - 2);
            int v0 = clamp((int)floor(v), 0, (int)assetH - 2);
            float s = clamp(u - (float)u0, 0.0f, 1.0f);
            float t = clamp(v - (float)v0, 0.0f, 1.0f);

            float4 p00 = FrameTex.Load(int3(u0, v0, 0));
            float4 p10 = FrameTex.Load(int3(u0 + 1, v0, 0));
            float4 p01 = FrameTex.Load(int3(u0, v0 + 1, 0));
            float4 p11 = FrameTex.Load(int3(u0 + 1, v0 + 1, 0));

            float4 src = (1.0f - s) * (1.0f - t) * p00 +
                         s * (1.0f - t) * p10 +
                         (1.0f - s) * t * p01 +
                         s * t * p11;

            if (src.a > 0.0005f) {
                float effAlpha = src.a * opacity;
                float invAlpha = 1.0f - effAlpha;
                color.rgb = (Frame.Control.z < 0.5f ? src.rgb * effAlpha : src.rgb * opacity) + color.rgb * invAlpha;
            }
        }
    }

    OutputUav[id.xy] = color;
}
)";

struct LayerGPUParams { float transform[4], anchorSize[4], control[4], geometry[4]; };
struct OverlayConstantBuffer { float viewport[4]; LayerGPUParams hat, glasses, frame; };
static_assert(sizeof(OverlayConstantBuffer) == 208);
} // anonymous namespace

struct D3D11StudioStackOverlay::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> cb;
    struct Asset {
        Overlays::RgbaImageBuffer image;
        uint64_t version = 1;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::array<Asset,3> assets;
    mutable std::mutex mutex;
    GpuFaultSeam seam = GpuFaultSeam::None;
    HRESULT seamHr = E_FAIL;
    bool lost = false;
    GpuOverlayTelemetry counters;
    Overlays::LayerReport layers;
    Impl(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c) {
        for (int i=0;i<3;++i) assets[i].image = Overlays::ProceduralAssetGenerator::GetAsset(static_cast<Overlays::ProceduralAssetId>(i));
    }
    HRESULT Fault(GpuFaultSeam at) const { return seam == at ? seamHr : S_OK; }
    GpuOverlayOutcome Error(HRESULT hr, const char* at, std::string* error) {
        lost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
        if (error) *error = std::string(at)+" failed (HRESULT "+std::to_string(static_cast<unsigned long>(hr))+")";
        for (auto* status : {&layers.glasses,&layers.hat,&layers.frame}) if (*status == 0) *status=3;
        return lost ? GpuOverlayOutcome::DeviceLost : (hr == E_OUTOFMEMORY ? GpuOverlayOutcome::AllocationFailed : GpuOverlayOutcome::ShaderError);
    }
    HRESULT EnsureResources() {
        if (!shader) {
            auto hr=Fault(GpuFaultSeam::OverlayShader_Compile); if (FAILED(hr)) return hr;
            ComPtr<ID3DBlob> blob, errors; ComPtr<ID3D11ComputeShader> next;
            hr=D3DCompile(g_overlayShader,std::strlen(g_overlayShader),"PrismForgeOverlays",nullptr,nullptr,
                "CS_CompositeOverlays","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
            if (FAILED(hr)) return hr;
            hr=device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&next);
            if (FAILED(hr)) return hr; shader=next; ++counters.resourceCreations;
        }
        if (!cb) {
            auto hr=Fault(GpuFaultSeam::OverlayCB_Creation); if (FAILED(hr)) return hr;
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sizeof(OverlayConstantBuffer);
            desc.Usage=D3D11_USAGE_DYNAMIC; desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> next; hr=device->CreateBuffer(&desc,nullptr,&next);
            if (FAILED(hr)) return hr; cb=next; ++counters.resourceCreations;
        }
        for (auto& a : assets) if (!a.srv) {
            auto hr=Fault(GpuFaultSeam::OverlayAsset_Upload); if (FAILED(hr)) return hr;
            if (!a.image.IsValid() || a.image.width < 2 || a.image.height < 2) return E_INVALIDARG;
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=a.image.width; desc.Height=a.image.height;
            desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
            desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=a.image.data.data(); data.SysMemPitch=a.image.strideBytes;
            ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view;
            hr=device->CreateTexture2D(&desc,&data,&texture); if (FAILED(hr)) return hr;
            hr=Fault(GpuFaultSeam::OverlayAsset_View); if (FAILED(hr)) return hr;
            hr=device->CreateShaderResourceView(texture.Get(),nullptr,&view); if (FAILED(hr)) return hr;
            a.texture=texture; a.srv=view; ++counters.assetUploads; counters.resourceCreations+=2;
        }
        return S_OK;
    }
    bool View(ID3D11View* view, int w, int h, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& texture) {
        if (!view) return false;
        ComPtr<ID3D11Device> owner; view->GetDevice(&owner); if (owner.Get()!=device.Get()) return false;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        if (FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        if (desc.Width!=UINT(w) || desc.Height!=UINT(h) || desc.Format!=format || desc.ArraySize!=1 ||
            desc.MipLevels!=1 || desc.SampleDesc.Count!=1) return false;
        return true;
    }
};

D3D11StudioStackOverlay::D3D11StudioStackOverlay(ID3D11Device* d, ID3D11DeviceContext* c)
    : m_impl(std::make_unique<Impl>(d,c)) {}
D3D11StudioStackOverlay::~D3D11StudioStackOverlay()=default;
D3D11StudioStackOverlay::D3D11StudioStackOverlay(D3D11StudioStackOverlay&&) noexcept=default;
D3D11StudioStackOverlay& D3D11StudioStackOverlay::operator=(D3D11StudioStackOverlay&&) noexcept=default;
bool D3D11StudioStackOverlay::ResetDevice(ID3D11Device* d, ID3D11DeviceContext* c) {
    if (!d || !c) return false;
    ComPtr<ID3D11Device> owner; c->GetDevice(&owner); if (owner.Get()!=d || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    auto next=std::make_unique<Impl>(d,c);
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (int i=0;i<3;++i) { next->assets[i].image=m_impl->assets[i].image; next->assets[i].version=m_impl->assets[i].version; }
    }
    if (FAILED(next->EnsureResources())) return false;
    m_impl=std::move(next); return true;
}
bool D3D11StudioStackOverlay::IsDeviceLost() const { if (!m_impl) return true; std::lock_guard<std::mutex> lock(m_impl->mutex); return m_impl->lost; }
GpuOverlayTelemetry D3D11StudioStackOverlay::GetTelemetry() const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->mutex); auto result=m_impl->counters;
    for (const auto& a:m_impl->assets) if (a.srv) ++result.cachedAssets;
    return result;
}
Overlays::LayerReport D3D11StudioStackOverlay::GetLayerReport() const {
    if (!m_impl) return {}; std::lock_guard<std::mutex> lock(m_impl->mutex); return m_impl->layers;
}
size_t D3D11StudioStackOverlay::GetCachedAssetCount() const { return GetTelemetry().cachedAssets; }
size_t D3D11StudioStackOverlay::GetTotalDispatches() const { return GetTelemetry().dispatches; }
void D3D11StudioStackOverlay::InvalidateAssetCache() {
    if (!m_impl) return; std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto& a:m_impl->assets) { a.texture.Reset(); a.srv.Reset(); }
}
bool D3D11StudioStackOverlay::SetAsset(Overlays::ProceduralAssetId id,const Overlays::RgbaImageBuffer& image,uint64_t version) {
    const int index=static_cast<int>(id);
    if (!m_impl || index<0 || index>=3 || !version || !image.IsValid() || image.width<2 || image.height<2) return false;
    try {
        Overlays::RgbaImageBuffer owned; owned.width=image.width; owned.height=image.height;
        owned.strideBytes=image.strideBytes; owned.alphaContract=image.alphaContract;
        owned.data.assign(image.data.begin(),image.data.begin()+size_t(image.strideBytes)*image.height);
        std::lock_guard<std::mutex> lock(m_impl->mutex); auto& a=m_impl->assets[index];
        if (a.version==version && a.image.width==owned.width && a.image.height==owned.height &&
            a.image.strideBytes==owned.strideBytes && a.image.alphaContract==owned.alphaContract && a.image.data==owned.data) return true;
        a.image=std::move(owned); a.version=version; a.texture.Reset(); a.srv.Reset(); return true;
    } catch (const std::bad_alloc&) { return false; }
}
void D3D11StudioStackOverlay::SetFaultSeam(GpuFaultSeam seam,HRESULT hr) {
    if (!m_impl) return; std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->seam!=seam) {
        if (seam==GpuFaultSeam::OverlayCB_Creation) m_impl->cb.Reset();
        if (seam==GpuFaultSeam::OverlayShader_Compile) m_impl->shader.Reset();
        if (seam==GpuFaultSeam::OverlayAsset_Upload || seam==GpuFaultSeam::OverlayAsset_View)
            for (auto& a:m_impl->assets) { a.texture.Reset(); a.srv.Reset(); }
    }
    m_impl->seam=seam; m_impl->seamHr=hr;
    if (seam==GpuFaultSeam::None && m_impl->device && SUCCEEDED(m_impl->device->GetDeviceRemovedReason())) m_impl->lost=false;
}
GpuOverlayOutcome D3D11StudioStackOverlay::Process(
    ID3D11ShaderResourceView* input,ID3D11UnorderedAccessView* output,ID3D11ShaderResourceView* mask,
    int width,int height,const Presets::StudioPreset& preset,
    const Overlays::TrackedTransform& glasses,const Overlays::TrackedTransform& hat,
    const Overlays::AssetAnchorMetrics& glassesMetrics,const Overlays::AssetAnchorMetrics& hatMetrics,
    const Overlays::AssetAnchorMetrics&,std::string* error) {
    if (error) error->clear();
    if (!m_impl) return GpuOverlayOutcome::ShaderError;
    auto& p=*m_impl; std::lock_guard<std::mutex> lock(p.mutex); p.layers={};
    if (!p.device || !p.context) return p.Error(E_POINTER,"Overlay device/context",error);
    ComPtr<ID3D11Device> owner; p.context->GetDevice(&owner);
    if (owner.Get()!=p.device.Get() || p.context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return GpuOverlayOutcome::InvalidInput;
    if (p.lost) return GpuOverlayOutcome::DeviceLost;
    if (width<=0 || height<=0 || width>16384 || height>16384 || int64_t(width)*height>64LL*1024*1024 ||
        !Overlays::ValidConfiguration(preset.overlays) || !Overlays::ValidTransform(glasses) || !Overlays::ValidTransform(hat)) {
        if (error) *error="Invalid overlay dimensions/configuration/transform"; return GpuOverlayOutcome::InvalidInput;
    }
    ComPtr<ID3D11Texture2D> inTexture,outTexture,maskTexture;
    D3D11_SHADER_RESOURCE_VIEW_DESC inDesc{}; D3D11_UNORDERED_ACCESS_VIEW_DESC outDesc{};
    if (!p.View(input,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,inTexture) ||
        !p.View(output,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,outTexture) || inTexture.Get()==outTexture.Get()) {
        if (error) *error="Unsupported overlay view/device/format or input-output alias"; return GpuOverlayOutcome::InvalidInput;
    }
    input->GetDesc(&inDesc); output->GetDesc(&outDesc);
    if (inDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || inDesc.Texture2D.MostDetailedMip ||
        inDesc.Texture2D.MipLevels!=1 || inDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM ||
        outDesc.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2D || outDesc.Texture2D.MipSlice || outDesc.Format!=inDesc.Format)
        return GpuOverlayOutcome::InvalidInput;
    if (mask) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        if (!p.View(mask,width,height,DXGI_FORMAT_R32_FLOAT,maskTexture)) return GpuOverlayOutcome::InvalidInput;
        mask->GetDesc(&desc);
        if (desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format!=DXGI_FORMAT_R32_FLOAT || desc.Texture2D.MostDetailedMip || desc.Texture2D.MipLevels!=1)
            return GpuOverlayOutcome::InvalidInput;
    }
    if (preset.bypassAll || preset.overlays.bypass) return GpuOverlayOutcome::Bypass;
    auto active=[&](const Overlays::TrackedAccessoryConfig& c,const Overlays::TrackedTransform& t,int& status) {
        if (!c.enabled || c.baseOpacity<=.001f) {status=1;return false;}
        if (!t.valid || t.opacity<=.001f || (c.placement==Overlays::OverlayPlacement::BehindSubject && !mask)) {status=2;return false;}
        status=0;return true;
    };
    const bool g=active(preset.overlays.glasses,glasses,p.layers.glasses),h=active(preset.overlays.hat,hat,p.layers.hat);
    const bool f=preset.overlays.studioFrame.enabled && preset.overlays.studioFrame.opacity>.001f;
    p.layers.frame=f?0:1;
    if (!g && !h && !f) return (p.layers.glasses==2 || p.layers.hat==2)?GpuOverlayOutcome::NotReady:GpuOverlayOutcome::Bypass;
    if (p.seam==GpuFaultSeam::DeviceRemoved_Seam) return p.Error(DXGI_ERROR_DEVICE_REMOVED,"Overlay injected device removal",error);
    auto hr=p.EnsureResources(); if (FAILED(hr)) return p.Error(hr,"Overlay resource creation",error);
    OverlayConstantBuffer constants{}; constants.viewport[0]=float(width); constants.viewport[1]=float(height);
    auto fill=[&](LayerGPUParams& dst,const Overlays::TrackedTransform& t,const Overlays::AssetAnchorMetrics& m,int index,bool enabled,Overlays::OverlayPlacement placement) {
        if (!enabled) return true;
        if (!std::isfinite(m.anchorX) || !std::isfinite(m.anchorY) || std::abs(m.anchorX)>32768 || std::abs(m.anchorY)>32768) return false;
        const auto& a=p.assets[index]; dst.transform[0]=t.posX;dst.transform[1]=t.posY;dst.transform[2]=t.scaleX;dst.transform[3]=t.rotationRadians;
        dst.geometry[0]=t.scaleY;dst.anchorSize[0]=m.anchorX;dst.anchorSize[1]=m.anchorY;dst.anchorSize[2]=float(a.image.width);dst.anchorSize[3]=float(a.image.height);
        dst.control[0]=1;dst.control[1]=t.opacity;dst.control[2]=float(a.image.alphaContract);dst.control[3]=float(placement);return true;
    };
    if (!fill(constants.glasses,glasses,glassesMetrics,0,g,preset.overlays.glasses.placement) ||
        !fill(constants.hat,hat,hatMetrics,1,h,preset.overlays.hat.placement)) return GpuOverlayOutcome::InvalidInput;
    if (f) {
        constants.frame.anchorSize[2]=float(p.assets[2].image.width);constants.frame.anchorSize[3]=float(p.assets[2].image.height);
        constants.frame.control[0]=1;constants.frame.control[1]=preset.overlays.studioFrame.opacity;constants.frame.control[2]=float(p.assets[2].image.alphaContract);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{}; hr=p.Fault(GpuFaultSeam::Overlay_Map);
    if (SUCCEEDED(hr)) hr=p.context->Map(p.cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if (FAILED(hr)) return p.Error(hr,"Overlay constant-buffer map",error);
    std::memcpy(mapped.pData,&constants,sizeof(constants)); p.context->Unmap(p.cb.Get(),0);
    hr=p.Fault(GpuFaultSeam::Overlay_Dispatch); if (FAILED(hr)) return p.Error(hr,"Overlay dispatch boundary",error);
    ID3D11Buffer* buffers[]{p.cb.Get()}; ID3D11ShaderResourceView* views[]{input,p.assets[1].srv.Get(),p.assets[0].srv.Get(),p.assets[2].srv.Get(),mask};
    p.context->CSSetConstantBuffers(0,1,buffers);p.context->CSSetShaderResources(0,5,views);p.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    p.context->CSSetShader(p.shader.Get(),nullptr,0);p.context->Dispatch((width+15)/16,(height+15)/16,1);
    ID3D11ShaderResourceView* nullViews[5]{};ID3D11UnorderedAccessView* nullOutput=nullptr;ID3D11Buffer* nullBuffer=nullptr;
    p.context->CSSetShaderResources(0,5,nullViews);p.context->CSSetUnorderedAccessViews(0,1,&nullOutput,nullptr);p.context->CSSetConstantBuffers(0,1,&nullBuffer);p.context->CSSetShader(nullptr,nullptr,0);
    hr=p.device->GetDeviceRemovedReason();if (FAILED(hr)) return p.Error(hr,"Overlay device status",error);
    ++p.counters.dispatches;return GpuOverlayOutcome::Success;
}
} // namespace PrismForge::Effects::StudioStack::Gpu
