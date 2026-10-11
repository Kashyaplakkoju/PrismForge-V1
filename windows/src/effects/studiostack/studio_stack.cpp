#include "studio_stack.h"
#include "windows/src/effects/gpu/d3d11_context.h"
#include "windows/src/effects/appearance/gpu/d3d11_appearanceprocessor.h"
#include "windows/src/effects/lighting/gpu/d3d11_studiolighting.h"
#include "windows/src/effects/studiostack/gpu/d3d11_studiostack_background.h"
#include "windows/src/effects/studiostack/gpu/d3d11_studiostack_lut.h"
#include "windows/src/effects/studiostack/gpu/d3d11_studiostack_overlay.h"
#include "windows/src/effects/studiostack/gpu/d3d11_face_accessories.h"
#include "windows/src/effects/studiostack/gpu/d3d11_desktop_looks.h"
#include "windows/src/effects/analysisbridge/gpu/d3d11_mask_preparer.h"
#include "windows/src/effects/analysisbridge/gpu/d3d11_normal_preparer.h"
#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/relighting/shadow_depth_preparer.h"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <array>
#include <cmath>
#include <iomanip>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace PrismForge::Effects::StudioStack {

using Microsoft::WRL::ComPtr;

// Exact face geometry cannot borrow coverage or normals from the general mailbox.
static bool ExactLightingGeometryReady(
    const AnalysisBridge::AnalysisBundle* geometry,
    const StackExecutionReport& report,
    bool requireDepth)
{
    if (!geometry || geometry->sourceGeneration == 0 || geometry->sequenceId == 0 ||
        geometry->timestampPts < 0 || geometry->sourceGeneration != report.sourceGeneration ||
        geometry->sequenceId != report.frameSequence || geometry->timestampPts != report.timestampPts ||
        !geometry->HasNormals() || !geometry->personMask.IsValid()) return false;
    const auto& mask = geometry->personMask;
    const auto& normals = geometry->normalMap;
    if (mask.width != normals.width || mask.height != normals.height) return false;
    const bool hasDepth = geometry->depthMap.IsValid();
    if ((requireDepth && !hasDepth) || (hasDepth && !geometry->HasDepth())) return false;
    if (hasDepth && (geometry->depthMap.width != mask.width || geometry->depthMap.height != mask.height)) return false;
    if (requireDepth && (geometry->depthMap.format != 0 ||
        !std::isfinite(geometry->depthMap.depthScale) || geometry->depthMap.depthScale <= 0.0f)) return false;
    bool covered = false;
    for (int y = 0; y < mask.height; ++y) {
        const auto* m = mask.data.data() + static_cast<size_t>(y) * mask.strideFloats;
        const auto* n = normals.data.data() + static_cast<size_t>(y) * normals.strideFloats;
        for (int x = 0; x < mask.width; ++x) {
            if (!std::isfinite(m[x]) || m[x] < 0.0f || m[x] > 1.0f) return false;
            if (m[x] <= 0.0f) continue;
            covered = true;
            const auto* v = n + static_cast<size_t>(x) * 3;
            const double norm2 = static_cast<double>(v[0]) * v[0] +
                static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2];
            if (!std::isfinite(norm2) || norm2 <= 1.0e-6) return false;
            if (hasDepth) {
                const auto index = static_cast<size_t>(y) * geometry->depthMap.strideFloats + x;
                const float d = geometry->depthMap.data[index];
                if (!std::isfinite(d) || d < 0.0f || d > 1.0f) return false;
                if (!geometry->depthMap.confidence.empty()) {
                    const float support = geometry->depthMap.confidence[index];
                    if (!std::isfinite(support) || support < (requireDepth ? 0.5f : 0.0f) || support <= 0.0f || support > 1.0f) return false;
                }
            }
        }
    }
    return covered;
}

// Explicit facial appearance requires matching owned surface and feature masks.
// It never interprets body segmentation as skin or accepts an older mailbox result.
static bool ExactAppearanceGeometryReady(const AnalysisBridge::AnalysisBundle* geometry,
    const StackExecutionReport& report) {
    if(!geometry||!geometry->sourceGeneration||!geometry->sequenceId||geometry->timestampPts<0||
        geometry->sourceGeneration!=report.sourceGeneration||geometry->sequenceId!=report.frameSequence||
        geometry->timestampPts!=report.timestampPts||!geometry->HasSkinMask()||!geometry->HasProtectionMask()||
        !geometry->HasPersonMask())return false;
    const auto& skin=geometry->skinMask;const auto& protect=geometry->protectionMask;const auto& face=geometry->personMask;
    if(skin.width>2048||skin.height>2048||size_t(skin.width)*skin.height>4*1024*1024||
        skin.width!=protect.width||skin.height!=protect.height||skin.width!=face.width||skin.height!=face.height)return false;
    bool eligible=false;
    for(int y=0;y<skin.height;++y)for(int x=0;x<skin.width;++x) {
        const float a=skin.data[size_t(y)*skin.strideFloats+x],p=protect.data[size_t(y)*protect.strideFloats+x],
            c=face.data[size_t(y)*face.strideFloats+x];
        if(!std::isfinite(a)||!std::isfinite(p)||!std::isfinite(c)||a<0||a>1||p<0||p>1||c<0||c>1||
            a>c||(p>=1.f&&a>0.f))return false;
        eligible=eligible||a>0.f;
    }
    return eligible;
}

// -------------------------------------------------------------
// CachedFrameScaler Implementation
// -------------------------------------------------------------

CachedFrameScaler::CachedFrameScaler() = default;

CachedFrameScaler::~CachedFrameScaler() {
    Reset();
}

void CachedFrameScaler::Reset() {
    for (auto& entry : m_cache) {
        if (entry.sws) {
            sws_freeContext(entry.sws);
            entry.sws = nullptr;
        }
    }
    m_cache.clear();

    if (m_reusableDst) {
        av_frame_free(&m_reusableDst);
        m_reusableDst = nullptr;
    }
}

AVFrame* CachedFrameScaler::Scale(const AVFrame* src, int targetW, int targetH) {
    if (!src || !src->data[0] || targetW <= 0 || targetH <= 0) {
        return nullptr;
    }

    const int srcW = src->width;
    const int srcH = src->height;
    const int srcFmt = src->format;
    const int dstFmt = AV_PIX_FMT_BGR24;

    SwsContext* sws = nullptr;
    for (const auto& entry : m_cache) {
        if (entry.srcW == srcW && entry.srcH == srcH && entry.srcFmt == srcFmt &&
            entry.dstW == targetW && entry.dstH == targetH && entry.dstFmt == dstFmt) {
            sws = entry.sws;
            break;
        }
    }

    if (!sws) {
        sws = sws_getContext(
            srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
            targetW, targetH, static_cast<AVPixelFormat>(dstFmt),
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return nullptr;

        Entry newEntry;
        newEntry.srcW = srcW; newEntry.srcH = srcH; newEntry.srcFmt = srcFmt;
        newEntry.dstW = targetW; newEntry.dstH = targetH; newEntry.dstFmt = dstFmt;
        newEntry.sws = sws;
        m_cache.push_back(newEntry);
    }

    AVFrame* dst = av_frame_alloc();
    if (!dst) return nullptr;

    dst->width = targetW;
    dst->height = targetH;
    dst->format = dstFmt;

    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    av_frame_copy_props(dst, src);

    sws_scale(
        sws,
        src->data, src->linesize, 0, srcH,
        dst->data, dst->linesize);

    return dst;
}

// -------------------------------------------------------------
// RAII GpuTextureSurface & OutputLeaseCore Implementation
// -------------------------------------------------------------

static std::string HrToHex(HRESULT hr) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << static_cast<uint32_t>(hr);
    return ss.str();
}

GpuTextureSurface::GpuTextureSurface() noexcept = default;

GpuTextureSurface::~GpuTextureSurface() noexcept {
    Release();
}

GpuTextureSurface::GpuTextureSurface(const GpuTextureSurface& other) noexcept
    : texture(other.texture),
      srv(other.srv),
      textureRef(other.textureRef),
      srvRef(other.srvRef),
      width(other.width),
      height(other.height),
      format(other.format),
      isValid(other.isValid),
      leaseId(other.leaseId),
      leaseCore(other.leaseCore)
{
}

GpuTextureSurface& GpuTextureSurface::operator=(const GpuTextureSurface& other) noexcept {
    if (this != &other) {
        Release();
        texture = other.texture;
        srv = other.srv;
        textureRef = other.textureRef;
        srvRef = other.srvRef;
        width = other.width;
        height = other.height;
        format = other.format;
        isValid = other.isValid;
        leaseId = other.leaseId;
        leaseCore = other.leaseCore;
    }
    return *this;
}

GpuTextureSurface::GpuTextureSurface(GpuTextureSurface&& other) noexcept
    : texture(other.texture),
      srv(other.srv),
      textureRef(std::move(other.textureRef)),
      srvRef(std::move(other.srvRef)),
      width(other.width),
      height(other.height),
      format(other.format),
      isValid(other.isValid),
      leaseId(other.leaseId),
      leaseCore(std::move(other.leaseCore))
{
    other.texture = nullptr;
    other.srv = nullptr;
    other.width = 0;
    other.height = 0;
    other.isValid = false;
    other.leaseId = 0;
}

GpuTextureSurface& GpuTextureSurface::operator=(GpuTextureSurface&& other) noexcept {
    if (this != &other) {
        Release();
        texture = other.texture;
        srv = other.srv;
        textureRef = std::move(other.textureRef);
        srvRef = std::move(other.srvRef);
        width = other.width;
        height = other.height;
        format = other.format;
        isValid = other.isValid;
        leaseId = other.leaseId;
        leaseCore = std::move(other.leaseCore);

        other.texture = nullptr;
        other.srv = nullptr;
        other.width = 0;
        other.height = 0;
        other.isValid = false;
        other.leaseId = 0;
    }
    return *this;
}

void GpuTextureSurface::Release() noexcept {
    texture = nullptr;
    srv = nullptr;
    textureRef.Reset();
    srvRef.Reset();
    width = 0;
    height = 0;
    isValid = false;
    leaseId = 0;
    leaseCore.reset();
}

// -------------------------------------------------------------
// OutputPoolCoordinator: Bounded Output Lease Arena across Epochs
// -------------------------------------------------------------

class OutputPoolCoordinator : public std::enable_shared_from_this<OutputPoolCoordinator> {
public:
    static constexpr size_t kMaxCapacity = 4;

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0;
        int height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool isLeased = false;
        uint64_t leaseId = 0;
        uint64_t epochId = 0;
    };

    enum class AcquireStatus {
        Success,
        Exhausted,
        AllocationFailed,
        ViewFailed,
        DeviceLost
    };

    struct AcquireOutcome {
        AcquireStatus status = AcquireStatus::Success;
        std::shared_ptr<OutputLeaseCore> leaseCore;
        std::string errorMessage;
        HRESULT hr = S_OK;
    };

    OutputPoolCoordinator() = default;
    ~OutputPoolCoordinator() {
        Reset();
    }

    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_device && m_device.Get() != device) {
            RetireEpochLocked();
        }
        m_device = device;
        m_context = context;
    }

    void RetireEpoch() {
        std::lock_guard<std::mutex> lock(m_mutex);
        RetireEpochLocked();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < kMaxCapacity; ++i) {
            m_slots[i].texture.Reset();
            m_slots[i].srv.Reset();
            m_slots[i].isLeased = false;
            m_slots[i].leaseId = 0;
            m_slots[i].width = 0;
            m_slots[i].height = 0;
        }
        m_device.Reset();
        m_context.Reset();
    }

    AcquireOutcome AcquireSlot(
        int width, int height, DXGI_FORMAT format,
        GpuFaultSeam seam, HRESULT seamHr)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AcquireOutcome outcome;

        if (!m_device) {
            outcome.status = AcquireStatus::DeviceLost;
            outcome.errorMessage = "Output pool coordinator has no active D3D11 device";
            outcome.hr = E_POINTER;
            return outcome;
        }

        // Hard capacity bound: live active leases across ALL epochs (active + retired) must not exceed 4
        if (m_activeLeases >= kMaxCapacity) {
            outcome.status = AcquireStatus::Exhausted;
            outcome.errorMessage = "Output surface pool exhausted (all 4 slots leased across active/retired epochs). Backpressure failure.";
            outcome.hr = E_FAIL;
            return outcome;
        }

        bool isOutputSeam = (seam == GpuFaultSeam::OutputTex_Creation ||
                             seam == GpuFaultSeam::OutputSrv_Creation ||
                             seam == GpuFaultSeam::DeviceRemoved_Seam);

        // 1. Look for existing unleased slot matching dimensions & format in current epoch (if no output fault seam)
        if (!isOutputSeam) {
            for (size_t i = 0; i < kMaxCapacity; ++i) {
                size_t idx = (m_nextSlotIndex + i) % kMaxCapacity;
                auto& slot = m_slots[idx];
                if (!slot.isLeased && slot.texture && slot.width == width && slot.height == height && slot.format == format && slot.epochId == m_currentEpoch) {
                    m_nextSlotIndex = (idx + 1) % kMaxCapacity;
                    slot.isLeased = true;
                    slot.leaseId = m_nextLeaseId++;
                    m_activeLeases++;

                    auto core = std::make_shared<OutputLeaseCore>();
                    core->coordinator = shared_from_this();
                    core->texture = slot.texture;
                    core->srv = slot.srv;
                    core->width = width;
                    core->height = height;
                    core->format = format;
                    core->leaseId = slot.leaseId;
                    core->epochId = m_currentEpoch;
                    core->slotIndex = idx;

                    outcome.status = AcquireStatus::Success;
                    outcome.leaseCore = std::move(core);
                    return outcome;
                }
            }
        }

        // 2. Look for existing unleased slot in current epoch to allocate or repurpose
        for (size_t i = 0; i < kMaxCapacity; ++i) {
            size_t idx = (m_nextSlotIndex + i) % kMaxCapacity;
            auto& slot = m_slots[idx];
            if (!slot.isLeased) {
                m_nextSlotIndex = (idx + 1) % kMaxCapacity;
                slot.texture.Reset();
                slot.srv.Reset();
                slot.width = 0;
                slot.height = 0;
                slot.format = format;
                slot.epochId = m_currentEpoch;

                // Fault seam check: output texture creation
                if (seam == GpuFaultSeam::OutputTex_Creation || seam == GpuFaultSeam::DeviceRemoved_Seam) {
                    HRESULT hrFault = (seam == GpuFaultSeam::DeviceRemoved_Seam) ? DXGI_ERROR_DEVICE_REMOVED : seamHr;
                    outcome.status = (hrFault == DXGI_ERROR_DEVICE_REMOVED || hrFault == DXGI_ERROR_DEVICE_RESET)
                        ? AcquireStatus::DeviceLost : AcquireStatus::AllocationFailed;
                    outcome.hr = hrFault;
                    outcome.errorMessage = "Simulated fault at output texture allocation boundary (HRESULT 0x" + HrToHex(hrFault) + ")";
                    return outcome;
                }

                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                // Only D3D11_BIND_SHADER_RESOURCE (never UAV so SRGB formats succeed)
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, slot.texture.GetAddressOf());
                if (FAILED(hr)) {
                    slot.texture.Reset();
                    outcome.status = (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                        ? AcquireStatus::DeviceLost : AcquireStatus::AllocationFailed;
                    outcome.hr = hr;
                    outcome.errorMessage = "Failed to create output texture 2D (HRESULT 0x" + HrToHex(hr) + ")";
                    return outcome;
                }

                // Fault seam check: output SRV creation
                if (seam == GpuFaultSeam::OutputSrv_Creation || seam == GpuFaultSeam::DeviceRemoved_Seam) {
                    slot.texture.Reset();
                    HRESULT hrFault = (seam == GpuFaultSeam::DeviceRemoved_Seam) ? DXGI_ERROR_DEVICE_REMOVED : seamHr;
                    outcome.status = (hrFault == DXGI_ERROR_DEVICE_REMOVED || hrFault == DXGI_ERROR_DEVICE_RESET)
                        ? AcquireStatus::DeviceLost : AcquireStatus::ViewFailed;
                    outcome.hr = hrFault;
                    outcome.errorMessage = "Simulated fault at output SRV creation boundary (HRESULT 0x" + HrToHex(hrFault) + ")";
                    return outcome;
                }

                hr = m_device->CreateShaderResourceView(slot.texture.Get(), nullptr, slot.srv.GetAddressOf());
                if (FAILED(hr)) {
                    slot.texture.Reset();
                    slot.srv.Reset();
                    outcome.status = (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                        ? AcquireStatus::DeviceLost : AcquireStatus::ViewFailed;
                    outcome.hr = hr;
                    outcome.errorMessage = "Failed to create output shader resource view (HRESULT 0x" + HrToHex(hr) + ")";
                    return outcome;
                }

                slot.width = width;
                slot.height = height;
                slot.isLeased = true;
                slot.leaseId = m_nextLeaseId++;

                m_cumulativeAllocations++;
                m_activeLeases++;

                auto core = std::make_shared<OutputLeaseCore>();
                core->coordinator = shared_from_this();
                core->texture = slot.texture;
                core->srv = slot.srv;
                core->width = width;
                core->height = height;
                core->format = format;
                core->leaseId = slot.leaseId;
                core->epochId = m_currentEpoch;
                core->slotIndex = idx;

                outcome.status = AcquireStatus::Success;
                outcome.leaseCore = std::move(core);
                return outcome;
            }
        }

        outcome.status = AcquireStatus::Exhausted;
        outcome.errorMessage = "Output surface pool exhausted";
        return outcome;
    }

    void ReleaseLease(uint64_t epochId, size_t slotIndex, uint64_t leaseId) noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (leaseId == 0) return;

        if (epochId == m_currentEpoch) {
            if (slotIndex < kMaxCapacity) {
                auto& slot = m_slots[slotIndex];
                if (slot.isLeased && slot.leaseId == leaseId && slot.epochId == epochId) {
                    slot.isLeased = false;
                    slot.leaseId = 0;
                    if (m_activeLeases > 0) m_activeLeases--;
                    return;
                }
            }
            for (size_t i = 0; i < kMaxCapacity; ++i) {
                auto& slot = m_slots[i];
                if (slot.isLeased && slot.leaseId == leaseId && slot.epochId == epochId) {
                    slot.isLeased = false;
                    slot.leaseId = 0;
                    if (m_activeLeases > 0) m_activeLeases--;
                    return;
                }
            }
        } else {
            for (auto it = m_retiredLeases.begin(); it != m_retiredLeases.end(); ++it) {
                if (it->epochId == epochId && it->leaseId == leaseId) {
                    m_retiredLeases.erase(it);
                    if (m_activeLeases > 0) m_activeLeases--;
                    return;
                }
            }
        }
    }

    size_t GetActiveLeaseCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_activeLeases;
    }

    size_t GetTotalTextureAllocations() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cumulativeAllocations;
    }

    size_t GetLiveOutputAllocations() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = 0;
        for (size_t i = 0; i < kMaxCapacity; ++i) {
            if (m_slots[i].texture != nullptr) {
                count++;
            }
        }
        count += m_retiredLeases.size();
        return count;
    }

    size_t GetRetiredEpochCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_retiredEpochCount;
    }

    static constexpr size_t GetMaxLeaseCapacity() {
        return kMaxCapacity;
    }

private:
    void RetireEpochLocked() {
        for (size_t i = 0; i < kMaxCapacity; ++i) {
            auto& slot = m_slots[i];
            if (slot.isLeased) {
                m_retiredLeases.push_back({ slot.epochId, slot.leaseId });
            }
            slot.texture.Reset();
            slot.srv.Reset();
            slot.width = 0;
            slot.height = 0;
            slot.isLeased = false;
            slot.leaseId = 0;
        }
        m_retiredEpochCount++;
        m_currentEpoch++;
        m_nextSlotIndex = 0;
        m_device.Reset();
        m_context.Reset();
    }

    mutable std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    uint64_t m_currentEpoch = 1;
    uint64_t m_nextLeaseId = 1;
    size_t m_retiredEpochCount = 0;

    size_t m_activeLeases = 0;
    size_t m_cumulativeAllocations = 0;

    Slot m_slots[kMaxCapacity];
    size_t m_nextSlotIndex = 0;

    struct RetiredLease {
        uint64_t epochId = 0;
        uint64_t leaseId = 0;
    };
    std::vector<RetiredLease> m_retiredLeases;
};

OutputLeaseCore::~OutputLeaseCore() noexcept {
    Release();
}

void OutputLeaseCore::Release() noexcept {
    bool expected = false;
    if (isReleased.compare_exchange_strong(expected, true)) {
        if (coordinator) {
            coordinator->ReleaseLease(epochId, slotIndex, leaseId);
        }
        texture.Reset();
        srv.Reset();
        coordinator.reset();
    }
}

// -------------------------------------------------------------
// GpuResources Internal Implementation
// -------------------------------------------------------------

struct StudioStack::GpuResources {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::unique_ptr<PrismForge::Effects::Gpu::D3D11Context> ownedContext;

    std::unique_ptr<Gpu::D3D11DesktopLooks> desktopLooksProcessor;
    std::unique_ptr<Appearance::Gpu::D3D11AppearanceProcessor> appearanceProcessor;
    std::unique_ptr<Lighting::Gpu::D3D11StudioLighting> lightingEngine;
    std::unique_ptr<Gpu::D3D11StudioStackBackground> backgroundCompositor;
    std::unique_ptr<Gpu::D3D11StudioStackLut> lutProcessor;
    std::unique_ptr<Gpu::D3D11StudioStackLut> inputLutProcessor;
    std::unique_ptr<Gpu::D3D11StudioStackOverlay> overlayRenderer;
    std::unique_ptr<Gpu::D3D11FaceAccessories> faceAccessories;

    int width = 0;
    int height = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texA;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvA;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uavA;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texB;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvB;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uavB;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> uploadStaging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> readbackStaging;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> skinMaskTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> skinMaskSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> protMaskTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> protMaskSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> personMaskTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> personMaskSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV;

    AnalysisBridge::Gpu::D3D11MaskPreparer gpuMaskPreparerPerson;
    AnalysisBridge::Gpu::D3D11MaskPreparer gpuMaskPreparerFace;
    AnalysisBridge::Gpu::D3D11MaskPreparer gpuMaskPreparerSkin;
    AnalysisBridge::Gpu::D3D11MaskPreparer gpuMaskPreparerProt;
    AnalysisBridge::Gpu::D3D11NormalPreparer gpuNormalPreparer;

    bool EnsureTextures(int w, int h, GpuFaultSeam seam, HRESULT seamHr, std::string& outError, bool& isDeviceLost) {
        bool isCreationSeam = (seam == GpuFaultSeam::TexA_Creation ||
                               seam == GpuFaultSeam::SrvA_Creation ||
                               seam == GpuFaultSeam::UavA_Creation ||
                               seam == GpuFaultSeam::TexB_Creation ||
                               seam == GpuFaultSeam::SrvB_Creation ||
                               seam == GpuFaultSeam::UavB_Creation ||
                               seam == GpuFaultSeam::UploadStaging_Creation ||
                               seam == GpuFaultSeam::ReadbackStaging_Creation ||
                               seam == GpuFaultSeam::SkinMaskTex_Creation ||
                               seam == GpuFaultSeam::SkinMaskSrv_Creation ||
                               seam == GpuFaultSeam::ProtMaskTex_Creation ||
                               seam == GpuFaultSeam::ProtMaskSrv_Creation ||
                               seam == GpuFaultSeam::PersonMaskTex_Creation ||
                               seam == GpuFaultSeam::PersonMaskSrv_Creation ||
                               seam == GpuFaultSeam::DepthTex_Creation ||
                               seam == GpuFaultSeam::DeviceRemoved_Seam);

        if (!isCreationSeam && width == w && height == h && texA && srvA && uavA && texB && srvB && uavB &&
            uploadStaging && readbackStaging && skinMaskTex && skinMaskSRV &&
            protMaskTex && protMaskSRV && personMaskTex && personMaskSRV && depthTex && depthSRV) {
            return true;
        }

        ReleaseTextures();

        auto checkSeam = [&](GpuFaultSeam target) -> HRESULT {
            if (seam == target || seam == GpuFaultSeam::DeviceRemoved_Seam) {
                return (seam == GpuFaultSeam::DeviceRemoved_Seam) ? DXGI_ERROR_DEVICE_REMOVED : seamHr;
            }
            return S_OK;
        };

        auto checkHr = [&](HRESULT hr, const char* name) -> bool {
            if (FAILED(hr)) {
                outError = std::string("Resource creation failure at ") + name + " (HRESULT 0x" + HrToHex(hr) + ")";
                isDeviceLost = (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET);
                ReleaseTextures();
                return false;
            }
            return true;
        };

        // VRAM Texture A (RGBA)
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = checkSeam(GpuFaultSeam::TexA_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, texA.GetAddressOf());
        if (!checkHr(hr, "texA")) return false;

        hr = checkSeam(GpuFaultSeam::SrvA_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texA.Get(), nullptr, srvA.GetAddressOf());
        if (!checkHr(hr, "srvA")) return false;

        hr = checkSeam(GpuFaultSeam::UavA_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(texA.Get(), nullptr, uavA.GetAddressOf());
        if (!checkHr(hr, "uavA")) return false;

        // VRAM Texture B (RGBA)
        hr = checkSeam(GpuFaultSeam::TexB_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, texB.GetAddressOf());
        if (!checkHr(hr, "texB")) return false;

        hr = checkSeam(GpuFaultSeam::SrvB_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texB.Get(), nullptr, srvB.GetAddressOf());
        if (!checkHr(hr, "srvB")) return false;

        hr = checkSeam(GpuFaultSeam::UavB_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(texB.Get(), nullptr, uavB.GetAddressOf());
        if (!checkHr(hr, "uavB")) return false;

        // Dynamic Upload Staging
        D3D11_TEXTURE2D_DESC upDesc = desc;
        upDesc.Usage = D3D11_USAGE_DYNAMIC;
        upDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        upDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = checkSeam(GpuFaultSeam::UploadStaging_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&upDesc, nullptr, uploadStaging.GetAddressOf());
        if (!checkHr(hr, "uploadStaging")) return false;

        // Staging Readback
        D3D11_TEXTURE2D_DESC rbDesc = desc;
        rbDesc.Usage = D3D11_USAGE_STAGING;
        rbDesc.BindFlags = 0;
        rbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = checkSeam(GpuFaultSeam::ReadbackStaging_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&rbDesc, nullptr, readbackStaging.GetAddressOf());
        if (!checkHr(hr, "readbackStaging")) return false;

        // Masks (R32_FLOAT)
        D3D11_TEXTURE2D_DESC maskDesc = desc;
        maskDesc.Format = DXGI_FORMAT_R32_FLOAT;
        maskDesc.Usage = D3D11_USAGE_DYNAMIC;
        maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        maskDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = checkSeam(GpuFaultSeam::SkinMaskTex_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&maskDesc, nullptr, skinMaskTex.GetAddressOf());
        if (!checkHr(hr, "skinMaskTex")) return false;

        hr = checkSeam(GpuFaultSeam::SkinMaskSrv_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(skinMaskTex.Get(), nullptr, skinMaskSRV.GetAddressOf());
        if (!checkHr(hr, "skinMaskSRV")) return false;

        hr = checkSeam(GpuFaultSeam::ProtMaskTex_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&maskDesc, nullptr, protMaskTex.GetAddressOf());
        if (!checkHr(hr, "protMaskTex")) return false;

        hr = checkSeam(GpuFaultSeam::ProtMaskSrv_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(protMaskTex.Get(), nullptr, protMaskSRV.GetAddressOf());
        if (!checkHr(hr, "protMaskSRV")) return false;

        hr = checkSeam(GpuFaultSeam::PersonMaskTex_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&maskDesc, nullptr, personMaskTex.GetAddressOf());
        if (!checkHr(hr, "personMaskTex")) return false;

        hr = checkSeam(GpuFaultSeam::PersonMaskSrv_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(personMaskTex.Get(), nullptr, personMaskSRV.GetAddressOf());
        if (!checkHr(hr, "personMaskSRV")) return false;

        // Depth Texture (R32_FLOAT)
        D3D11_TEXTURE2D_DESC depthDesc = desc;
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.Usage = D3D11_USAGE_DYNAMIC;
        depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        depthDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = checkSeam(GpuFaultSeam::DepthTex_Creation);
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&depthDesc, nullptr, depthTex.GetAddressOf());
        if (!checkHr(hr, "depthTex")) return false;

        hr = device->CreateShaderResourceView(depthTex.Get(), nullptr, depthSRV.GetAddressOf());
        if (!checkHr(hr, "depthSRV")) return false;

        width = w;
        height = h;
        return true;
    }

    void ReleaseTextures() {
        texA.Reset(); srvA.Reset(); uavA.Reset();
        texB.Reset(); srvB.Reset(); uavB.Reset();
        uploadStaging.Reset();
        readbackStaging.Reset();
        skinMaskTex.Reset(); skinMaskSRV.Reset();
        protMaskTex.Reset(); protMaskSRV.Reset();
        personMaskTex.Reset(); personMaskSRV.Reset();
        depthTex.Reset(); depthSRV.Reset();
        gpuMaskPreparerPerson.ReleaseResources();
        gpuMaskPreparerFace.ReleaseResources();
        gpuMaskPreparerSkin.ReleaseResources();
        gpuMaskPreparerProt.ReleaseResources();
        gpuNormalPreparer.ReleaseResources();
        width = 0;
        height = 0;
    }

    void Release() {
        ReleaseTextures();
        desktopLooksProcessor.reset();
        appearanceProcessor.reset();
        lightingEngine.reset();
        backgroundCompositor.reset();
        lutProcessor.reset();
        inputLutProcessor.reset();
        overlayRenderer.reset();
        faceAccessories.reset();
        context.Reset();
        device.Reset();
        ownedContext.reset();
    }
};

// -------------------------------------------------------------
// StudioStack Construction & Lifecycle
// -------------------------------------------------------------

StudioStack::StudioStack(FallbackPolicy policy)
    : m_fallbackPolicy(policy),
      m_currentPreset(Presets::StudioPreset::Neutral()),
      m_poolCoordinator(std::make_shared<OutputPoolCoordinator>())
{
}

StudioStack::~StudioStack() {
    ReleaseGpu();
    if (m_readbackFrame) {
        av_frame_free(&m_readbackFrame);
        m_readbackFrame = nullptr;
    }
    if (m_cpuBgFrame) {
        av_frame_free(&m_cpuBgFrame);
        m_cpuBgFrame = nullptr;
    }
    if (m_cpuLookScratchFrame) av_frame_free(&m_cpuLookScratchFrame);
    if (m_cpuOutputFrame) av_frame_free(&m_cpuOutputFrame);
    if (m_cpuBgOutputFrame) {
        av_frame_free(&m_cpuBgOutputFrame);
        m_cpuBgOutputFrame = nullptr;
    }
}

void StudioStack::SetGpuFailureInjection(bool inject) {
    m_gpuFailureInjected.store(inject);
    m_gpuFailureTarget.store(inject ? GpuFailureTarget::AnyGpu : GpuFailureTarget::None);
}

bool StudioStack::IsGpuFailureInjected() const {
    return m_gpuFailureInjected.load() || (m_gpuFailureTarget.load() != GpuFailureTarget::None);
}

void StudioStack::SetGpuFailureTarget(GpuFailureTarget target) {
    m_gpuFailureTarget.store(target);
    m_gpuFailureInjected.store(target == GpuFailureTarget::AnyGpu);
}

GpuFailureTarget StudioStack::GetGpuFailureTarget() const {
    return m_gpuFailureTarget.load();
}

void StudioStack::SetSourceGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);
    m_sourceGeneration = generation;
    if (m_analysisMailbox) {
        m_analysisMailbox->ResetSource(m_sourceGeneration);
    }
    m_scratchSkin.data.clear();
    m_scratchProt.data.clear();
    m_scratchPerson.data.clear();
    m_scratchFace.data.clear();
    m_scratchNormal.data.clear();
    m_maskPreparerPerson.Reset();
    m_maskPreparerFace.Reset();
    m_maskPreparerSkin.Reset();
    m_maskPreparerProt.Reset();
    m_normalPreparer.Reset();
    if (m_gpu) {
        m_gpu->gpuMaskPreparerPerson.Reset();
        m_gpu->gpuMaskPreparerFace.Reset();
        m_gpu->gpuMaskPreparerSkin.Reset();
        m_gpu->gpuMaskPreparerProt.Reset();
        m_gpu->gpuNormalPreparer.Reset();
    }
    m_lastFrameW = 0;
    m_lastFrameH = 0;
    m_lastTimestampPts = -1;
    m_overlayRenderer.Reset();
    m_faceAccessoryRenderer.Reset();
    m_trackingEvaluator.Reset();
    m_scaler.Reset();
}

uint64_t StudioStack::GetSourceGeneration() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_sourceGeneration;
}

void StudioStack::ResetSource() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);
    m_sourceGeneration++;
    if (m_analysisMailbox) {
        m_analysisMailbox->ResetSource(m_sourceGeneration);
    }
    if (m_gpu) {
        m_gpu->ReleaseTextures();
        m_gpu->gpuMaskPreparerPerson.Reset();
        m_gpu->gpuMaskPreparerFace.Reset();
        m_gpu->gpuMaskPreparerSkin.Reset();
        m_gpu->gpuMaskPreparerProt.Reset();
        m_gpu->gpuNormalPreparer.Reset();
    }
    m_scratchSkin.data.clear();
    m_scratchProt.data.clear();
    m_scratchPerson.data.clear();
    m_scratchFace.data.clear();
    m_scratchNormal.data.clear();
    m_maskPreparerPerson.Reset();
    m_maskPreparerFace.Reset();
    m_maskPreparerSkin.Reset();
    m_maskPreparerProt.Reset();
    m_normalPreparer.Reset();
    m_lastFrameW = 0;
    m_lastFrameH = 0;
    m_lastTimestampPts = -1;
    m_overlayRenderer.Reset();
    m_faceAccessoryRenderer.Reset();
    m_trackingEvaluator.Reset();
    m_scaler.Reset();
}

void StudioStack::SetFallbackPolicy(FallbackPolicy policy) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_fallbackPolicy = policy;
}

FallbackPolicy StudioStack::GetFallbackPolicy() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_fallbackPolicy;
}

void StudioStack::SetExecutionBackend(ExecutionBackend backend) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_backend = backend;
}

ExecutionBackend StudioStack::GetExecutionBackend() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_backend;
}

bool StudioStack::InitializeGpu(ID3D11Device* device, ID3D11DeviceContext* context) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);
    auto gpu = std::make_unique<GpuResources>();

    if (device && context) {
        gpu->device = device;
        gpu->context = context;
    } else {
        gpu->ownedContext = std::make_unique<PrismForge::Effects::Gpu::D3D11Context>();
        if (!gpu->ownedContext->Initialize(true)) {
            return false;
        }
        gpu->device = gpu->ownedContext->GetDevice();
        gpu->context = gpu->ownedContext->GetContext();
    }

    if (!gpu->device || !gpu->context) {
        return false;
    }

    gpu->desktopLooksProcessor = std::make_unique<Gpu::D3D11DesktopLooks>(
        gpu->device.Get(), gpu->context.Get());
    gpu->appearanceProcessor = std::make_unique<Appearance::Gpu::D3D11AppearanceProcessor>(
        gpu->device.Get(), gpu->context.Get());
    gpu->lightingEngine = std::make_unique<Lighting::Gpu::D3D11StudioLighting>(
        gpu->device.Get(), gpu->context.Get());
    gpu->backgroundCompositor = std::make_unique<Gpu::D3D11StudioStackBackground>(
        gpu->device.Get(), gpu->context.Get());
    gpu->inputLutProcessor = std::make_unique<Gpu::D3D11StudioStackLut>(gpu->device.Get(),gpu->context.Get());
    gpu->lutProcessor = std::make_unique<Gpu::D3D11StudioStackLut>(
        gpu->device.Get(), gpu->context.Get());
    gpu->overlayRenderer = std::make_unique<Gpu::D3D11StudioStackOverlay>(
        gpu->device.Get(), gpu->context.Get());
    gpu->faceAccessories = std::make_unique<Gpu::D3D11FaceAccessories>(gpu->device.Get(), gpu->context.Get());

    if (!m_poolCoordinator) {
        m_poolCoordinator = std::make_shared<OutputPoolCoordinator>();
    }
    m_poolCoordinator->SetDevice(gpu->device.Get(), gpu->context.Get());

    m_gpu = std::move(gpu);
    m_backend = ExecutionBackend::GpuWithCpuFallback;
    return true;
}

void StudioStack::ReleaseGpu() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);
    if (m_poolCoordinator) {
        m_poolCoordinator->RetireEpoch();
    }
    if (m_gpu) {
        m_gpu->Release();
        m_gpu.reset();
    }
    m_bgSRV.Reset();
    m_bgTexture.Reset();
    m_backend = ExecutionBackend::CpuOnly;
}

void StudioStack::SetPreset(const Presets::StudioPreset& preset) {
    // In-memory settings need not have a persisted preset identity; file imports do.
    auto checked=preset;
    if (checked.presetId.empty()) checked.presetId="runtime.settings";
    std::string validation; if(!checked.Validate(validation)) return;
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_currentPreset = preset;
}

Presets::StudioPreset StudioStack::GetPreset() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_currentPreset;
}

static std::unique_ptr<CubeLut> LoadLutFromFileInternal(const std::string& filePath, std::string& outError) {
    std::ifstream file(std::filesystem::u8path(filePath), std::ios::in | std::ios::binary);
    if (!file) {
        outError = "Could not open LUT file: " + filePath;
        return nullptr;
    }
    std::string text(CubeLut::MaxPayloadBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = file.gcount();
    if (file.bad() || count < 0 || static_cast<size_t>(count) > CubeLut::MaxPayloadBytes) {
        outError = "LUT read failed or exceeds 16 MiB";
        return nullptr;
    }
    text.resize(static_cast<size_t>(count));
    return CubeLut::Parse(text, &outError);
}

bool StudioStack::ApplyPreset(
    const Presets::StudioPreset& preset,
    const std::string& packLibraryRoot,
    std::string& outError)
{
    // 1. Strict validation of all numerical bounds and finite floats
    if (!preset.Validate(outError)) {
        return false;
    }

    // Pack effects consume the immutable verified snapshot, never a live path.
    std::unique_ptr<Packs::LoadedPack> referencedPack;
    if (!preset.packIdReference.empty()) {
        if (packLibraryRoot.empty()) {
            outError = "A pack library is required for the referenced preset";
            return false;
        }
        if (!Packs::PackValidator::ValidateIdentifier(preset.packIdReference, false, outError)
            || (!preset.packVersionReference.empty()
                && !Packs::PackValidator::ValidateIdentifier(preset.packVersionReference, true, outError))) return false;
        std::string selectedVersion;
        for (const auto& pack : Packs::PackManager::EnumeratePacks(packLibraryRoot)) {
            if (pack.packId != preset.packIdReference) continue;
            if (!preset.packVersionReference.empty() && pack.version != preset.packVersionReference) continue;
            if (!selectedVersion.empty()) {
                outError = "Several versions of the referenced pack are installed; choose an exact version";
                return false;
            }
            selectedVersion = pack.version;
        }
        if (selectedVersion.empty()) {
            outError = "Referenced pack/version is unavailable: " + preset.packIdReference;
            return false;
        }
        const auto directory = std::filesystem::u8path(packLibraryRoot)
            / std::filesystem::u8path(preset.packIdReference) / std::filesystem::u8path(selectedVersion);
        referencedPack = Packs::PackManager::LoadPack(directory.u8string(), outError);
        if (!referencedPack) return false;
        const auto* verified = referencedPack->GetVerifiedManifest();
        if (!verified || verified->packId != preset.packIdReference || verified->version != selectedVersion) {
            outError = "Referenced pack identity changed before its verified bytes were loaded";
            return false;
        }
    }
    auto resolveLut = [&](const std::string& reference, std::shared_ptr<const CubeLut>& output) {
        if (reference.empty()) return true;
        if (!referencedPack) {
            output = LoadLutFromFileInternal(reference, outError);
            return bool(output);
        }
        const auto* descriptor = referencedPack->GetAssetDescriptor(reference);
        if (!descriptor) {
            const auto* manifest = referencedPack->GetVerifiedManifest();
            if (manifest) for (const auto& asset : manifest->assets) {
                if (asset.relativePath == reference) { descriptor = &asset; break; }
            }
        }
        if (!descriptor || descriptor->assetType != Packs::PackAssetType::CubeLut) {
            outError = "Referenced LUT must be a declared CubeLut asset: " + reference;
            return false;
        }
        const auto bytes = referencedPack->ReadAssetBytes(descriptor->assetId);
        if (bytes.empty() || bytes.size() > CubeLut::MaxPayloadBytes) {
            outError = "Verified LUT bytes are unavailable or oversized: " + reference;
            return false;
        }
        output = CubeLut::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &outError);
        return bool(output);
    };
    std::shared_ptr<const CubeLut> resolvedLut, resolvedInputLut;
    if (!resolveLut(preset.selectedLutPath, resolvedLut)
        || !resolveLut(preset.selectedInputLutPath, resolvedInputLut)) return false;

    // 3. Atomically apply configuration only after full verification
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_currentPreset = preset;
        m_inputLut=std::move(resolvedInputLut);
        if (resolvedLut) {
            m_activeLut = std::move(resolvedLut);
        } else if (preset.selectedLutPath.empty()) {
            m_activeLut.reset();
        }
    }

    return true;
}

bool StudioStack::LoadLutFromBuffer(const std::string& cubeLutText, std::string& outError) {
    auto lut = CubeLut::Parse(cubeLutText, &outError);
    if (!lut) return false;
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_activeLut = std::move(lut);
    return true;
}

bool StudioStack::LoadLutFromFile(const std::string& cubeLutFilePath, std::string& outError) {
    auto lut = LoadLutFromFileInternal(cubeLutFilePath, outError);
    if (!lut) return false;
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_activeLut = std::move(lut);
    return true;
}

bool StudioStack::LoadInputLutFromBuffer(const std::string& text,std::string& error) {
    auto lut=CubeLut::Parse(text,&error);if(!lut)return false;
    std::lock_guard<std::mutex> lock(m_configMutex);m_inputLut=std::move(lut);return true;
}
bool StudioStack::LoadInputLutFromFile(const std::string& path,std::string& error) {
    auto lut=LoadLutFromFileInternal(path,error);if(!lut)return false;
    std::lock_guard<std::mutex> lock(m_configMutex);m_inputLut=std::move(lut);return true;
}
void StudioStack::SetInputLut(std::shared_ptr<const CubeLut> lut) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_inputLut = std::move(lut);
}
void StudioStack::ClearInputLut() { SetInputLut(nullptr); }

void StudioStack::AttachAnalysisMailbox(std::shared_ptr<AnalysisBridge::AnalysisMailbox> mailbox) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_analysisMailbox = std::move(mailbox);
    if (m_analysisMailbox) {
        m_analysisMailbox->ResetSource(m_sourceGeneration);
    }
}

void StudioStack::RegisterConsumer(const std::string& name, FrameConsumerCallback callback) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_consumers[name] = std::move(callback);
}

void StudioStack::UnregisterConsumer(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_consumers.erase(name);
}

void StudioStack::ReleaseOutputSurface(GpuTextureSurface& surface) {
    surface.Release();
}

size_t StudioStack::GetTotalTextureAllocations() const {
    if (!m_poolCoordinator) return 0;
    return m_poolCoordinator->GetTotalTextureAllocations();
}

size_t StudioStack::GetActiveLeaseCount() const {
    if (!m_poolCoordinator) return 0;
    return m_poolCoordinator->GetActiveLeaseCount();
}

size_t StudioStack::GetLiveOutputAllocations() const {
    if (!m_poolCoordinator) return 0;
    return m_poolCoordinator->GetLiveOutputAllocations();
}

size_t StudioStack::GetRetiredEpochCount() const {
    if (!m_poolCoordinator) return 0;
    return m_poolCoordinator->GetRetiredEpochCount();
}

size_t StudioStack::GetMaxLeaseCapacity() const {
    return OutputPoolCoordinator::GetMaxLeaseCapacity();
}

void StudioStack::SetGpuFaultSeam(GpuFaultSeam seam, HRESULT customHr) {
    m_gpuFaultSeam.store(seam);
    m_gpuFaultHResult.store(customHr);
}

GpuFaultSeam StudioStack::GetGpuFaultSeam() const {
    return m_gpuFaultSeam.load();
}

HRESULT StudioStack::GetGpuFaultHResult() const {
    return m_gpuFaultHResult.load();
}

void StudioStack::SetMaskPreparationFault(bool fail) {
    m_maskPrepFaultInjected.store(fail);
}

bool StudioStack::IsMaskPreparationFault() const {
    return m_maskPrepFaultInjected.load();
}

Overlays::TrackedTransform StudioStack::GetTrackedTransform(Overlays::AnchorMode mode) const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_trackingEvaluator.GetTrackedTransform(mode);
}

size_t StudioStack::GetOverlayDispatchCount() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (m_gpu && m_gpu->overlayRenderer) {
        return m_gpu->overlayRenderer->GetTotalDispatches();
    }
    return 0;
}

size_t StudioStack::GetOverlayCachedAssetCount() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (m_gpu && m_gpu->overlayRenderer) {
        return m_gpu->overlayRenderer->GetCachedAssetCount();
    }
    return 0;
}

Gpu::GpuOverlayTelemetry StudioStack::GetOverlayTelemetry() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_gpu && m_gpu->overlayRenderer ? m_gpu->overlayRenderer->GetTelemetry() : Gpu::GpuOverlayTelemetry{};
}

void StudioStack::SetStageProfilingEnabled(bool enabled) {
    m_stageProfilingEnabled.store(enabled);
}

bool StudioStack::IsStageProfilingEnabled() const {
    return m_stageProfilingEnabled.load();
}

void StudioStack::GetBackgroundImageDimensions(int& outW, int& outH) const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    outW = m_bgWidth;
    outH = m_bgHeight;
}

bool StudioStack::SetBackgroundImage(
    ID3D11ShaderResourceView* gpuSRV,
    const AVFrame* cpuFrame,
    Presets::BackgroundFitMode fitMode,
    std::string* outError)
{
    auto setError = [&](const std::string& err) {
        if (outError) *outError = err;
    };

    // Lock both configuration and execution mutexes BEFORE reading or touching ANY GPU/resource state
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);

    if (!gpuSRV && !cpuFrame) {
        setError("Neither GPU SRV nor CPU frame was provided");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> validatedOwnedTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> validatedOwnedSRV;
    int bgW = 0;
    int bgH = 0;

    // 1. Validate GPU SRV if provided
    if (gpuSRV) {
        if (!m_gpu || !m_gpu->device || !m_gpu->context) {
            setError("GPU resources not initialized for SetBackgroundImage with GPU SRV");
            return false;
        }

        if (m_gpuFaultSeam.load() == GpuFaultSeam::ImageUpload_View) {
            setError("Simulated fault at image upload view boundary");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        gpuSRV->GetResource(res.GetAddressOf());
        if (!res) {
            setError("Invalid background SRV: null underlying resource");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> srvDev;
        res->GetDevice(srvDev.GetAddressOf());
        if (srvDev.Get() != m_gpu->device.Get()) {
            setError("Background SRV belongs to a different D3D11 device");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        gpuSRV->GetDesc(&srvDesc);
        if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
            setError("Unsupported background SRV view dimension (only Texture2D supported)");
            return false;
        }
        if (srvDesc.Texture2D.MostDetailedMip != 0) {
            setError("Background SRV must target mip level 0");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTex;
        HRESULT hr = res.As(&inputTex);
        if (FAILED(hr) || !inputTex) {
            setError("Background SRV resource is not a 2D texture");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        inputTex->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) {
            setError("Background texture has zero dimensions");
            return false;
        }
        if (desc.Width > 8192 || desc.Height > 8192) {
            setError("Background texture dimensions exceed 8192x8192 maximum");
            return false;
        }
        if (desc.ArraySize != 1) {
            setError("Texture arrays are not supported for background images");
            return false;
        }
        if (desc.SampleDesc.Count > 1) {
            setError("Multisampled MSAA textures are not supported for background images");
            return false;
        }

        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            setError("Unsupported background texture format (only RGBA8/BGRA8 supported)");
            return false;
        }

        bgW = static_cast<int>(desc.Width);
        bgH = static_cast<int>(desc.Height);

        if (m_gpuFaultSeam.load() == GpuFaultSeam::ImageUpload_Creation) {
            setError("Simulated fault at image upload creation boundary");
            return false;
        }

        // Snapshot into owned texture storage for immutable mid-frame safety
        D3D11_TEXTURE2D_DESC ownedDesc = desc;
        ownedDesc.Usage = D3D11_USAGE_DEFAULT;
        ownedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ownedDesc.CPUAccessFlags = 0;
        ownedDesc.MiscFlags = 0;

        hr = m_gpu->device->CreateTexture2D(&ownedDesc, nullptr, validatedOwnedTex.GetAddressOf());
        if (FAILED(hr)) {
            setError("Failed to create owned background texture snapshot");
            return false;
        }

        m_gpu->context->CopyResource(validatedOwnedTex.Get(), inputTex.Get());

        hr = m_gpu->device->CreateShaderResourceView(validatedOwnedTex.Get(), nullptr, validatedOwnedSRV.GetAddressOf());
        if (FAILED(hr)) {
            setError("Failed to create SRV for owned background texture snapshot");
            return false;
        }
    }

    // 2. Validate and convert CPU frame if provided
    AVFrame* deepCopyFrame = nullptr;
    if (cpuFrame) {
        if (cpuFrame->width <= 0 || cpuFrame->height <= 0) {
            setError("Invalid CPU background frame dimensions (width/height <= 0)");
            return false;
        }
        if (cpuFrame->width > 8192 || cpuFrame->height > 8192) {
            setError("CPU background frame dimensions exceed 8192x8192 maximum");
            return false;
        }
        int64_t totalPixels = static_cast<int64_t>(cpuFrame->width) * static_cast<int64_t>(cpuFrame->height);
        if (totalPixels > 8192LL * 8192LL) {
            setError("CPU background frame pixel count exceeds limit");
            return false;
        }

        int fmt = cpuFrame->format;
        if (fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_YUV420P10BE ||
            fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_P010BE ||
            fmt == AV_PIX_FMT_GRAYF32LE || fmt == AV_PIX_FMT_GRAYF32BE)
        {
            setError("HDR pixel formats are not supported for background images");
            return false;
        }

        // Reject HDR transfer characteristics in SDR pipeline
        if (cpuFrame->color_trc == AVCOL_TRC_SMPTE2084 || cpuFrame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            setError("Unsupported HDR transfer characteristics in SDR background image");
            return false;
        }

        if (fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_CUDA ||
            fmt == AV_PIX_FMT_VAAPI || fmt == AV_PIX_FMT_DXVA2_VLD)
        {
            setError("Hardware pixel formats are not supported for background images");
            return false;
        }

        if (fmt != AV_PIX_FMT_BGR24 && fmt != AV_PIX_FMT_RGB24 &&
            fmt != AV_PIX_FMT_RGBA && fmt != AV_PIX_FMT_BGRA &&
            fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_NV12)
        {
            setError("Unsupported pixel format for background image");
            return false;
        }

        if (fmt == AV_PIX_FMT_BGR24 || fmt == AV_PIX_FMT_RGB24) {
            if (!cpuFrame->data[0] || cpuFrame->linesize[0] < cpuFrame->width * 3) {
                setError("Invalid line stride or missing data buffer for RGB/BGR background image");
                return false;
            }
        } else if (fmt == AV_PIX_FMT_RGBA || fmt == AV_PIX_FMT_BGRA) {
            if (!cpuFrame->data[0] || cpuFrame->linesize[0] < cpuFrame->width * 4) {
                setError("Invalid line stride or missing data buffer for RGBA/BGRA background image");
                return false;
            }
        } else if (fmt == AV_PIX_FMT_YUV420P) {
            if (!cpuFrame->data[0] || !cpuFrame->data[1] || !cpuFrame->data[2] ||
                cpuFrame->linesize[0] < cpuFrame->width ||
                cpuFrame->linesize[1] < cpuFrame->width / 2 ||
                cpuFrame->linesize[2] < cpuFrame->width / 2)
            {
                setError("Missing or truncated chroma plane in YUV420P background image");
                return false;
            }
        } else if (fmt == AV_PIX_FMT_NV12) {
            if (!cpuFrame->data[0] || !cpuFrame->data[1] ||
                cpuFrame->linesize[0] < cpuFrame->width ||
                cpuFrame->linesize[1] < cpuFrame->width)
            {
                setError("Missing or truncated UV plane in NV12 background image");
                return false;
            }
        }

        if (gpuSRV && (cpuFrame->width != bgW || cpuFrame->height != bgH)) {
            setError("GPU and CPU background image dimensions do not match");
            return false;
        }

        if (bgW == 0) {
            bgW = cpuFrame->width;
            bgH = cpuFrame->height;
        }

        deepCopyFrame = av_frame_alloc();
        if (!deepCopyFrame) {
            setError("Failed to allocate AVFrame for CPU background");
            return false;
        }
        deepCopyFrame->width = cpuFrame->width;
        deepCopyFrame->height = cpuFrame->height;
        deepCopyFrame->format = AV_PIX_FMT_BGR24;

        if (av_frame_get_buffer(deepCopyFrame, 32) < 0) {
            av_frame_free(&deepCopyFrame);
            setError("Failed to allocate buffer for CPU background image");
            return false;
        }

        av_frame_copy_props(deepCopyFrame, cpuFrame);
        deepCopyFrame->color_range = AVCOL_RANGE_JPEG; // SDR Full-range BGR

        if (cpuFrame->format == AV_PIX_FMT_BGR24 && cpuFrame->linesize[0] == deepCopyFrame->linesize[0]) {
            std::memcpy(deepCopyFrame->data[0], cpuFrame->data[0], deepCopyFrame->linesize[0] * deepCopyFrame->height);
        } else {
            SwsContext* sws = sws_getContext(
                cpuFrame->width, cpuFrame->height, static_cast<AVPixelFormat>(cpuFrame->format),
                cpuFrame->width, cpuFrame->height, AV_PIX_FMT_BGR24,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                av_frame_free(&deepCopyFrame);
                setError("Failed to create SwsContext for CPU background conversion");
                return false;
            }
            int scaledH = sws_scale(
                sws,
                cpuFrame->data, cpuFrame->linesize, 0, cpuFrame->height,
                deepCopyFrame->data, deepCopyFrame->linesize);
            sws_freeContext(sws);

            if (scaledH != cpuFrame->height) {
                av_frame_free(&deepCopyFrame);
                setError("SwsContext scaling failed to process all image rows");
                return false;
            }
        }

        // If GPU is initialized and no GPU SRV was provided, synthesize owned GPU texture from CPU frame
        if (!gpuSRV && m_gpu && m_gpu->device && m_gpu->context) {
            D3D11_TEXTURE2D_DESC texDesc{};
            texDesc.Width = bgW;
            texDesc.Height = bgH;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            std::vector<uint8_t> rgbaUpload(bgW * bgH * 4);
            for (int y = 0; y < bgH; ++y) {
                const uint8_t* srcRow = deepCopyFrame->data[0] + y * deepCopyFrame->linesize[0];
                for (int x = 0; x < bgW; ++x) {
                    rgbaUpload[(y * bgW + x) * 4 + 0] = srcRow[x * 3 + 2]; // R
                    rgbaUpload[(y * bgW + x) * 4 + 1] = srcRow[x * 3 + 1]; // G
                    rgbaUpload[(y * bgW + x) * 4 + 2] = srcRow[x * 3 + 0]; // B
                    rgbaUpload[(y * bgW + x) * 4 + 3] = 255;                // A
                }
            }

            D3D11_SUBRESOURCE_DATA subData{};
            subData.pSysMem = rgbaUpload.data();
            subData.SysMemPitch = bgW * 4;

            HRESULT hr = m_gpu->device->CreateTexture2D(&texDesc, &subData, validatedOwnedTex.GetAddressOf());
            if (SUCCEEDED(hr)) {
                m_gpu->device->CreateShaderResourceView(validatedOwnedTex.Get(), nullptr, validatedOwnedSRV.GetAddressOf());
            }
        }
    }

    // 3. If GPU SRV was provided without CPU frame, read back texture to CPU frame for CPU fallback
    if (gpuSRV && !deepCopyFrame && m_gpu && m_gpu->device && m_gpu->context) {
        D3D11_TEXTURE2D_DESC desc{};
        validatedOwnedTex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(m_gpu->device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            m_gpu->context->CopyResource(staging.Get(), validatedOwnedTex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_gpu->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                deepCopyFrame = av_frame_alloc();
                if (deepCopyFrame) {
                    deepCopyFrame->width = bgW;
                    deepCopyFrame->height = bgH;
                    deepCopyFrame->format = AV_PIX_FMT_BGR24;
                    if (av_frame_get_buffer(deepCopyFrame, 32) >= 0) {
                        deepCopyFrame->color_range = AVCOL_RANGE_JPEG;
                        for (int y = 0; y < bgH; ++y) {
                            const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                            uint8_t* dstRow = deepCopyFrame->data[0] + y * deepCopyFrame->linesize[0];
                            for (int x = 0; x < bgW; ++x) {
                                dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // B
                                dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                                dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // R
                            }
                        }
                    } else {
                        av_frame_free(&deepCopyFrame);
                    }
                }
                m_gpu->context->Unmap(staging.Get(), 0);
            }
        }
    }

    // 4. Atomic Commit: only reached if all validation succeeded!
    m_bgSRV = validatedOwnedSRV;
    m_bgTexture = validatedOwnedTex;
    m_bgWidth = bgW;
    m_bgHeight = bgH;
    m_bgFitMode = fitMode;

    if (m_cpuBgFrame) {
        av_frame_free(&m_cpuBgFrame);
        m_cpuBgFrame = nullptr;
    }
    m_cpuBgFrame = deepCopyFrame;

    return true;
}

void StudioStack::ClearBackgroundImage() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> execLock(m_executionMutex);
    m_bgSRV.Reset();
    m_bgTexture.Reset();
    m_bgWidth = 0;
    m_bgHeight = 0;
    m_bgFitMode = Presets::BackgroundFitMode::CropFill;
    if (m_cpuBgFrame) {
        av_frame_free(&m_cpuBgFrame);
        m_cpuBgFrame = nullptr;
    }
}

bool StudioStack::HasBackgroundImage() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return (m_bgSRV != nullptr) || (m_cpuBgFrame != nullptr);
}

AVFrame* StudioStack::ComposeImageBackgroundCpu(
    AVFrame* currentFrame,
    const AlphaMask* mask,
    Presets::BackgroundFitMode fitMode)
{
    if (!currentFrame || !m_cpuBgFrame || !mask || !mask->data) {
        return nullptr;
    }

    const int w = currentFrame->width;
    const int h = currentFrame->height;
    if (w <= 0 || h <= 0) return nullptr;

    const int bgW = m_cpuBgFrame->width;
    const int bgH = m_cpuBgFrame->height;
    if (bgW <= 0 || bgH <= 0) return nullptr;

    if (!m_cpuBgOutputFrame ||
        m_cpuBgOutputFrame->width != w ||
        m_cpuBgOutputFrame->height != h ||
        m_cpuBgOutputFrame->format != currentFrame->format)
    {
        if (m_cpuBgOutputFrame) {
            av_frame_free(&m_cpuBgOutputFrame);
            m_cpuBgOutputFrame = nullptr;
        }

        m_cpuBgOutputFrame = av_frame_alloc();
        if (!m_cpuBgOutputFrame) return nullptr;

        m_cpuBgOutputFrame->width = w;
        m_cpuBgOutputFrame->height = h;
        m_cpuBgOutputFrame->format = currentFrame->format;

        if (av_frame_get_buffer(m_cpuBgOutputFrame, 32) < 0) {
            av_frame_free(&m_cpuBgOutputFrame);
            m_cpuBgOutputFrame = nullptr;
            return nullptr;
        }
    }

    av_frame_copy_props(m_cpuBgOutputFrame, currentFrame);

    const uint8_t* fgData = currentFrame->data[0];
    const int fgStride = currentFrame->linesize[0];

    uint8_t* outData = m_cpuBgOutputFrame->data[0];
    const int outStride = m_cpuBgOutputFrame->linesize[0];

    const uint8_t* bgData = m_cpuBgFrame->data[0];
    const int bgStride = m_cpuBgFrame->linesize[0];

    const float* maskData = mask->data;
    const int maskW = mask->width;
    const int maskH = mask->height;
    const int maskStride = mask->strideFloats;

    float scale = 1.0f;
    if (fitMode == Presets::BackgroundFitMode::CropFill) {
        scale = std::max(static_cast<float>(w) / static_cast<float>(bgW),
                         static_cast<float>(h) / static_cast<float>(bgH));
    } else if (fitMode == Presets::BackgroundFitMode::FitLetterbox) {
        scale = std::min(static_cast<float>(w) / static_cast<float>(bgW),
                         static_cast<float>(h) / static_cast<float>(bgH));
    }

    auto sampleBgPixel = [&](float xInBg, float yInBg, uint8_t& outB, uint8_t& outG, uint8_t& outR) {
        if (xInBg < 0.0f || xInBg >= static_cast<float>(bgW) ||
            yInBg < 0.0f || yInBg >= static_cast<float>(bgH))
        {
            outB = outG = outR = 0;
            return;
        }

        float px = xInBg - 0.5f;
        float py = yInBg - 0.5f;
        int x0 = std::max(0, std::min(static_cast<int>(std::floor(px)), bgW - 1));
        int y0 = std::max(0, std::min(static_cast<int>(std::floor(py)), bgH - 1));
        int x1 = std::max(0, std::min(x0 + 1, bgW - 1));
        int y1 = std::max(0, std::min(y0 + 1, bgH - 1));
        float fx = px - std::floor(px);
        float fy = py - std::floor(py);

        const uint8_t* row0 = bgData + y0 * bgStride;
        const uint8_t* row1 = bgData + y1 * bgStride;

        for (int c = 0; c < 3; ++c) {
            float p00 = row0[x0 * 3 + c];
            float p10 = row0[x1 * 3 + c];
            float p01 = row1[x0 * 3 + c];
            float p11 = row1[x1 * 3 + c];
            float top = p00 * (1.0f - fx) + p10 * fx;
            float bot = p01 * (1.0f - fx) + p11 * fx;
            float val = top * (1.0f - fy) + bot * fy;
            uint8_t byteVal = static_cast<uint8_t>(std::round(std::max(0.0f, std::min(255.0f, val))));
            if (c == 0) outB = byteVal;
            else if (c == 1) outG = byteVal;
            else outR = byteVal;
        }
    };

    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = fgData + y * fgStride;
        uint8_t* dstRow = outData + y * outStride;

        int my = (maskH == h) ? y : std::max(0, std::min(static_cast<int>((y + 0.5f) / h * maskH), maskH - 1));
        const float* maskRow = maskData + my * maskStride;

        for (int x = 0; x < w; ++x) {
            int mx = (maskW == w) ? x : std::max(0, std::min(static_cast<int>((x + 0.5f) / w * maskW), maskW - 1));
            float alpha = std::max(0.0f, std::min(1.0f, maskRow[mx]));

            if (alpha >= 1.0f - 1e-5f) {
                dstRow[x * 3 + 0] = srcRow[x * 3 + 0];
                dstRow[x * 3 + 1] = srcRow[x * 3 + 1];
                dstRow[x * 3 + 2] = srcRow[x * 3 + 2];
                continue;
            }

            float xInBg = 0.0f;
            float yInBg = 0.0f;
            if (fitMode == Presets::BackgroundFitMode::CropFill || fitMode == Presets::BackgroundFitMode::FitLetterbox) {
                xInBg = (static_cast<float>(x) + 0.5f - static_cast<float>(w) * 0.5f) / scale + static_cast<float>(bgW) * 0.5f;
                yInBg = (static_cast<float>(y) + 0.5f - static_cast<float>(h) * 0.5f) / scale + static_cast<float>(bgH) * 0.5f;
            } else {
                xInBg = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * static_cast<float>(bgW);
                yInBg = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * static_cast<float>(bgH);
            }

            uint8_t bgB = 0, bgG = 0, bgR = 0;
            sampleBgPixel(xInBg, yInBg, bgB, bgG, bgR);

            if (alpha <= 1e-5f) {
                dstRow[x * 3 + 0] = bgB;
                dstRow[x * 3 + 1] = bgG;
                dstRow[x * 3 + 2] = bgR;
            } else {
                dstRow[x * 3 + 0] = static_cast<uint8_t>(std::round(srcRow[x * 3 + 0] * alpha + bgB * (1.0f - alpha)));
                dstRow[x * 3 + 1] = static_cast<uint8_t>(std::round(srcRow[x * 3 + 1] * alpha + bgG * (1.0f - alpha)));
                dstRow[x * 3 + 2] = static_cast<uint8_t>(std::round(srcRow[x * 3 + 2] * alpha + bgR * (1.0f - alpha)));
            }
        }
    }

    return m_cpuBgOutputFrame;
}

// -------------------------------------------------------------
// Shared GPU Stage Pipeline Execution
// -------------------------------------------------------------

StudioStack::GpuPipelineResult StudioStack::ExecuteGpuStages(
    ID3D11ShaderResourceView* initialInputSRV,
    ID3D11Texture2D* initialInputTex,
    int width, int height,
    const Presets::StudioPreset& preset,
    std::shared_ptr<const CubeLut> activeLut,
    std::shared_ptr<const CubeLut> inputLut,
    const AnalysisBridge::AnalysisBundle* analysisBundle,
    StackExecutionReport& report,
    const AnalysisBridge::AnalysisBundle* lightingGeometry,
    const AnalysisBridge::AnalysisBundle* appearanceGeometry,
    const AnalysisBridge::AnalysisBundle* accessoryGeometry)
{
    GpuPipelineResult result;
    if (!m_gpu || !m_gpu->device || !m_gpu->context) {
        result.error = "GPU resources unavailable";
        return result;
    }

    GpuFailureTarget failTarget = m_gpuFailureTarget.load();
    if (m_gpuFailureInjected.load() || failTarget == GpuFailureTarget::AnyGpu) {
        result.error = "Simulated GPU device failure (injected failure point)";
        return result;
    }
    if (failTarget == GpuFailureTarget::ResourceCreation) {
        result.error = "Simulated GPU resource creation failure (injected failure point)";
        return result;
    }

    if (m_gpu->appearanceProcessor->IsDeviceLost() ||
        m_gpu->lightingEngine->IsDeviceLost() ||
        (m_gpu->backgroundCompositor && m_gpu->backgroundCompositor->IsDeviceLost()) ||
        (m_gpu->lutProcessor && m_gpu->lutProcessor->IsDeviceLost()) ||
        (m_gpu->overlayRenderer && m_gpu->overlayRenderer->IsDeviceLost())) {
        result.isDeviceLost = true;
        result.error = "GPU device was lost or removed";
        return result;
    }

    std::string ensureError;
    bool isDevLost = false;
    if (!m_gpu->EnsureTextures(width, height, m_gpuFaultSeam.load(), m_gpuFaultHResult.load(), ensureError, isDevLost)) {
        result.error = ensureError;
        result.isDeviceLost = isDevLost;
        return result;
    }

    // Ping-pong state tracking:
    // srcIsInput: true if no stage has written to ping-pong buffers yet (source is initialInputSRV).
    // When a stage writes:
    //   if srcIsInput: writes to texB (uavB) -> srcIsInput = false, srcIsA = false (texB is now source).
    //   if !srcIsInput:
    //     if srcIsA: reads texA (srvA), writes to texB (uavB) -> srcIsA = false
    //     if !srcIsA: reads texB (srvB), writes to texA (uavA) -> srcIsA = true
    bool srcIsInput = true;
    bool srcIsA = false;
    bool personMaskUploadedGpu = false;
    const bool isProfiling = m_stageProfilingEnabled.load();

    auto getCurSRV = [&]() -> ID3D11ShaderResourceView* {
        if (srcIsInput) return initialInputSRV;
        return srcIsA ? m_gpu->srvA.Get() : m_gpu->srvB.Get();
    };

    auto getCurUAV = [&]() -> ID3D11UnorderedAccessView* {
        if (srcIsInput) return m_gpu->uavB.Get();
        return srcIsA ? m_gpu->uavB.Get() : m_gpu->uavA.Get();
    };

    auto colourFault = [this](bool lut) {
        auto seam=m_gpuFaultSeam.load();
        using F=Gpu::DesktopColourFault;
        if(!lut) {
            if(seam==GpuFaultSeam::InputLook_ShaderCompile)return F::ShaderCompile;
            if(seam==GpuFaultSeam::InputLook_ShaderCreation)return F::ShaderCreation;
            if(seam==GpuFaultSeam::InputLook_BufferCreation)return F::BufferCreation;
            if(seam==GpuFaultSeam::InputLook_Map)return F::Map;
            if(seam==GpuFaultSeam::InputLook_Dispatch)return F::Dispatch;
        } else {
            if(seam==GpuFaultSeam::InputLut_ShaderCompile)return F::ShaderCompile;
            if(seam==GpuFaultSeam::InputLut_ShaderCreation)return F::ShaderCreation;
            if(seam==GpuFaultSeam::InputLut_BufferCreation)return F::BufferCreation;
            if(seam==GpuFaultSeam::InputLut_Texture)return F::LutTexture;
            if(seam==GpuFaultSeam::InputLut_View)return F::LutView;
            if(seam==GpuFaultSeam::InputLut_Map)return F::Map;
            if(seam==GpuFaultSeam::InputLut_Dispatch)return F::Dispatch;
        }
        return F::None;
    };
    if(m_gpu->desktopLooksProcessor) m_gpu->desktopLooksProcessor->SetFault(colourFault(false));
    if(m_gpu->inputLutProcessor) m_gpu->inputLutProcessor->SetFault(colourFault(true));

    // 0. GPU Input Colour Look Stage (Warm / Cool / Mono â€” applied BEFORE appearance/lighting)
    {
        const Gpu::DesktopLook look = static_cast<Gpu::DesktopLook>(
            std::clamp(preset.inputLook, 0, 3));
        const float lookStrength = std::clamp(preset.inputLookStrength, 0.0f, 1.0f);

        const bool lookActive = (!preset.bypassAll &&
                                 look != Gpu::DesktopLook::Original &&
                                 lookStrength > 0.0f);
        if (!lookActive) {
            report.inputLookStatus = StageStatus::Bypassed;
        } else if (failTarget == GpuFailureTarget::InputLookStage) {
            report.inputLookStatus=StageStatus::Failed;
            result.error = "Simulated GPU InputLook stage failure (injected failure point)";
            return result;
        } else if (m_gpu->desktopLooksProcessor) {
            auto lookOutcome = m_gpu->desktopLooksProcessor->Process(
                getCurSRV(), getCurUAV(), width, height, look, lookStrength);

            if (lookOutcome == Gpu::GpuDesktopLooksOutcome::Success) {
                report.inputLookStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (lookOutcome == Gpu::GpuDesktopLooksOutcome::Bypass) {
                report.inputLookStatus = StageStatus::Bypassed;
            } else if (lookOutcome == Gpu::GpuDesktopLooksOutcome::DeviceLost) {
                report.inputLookStatus=StageStatus::Failed;
                result.isDeviceLost = true;
                result.error = "Desktop looks processor encountered device loss";
                return result;
            } else {
                report.inputLookStatus=StageStatus::Failed;
                result.error = "Desktop looks processor shader error";
                return result;
            }
        } else {
            report.inputLookStatus = StageStatus::NotReady;
        }
    }

    // Independent INPUT LUT: runs after look, before appearance/lighting; post-background LUT is unchanged.
    if(preset.bypassAll || !inputLut || preset.inputLutIntensity==0) report.inputLutStatus=StageStatus::Bypassed;
    else {
        if(failTarget==GpuFailureTarget::InputLutStage || !m_gpu->inputLutProcessor) {
            report.inputLutStatus=StageStatus::Failed; result.error="Input LUT stage unavailable/injected failure";return result;
        }
        auto outcome=m_gpu->inputLutProcessor->Process(getCurSRV(),getCurUAV(),width,height,inputLut,preset.inputLutIntensity);
        if(outcome!=Gpu::GpuLutOutcome::Success) {
            report.inputLutStatus=StageStatus::Failed;
            result.isDeviceLost=(outcome==Gpu::GpuLutOutcome::DeviceLost);
            result.error="Input LUT stage failed";return result;
        }
        report.inputLutStatus=StageStatus::Applied;
        if(srcIsInput){srcIsInput=false;srcIsA=false;}else srcIsA=!srcIsA;
    }

    // 1. GPU Appearance Stage
    const bool useExactAppearance=appearanceGeometry!=nullptr;
    const auto* appearanceAnalysis=useExactAppearance?appearanceGeometry:analysisBundle;
    if (preset.bypassAll || preset.appearance.bypass ||
        (preset.appearance.smoothingStrength <= 0.001f && preset.appearance.sharpenStrength <= 0.001f))
    {
        report.appearanceStatus = StageStatus::Bypassed;
    }
    else
    {
        if (failTarget == GpuFailureTarget::AppearanceStage) {
            result.error = "Simulated GPU Appearance stage failure (injected failure point)";
            return result;
        }

        if (useExactAppearance?ExactAppearanceGeometryReady(appearanceAnalysis,report):
            (appearanceAnalysis && appearanceAnalysis->skinMask.IsValid()))
        {
            if (m_maskPrepFaultInjected.load()) {
                result.error = "Simulated mask preparation fault (injected fault point)";
                return result;
            }
            AnalysisBridge::MaskPreparationConfig prepCfgSkin;
            prepCfgSkin.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
            prepCfgSkin.featherRadius = useExactAppearance?0.0f:2.0f;
            prepCfgSkin.enableTemporalStabilization = false;

            AnalysisBridge::Gpu::GpuMaskFaultSeam prepSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::None;
            GpuFaultSeam seam = m_gpuFaultSeam.load();
            HRESULT seamHr = m_gpuFaultHResult.load();
            if (seam == GpuFaultSeam::SkinMask_Map) {
                prepSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::NativeUpload_Map;
            } else if (seam == GpuFaultSeam::DeviceRemoved_Seam) {
                prepSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::DeviceRemoved;
            }

            auto prepSkinStart = std::chrono::steady_clock::now();
            std::string prepError;
            auto outcome = m_gpu->gpuMaskPreparerSkin.PrepareMask(
                m_gpu->device.Get(),
                m_gpu->context.Get(),
                appearanceAnalysis->skinMask,
                width, height,
                prepCfgSkin,
                prepSeam,
                seamHr,
                &prepError
            );
            if (outcome != AnalysisBridge::Gpu::GpuMaskOutcome::Success) {
                if (outcome == AnalysisBridge::Gpu::GpuMaskOutcome::DeviceLost) {
                    result.isDeviceLost = true;
                }
                if (seam == GpuFaultSeam::SkinMask_Map) {
                    result.error = "Simulated fault at skin mask map boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
                } else {
                    result.error = prepError.empty() ? "Skin mask preparation failed" : prepError;
                }
                return result;
            }
            report.maskPreparationCount++;
            report.maskUploadCount++;
            if (isProfiling) {
                report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepSkinStart).count();
            }

            ID3D11ShaderResourceView* protSRV = nullptr;
            if (appearanceAnalysis->protectionMask.IsValid()) {
                if (m_maskPrepFaultInjected.load()) {
                    result.error = "Simulated mask preparation fault (injected fault point)";
                    return result;
                }
                AnalysisBridge::MaskPreparationConfig prepCfgProt;
                prepCfgProt.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                prepCfgProt.featherRadius = useExactAppearance?0.0f:1.0f;
                prepCfgProt.enableTemporalStabilization = false;

                AnalysisBridge::Gpu::GpuMaskFaultSeam protSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::None;
                if (seam == GpuFaultSeam::ProtMask_Map) {
                    protSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::NativeUpload_Map;
                } else if (seam == GpuFaultSeam::DeviceRemoved_Seam) {
                    protSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::DeviceRemoved;
                }

                auto prepProtStart = std::chrono::steady_clock::now();
                std::string protError;
                auto protOutcome = m_gpu->gpuMaskPreparerProt.PrepareMask(
                    m_gpu->device.Get(),
                    m_gpu->context.Get(),
                    appearanceAnalysis->protectionMask,
                    width, height,
                    prepCfgProt,
                    protSeam,
                    seamHr,
                    &protError
                );
                if (protOutcome != AnalysisBridge::Gpu::GpuMaskOutcome::Success) {
                    if (protOutcome == AnalysisBridge::Gpu::GpuMaskOutcome::DeviceLost) {
                        result.isDeviceLost = true;
                    }
                    if (seam == GpuFaultSeam::ProtMask_Map) {
                        result.error = "Simulated fault at protection mask map boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
                    } else {
                        result.error = protError.empty() ? "Protection mask preparation failed" : protError;
                    }
                    return result;
                }
                report.maskPreparationCount++;
                report.maskUploadCount++;
                if (isProfiling) {
                    report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepProtStart).count();
                }
                protSRV = m_gpu->gpuMaskPreparerProt.GetOutputSRV();
            }

            auto appOutcome = m_gpu->appearanceProcessor->Process(
                getCurSRV(), getCurUAV(), m_gpu->gpuMaskPreparerSkin.GetOutputSRV(), protSRV, width, height, preset.appearance);

            if (appOutcome == Appearance::Gpu::GpuAppearanceOutcome::Success) {
                report.appearanceStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (appOutcome == Appearance::Gpu::GpuAppearanceOutcome::DeviceLost) {
                result.isDeviceLost = true;
                result.error = "Appearance processor encountered device loss";
                return result;
            } else {
                result.error = "Appearance processor shader error";
                return result;
            }
        }
        else
        {
            report.appearanceStatus = StageStatus::NotReady;
        }
    }

    // Helper lambda for texture-native GPU person mask preparation and reuse
    auto preparePersonMaskGpu = [&](const AnalysisBridge::MaskBuffer& pMask) -> bool {
        if (personMaskUploadedGpu) {
            report.maskReuseCount++;
            return true;
        }
        if (m_maskPrepFaultInjected.load()) {
            result.error = "Simulated mask preparation fault (injected fault point)";
            return false;
        }
        AnalysisBridge::MaskPreparationConfig prepCfgPerson;
        prepCfgPerson.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
        prepCfgPerson.featherRadius = 3.0f;
        prepCfgPerson.enableTemporalStabilization = true;

        AnalysisBridge::Gpu::GpuMaskFaultSeam personSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::None;
        GpuFaultSeam seam = m_gpuFaultSeam.load();
        HRESULT seamHr = m_gpuFaultHResult.load();
        if (seam == GpuFaultSeam::PersonMask_Map) {
            personSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::NativeUpload_Map;
        } else if (seam == GpuFaultSeam::DeviceRemoved_Seam) {
            personSeam = AnalysisBridge::Gpu::GpuMaskFaultSeam::DeviceRemoved;
        }

        auto prepStart = std::chrono::steady_clock::now();
        std::string prepErr;
        auto outcome = m_gpu->gpuMaskPreparerPerson.PrepareMask(
            m_gpu->device.Get(),
            m_gpu->context.Get(),
            pMask,
            width, height,
            prepCfgPerson,
            personSeam,
            seamHr,
            &prepErr
        );
        if (outcome != AnalysisBridge::Gpu::GpuMaskOutcome::Success) {
            if (outcome == AnalysisBridge::Gpu::GpuMaskOutcome::DeviceLost) {
                result.isDeviceLost = true;
            }
            if (seam == GpuFaultSeam::PersonMask_Map) {
                result.error = "Simulated fault at person mask map boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
            } else {
                result.error = prepErr.empty() ? "Person mask preparation failed" : prepErr;
            }
            return false;
        }
        report.maskPreparationCount++;
        report.maskUploadCount++;
        if (isProfiling) {
            report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart).count();
        }
        personMaskUploadedGpu = true;
        return true;
    };

    // 2. GPU Studio Lighting Stage
    report.effectiveLightingMode = preset.lighting.lightingMode;
    const bool isNormalGuided = (preset.lighting.lightingMode == Lighting::LightingMode::NormalGuided);
    const bool useExactGeometry = isNormalGuided && lightingGeometry;
    const auto* lightAnalysis = useExactGeometry ? lightingGeometry : analysisBundle;
    auto lightConfig = preset.lighting;
    if (useExactGeometry) lightConfig.subjectOnlyMode = true;

    if (preset.bypassAll || preset.lighting.bypass ||
        (!preset.lighting.light1.enabled &&
         !preset.lighting.light2.enabled &&
         preset.lighting.fillIntensity <= 0.001f))
    {
        report.lightingStatus = StageStatus::Bypassed;
    }
    else
    {
        if (failTarget == GpuFailureTarget::LightingStage) {
            result.error = "Simulated GPU Lighting stage failure (injected failure point)";
            return result;
        }

        const bool hasValidNormals = useExactGeometry
            ? ExactLightingGeometryReady(lightAnalysis, report, lightConfig.shadowConfig.enabled)
            : (lightAnalysis && lightAnalysis->sourceGeneration == report.sourceGeneration &&
               lightAnalysis->sequenceId == report.frameSequence &&
               lightAnalysis->timestampPts == report.timestampPts && lightAnalysis->HasNormals());

        if (isNormalGuided && !hasValidNormals) {
            report.lightingStatus = StageStatus::NotReady;
        } else {
            ID3D11ShaderResourceView* lightMaskSRV = nullptr;
            if (useExactGeometry) {
                AnalysisBridge::MaskPreparationConfig faceConfig;
                faceConfig.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                faceConfig.featherRadius = 0.0f;
                faceConfig.enableTemporalStabilization = false;
                const auto prepStart = std::chrono::steady_clock::now();
                std::string faceError;
                const auto outcome = m_maskPrepFaultInjected.load()
                    ? AnalysisBridge::Gpu::GpuMaskOutcome::InjectedFault
                    : m_gpu->gpuMaskPreparerFace.PrepareMask(
                        m_gpu->device.Get(), m_gpu->context.Get(), lightAnalysis->personMask,
                        width, height, faceConfig, AnalysisBridge::Gpu::GpuMaskFaultSeam::None,
                        E_FAIL, &faceError);
                if (outcome != AnalysisBridge::Gpu::GpuMaskOutcome::Success) {
                    result.isDeviceLost = outcome == AnalysisBridge::Gpu::GpuMaskOutcome::DeviceLost;
                    result.error = faceError.empty() ? "Face coverage preparation failed" : faceError;
                    return result;
                }
                lightMaskSRV = m_gpu->gpuMaskPreparerFace.GetOutputSRV();
                ++report.maskPreparationCount;
                ++report.maskUploadCount;
                if (isProfiling) report.maskPrepMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - prepStart).count();
            } else if (lightConfig.subjectOnlyMode) {
                if (analysisBundle && analysisBundle->personMask.IsValid()) {
                    if (!preparePersonMaskGpu(analysisBundle->personMask)) {
                        return result;
                    }
                    lightMaskSRV = m_gpu->gpuMaskPreparerPerson.GetOutputSRV();
                }
            }

            ID3D11ShaderResourceView* lightNormalsSRV = nullptr;
            if (hasValidNormals) {
                GpuFaultSeam seam = m_gpuFaultSeam.load();
                HRESULT seamHr = m_gpuFaultHResult.load();
                AnalysisBridge::Gpu::GpuNormalFaultSeam normSeam = AnalysisBridge::Gpu::GpuNormalFaultSeam::None;
                if (seam == GpuFaultSeam::Normal_Map) normSeam = AnalysisBridge::Gpu::GpuNormalFaultSeam::NativeUpload_Map;
                else if (seam == GpuFaultSeam::NormalTex_Creation) normSeam = AnalysisBridge::Gpu::GpuNormalFaultSeam::TargetAlloc;
                else if (seam == GpuFaultSeam::Normal_Resample) normSeam = AnalysisBridge::Gpu::GpuNormalFaultSeam::ResampleDispatch;
                else if (seam == GpuFaultSeam::DeviceRemoved_Seam) normSeam = AnalysisBridge::Gpu::GpuNormalFaultSeam::DeviceRemoved;

                AnalysisBridge::NormalPreparationConfig normConfig;
                normConfig.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;

                std::string normError;
                auto normOutcome = m_gpu->gpuNormalPreparer.PrepareNormals(
                    m_gpu->device.Get(), m_gpu->context.Get(),
                    lightAnalysis->normalMap, width, height, normConfig,
                    normSeam, seamHr, &normError
                );
                if (normOutcome == AnalysisBridge::Gpu::GpuNormalOutcome::Success) {
                    lightNormalsSRV = m_gpu->gpuNormalPreparer.GetOutputSRV();
                    report.normalPreparationCount++;
                    report.normalUploadCount++;
                } else if (normOutcome == AnalysisBridge::Gpu::GpuNormalOutcome::DeviceLost) {
                    result.isDeviceLost = true;
                    result.error = "Device loss during normal preparation";
                    return result;
                } else {
                    if (seam == GpuFaultSeam::Normal_Map) {
                        result.error = "Simulated fault at normal map boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
                        return result;
                    } else if (seam == GpuFaultSeam::NormalTex_Creation) {
                        result.error = "Simulated fault at normal texture creation boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
                        return result;
                    } else if (seam == GpuFaultSeam::Normal_Resample) {
                        result.error = "Simulated fault at normal resample boundary (HRESULT 0x" + HrToHex(seamHr) + ")";
                        return result;
                    }
                    if (isNormalGuided) {
                        report.lightingStatus = StageStatus::NotReady;
                        result.error = normError.empty() ? "Normal preparation failed" : normError;
                        return result;
                    }
                }
            }

            ID3D11ShaderResourceView* lightDepthSRV = nullptr;
            if (lightConfig.shadowConfig.enabled && lightAnalysis && lightAnalysis->HasDepth()) {
                const auto prep = Relighting::PrepareShadowDepth(lightAnalysis->depthMap, width, height, m_scratchShadowDepth);
                if (prep == Relighting::ShadowDepthPreparation::OutOfMemory) {
                    result.error = "Shadow depth preparation allocation failed";
                    return result;
                }
                if (prep == Relighting::ShadowDepthPreparation::Success) {
                    const auto seam = m_gpuFaultSeam.load();
                    const auto seamHr = m_gpuFaultHResult.load();
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    const HRESULT hr = seam == GpuFaultSeam::Depth_Map ? seamHr :
                        m_gpu->context->Map(m_gpu->depthTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                    if (FAILED(hr)) {
                        result.isDeviceLost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
                        result.error = "Shadow depth map failed (HRESULT 0x" + HrToHex(hr) + ")";
                        return result;
                    }
                    if (!mapped.pData || mapped.RowPitch < static_cast<size_t>(width) * sizeof(float)) {
                        m_gpu->context->Unmap(m_gpu->depthTex.Get(), 0);
                        result.error = "Invalid shadow depth mapping stride";
                        return result;
                    }
                    for (int y = 0; y < height; ++y)
                        std::memcpy(static_cast<unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                    m_scratchShadowDepth.data.data() + static_cast<size_t>(y) * width,
                                    static_cast<size_t>(width) * sizeof(float));
                    m_gpu->context->Unmap(m_gpu->depthTex.Get(), 0);
                    lightDepthSRV = m_gpu->depthSRV.Get();
                }
            }

            auto lightOutcome = m_gpu->lightingEngine->Process(
                getCurSRV(), getCurUAV(), lightMaskSRV, lightNormalsSRV, lightDepthSRV, width, height, lightConfig);

            if (lightOutcome == Lighting::Gpu::GpuLightingOutcome::Success) {
                report.lightingStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (lightOutcome == Lighting::Gpu::GpuLightingOutcome::NotReady) {
                report.lightingStatus = StageStatus::NotReady;
            } else if (lightOutcome == Lighting::Gpu::GpuLightingOutcome::DeviceLost) {
                result.isDeviceLost = true;
                result.error = "Lighting engine encountered device loss";
                return result;
            } else {
                result.error = "Lighting engine shader error";
                return result;
            }
        }
    }

    // 3. GPU Background Compositing Stage
    Presets::StudioBackgroundMode effectiveBgMode = preset.backgroundMode;
    if (effectiveBgMode == Presets::StudioBackgroundMode::Original &&
        preset.background.mode != PrismForge::Effects::BackgroundMode::Original)
    {
        if (preset.background.mode == PrismForge::Effects::BackgroundMode::Blur) {
            effectiveBgMode = Presets::StudioBackgroundMode::Blur;
        } else if (preset.background.mode == PrismForge::Effects::BackgroundMode::Solid) {
            effectiveBgMode = Presets::StudioBackgroundMode::Solid;
        }
    }

    bool isImageMode = (effectiveBgMode == Presets::StudioBackgroundMode::Image);
    bool isOriginalMode = (effectiveBgMode == Presets::StudioBackgroundMode::Original);

    if (preset.bypassAll || isOriginalMode)
    {
        report.backgroundStatus = StageStatus::Bypassed;
    }
    else
    {
        if (failTarget == GpuFailureTarget::BackgroundStage) {
            result.error = "Simulated GPU Background stage failure (injected failure point)";
            return result;
        }

        if (analysisBundle && analysisBundle->personMask.IsValid() && m_gpu->backgroundCompositor)
        {
            if (!preparePersonMaskGpu(analysisBundle->personMask)) {
                return result;
            }
            ID3D11ShaderResourceView* bgMaskSRV = m_gpu->gpuMaskPreparerPerson.GetOutputSRV();

            PrismForge::Effects::BackgroundSettings bgSettings = preset.background;
            if (effectiveBgMode == Presets::StudioBackgroundMode::Blur) {
                bgSettings.mode = PrismForge::Effects::BackgroundMode::Blur;
            } else if (effectiveBgMode == Presets::StudioBackgroundMode::Solid) {
                bgSettings.mode = PrismForge::Effects::BackgroundMode::Solid;
            }

            Gpu::GpuBackgroundOutcome bgOutcome = Gpu::GpuBackgroundOutcome::Success;
            if (isImageMode) {
                if (!m_bgSRV) {
                    result.error = "Background image SRV is not set for Image mode";
                    return result;
                }
                bgOutcome = m_gpu->backgroundCompositor->Process(
                    getCurSRV(),
                    getCurUAV(),
                    bgMaskSRV,
                    width,
                    height,
                    bgSettings,
                    m_bgSRV.Get(),
                    m_bgWidth,
                    m_bgHeight,
                    static_cast<int>(preset.backgroundFitMode),
                    true
                );
            } else {
                bgOutcome = m_gpu->backgroundCompositor->Process(
                    getCurSRV(),
                    getCurUAV(),
                    bgMaskSRV,
                    width,
                    height,
                    bgSettings,
                    nullptr,
                    0,
                    0,
                    0,
                    false
                );
            }

            if (bgOutcome == Gpu::GpuBackgroundOutcome::Success) {
                report.backgroundStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (bgOutcome == Gpu::GpuBackgroundOutcome::Bypass) {
                report.backgroundStatus = StageStatus::Bypassed;
            } else if (bgOutcome == Gpu::GpuBackgroundOutcome::DeviceLost) {
                result.isDeviceLost = true;
                result.error = "Background compositor encountered device loss";
                return result;
            } else {
                result.error = "Background compositor shader error";
                return result;
            }
        }
        else
        {
            report.backgroundStatus = StageStatus::NotReady;
        }
    }

    // 4. GPU 3D LUT Stage
    if (preset.bypassAll || !activeLut || preset.lutIntensity <= 0.0f)
    {
        report.lutStatus = StageStatus::Bypassed;
    }
    else
    {
        if (failTarget == GpuFailureTarget::LutStage) {
            result.error = "Simulated GPU LUT stage failure (injected failure point)";
            return result;
        }

        if (m_gpu->lutProcessor) {
            auto lutOutcome = m_gpu->lutProcessor->Process(
                getCurSRV(), getCurUAV(), width, height, activeLut, preset.lutIntensity);

            if (lutOutcome == Gpu::GpuLutOutcome::Success) {
                report.lutStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (lutOutcome == Gpu::GpuLutOutcome::Bypass) {
                report.lutStatus = StageStatus::Bypassed;
            } else if (lutOutcome == Gpu::GpuLutOutcome::DeviceLost) {
                result.isDeviceLost = true;
                result.error = "LUT processor encountered device loss";
                return result;
            } else {
                result.error = "LUT processor shader error";
                return result;
            }
        } else {
            report.lutStatus = StageStatus::NotReady;
        }
    }

    // 5. GPU Tracked Overlays & Procedural Accessories Stage
    if (preset.bypassAll || preset.overlays.bypass ||
        (!preset.overlays.glasses.enabled &&
         !preset.overlays.hat.enabled &&
         !preset.overlays.studioFrame.enabled && preset.faceFilterStyle==0))
    {
        m_trackingEvaluator.Reset();
        report.overlayStatus = StageStatus::Bypassed;
    }
    else
    {
        if (failTarget == GpuFailureTarget::OverlayStage) {
            result.error = "Simulated GPU Overlay stage failure (injected failure point)";
            return result;
        }

        const auto* overlaySource=accessoryGeometry?accessoryGeometry:analysisBundle;
        const auto* overlayAnalysis=Overlays::MatchesFrame(overlaySource,report.sourceGeneration,report.frameSequence,report.timestampPts)?overlaySource:nullptr;
        if(accessoryGeometry && (!overlayAnalysis || !overlayAnalysis->faceLandmarks.NativeGeometryMatches(width,height,
            report.sourceGeneration,report.frameSequence,report.timestampPts))) {overlayAnalysis=nullptr;m_trackingEvaluator.Reset();}
        if (preset.faceFilterStyle>0 || (preset.realisticAccessories && (preset.overlays.glasses.enabled || preset.overlays.hat.enabled))) {
            // Native 3D accessories have their own depth-only face occluder.
            // Keep the screen-space frame separate; never fall back to a flat face sticker.
            const auto* landmarks = overlayAnalysis ? &overlayAnalysis->faceLandmarks : nullptr;
            std::string error;
            const auto outcome = m_gpu->faceAccessories ? m_gpu->faceAccessories->Process(
                getCurSRV(), getCurUAV(), width, height, landmarks,
                preset.faceFilterStyle==0&&preset.overlays.glasses.enabled, preset.faceFilterStyle==0&&preset.overlays.hat.enabled,
                preset.overlays.glasses.enabled ? preset.overlays.glasses.scaleMultiplier : preset.overlays.hat.scaleMultiplier,
                &report.overlayLayers, &error, preset.realisticCapStyle,preset.realisticGlassesStyle,preset.realisticCapScale,preset.realisticCapVerticalOffset,
                preset.faceFilterStyle,preset.faceFilterScale,preset.faceFilterVerticalOffset,preset.faceFilterCustomModel,preset.faceFilterCustomFit,preset.faceFilterRenderEdge) : Gpu::GpuOverlayOutcome::NotReady;
            // A rejected model/fit has not touched the GPU input. Preserve its
            // failure while allowing the independent screen border to use that input.
            const bool filterBuildFailed=m_gpu->faceAccessories &&
                m_gpu->faceAccessories->GetLastFilterBuildFailure()!=Overlays::Realistic::FilterBuildFailure::None;
            auto advance = [&] { if (srcIsInput) { srcIsInput=false;srcIsA=false; } else srcIsA=!srcIsA; };
            if (outcome == Gpu::GpuOverlayOutcome::Success) { report.overlayStatus=StageStatus::Applied;advance(); }
            else if (outcome == Gpu::GpuOverlayOutcome::NotReady) {
                report.overlayStatus=StageStatus::NotReady;
                report.overlayLayers={preset.faceFilterStyle==0&&preset.overlays.glasses.enabled?2:1,preset.faceFilterStyle==0&&preset.overlays.hat.enabled?2:1,1,preset.faceFilterStyle>0?2:1};
            } else if (outcome == Gpu::GpuOverlayOutcome::Bypass) report.overlayStatus=StageStatus::Bypassed;
            else if (filterBuildFailed) { report.overlayStatus=StageStatus::Failed;
                report.errorDescription=error.empty()?"3D face filter model or fit is unavailable":error; }
            else { result.isDeviceLost=outcome==Gpu::GpuOverlayOutcome::DeviceLost;
                result.error=error.empty()?"3D accessory GPU stage failed":error;return result; }
            if (preset.overlays.studioFrame.enabled && m_gpu->overlayRenderer) {
                auto framePreset=preset;framePreset.overlays.glasses.enabled=false;framePreset.overlays.hat.enabled=false;
                const auto metrics=Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioDecorativeFrame);
                const auto frameOutcome=m_gpu->overlayRenderer->Process(getCurSRV(),getCurUAV(),nullptr,width,height,
                    framePreset,{}, {},metrics,metrics,metrics,&error);
                report.overlayLayers.frame=m_gpu->overlayRenderer->GetLayerReport().frame;
                if (frameOutcome==Gpu::GpuOverlayOutcome::Success) {
                    if (!filterBuildFailed) report.overlayStatus=StageStatus::Applied;advance(); }
                else if (frameOutcome!=Gpu::GpuOverlayOutcome::Bypass && frameOutcome!=Gpu::GpuOverlayOutcome::NotReady) {
                    result.isDeviceLost=frameOutcome==Gpu::GpuOverlayOutcome::DeviceLost;
                    result.error=error.empty()?"Screen frame GPU stage failed":error;return result;
                }
            }
        } else if (m_gpu->overlayRenderer) {
            // Check if behind-subject placement needs person mask uploaded
            bool needBehindSubjectMask =
                (preset.overlays.glasses.enabled && preset.overlays.glasses.placement == Overlays::OverlayPlacement::BehindSubject) ||
                (preset.overlays.hat.enabled && preset.overlays.hat.placement == Overlays::OverlayPlacement::BehindSubject);

            if (needBehindSubjectMask && !accessoryGeometry && overlayAnalysis && Overlays::ValidMask(overlayAnalysis->personMask)) {
                if (!preparePersonMaskGpu(overlayAnalysis->personMask)) {
                    return result;
                }
            }

            m_gpu->overlayRenderer->SetFaultSeam(m_gpuFaultSeam.load(), m_gpuFaultHResult.load());

            const AnalysisBridge::FaceLandmarkBundle* landmarks =
                (overlayAnalysis && overlayAnalysis->faceLandmarks.detected) ? &overlayAnalysis->faceLandmarks : nullptr;

            m_trackingEvaluator.Update(
                preset.overlays,
                landmarks,
                width,
                height,
                report.sourceGeneration,
                report.timestampPts,
                report.frameSequence);

            Overlays::TrackedTransform glassesT = m_trackingEvaluator.GetGlassesTransform();
            Overlays::TrackedTransform hatT = m_trackingEvaluator.GetHatTransform();
            Overlays::AssetAnchorMetrics glassesM = Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioAviatorGlasses);
            Overlays::AssetAnchorMetrics hatM = Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioCap);
            Overlays::AssetAnchorMetrics frameM = Overlays::ProceduralAssetGenerator::GetAnchorMetrics(Overlays::ProceduralAssetId::StudioDecorativeFrame);

            ID3D11ShaderResourceView* maskSRV = (!accessoryGeometry && personMaskUploadedGpu && overlayAnalysis && Overlays::ValidMask(overlayAnalysis->personMask))
                ? m_gpu->gpuMaskPreparerPerson.GetOutputSRV() : nullptr;

            std::string overlayError;
            auto ovOutcome = m_gpu->overlayRenderer->Process(
                getCurSRV(),
                getCurUAV(),
                maskSRV,
                width,
                height,
                preset,
                glassesT,
                hatT,
                glassesM,
                hatM,
                frameM,
                &overlayError
            );

            report.overlayLayers = m_gpu->overlayRenderer->GetLayerReport();
            if (ovOutcome == Gpu::GpuOverlayOutcome::Success) {
                report.overlayStatus = StageStatus::Applied;
                if (srcIsInput) {
                    srcIsInput = false;
                    srcIsA = false;
                } else {
                    srcIsA = !srcIsA;
                }
            } else if (ovOutcome == Gpu::GpuOverlayOutcome::Bypass) {
                report.overlayStatus = StageStatus::Bypassed;
            } else if (ovOutcome == Gpu::GpuOverlayOutcome::NotReady) {
                report.overlayStatus = StageStatus::NotReady;
            } else if (ovOutcome == Gpu::GpuOverlayOutcome::DeviceLost) {
                result.isDeviceLost = true;
                result.error = overlayError.empty() ? "Overlay engine encountered device loss" : overlayError;
                return result;
            } else {
                report.overlayStatus = StageStatus::Failed;
                result.error = overlayError.empty() ? "Overlay engine shader error" : overlayError;
                return result;
            }
        } else {
            report.overlayStatus = StageStatus::NotReady;
        }
    }

    // Determine final output texture and SRV
    if (srcIsInput) {
        // Neither stage modified the input. Copy to texA if distinct.
        if (initialInputTex && initialInputTex != m_gpu->texA.Get()) {
            D3D11_TEXTURE2D_DESC inDesc{};
            initialInputTex->GetDesc(&inDesc);
            D3D11_TEXTURE2D_DESC aDesc{};
            m_gpu->texA->GetDesc(&aDesc);
            if (inDesc.Format == aDesc.Format) {
                m_gpu->context->CopyResource(m_gpu->texA.Get(), initialInputTex);
            } else {
                Appearance::AppearanceConfig bypassCfg;
                bypassCfg.bypass = true;
                m_gpu->appearanceProcessor->Process(
                    initialInputSRV, m_gpu->uavA.Get(), nullptr, nullptr, width, height, bypassCfg);
            }
        }
        result.outputTexture = m_gpu->texA.Get();
        result.outputSRV = m_gpu->srvA.Get();
    } else {
        result.outputTexture = srcIsA ? m_gpu->texA.Get() : m_gpu->texB.Get();
        result.outputSRV = srcIsA ? m_gpu->srvA.Get() : m_gpu->srvB.Get();
    }

    result.success = true;
    return result;
}

// -------------------------------------------------------------
// Main Frame Processing Entry Point
// -------------------------------------------------------------

void StudioStack::ResetTrackedAccessories() {
    std::lock_guard<std::mutex> lock(m_executionMutex);
    m_faceAccessoryRenderer.Reset();
    m_trackingEvaluator.Reset();
}

AVFrame* StudioStack::ProcessFrame(
    AVFrame* inputFrame,
    uint64_t sequenceId,
    int64_t timestampPts,
    StackExecutionReport* outReport,
    uint64_t frameSourceGeneration,
    std::shared_ptr<const AnalysisBridge::AnalysisBundle> lightingGeometry,
    std::shared_ptr<const AnalysisBridge::AnalysisBundle> appearanceGeometry,
    std::shared_ptr<const AnalysisBridge::AnalysisBundle> accessoryGeometry)
{
    StackExecutionReport report;
    report.frameSequence = sequenceId;
    report.timestampPts = timestampPts;

    // 1. Validate input frame pointer, format, and dimensions
    if (!inputFrame || !inputFrame->data[0] || inputFrame->width <= 0 || inputFrame->height <= 0 || inputFrame->width > 8192 || inputFrame->height > 8192) {
        report.errorDescription = "Invalid frame pointer or dimensions";
        report.analysisStatus = StageStatus::Failed;
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        report.overlayStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return nullptr;
    }

    if (inputFrame->format != AV_PIX_FMT_BGR24) {
        report.errorDescription = "Unsupported pixel format (only BGR24 supported)";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        report.overlayStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return nullptr;
    }

    if (inputFrame->linesize[0] < inputFrame->width * 3) {
        report.errorDescription = "Invalid frame stride (linesize < width * 3)";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return nullptr;
    }

    // Reject HDR transfers in 8-bit SDR pipeline
    if (inputFrame->color_trc == AVCOL_TRC_SMPTE2084 || inputFrame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        report.errorDescription = "Unsupported HDR transfer characteristics in SDR pipeline";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return nullptr;
    }

    // 2. Reject concurrent or recursive processing on the same stack
    bool expectedProcessing = false;
    if (!m_isProcessing.compare_exchange_strong(expectedProcessing, true)) {
        report.errorDescription = "Unsupported concurrent or recursive ProcessFrame on single StudioStack";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return nullptr;
    }

    struct ProcessingGuard {
        std::atomic<bool>& flag;
        ~ProcessingGuard() { flag.store(false); }
    } guard{ m_isProcessing };

    auto t0 = std::chrono::steady_clock::now();

    // 3. Take atomic immutable snapshot of configuration and consumers
    Presets::StudioPreset snapPreset;
    uint64_t snapSourceGen = 1;
    FallbackPolicy snapPolicy = FallbackPolicy::FallbackToOriginalFrame;
    ExecutionBackend snapBackend = ExecutionBackend::CpuOnly;
    std::shared_ptr<AnalysisBridge::AnalysisMailbox> snapMailbox;
    std::shared_ptr<const CubeLut> snapLut, snapInputLut;
    std::unordered_map<std::string, FrameConsumerCallback> snapConsumers;

    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        snapPreset = m_currentPreset;
        snapSourceGen = m_sourceGeneration;
        snapPolicy = m_fallbackPolicy;
        snapBackend = m_backend;
        snapMailbox = m_analysisMailbox;
        snapLut = m_activeLut;
        snapInputLut = m_inputLut;
        snapConsumers = m_consumers;
    }

    report.sourceGeneration = snapSourceGen;

    // 4. Suppress stale frames from superseded source generations
    if (frameSourceGeneration != 0 && frameSourceGeneration != snapSourceGen) {
        report.errorDescription = "Stale input frame rejected (source generation mismatch)";
        report.analysisStatus = StageStatus::Bypassed;
        report.inputLookStatus=StageStatus::Bypassed; report.inputLutStatus=StageStatus::Bypassed;
        report.appearanceStatus = StageStatus::Bypassed;
        report.lightingStatus = StageStatus::Bypassed;
        report.backgroundStatus = StageStatus::Bypassed;
        report.lutStatus = StageStatus::Bypassed;
        report.overlayStatus = StageStatus::Bypassed;
        if (outReport) *outReport = report;
        return nullptr;
    }

    // 5. Fast-path bypass if entire preset is bypassed
    if (snapPreset.bypassAll) {
        m_trackingEvaluator.Reset();
        report.inputLookStatus=StageStatus::Bypassed; report.inputLutStatus=StageStatus::Bypassed;
        report.appearanceStatus = StageStatus::Bypassed;
        report.lightingStatus = StageStatus::Bypassed;
        report.backgroundStatus = StageStatus::Bypassed;
        report.lutStatus = StageStatus::Bypassed;
        report.overlayStatus = StageStatus::Bypassed;

        auto t1 = std::chrono::steady_clock::now();
        report.totalLatencyMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Dispatch outside configuration lock
        for (const auto& kv : snapConsumers) {
            if (kv.second) {
                try {
                    kv.second(inputFrame, report);
                } catch (...) {
                    // Prevent consumer exceptions from propagating
                }
            }
        }
        if (outReport) *outReport = report;
        return inputFrame;
    }

    const int frameW = inputFrame->width;
    const int frameH = inputFrame->height;

    // 6. Stage 1: Analysis Alignment
    std::shared_ptr<const AnalysisBridge::AnalysisBundle> analysisBundle;
    if (snapMailbox) {
        AnalysisBridge::AnalysisAlignmentStatus alignStatus = AnalysisBridge::AnalysisAlignmentStatus::NotReady;
        analysisBundle = snapMailbox->GetAlignedAnalysis(
            snapSourceGen, sequenceId, timestampPts, alignStatus);

        if (analysisBundle) {
            report.analysisStatus = StageStatus::Applied;
        } else if (alignStatus == AnalysisBridge::AnalysisAlignmentStatus::NotReady) {
            report.analysisStatus = StageStatus::NotReady;
        } else {
            report.analysisStatus = StageStatus::Bypassed;
        }
    } else {
        report.analysisStatus = StageStatus::Bypassed;
    }

    AVFrame* currentFrame = inputFrame;
    std::unique_lock<std::mutex> execLock(m_executionMutex);

    if (m_lastFrameW != frameW || m_lastFrameH != frameH ||
        (m_lastTimestampPts != -1 && timestampPts < m_lastTimestampPts))
    {
        m_maskPreparerPerson.Reset();
        m_maskPreparerFace.Reset();
        m_maskPreparerSkin.Reset();
        m_maskPreparerProt.Reset();
        m_normalPreparer.Reset();
        if (m_gpu) {
            m_gpu->gpuMaskPreparerPerson.Reset();
            m_gpu->gpuMaskPreparerFace.Reset();
            m_gpu->gpuMaskPreparerSkin.Reset();
            m_gpu->gpuMaskPreparerProt.Reset();
            m_gpu->gpuNormalPreparer.Reset();
        }
    }
    m_lastFrameW = frameW;
    m_lastFrameH = frameH;
    m_lastTimestampPts = timestampPts;

    auto handleStageFailure = [&](StageStatus& stageStatus) -> bool {
        stageStatus = StageStatus::Failed;
        if (snapPolicy == FallbackPolicy::FallbackToOriginalFrame) {
            currentFrame = inputFrame;
            report.fallbackToOriginal = true;
            return false;
        } else if (snapPolicy == FallbackPolicy::AbortOnError) {
            currentFrame = nullptr;
            return false;
        }
        return true;
    };

    // -------------------------------------------------------------
    // GPU Combined Processing Route (with fallback to CPU)
    // -------------------------------------------------------------
    bool gpuSucceeded = false;
    GpuFailureTarget initFailTarget = m_gpuFailureTarget.load();
    if (initFailTarget == GpuFailureTarget::ResourceCreation) {
        report.errorDescription = "Simulated GPU resource creation failure (injected failure point)";
    } else if (snapBackend == ExecutionBackend::GpuWithCpuFallback && m_gpu && m_gpu->device) {
        std::string ensureError;
        bool isDevLost = false;
        if (m_gpu->EnsureTextures(frameW, frameH, m_gpuFaultSeam.load(), m_gpuFaultHResult.load(), ensureError, isDevLost)) {
            auto cameraUploadStart=std::chrono::steady_clock::now();
            // Upload BGR24 input to uploadStaging texture -> texA
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_gpu->context->Map(m_gpu->uploadStaging.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                report.imageUploadCount++;
                for (int y = 0; y < frameH; ++y) {
                    const uint8_t* srcRow = inputFrame->data[0] + y * inputFrame->linesize[0];
                    uint8_t* dstRow = static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                    for (int x = 0; x < frameW; ++x) {
                        dstRow[x * 4 + 0] = srcRow[x * 3 + 2]; // R
                        dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                        dstRow[x * 4 + 2] = srcRow[x * 3 + 0]; // B
                        dstRow[x * 4 + 3] = 255;               // A
                    }
                }
                m_gpu->context->Unmap(m_gpu->uploadStaging.Get(), 0);
                m_gpu->context->CopyResource(m_gpu->texA.Get(), m_gpu->uploadStaging.Get());

                report.uploadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cameraUploadStart).count();
                auto tGpuStart = std::chrono::steady_clock::now();
                GpuPipelineResult pipeRes = ExecuteGpuStages(
                    m_gpu->srvA.Get(),
                    m_gpu->texA.Get(),
                    frameW, frameH,
                    snapPreset,
                    snapLut,
                    snapInputLut,
                    analysisBundle.get(),
                    report,
                    lightingGeometry.get(), appearanceGeometry.get(), accessoryGeometry.get()
                );
                auto tGpuEnd = std::chrono::steady_clock::now();
                report.gpuDispatchMs = std::chrono::duration<double, std::milli>(tGpuEnd - tGpuStart).count();
                report.effectDispatchMs = report.gpuDispatchMs;

                if (pipeRes.success && pipeRes.outputTexture) {
                    GpuFailureTarget failTarget = m_gpuFailureTarget.load();
                    GpuFaultSeam faultSeam = m_gpuFaultSeam.load();
                    if (failTarget == GpuFailureTarget::Readback || faultSeam == GpuFaultSeam::Readback_Map) {
                        pipeRes.success = false;
                        pipeRes.error = (faultSeam == GpuFaultSeam::Readback_Map)
                            ? ("Simulated fault at readback map boundary (HRESULT 0x" + HrToHex(m_gpuFaultHResult.load()) + ")")
                            : "Simulated GPU readback failure (injected failure point)";
                        report.errorDescription = pipeRes.error;
                    } else {
                        // Readback exactly once
                        m_gpu->context->CopyResource(m_gpu->readbackStaging.Get(), pipeRes.outputTexture);
                        D3D11_MAPPED_SUBRESOURCE rbMap{};
                        hr = m_gpu->context->Map(m_gpu->readbackStaging.Get(), 0, D3D11_MAP_READ, 0, &rbMap);
                        if (SUCCEEDED(hr)) {
                            report.imageReadbackCount++;

                            bool writable=true;
                            if(!m_readbackFrame || m_readbackFrame->width!=frameW || m_readbackFrame->height!=frameH) {
                                AVFrame* replacement=av_frame_alloc();
                                if(replacement) {replacement->width=frameW;replacement->height=frameH;replacement->format=AV_PIX_FMT_BGR24;}
                                if(!replacement || av_frame_get_buffer(replacement,32)<0) {
                                    if(replacement)av_frame_free(&replacement);writable=false;
                                } else {if(m_readbackFrame)av_frame_free(&m_readbackFrame);m_readbackFrame=replacement;}
                            }
                            if(writable && av_frame_make_writable(m_readbackFrame)<0) writable=false;
                            if(writable && av_frame_copy_props(m_readbackFrame,inputFrame)<0) writable=false;
                            if(writable) for(int y=0;y<frameH;++y) {
                                const auto* srcRow=static_cast<const uint8_t*>(rbMap.pData)+static_cast<size_t>(y)*rbMap.RowPitch;
                                auto* dstRow=m_readbackFrame->data[0]+static_cast<ptrdiff_t>(y)*m_readbackFrame->linesize[0];
                                for(int x=0;x<frameW;++x) {dstRow[x*3]=srcRow[x*4+2];dstRow[x*3+1]=srcRow[x*4+1];dstRow[x*3+2]=srcRow[x*4];}
                            }
                            m_gpu->context->Unmap(m_gpu->readbackStaging.Get(), 0);

                            if(writable){currentFrame=m_readbackFrame;gpuSucceeded=true;}
                            else report.errorDescription="GPU readback frame allocation/metadata copy failed";
                        }
                    }
                } else {
                    report.errorDescription = pipeRes.error.empty() ? "GPU pipeline stage failed" : pipeRes.error;
                }
            }
        } else {
            report.errorDescription = ensureError;
        }
    }

    if (!gpuSucceeded && snapBackend == ExecutionBackend::GpuWithCpuFallback) {
        report.gpuFallbackUsed = true;
    }

    // -------------------------------------------------------------
    // CPU Stage Processing (Direct CPU route or fallback from GPU)
    // -------------------------------------------------------------
    if (!gpuSucceeded) {
        auto tCpuEffectStart = std::chrono::steady_clock::now();
        const bool isProfiling = m_stageProfilingEnabled.load();
        // Stage 1: Input Colour Look (CPU oracle â€” matches GPU shader exactly)
        {
            const Gpu::DesktopLook look = static_cast<Gpu::DesktopLook>(
                std::clamp(snapPreset.inputLook, 0, 3));
            const float lookStrength = std::clamp(snapPreset.inputLookStrength, 0.0f, 1.0f);

            const bool lookActive = (!snapPreset.bypassAll &&
                                     look != Gpu::DesktopLook::Original &&
                                     lookStrength > 0.0f);
            if (!lookActive) {
                report.inputLookStatus = StageStatus::Bypassed;
            } else if (currentFrame && currentFrame->data[0] &&
                       currentFrame->format == AV_PIX_FMT_BGR24 &&
                       currentFrame->width > 0 && currentFrame->height > 0)
            {
                // A private, owning COW frame: caller storage/properties survive look processing;
                // replacement is committed only after refs and writable allocation succeed.
                AVFrame* workFrame=av_frame_alloc();
                // A valid packed-video descriptor may omit its auxiliary
                // extended_data array. Ref a local view with standard video
                // planes instead; never change the caller's descriptor.
                AVFrame videoView=*currentFrame;
                videoView.extended_data=videoView.data;
                if(!workFrame || av_frame_ref(workFrame,&videoView)<0 || av_frame_make_writable(workFrame)<0) {
                    if(workFrame) av_frame_free(&workFrame);
                    if(!handleStageFailure(report.inputLookStatus)) goto FinishExecution;
                } else {
                    for(int y=0;y<workFrame->height;++y) {
                        uint8_t* row=workFrame->data[0]+static_cast<ptrdiff_t>(y)*workFrame->linesize[0];
                        for(int x=0;x<workFrame->width;++x) {
                            auto* px=row+static_cast<ptrdiff_t>(x)*3;
                            Gpu::DesktopLookApplyPixelCpu(px[2],px[1],px[0],look,lookStrength);
                        }
                    }
                    if(m_cpuLookScratchFrame) av_frame_free(&m_cpuLookScratchFrame);
                    m_cpuLookScratchFrame=workFrame; currentFrame=workFrame;
                    report.inputLookStatus=StageStatus::Applied;
                }
            } else {
                if(!handleStageFailure(report.inputLookStatus)) goto FinishExecution;
            }
        }
        if(snapInputLut && snapPreset.inputLutIntensity>0) {
            auto* graded=m_inputLutProcessor.Process(currentFrame,snapInputLut.get(),snapPreset.inputLutIntensity);
            if(graded && graded!=currentFrame) {currentFrame=graded;report.inputLutStatus=StageStatus::Applied;}
            else if(!handleStageFailure(report.inputLutStatus)) goto FinishExecution;
        } else report.inputLutStatus=StageStatus::Bypassed;

        // Stage 2: Appearance Processing (smoothing + detail + sharpening)
        const bool useExactAppearance=appearanceGeometry!=nullptr;
        const auto* appearanceAnalysis=useExactAppearance?appearanceGeometry.get():analysisBundle.get();
        if (snapPreset.appearance.bypass ||
            (snapPreset.appearance.smoothingStrength <= 0.001f && snapPreset.appearance.sharpenStrength <= 0.001f))
        {
            report.appearanceStatus = StageStatus::Bypassed;
        } else {
            if (useExactAppearance?ExactAppearanceGeometryReady(appearanceAnalysis,report):
                (appearanceAnalysis && appearanceAnalysis->skinMask.IsValid())) {
                AnalysisBridge::MaskPreparationConfig prepCfgSkin;
                prepCfgSkin.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                prepCfgSkin.featherRadius = useExactAppearance?0.0f:2.0f;
                prepCfgSkin.enableTemporalStabilization = false;

                bool skinOk = false;
                auto prepSkinStart = std::chrono::steady_clock::now();
                if (m_maskPrepFaultInjected.load() || !m_maskPreparerSkin.PrepareMask(appearanceAnalysis->skinMask, frameW, frameH, prepCfgSkin, m_scratchSkin)) {
                    if (!handleStageFailure(report.appearanceStatus)) goto FinishExecution;
                } else {
                    skinOk = true;
                    report.maskPreparationCount++;
                    if (isProfiling) {
                        report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepSkinStart).count();
                    }
                }

                if (skinOk) {
                    Appearance::SkinMask skinDesc;
                    skinDesc.data = m_scratchSkin.data.data();
                    skinDesc.width = m_scratchSkin.width;
                    skinDesc.height = m_scratchSkin.height;
                    skinDesc.strideFloats = m_scratchSkin.strideFloats;

                    Appearance::ProtectionMask protDesc;
                    Appearance::ProtectionMask* protPtr = nullptr;
                    bool protectionOk=true;
                    if (appearanceAnalysis->protectionMask.IsValid()) {
                        AnalysisBridge::MaskPreparationConfig prepCfgProt;
                        prepCfgProt.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                        prepCfgProt.featherRadius = useExactAppearance?0.0f:1.0f;
                        prepCfgProt.enableTemporalStabilization = false;

                        auto prepProtStart = std::chrono::steady_clock::now();
                        if (!m_maskPrepFaultInjected.load() && m_maskPreparerProt.PrepareMask(appearanceAnalysis->protectionMask, frameW, frameH, prepCfgProt, m_scratchProt)) {
                            protDesc.data = m_scratchProt.data.data();
                            protDesc.width = m_scratchProt.width;
                            protDesc.height = m_scratchProt.height;
                            protDesc.strideFloats = m_scratchProt.strideFloats;
                            protPtr = &protDesc;
                            report.maskPreparationCount++;
                            if (isProfiling) {
                                report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepProtStart).count();
                            }
                        } else if(useExactAppearance) {
                            // Protection preparation failure cannot enable unprotected smoothing.
                            protectionOk=false;
                        }
                    }

                    if(!protectionOk) {
                        if(!handleStageFailure(report.appearanceStatus))goto FinishExecution;
                    } else {
                    Appearance::AppearanceOutcome outcome;
                    AVFrame* appFrame = m_appearanceProcessor.ProcessFrame(
                        currentFrame,
                        &skinDesc,
                        protPtr,
                        snapPreset.appearance,
                        outcome);

                    if (outcome == Appearance::AppearanceOutcome::Success && appFrame) {
                        currentFrame = appFrame;
                        report.appearanceStatus = StageStatus::Applied;
                    } else if (outcome == Appearance::AppearanceOutcome::Bypass) {
                        report.appearanceStatus = StageStatus::Bypassed;
                    } else {
                        if (!handleStageFailure(report.appearanceStatus)) goto FinishExecution;
                    }
                    }
                }
            } else {
                report.appearanceStatus = StageStatus::NotReady;
            }
        }

        // Stage 3: Two-Light Studio Engine
        bool personMaskPreparedCpu = false;
        bool normalPreparedCpu = false;
        report.effectiveLightingMode = snapPreset.lighting.lightingMode;
        const bool isNormalGuided = (snapPreset.lighting.lightingMode == Lighting::LightingMode::NormalGuided);
        const bool useExactGeometry = isNormalGuided && lightingGeometry;
        const auto* lightAnalysis = useExactGeometry ? lightingGeometry.get() : analysisBundle.get();
        auto lightConfig = snapPreset.lighting;
        if (useExactGeometry) lightConfig.subjectOnlyMode = true;

        if (snapPreset.lighting.bypass ||
            (!snapPreset.lighting.light1.enabled &&
             !snapPreset.lighting.light2.enabled &&
             snapPreset.lighting.fillIntensity <= 0.001f))
        {
            report.lightingStatus = StageStatus::Bypassed;
        } else {
            const bool hasValidNormals = useExactGeometry
                ? ExactLightingGeometryReady(lightAnalysis, report, lightConfig.shadowConfig.enabled)
                : (lightAnalysis && lightAnalysis->sourceGeneration == report.sourceGeneration &&
                   lightAnalysis->sequenceId == report.frameSequence &&
                   lightAnalysis->timestampPts == report.timestampPts && lightAnalysis->HasNormals());

            if (isNormalGuided && !hasValidNormals) {
                report.lightingStatus = StageStatus::NotReady;
            } else {
                const Lighting::NormalField* pNormDesc = nullptr;
                Lighting::NormalField normDesc;
                if (hasValidNormals) {
                    if (!normalPreparedCpu) {
                        AnalysisBridge::NormalPreparationConfig normConfig;
                        normConfig.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                        if (m_normalPreparer.PrepareNormals(lightAnalysis->normalMap, frameW, frameH, normConfig, m_scratchNormal)) {
                            normalPreparedCpu = true;
                            report.normalPreparationCount++;
                        }
                    }
                    if (normalPreparedCpu) {
                        normDesc.data = m_scratchNormal.data.data();
                        normDesc.width = m_scratchNormal.width;
                        normDesc.height = m_scratchNormal.height;
                        normDesc.strideFloats = m_scratchNormal.strideFloats;
                        pNormDesc = &normDesc;
                    }
                }

                if (lightConfig.subjectOnlyMode) {
                    if (lightAnalysis && lightAnalysis->personMask.IsValid()) {
                        bool maskOk = !useExactGeometry && personMaskPreparedCpu;
                        if (useExactGeometry || !personMaskPreparedCpu) {
                            AnalysisBridge::MaskPreparationConfig prepCfgPerson;
                            prepCfgPerson.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                            prepCfgPerson.featherRadius = useExactGeometry ? 0.0f : 3.0f;
                            prepCfgPerson.enableTemporalStabilization = !useExactGeometry;

                            auto prepStart = std::chrono::steady_clock::now();
                            if (m_maskPrepFaultInjected.load() ||
                                !(useExactGeometry ? m_maskPreparerFace : m_maskPreparerPerson).PrepareMask(
                                    lightAnalysis->personMask, frameW, frameH, prepCfgPerson,
                                    useExactGeometry ? m_scratchFace : m_scratchPerson)) {
                                if (!handleStageFailure(report.lightingStatus)) goto FinishExecution;
                                maskOk = false;
                            } else {
                                if (!useExactGeometry) personMaskPreparedCpu = true;
                                maskOk = true;
                                report.maskPreparationCount++;
                                if (isProfiling) {
                                    report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart).count();
                                }
                            }
                        } else {
                            report.maskReuseCount++;
                        }

                        Lighting::DepthField depthDesc;
                        const Lighting::DepthField* pDepthDesc = nullptr;
                        if (lightConfig.shadowConfig.enabled && lightAnalysis && lightAnalysis->HasDepth()) {
                            const auto prep = Relighting::PrepareShadowDepth(lightAnalysis->depthMap, frameW, frameH, m_scratchShadowDepth);
                            if (prep == Relighting::ShadowDepthPreparation::OutOfMemory) {
                                report.errorDescription = "Shadow depth preparation allocation failed";
                                if (!handleStageFailure(report.lightingStatus)) goto FinishExecution;
                            } else if (prep == Relighting::ShadowDepthPreparation::Success) {
                                depthDesc.data = m_scratchShadowDepth.data.data();
                                depthDesc.width = frameW; depthDesc.height = frameH;
                                depthDesc.strideFloats = frameW; pDepthDesc = &depthDesc;
                            }
                        }

                        if (maskOk && report.lightingStatus != StageStatus::Failed) {
                            Lighting::AlphaMask maskDesc;
                            const auto& lightMask = useExactGeometry ? m_scratchFace : m_scratchPerson;
                            maskDesc.data = lightMask.data.data();
                            maskDesc.width = lightMask.width;
                            maskDesc.height = lightMask.height;
                            maskDesc.strideFloats = lightMask.strideFloats;

                            Lighting::LightingOutcome outcome;
                            AVFrame* litFrame = m_lightingEngine.ProcessFrame(
                                currentFrame,
                                &maskDesc,
                                pNormDesc,
                                pDepthDesc,
                                lightConfig,
                                outcome);

                            if (outcome == Lighting::LightingOutcome::Success && litFrame) {
                                currentFrame = litFrame;
                                report.lightingStatus = StageStatus::Applied;
                            } else if (outcome == Lighting::LightingOutcome::Bypass) {
                                report.lightingStatus = StageStatus::Bypassed;
                            } else if (outcome == Lighting::LightingOutcome::NotReady) {
                                report.lightingStatus = StageStatus::NotReady;
                            } else {
                                if (!handleStageFailure(report.lightingStatus)) goto FinishExecution;
                            }
                        }
                    } else {
                        report.lightingStatus = StageStatus::NotReady;
                    }
                } else {
                    Lighting::DepthField depthDesc;
                    const Lighting::DepthField* pDepthDesc = nullptr;
                    if (lightConfig.shadowConfig.enabled && lightAnalysis && lightAnalysis->HasDepth()) {
                        const auto prep = Relighting::PrepareShadowDepth(lightAnalysis->depthMap, frameW, frameH, m_scratchShadowDepth);
                        if (prep == Relighting::ShadowDepthPreparation::OutOfMemory) {
                            report.errorDescription = "Shadow depth preparation allocation failed";
                            if (!handleStageFailure(report.lightingStatus)) goto FinishExecution;
                        } else if (prep == Relighting::ShadowDepthPreparation::Success) {
                            depthDesc.data = m_scratchShadowDepth.data.data();
                            depthDesc.width = frameW; depthDesc.height = frameH;
                            depthDesc.strideFloats = frameW; pDepthDesc = &depthDesc;
                        }
                    }

                    Lighting::LightingOutcome outcome = Lighting::LightingOutcome::OutOfMemory;
                    AVFrame* litFrame = report.lightingStatus == StageStatus::Failed ? nullptr : m_lightingEngine.ProcessFrame(
                        currentFrame,
                        nullptr,
                        pNormDesc,
                        pDepthDesc,
                        lightConfig,
                        outcome);

                    if (outcome == Lighting::LightingOutcome::Success && litFrame) {
                        currentFrame = litFrame;
                        report.lightingStatus = StageStatus::Applied;
                    } else if (outcome == Lighting::LightingOutcome::Bypass) {
                        report.lightingStatus = StageStatus::Bypassed;
                    } else if (outcome == Lighting::LightingOutcome::NotReady) {
                        report.lightingStatus = StageStatus::NotReady;
                    } else {
                        if (!handleStageFailure(report.lightingStatus)) goto FinishExecution;
                    }
                }
            }
        }

        // Stage 4: Background Compositing (CPU fallback / CPU path)
        Presets::StudioBackgroundMode effectiveBgMode = snapPreset.backgroundMode;
        if (effectiveBgMode == Presets::StudioBackgroundMode::Original &&
            snapPreset.background.mode != BackgroundMode::Original)
        {
            if (snapPreset.background.mode == BackgroundMode::Blur) {
                effectiveBgMode = Presets::StudioBackgroundMode::Blur;
            } else if (snapPreset.background.mode == BackgroundMode::Solid) {
                effectiveBgMode = Presets::StudioBackgroundMode::Solid;
            }
        }

        bool isImageMode = (effectiveBgMode == Presets::StudioBackgroundMode::Image);
        bool isOriginalMode = (effectiveBgMode == Presets::StudioBackgroundMode::Original);

        if (isOriginalMode) {
            report.backgroundStatus = StageStatus::Bypassed;
        } else {
            if (analysisBundle && analysisBundle->personMask.IsValid()) {
                bool maskOk = personMaskPreparedCpu;
                if (!personMaskPreparedCpu) {
                    AnalysisBridge::MaskPreparationConfig prepCfgPerson;
                    prepCfgPerson.resizeMode = AnalysisBridge::MaskResizeMode::Stretch;
                    prepCfgPerson.featherRadius = 3.0f;
                    prepCfgPerson.enableTemporalStabilization = true;
                    auto prepStart = std::chrono::steady_clock::now();
                    if (m_maskPrepFaultInjected.load() || !m_maskPreparerPerson.PrepareMask(analysisBundle->personMask, frameW, frameH, prepCfgPerson, m_scratchPerson)) {
                        if (!handleStageFailure(report.backgroundStatus)) goto FinishExecution;
                        maskOk = false;
                    } else {
                        personMaskPreparedCpu = true;
                        maskOk = true;
                        report.maskPreparationCount++;
                        if (isProfiling) {
                            report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart).count();
                        }
                    }
                } else {
                    report.maskReuseCount++;
                }

                if (maskOk) {
                    AlphaMask bgMask;
                    bgMask.data = m_scratchPerson.data.data();
                    bgMask.width = m_scratchPerson.width;
                    bgMask.height = m_scratchPerson.height;
                    bgMask.strideFloats = m_scratchPerson.strideFloats;

                    if (isImageMode) {
                        if (!m_cpuBgFrame) {
                            report.errorDescription = "Background image CPU fallback asset unavailable";
                            if (!handleStageFailure(report.backgroundStatus)) goto FinishExecution;
                        } else {
                            AVFrame* compResult = ComposeImageBackgroundCpu(
                                currentFrame,
                                &bgMask,
                                snapPreset.backgroundFitMode);

                            if (compResult) {
                                currentFrame = compResult;
                                report.backgroundStatus = StageStatus::Applied;
                            } else {
                                if (!handleStageFailure(report.backgroundStatus)) goto FinishExecution;
                            }
                        }
                    } else {
                        BackgroundSettings bgSettings = snapPreset.background;
                        if (effectiveBgMode == Presets::StudioBackgroundMode::Blur) {
                            bgSettings.mode = BackgroundMode::Blur;
                        } else if (effectiveBgMode == Presets::StudioBackgroundMode::Solid) {
                            bgSettings.mode = BackgroundMode::Solid;
                        }

                        AVFrame* compResult = m_backgroundCompositor.Process(
                            currentFrame,
                            &bgMask,
                            bgSettings);

                        if (compResult) {
                            currentFrame = compResult;
                            report.backgroundStatus = StageStatus::Applied;
                        } else {
                            if (!handleStageFailure(report.backgroundStatus)) goto FinishExecution;
                        }
                    }
                }
            } else {
                report.backgroundStatus = StageStatus::NotReady;
            }
        }

        // Stage 5: Cube LUT Creative Grade (CPU fallback / CPU path)
        if (snapLut && snapPreset.lutIntensity > 0.0f) {
            AVFrame* lutResult = m_cubeLutProcessor.Process(
                currentFrame,
                snapLut.get(),
                snapPreset.lutIntensity);

            if (lutResult) {
                currentFrame = lutResult;
                report.lutStatus = StageStatus::Applied;
            } else {
                if (!handleStageFailure(report.lutStatus)) goto FinishExecution;
            }
        } else {
            report.lutStatus = StageStatus::Bypassed;
        }

        // Stage 6: Tracked Overlays & Procedural Accessories (CPU fallback / CPU path)
        if (snapPreset.overlays.bypass ||
            (!snapPreset.overlays.glasses.enabled &&
             !snapPreset.overlays.hat.enabled &&
             !snapPreset.overlays.studioFrame.enabled && snapPreset.faceFilterStyle==0))
        {
            m_trackingEvaluator.Reset();
            report.overlayStatus = StageStatus::Bypassed;
        } else {
            Overlays::OverlayOutcome ovOutcome = Overlays::OverlayOutcome::Bypass;
            AVFrame* ovFrame = nullptr;

            const auto* overlaySource=accessoryGeometry?accessoryGeometry.get():analysisBundle.get();
            const auto* overlayAnalysis=Overlays::MatchesFrame(overlaySource,report.sourceGeneration,report.frameSequence,report.timestampPts)?overlaySource:nullptr;
            if(accessoryGeometry && (!overlayAnalysis || !overlayAnalysis->faceLandmarks.NativeGeometryMatches(frameW,frameH,
                report.sourceGeneration,report.frameSequence,report.timestampPts))) {overlayAnalysis=nullptr;m_trackingEvaluator.Reset();}
            const bool behind = (snapPreset.overlays.glasses.enabled && snapPreset.overlays.glasses.placement == Overlays::OverlayPlacement::BehindSubject) ||
                (snapPreset.overlays.hat.enabled && snapPreset.overlays.hat.placement == Overlays::OverlayPlacement::BehindSubject);
            const AnalysisBridge::MaskBuffer* overlayMask = nullptr;
            if (behind && !accessoryGeometry && overlayAnalysis && Overlays::ValidMask(overlayAnalysis->personMask)) {
                if (!personMaskPreparedCpu) {
                    AnalysisBridge::MaskPreparationConfig config;
                    config.resizeMode = AnalysisBridge::MaskResizeMode::Stretch; config.featherRadius=3;
                    config.enableTemporalStabilization = true;
                    auto prepStart = std::chrono::steady_clock::now();
                    if (m_maskPrepFaultInjected.load() || !m_maskPreparerPerson.PrepareMask(overlayAnalysis->personMask,frameW,frameH,config,m_scratchPerson)) {
                        if (!handleStageFailure(report.overlayStatus)) goto FinishExecution;
                    } else {
                        personMaskPreparedCpu = true;
                        overlayMask = &m_scratchPerson;
                        report.maskPreparationCount++;
                        if (isProfiling) {
                            report.maskPrepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart).count();
                        }
                    }
                } else {
                    overlayMask = &m_scratchPerson;
                    report.maskReuseCount++;
                }
            }
            bool failedFilterWithFrame=false;
            if (snapPreset.faceFilterStyle>0 || (snapPreset.realisticAccessories && (snapPreset.overlays.glasses.enabled || snapPreset.overlays.hat.enabled))) {
                ovFrame=m_faceAccessoryRenderer.ProcessFrame(currentFrame,
                    overlayAnalysis ? &overlayAnalysis->faceLandmarks : nullptr,
                    snapPreset.faceFilterStyle==0&&snapPreset.overlays.glasses.enabled,snapPreset.faceFilterStyle==0&&snapPreset.overlays.hat.enabled,
                    snapPreset.overlays.glasses.enabled?snapPreset.overlays.glasses.scaleMultiplier:snapPreset.overlays.hat.scaleMultiplier,
                    &ovOutcome,&report.overlayLayers,snapPreset.realisticCapStyle,snapPreset.realisticGlassesStyle,snapPreset.realisticCapScale,snapPreset.realisticCapVerticalOffset,
                    snapPreset.faceFilterStyle,snapPreset.faceFilterScale,snapPreset.faceFilterVerticalOffset,snapPreset.faceFilterCustomModel,snapPreset.faceFilterCustomFit,snapPreset.faceFilterRenderEdge);
                if (report.overlayLayers.faceFilter==3 && !m_faceAccessoryRenderer.GetLastError().empty())
                    report.errorDescription=m_faceAccessoryRenderer.GetLastError();
                // Invalid model/fit preparation returns the exact valid input;
                // invalid planes or allocation failures return null and cannot recover here.
                const bool filterBuildFailed=ovOutcome==Overlays::OverlayOutcome::InvalidInput &&
                    ovFrame==currentFrame && report.overlayLayers.faceFilter==3;
                if ((ovOutcome==Overlays::OverlayOutcome::Success || ovOutcome==Overlays::OverlayOutcome::NotReady || filterBuildFailed) &&
                    ovFrame && snapPreset.overlays.studioFrame.enabled) {
                    auto frameConfig=snapPreset.overlays;frameConfig.glasses.enabled=false;frameConfig.hat.enabled=false;
                    Overlays::OverlayOutcome frameOutcome;
                    auto* framed=m_overlayRenderer.ProcessFrame(ovFrame,frameConfig,nullptr,nullptr,&frameOutcome,
                        nullptr,report.sourceGeneration,report.timestampPts,report.frameSequence);
                    report.overlayLayers.frame=frameOutcome==Overlays::OverlayOutcome::Success?0:
                        frameOutcome==Overlays::OverlayOutcome::Bypass?1:frameOutcome==Overlays::OverlayOutcome::NotReady?2:3;
                    if (frameOutcome==Overlays::OverlayOutcome::Success || report.overlayLayers.frame==3) {
                        failedFilterWithFrame=filterBuildFailed && frameOutcome==Overlays::OverlayOutcome::Success && framed;
                        ovFrame=framed;ovOutcome=frameOutcome;
                    }
                }
            } else {
            ovFrame = m_overlayRenderer.ProcessFrame(
                currentFrame, snapPreset.overlays,
                overlayAnalysis ? &overlayAnalysis->faceLandmarks : nullptr,
                overlayMask,
                &ovOutcome, &m_trackingEvaluator, report.sourceGeneration, report.timestampPts, report.frameSequence);
            Overlays::LayerStatus gs, hs, fs;
            m_trackingEvaluator.QueryLayerStatuses(snapPreset.overlays, gs, hs, fs);
            report.overlayLayers = {static_cast<int>(gs), static_cast<int>(hs), static_cast<int>(fs)};
            if (!overlayMask) {
                if (snapPreset.overlays.glasses.enabled && snapPreset.overlays.glasses.placement == Overlays::OverlayPlacement::BehindSubject) report.overlayLayers.glasses=2;
                if (snapPreset.overlays.hat.enabled && snapPreset.overlays.hat.placement == Overlays::OverlayPlacement::BehindSubject) report.overlayLayers.hat=2;
            }

            }
            if (failedFilterWithFrame) {
                currentFrame=ovFrame;report.overlayStatus=StageStatus::Failed;
            } else if (ovOutcome == Overlays::OverlayOutcome::Success && ovFrame) {
                currentFrame = ovFrame;
                report.overlayStatus = StageStatus::Applied;
            } else if (ovOutcome == Overlays::OverlayOutcome::Bypass) {
                report.overlayStatus = StageStatus::Bypassed;
            } else if (ovOutcome == Overlays::OverlayOutcome::NotReady) {
                report.overlayStatus = StageStatus::NotReady;
            } else {
                for (auto* layer:{&report.overlayLayers.glasses,&report.overlayLayers.hat,&report.overlayLayers.frame,&report.overlayLayers.faceFilter}) if (*layer==0) *layer=3;
                if (!handleStageFailure(report.overlayStatus)) goto FinishExecution;
            }
        }
        auto tCpuEffectEnd = std::chrono::steady_clock::now();
        report.effectDispatchMs = std::chrono::duration<double, std::milli>(tCpuEffectEnd - tCpuEffectStart).count();
    }

FinishExecution:
    // Legacy CPU processors retain reusable internal output and can omit input
    // metadata. Snapshot their current pixels at the stack boundary, retaining
    // original properties without modifying the frozen look/LUT implementation.
    if (currentFrame && currentFrame != inputFrame && currentFrame != m_readbackFrame) {
        AVFrame* owned=av_frame_alloc();
        if (owned) {
            owned->format=currentFrame->format;owned->width=currentFrame->width;owned->height=currentFrame->height;
            if (av_frame_get_buffer(owned,32)<0 || av_frame_copy(owned,currentFrame)<0 ||
                av_frame_copy_props(owned,inputFrame)<0) av_frame_free(&owned);
        }
        if (owned) {
            owned->color_range=currentFrame->color_range;
            owned->colorspace=currentFrame->colorspace;
            if (m_cpuOutputFrame) av_frame_free(&m_cpuOutputFrame);
            m_cpuOutputFrame=owned;currentFrame=owned;
        } else {
            report.errorDescription="Cannot retain current CPU output";
            currentFrame=nullptr;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    report.totalLatencyMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    report.completionMs = report.totalLatencyMs;

    // Release execution mutex before executing consumer callbacks
    execLock.unlock();

    // -------------------------------------------------------------
    // Stage 7: Synchronous Fan-Out Delivery (Outside Lock)
    // -------------------------------------------------------------
    if (currentFrame) {
        for (const auto& kv : snapConsumers) {
            if (kv.second) {
                try {
                    kv.second(currentFrame, report);
                } catch (const std::exception& ex) {
                    // Diagnostic log without aborting remaining consumers
                    std::cerr << "[StudioStack] Consumer exception in '" << kv.first << "': " << ex.what() << std::endl;
                } catch (...) {
                    std::cerr << "[StudioStack] Unknown consumer exception in '" << kv.first << "'" << std::endl;
                }
            }
        }
    }

    if (outReport) {
        *outReport = report;
    }
    return currentFrame;
}

GpuTextureSurface StudioStack::ProcessTextureNative(
    ID3D11ShaderResourceView* inputSRV,
    int width, int height,
    uint64_t sequenceId,
    int64_t timestampPts,
    StackExecutionReport* outReport,
    uint64_t frameSourceGeneration)
{
    GpuTextureSurface surf;
    surf.isValid = false;
    surf.texture = nullptr;
    surf.srv = nullptr;
    surf.width = width;
    surf.height = height;

    StackExecutionReport report;
    report.frameSequence = sequenceId;
    report.timestampPts = timestampPts;

    // 1. Basic dimension validation
    if (width <= 0 || height <= 0) {
        report.errorDescription = "Invalid texture dimensions (width <= 0 or height <= 0)";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // 2. Validate input SRV
    if (!inputSRV) {
        report.errorDescription = "Invalid input SRV (null pointer)";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // 3. Concurrency / recursive processing guard
    bool expectedProcessing = false;
    if (!m_isProcessing.compare_exchange_strong(expectedProcessing, true)) {
        report.errorDescription = "Unsupported concurrent or recursive ProcessTextureNative on single StudioStack";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    struct ProcessingGuard {
        std::atomic<bool>& flag;
        ~ProcessingGuard() { flag.store(false); }
    } guard{ m_isProcessing };

    // 4. Snapshot configuration
    Presets::StudioPreset snapPreset;
    uint64_t snapSourceGen = 1;
    ExecutionBackend snapBackend = ExecutionBackend::CpuOnly;
    std::shared_ptr<AnalysisBridge::AnalysisMailbox> snapMailbox;
    std::shared_ptr<const CubeLut> snapLut, snapInputLut;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        snapPreset = m_currentPreset;
        snapSourceGen = m_sourceGeneration;
        snapBackend = m_backend;
        snapMailbox = m_analysisMailbox;
        snapLut = m_activeLut;
        snapInputLut = m_inputLut;
    }
    report.sourceGeneration = snapSourceGen;

    // 5. Source generation validation (pre-lock check)
    if (frameSourceGeneration != 0 && frameSourceGeneration != snapSourceGen) {
        report.errorDescription = "Stale input texture rejected (source generation mismatch)";
        report.analysisStatus = StageStatus::Bypassed;
        report.inputLookStatus=StageStatus::Bypassed; report.inputLutStatus=StageStatus::Bypassed;
        report.appearanceStatus = StageStatus::Bypassed;
        report.lightingStatus = StageStatus::Bypassed;
        report.backgroundStatus = StageStatus::Bypassed;
        report.lutStatus = StageStatus::Bypassed;
        report.overlayStatus = StageStatus::Bypassed;
        if (outReport) *outReport = report;
        return surf;
    }

    // 6. Acquire execution lock for GPU operations
    std::lock_guard<std::mutex> execLock(m_executionMutex);

    // Recheck source generation immediately after acquiring execution lock
    uint64_t postLockSourceGen = m_sourceGeneration;
    if (frameSourceGeneration != 0 && frameSourceGeneration != postLockSourceGen) {
        report.errorDescription = "Stale input texture rejected after acquiring lock (source generation mismatch)";
        report.analysisStatus = StageStatus::Bypassed;
        report.inputLookStatus=StageStatus::Bypassed; report.inputLutStatus=StageStatus::Bypassed;
        report.appearanceStatus = StageStatus::Bypassed;
        report.lightingStatus = StageStatus::Bypassed;
        report.backgroundStatus = StageStatus::Bypassed;
        report.lutStatus = StageStatus::Bypassed;
        report.overlayStatus = StageStatus::Bypassed;
        if (outReport) *outReport = report;
        return surf;
    }

    if (!m_gpu || !m_gpu->device || !m_gpu->context) {
        report.errorDescription = "GPU device not initialized for ProcessTextureNative";
        report.appearanceStatus = StageStatus::NotReady;
        report.lightingStatus = StageStatus::NotReady;
        report.backgroundStatus = StageStatus::NotReady;
        report.lutStatus = StageStatus::NotReady;
        if (outReport) *outReport = report;
        return surf;
    }

    // Validate SRV description: view dimension and mip levels
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    inputSRV->GetDesc(&srvDesc);
    if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS ||
        srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY) {
        report.errorDescription = "Multisampled textures are not supported (MSAA view dimension)";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return surf;
    }
    if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        report.errorDescription = "Unsupported SRV view dimension (only Texture2D supported)";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return surf;
    }
    if (srvDesc.Texture2D.MostDetailedMip != 0) {
        report.errorDescription = "SRV subresource must target mip level 0";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return surf;
    }

    // Validate resource from SRV
    ComPtr<ID3D11Resource> inputRes;
    inputSRV->GetResource(&inputRes);
    if (!inputRes) {
        report.errorDescription = "Failed to retrieve resource from inputSRV";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    ComPtr<ID3D11Texture2D> inputTex;
    HRESULT hr = inputRes.As(&inputTex);
    if (FAILED(hr) || !inputTex) {
        report.errorDescription = "Supplied resource is not an ID3D11Texture2D";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // Validate device identity
    ComPtr<ID3D11Device> texDev;
    inputTex->GetDevice(&texDev);
    if (texDev.Get() != m_gpu->device.Get()) {
        report.errorDescription = "Input texture belongs to a different ID3D11Device";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // Validate dimensions and sample count (reject multisampled MSAA)
    D3D11_TEXTURE2D_DESC desc{};
    inputTex->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) {
        report.errorDescription = "Multisampled textures are not supported (sample count > 1)";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return surf;
    }

    if (static_cast<int>(desc.Width) != width || static_cast<int>(desc.Height) != height) {
        report.errorDescription = "Input texture dimensions do not match specified width/height";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // Validate format with explicit sRGB handling
    bool isSrgb = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                   srvDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || srvDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        report.errorDescription = "Unsupported texture format (only RGBA8/BGRA8 supported)";
        report.inputLookStatus=StageStatus::Unsupported; report.inputLutStatus=StageStatus::Unsupported;
        report.appearanceStatus = StageStatus::Unsupported;
        report.lightingStatus = StageStatus::Unsupported;
        report.backgroundStatus = StageStatus::Unsupported;
        report.lutStatus = StageStatus::Unsupported;
        if (outReport) *outReport = report;
        return surf;
    }

    // 7. Analysis alignment
    std::shared_ptr<const AnalysisBridge::AnalysisBundle> analysisBundle;
    if (snapMailbox) {
        AnalysisBridge::AnalysisAlignmentStatus alignStatus = AnalysisBridge::AnalysisAlignmentStatus::NotReady;
        analysisBundle = snapMailbox->GetAlignedAnalysis(
            snapSourceGen, sequenceId, timestampPts, alignStatus);

        if (analysisBundle) {
            report.analysisStatus = StageStatus::Applied;
        } else if (alignStatus == AnalysisBridge::AnalysisAlignmentStatus::NotReady) {
            report.analysisStatus = StageStatus::NotReady;
        } else {
            report.analysisStatus = StageStatus::Bypassed;
        }
    } else {
        report.analysisStatus = StageStatus::Bypassed;
    }

    // 8. Check targeted failure injection before stage execution
    GpuFailureTarget failTarget = m_gpuFailureTarget.load();
    if (failTarget == GpuFailureTarget::Readback) {
        report.errorDescription = "Simulated GPU readback failure (injected failure point)";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }
    if (failTarget == GpuFailureTarget::ResourceCreation) {
        report.errorDescription = "Simulated GPU resource creation failure (injected failure point)";
        report.inputLookStatus=StageStatus::Failed; report.inputLutStatus=StageStatus::Failed;
        report.appearanceStatus = StageStatus::Failed;
        report.lightingStatus = StageStatus::Failed;
        report.backgroundStatus = StageStatus::Failed;
        report.lutStatus = StageStatus::Failed;
        if (outReport) *outReport = report;
        return surf;
    }

    // Execute GPU stages via shared pipeline
    auto tGpuStart = std::chrono::steady_clock::now();
    GpuPipelineResult pipeRes = ExecuteGpuStages(
        inputSRV,
        inputTex.Get(),
        width, height,
        snapPreset,
        snapLut,
        snapInputLut,
        analysisBundle.get(),
        report
    );
    auto tGpuEnd = std::chrono::steady_clock::now();
    report.gpuDispatchMs = std::chrono::duration<double, std::milli>(tGpuEnd - tGpuStart).count();
    report.effectDispatchMs = report.gpuDispatchMs;
    report.totalLatencyMs = report.gpuDispatchMs;
    report.completionMs = report.totalLatencyMs;

    if (!pipeRes.success || !pipeRes.outputTexture) {
        report.errorDescription = pipeRes.error.empty() ? "GPU stage execution failed" : pipeRes.error;
        if (outReport) *outReport = report;
        return surf;
    }

    // Recheck source generation before surface delivery
    uint64_t deliverySourceGen = m_sourceGeneration;
    if (snapSourceGen != deliverySourceGen) {
        report.errorDescription = "Source generation superseded prior to surface delivery";
        report.inputLookStatus=StageStatus::Bypassed; report.inputLutStatus=StageStatus::Bypassed;
        report.appearanceStatus = StageStatus::Bypassed;
        report.lightingStatus = StageStatus::Bypassed;
        report.backgroundStatus = StageStatus::Bypassed;
        report.lutStatus = StageStatus::Bypassed;
        report.overlayStatus = StageStatus::Bypassed;
        if (outReport) *outReport = report;
        return GpuTextureSurface{};
    }



    // 10. Populate safe retained surface using bounded resource ownership pool
    DXGI_FORMAT outFmt = isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    auto acquireRes = m_poolCoordinator->AcquireSlot(
        width, height, outFmt,
        m_gpuFaultSeam.load(), m_gpuFaultHResult.load());

    if (acquireRes.status != OutputPoolCoordinator::AcquireStatus::Success || !acquireRes.leaseCore) {
        // Honest error classification without wiping truthful stage outcomes!
        report.errorDescription = acquireRes.errorMessage.empty()
            ? "Output lease acquisition failed"
            : acquireRes.errorMessage;
        if (outReport) *outReport = report;
        surf.isValid = false;
        return surf;
    }

    m_gpu->context->CopyResource(acquireRes.leaseCore->texture.Get(), pipeRes.outputTexture);

    surf.texture = acquireRes.leaseCore->texture.Get();
    surf.srv = acquireRes.leaseCore->srv.Get();
    surf.textureRef = acquireRes.leaseCore->texture;
    surf.srvRef = acquireRes.leaseCore->srv;
    surf.width = width;
    surf.height = height;
    surf.format = outFmt;
    surf.leaseId = acquireRes.leaseCore->leaseId;
    surf.leaseCore = acquireRes.leaseCore;
    surf.isValid = true;

    if (outReport) *outReport = report;
    return surf;
}

AVFrame* StudioStack::CreateScaledFrame(const AVFrame* sourceFrame, int targetWidth, int targetHeight) {
    if (!sourceFrame || !sourceFrame->data[0] || targetWidth <= 0 || targetHeight <= 0) {
        return nullptr;
    }

    static thread_local CachedFrameScaler s_threadScaler;
    return s_threadScaler.Scale(sourceFrame, targetWidth, targetHeight);
}

} // namespace PrismForge::Effects::StudioStack
