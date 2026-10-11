#include "windows/src/effects/studiostack/studio_stack.h"
#include "windows/src/effects/presets/studio_preset.h"
#include "windows/src/effects/analysisbridge/analysis_mailbox.h"
#include "windows/src/effects/analysisbridge/analysis_types.h"
#include "windows/src/effects/gpu/d3d11_context.h"
#include "windows/src/effects/relighting/depth_types.h"
#include "windows/src/effects/relighting/depth_to_normal.h"
#include "windows/src/effects/cubelut.h"
#include "windows/src/effects/overlays/procedural_assets.h"

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cassert>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

using namespace PrismForge::Effects::StudioStack;
using namespace PrismForge::Effects::Presets;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Effects::Lighting;
using namespace PrismForge::Effects::Relighting;
using namespace PrismForge::Effects::Gpu;
using Microsoft::WRL::ComPtr;

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[ASSERTION FAILED] " << (msg) << " at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        std::cout << "[TEST] Running " << #fn << " ..." << std::endl; \
        if (fn()) { \
            std::cout << "  -> PASSED" << std::endl; \
        } else { \
            std::cerr << "  -> FAILED: " << #fn << std::endl; \
            return 1; \
        } \
    } while (0)

static AVFrame* AllocTestBgrFrame(int width, int height, uint8_t fillVal = 128) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_BGR24;
    frame->pts = 1000;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < width; ++x) {
            row[3 * x + 0] = fillVal;
            row[3 * x + 1] = fillVal;
            row[3 * x + 2] = fillVal;
        }
    }
    return frame;
}

static std::shared_ptr<AnalysisBundle> CreateFullAnalysisBundle(
    uint64_t gen, uint64_t seq, int64_t pts, int width, int height)
{
    auto b = std::make_shared<AnalysisBundle>();
    b->sourceGeneration = gen;
    b->sequenceId = seq;
    b->timestampPts = pts;

    // 1. Person Mask
    b->personMask.width = width;
    b->personMask.height = height;
    b->personMask.strideFloats = width;
    b->personMask.data.assign(width * height, 0.0f);

    float cx = width * 0.5f;
    float cy = height * 0.5f;
    float rx = width * 0.35f;
    float ry = height * 0.40f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = (x - cx) / rx;
            float dy = (y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) {
                b->personMask.data[y * width + x] = 1.0f;
            }
        }
    }

    // 2. Skin Mask
    b->skinMask.width = width;
    b->skinMask.height = height;
    b->skinMask.strideFloats = width;
    b->skinMask.data.assign(width * height, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = (x - cx) / (rx * 0.7f);
            float dy = (y - (cy - height * 0.05f)) / (ry * 0.6f);
            if (dx * dx + dy * dy <= 1.0f) {
                b->skinMask.data[y * width + x] = 1.0f;
            }
        }
    }

    // 3. Depth Map (Relative depth with raised nose at center)
    b->depthMap.width = width;
    b->depthMap.height = height;
    b->depthMap.strideFloats = width;
    b->depthMap.format = 0; // RelativeDepth
    b->depthMap.depthScale = 0.002f;
    b->depthMap.sourceGeneration = gen;
    b->depthMap.sequenceId = seq;
    b->depthMap.timestampPts = pts;
    b->depthMap.data.assign(width * height, 0.70f); // background plane depth

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = (x - cx) / (rx * 0.25f);
            float dy = (y - cy) / (ry * 0.25f);
            float rSq = dx * dx + dy * dy;
            if (rSq <= 1.0f) {
                // Nose protrudes forward -> smaller depth
                b->depthMap.data[y * width + x] = 0.70f - 0.40f * (1.0f - rSq);
            }
        }
    }

    // 4. Reconstruct Normals from Depth
    DepthToNormal d2n;
    d2n.ReconstructNormals(b->depthMap, b->normalMap);

    return b;
}

static StudioPreset CreateFullGraphPreset() {
    StudioPreset p;
    p.schemaVersion = 2;
    p.presetId = "test.full_graph";
    p.displayName = "Full Graph Production Test";
    p.bypassAll = false;

    // Appearance
    p.appearance.bypass = false;
    p.appearance.smoothingStrength = 0.60f;
    p.appearance.sharpenStrength = 0.35f;

    // Lighting (NormalGuided + Approximate Shadows + Kelvin)
    p.lighting = LightingConfig::DefaultTwoLight();
    p.lighting.lightingMode = LightingMode::NormalGuided;
    p.lighting.bypass = false;
    p.lighting.subjectOnlyMode = false;
    p.lighting.light1.enabled = true;
    p.lighting.light1.colorTemperature = 3200.0f; // Warm tungsten
    p.lighting.light1.tint = 0.05f;
    p.lighting.light1.posX = -0.5f;
    p.lighting.light1.posY = -0.3f;
    p.lighting.light1.intensity = 1.0f;
    p.lighting.light2.enabled = true;
    p.lighting.light2.colorTemperature = 7500.0f; // Cool daylight
    p.lighting.light2.tint = -0.05f;
    p.lighting.light2.posX = 0.5f;
    p.lighting.light2.posY = 0.2f;
    p.lighting.light2.intensity = 0.6f;
    p.lighting.shadowConfig.enabled = true;
    p.lighting.shadowConfig.maxSteps = 16;
    p.lighting.shadowConfig.maxDistance = 0.25f;
    p.lighting.shadowConfig.shadowIntensity = 0.60f;
    p.lighting.shadowConfig.shadowSoftness = 0.50f;

    // Background (Blur)
    p.backgroundMode = StudioBackgroundMode::Original;

    // Overlays
    p.overlays.bypass = true;
    p.overlays.studioFrame.enabled = false;

    return p;
}

static std::string GetIdentityCubeLutText() {
    std::string s = "TITLE \"TestIdentity\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
    s += "0.0 0.0 0.0\n1.0 0.0 0.0\n0.0 1.0 0.0\n1.0 1.0 0.0\n";
    s += "0.0 0.0 1.0\n1.0 0.0 1.0\n0.0 1.0 1.0\n1.0 1.0 1.0\n";
    return s;
}

// ----------------------------------------------------------------------------
// Test 1: Full Graph Execution on Native and Software GPU Routes
// ----------------------------------------------------------------------------
bool TestFullGraphExecutionNativeAndSoftware() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");
    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(device, context), "InitializeGpu failed");

    StudioPreset preset = CreateFullGraphPreset();
    stack.SetPreset(preset);

    std::string lutErr;
    ASSERT_TRUE(stack.LoadLutFromBuffer(GetIdentityCubeLutText(), lutErr), "LoadLutFromBuffer failed");

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    const int width = 320;
    const int height = 240;
    auto bundle = CreateFullAnalysisBundle(1, 1, 10000, width, height);
    mailbox->PostAnalysis(bundle);

    // 1. Software-GPU route (ProcessFrame with CPU input frame)
    AVFrame* inFrame = AllocTestBgrFrame(width, height, 150);
    StackExecutionReport swReport{};
    AVFrame* outFrame = stack.ProcessFrame(inFrame, 1, 10000, &swReport, 1);
    ASSERT_TRUE(outFrame != nullptr, "ProcessFrame returned null");
    ASSERT_TRUE(!swReport.gpuFallbackUsed, "GPU execution must succeed without CPU fallback");
    ASSERT_TRUE(swReport.appearanceStatus == StageStatus::Applied, "Appearance must be Applied");
    ASSERT_TRUE(swReport.lightingStatus == StageStatus::Applied, "Lighting must be Applied");
    ASSERT_TRUE(swReport.imageUploadCount >= 1, "Image upload count must be >= 1");
    ASSERT_TRUE(swReport.imageReadbackCount >= 1, "Image readback count must be >= 1");

    // 2. Native GPU route (ProcessTextureNative)
    // Create input GPU texture
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> inTex;
    ComPtr<ID3D11ShaderResourceView> inSRV;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, inTex.GetAddressOf());
    ASSERT_TRUE(SUCCEEDED(hr), "CreateTexture2D for native input failed");
    hr = device->CreateShaderResourceView(inTex.Get(), nullptr, inSRV.GetAddressOf());
    ASSERT_TRUE(SUCCEEDED(hr), "CreateShaderResourceView for native input failed");

    auto bundle2 = CreateFullAnalysisBundle(1, 2, 20000, width, height);
    mailbox->PostAnalysis(bundle2);

    StackExecutionReport natReport{};
    GpuTextureSurface natSurf = stack.ProcessTextureNative(
        inSRV.Get(), width, height, 2, 20000, &natReport, 1);

    ASSERT_TRUE(natSurf.isValid, "Native surface must be valid");
    ASSERT_TRUE(natSurf.texture != nullptr, "Native texture must not be null");
    ASSERT_TRUE(natSurf.srv != nullptr, "Native SRV must not be null");
    ASSERT_TRUE(natReport.appearanceStatus == StageStatus::Applied, "Native appearance must be Applied");
    ASSERT_TRUE(natReport.lightingStatus == StageStatus::Applied, "Native lighting must be Applied");
    ASSERT_TRUE(natReport.imageReadbackCount == 0, "Native route must NEVER perform image readback");

    stack.ReleaseOutputSurface(natSurf);
    ASSERT_TRUE(!natSurf.isValid, "Surface must be invalid after ReleaseOutputSurface");

    av_frame_free(&inFrame);
    return true;
}

// ----------------------------------------------------------------------------
// Test 2: Software-GPU Seam Failures Trigger Same-Frame CPU Fallback
// ----------------------------------------------------------------------------
bool TestSoftwareGpuSeamFailuresTriggerSameFrameCpuFallback() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(d3dCtx.GetDevice(), d3dCtx.GetContext()), "InitializeGpu failed");

    StudioPreset preset = CreateFullGraphPreset();
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    const int width = 160;
    const int height = 120;
    auto bundle = CreateFullAnalysisBundle(1, 1, 10000, width, height);
    mailbox->PostAnalysis(bundle);

    AVFrame* inFrame = AllocTestBgrFrame(width, height, 140);

    const GpuFaultSeam testSeams[] = {
        GpuFaultSeam::TexA_Creation,
        GpuFaultSeam::SrvA_Creation,
        GpuFaultSeam::UavB_Creation,
        GpuFaultSeam::UploadStaging_Creation,
        GpuFaultSeam::ReadbackStaging_Creation,
        GpuFaultSeam::Readback_Map,
        GpuFaultSeam::NormalTex_Creation,
        GpuFaultSeam::Normal_Map,
        GpuFaultSeam::Normal_Resample,
        GpuFaultSeam::DepthTex_Creation,
        GpuFaultSeam::Depth_Map,
        GpuFaultSeam::DeviceRemoved_Seam
    };

    uint64_t seq = 1;
    for (GpuFaultSeam seam : testSeams) {
        stack.SetGpuFaultSeam(seam, E_FAIL);

        uint64_t s1 = seq++;
        int64_t pts1 = s1 * 10000;
        mailbox->PostAnalysis(CreateFullAnalysisBundle(1, s1, pts1, width, height));

        StackExecutionReport report{};
        AVFrame* out = stack.ProcessFrame(inFrame, s1, pts1, &report, 1);

        ASSERT_TRUE(out != nullptr, "ProcessFrame must return non-null frame on fallback");
        ASSERT_TRUE(report.gpuFallbackUsed, "GPU fallback must be marked used when seam is injected");
        ASSERT_TRUE(!report.errorDescription.empty(), "Error description must be populated");

        // Clear seam and assert immediate recovery
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
        uint64_t s2 = seq++;
        int64_t pts2 = s2 * 10000;
        mailbox->PostAnalysis(CreateFullAnalysisBundle(1, s2, pts2, width, height));

        StackExecutionReport recReport{};
        AVFrame* recOut = stack.ProcessFrame(inFrame, s2, pts2, &recReport, 1);
        ASSERT_TRUE(recOut != nullptr, "ProcessFrame must succeed on recovery");
        ASSERT_TRUE(!recReport.gpuFallbackUsed, "Recovery frame must execute on GPU without fallback");
    }

    av_frame_free(&inFrame);
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: Native Route Seam Failures Fail Explicitly Without Readback
// ----------------------------------------------------------------------------
bool TestNativeRouteSeamFailuresFailExplicitlyWithoutReadback() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");
    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(device, context), "InitializeGpu failed");

    StudioPreset preset = CreateFullGraphPreset();
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    const int width = 160;
    const int height = 120;
    auto bundle = CreateFullAnalysisBundle(1, 1, 10000, width, height);
    mailbox->PostAnalysis(bundle);

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> inTex;
    ComPtr<ID3D11ShaderResourceView> inSRV;
    device->CreateTexture2D(&texDesc, nullptr, inTex.GetAddressOf());
    device->CreateShaderResourceView(inTex.Get(), nullptr, inSRV.GetAddressOf());

    const GpuFaultSeam testSeams[] = {
        GpuFaultSeam::TexA_Creation,
        GpuFaultSeam::NormalTex_Creation,
        GpuFaultSeam::DepthTex_Creation,
        GpuFaultSeam::OutputTex_Creation,
        GpuFaultSeam::OutputSrv_Creation,
        GpuFaultSeam::DeviceRemoved_Seam
    };

    uint64_t seq = 1;
    for (GpuFaultSeam seam : testSeams) {
        stack.SetGpuFaultSeam(seam, E_FAIL);

        uint64_t s1 = seq++;
        int64_t pts1 = s1 * 10000;
        mailbox->PostAnalysis(CreateFullAnalysisBundle(1, s1, pts1, width, height));

        StackExecutionReport report{};
        GpuTextureSurface surf = stack.ProcessTextureNative(
            inSRV.Get(), width, height, s1, pts1, &report, 1);

        ASSERT_TRUE(!surf.isValid, "Native surface must be invalid on fault seam failure");
        ASSERT_TRUE(surf.texture == nullptr, "Native texture must be null on failure");
        ASSERT_TRUE(report.imageReadbackCount == 0, "No camera readback allowed on native failure");
        ASSERT_TRUE(!report.errorDescription.empty(), "Error description must describe failure");

        // Clear seam & verify recovery
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
        uint64_t s2 = seq++;
        int64_t pts2 = s2 * 10000;
        mailbox->PostAnalysis(CreateFullAnalysisBundle(1, s2, pts2, width, height));

        StackExecutionReport recReport{};
        GpuTextureSurface recSurf = stack.ProcessTextureNative(
            inSRV.Get(), width, height, s2, pts2, &recReport, 1);
        ASSERT_TRUE(recSurf.isValid, "Recovery surface must be valid");
        stack.ReleaseOutputSurface(recSurf);
    }

    return true;
}

// ----------------------------------------------------------------------------
// Test 4: No Stale Normals or Shadows Across Source Generation and PTS Jumps
// ----------------------------------------------------------------------------
bool TestNoStaleNormalsOrShadowsAcrossSourceGenerationAndPTS() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(d3dCtx.GetDevice(), d3dCtx.GetContext()), "InitializeGpu failed");

    StudioPreset preset = CreateFullGraphPreset();
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    const int width = 160;
    const int height = 120;

    // Post analysis bundle with generation 1
    auto bundleGen1 = CreateFullAnalysisBundle(1, 1, 10000, width, height);
    mailbox->PostAnalysis(bundleGen1);

    AVFrame* inFrame = AllocTestBgrFrame(width, height, 150);

    // Frame 1 with generation 1: success
    StackExecutionReport rep1{};
    AVFrame* out1 = stack.ProcessFrame(inFrame, 1, 10000, &rep1, 1);
    ASSERT_TRUE(out1 != nullptr && rep1.lightingStatus == StageStatus::Applied, "Gen 1 frame must succeed");

    // Advance stack generation to 2, but frame provided with gen 2 while mailbox still has gen 1
    stack.SetSourceGeneration(2);
    StackExecutionReport rep2{};
    AVFrame* out2 = stack.ProcessFrame(inFrame, 2, 20000, &rep2, 2);
    ASSERT_TRUE(out2 != nullptr, "Frame must return valid fallback");
    // Normal-guided mode without matching source generation normals must truthfully report NotReady
    ASSERT_TRUE(rep2.lightingStatus == StageStatus::NotReady, "Stale normal must result in NotReady, never reused");

    // Backward PTS jump: sequenceId 3, pts 5000 (jumps backward from 20000)
    mailbox->ResetSource(2);
    auto bundleGen2 = CreateFullAnalysisBundle(2, 3, 5000, width, height);
    mailbox->PostAnalysis(bundleGen2);

    StackExecutionReport rep3{};
    AVFrame* out3 = stack.ProcessFrame(inFrame, 3, 5000, &rep3, 2);
    ASSERT_TRUE(out3 != nullptr, "Frame after backward PTS must succeed");
    ASSERT_TRUE(rep3.lightingStatus == StageStatus::Applied, "Lighting must be Applied after valid post-reset bundle");

    av_frame_free(&inFrame);
    return true;
}

// ----------------------------------------------------------------------------
// Test 5: Hard Four-Output Lease Capacity and Alias Lifetime
// ----------------------------------------------------------------------------
bool TestHardFourOutputLeaseCapacityAndAliasLifetime() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");
    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(device, context), "InitializeGpu failed");

    StudioPreset preset = CreateFullGraphPreset();
    preset.lighting.lightingMode = LightingMode::SpatialApproximation; // Spatial mode so normals are optional
    stack.SetPreset(preset);

    const int width = 160;
    const int height = 120;

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> inTex;
    ComPtr<ID3D11ShaderResourceView> inSRV;
    device->CreateTexture2D(&texDesc, nullptr, inTex.GetAddressOf());
    device->CreateShaderResourceView(inTex.Get(), nullptr, inSRV.GetAddressOf());

    ASSERT_TRUE(stack.GetMaxLeaseCapacity() == 4, "Max lease capacity must be hard-capped at 4");
    ASSERT_TRUE(stack.GetActiveLeaseCount() == 0, "Initial active lease count must be 0");

    // Acquire 4 concurrent surfaces
    StackExecutionReport rep1{}, rep2{}, rep3{}, rep4{};
    GpuTextureSurface s1 = stack.ProcessTextureNative(inSRV.Get(), width, height, 1, 10000, &rep1, 1);
    GpuTextureSurface s2 = stack.ProcessTextureNative(inSRV.Get(), width, height, 2, 20000, &rep2, 1);
    GpuTextureSurface s3 = stack.ProcessTextureNative(inSRV.Get(), width, height, 3, 30000, &rep3, 1);
    GpuTextureSurface s4 = stack.ProcessTextureNative(inSRV.Get(), width, height, 4, 40000, &rep4, 1);

    ASSERT_TRUE(s1.isValid && s2.isValid && s3.isValid && s4.isValid, "All 4 leases must be successfully acquired");
    ASSERT_TRUE(stack.GetActiveLeaseCount() == 4, "Active lease count must be 4");

    // 5th acquisition attempt must fail because pool is saturated at capacity 4
    StackExecutionReport rep5{};
    GpuTextureSurface s5 = stack.ProcessTextureNative(inSRV.Get(), width, height, 5, 50000, &rep5, 1);
    ASSERT_TRUE(!s5.isValid, "5th lease must fail when pool is full (capacity 4)");
    ASSERT_TRUE(stack.GetActiveLeaseCount() == 4, "Active lease count must remain 4");

    // Create an alias of s1 (shared lease semantics)
    {
        GpuTextureSurface s1_alias = s1;
        ASSERT_TRUE(s1_alias.isValid, "Alias must be valid");
        ASSERT_TRUE(s1_alias.leaseId == s1.leaseId, "Lease ID must match across aliases");

        // Release the original handle: alias still holds slot
        s1.Release();
        ASSERT_TRUE(!s1.isValid, "Original s1 must be invalid after Release()");
        ASSERT_TRUE(stack.GetActiveLeaseCount() == 4, "Slot must stay active while alias is alive");
    } // s1_alias destructs here -> slot released!

    ASSERT_TRUE(stack.GetActiveLeaseCount() == 3, "Active lease count must drop to 3 after alias destruction");

    // Now 6th acquisition must succeed
    StackExecutionReport rep6{};
    GpuTextureSurface s6 = stack.ProcessTextureNative(inSRV.Get(), width, height, 6, 60000, &rep6, 1);
    ASSERT_TRUE(s6.isValid, "Lease acquisition must succeed after freeing a slot");
    ASSERT_TRUE(stack.GetActiveLeaseCount() == 4, "Active lease count must return to 4");

    // Release all
    s2.Release();
    s3.Release();
    s4.Release();
    s6.Release();
    ASSERT_TRUE(stack.GetActiveLeaseCount() == 0, "Active lease count must return to 0");

    return true;
}

// ----------------------------------------------------------------------------
// Test 6: Asset and Metadata Preservation Across Resolution Churn
// ----------------------------------------------------------------------------
bool TestAssetAndMetadataPreservationAcrossResolutionChurn() {
    D3D11Context d3dCtx;
    ASSERT_TRUE(d3dCtx.Initialize(), "D3D11 initialization failed");
    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();

    StudioStack stack;
    stack.SetSourceGeneration(1);
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    ASSERT_TRUE(stack.InitializeGpu(device, context), "InitializeGpu failed");

    // Load LUT
    std::string lutErr;
    ASSERT_TRUE(stack.LoadLutFromBuffer(GetIdentityCubeLutText(), lutErr), "LoadLutFromBuffer failed");

    // Create background image
    D3D11_TEXTURE2D_DESC bgDesc{};
    bgDesc.Width = 640;
    bgDesc.Height = 480;
    bgDesc.MipLevels = 1;
    bgDesc.ArraySize = 1;
    bgDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bgDesc.SampleDesc.Count = 1;
    bgDesc.Usage = D3D11_USAGE_DEFAULT;
    bgDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> bgTex;
    ComPtr<ID3D11ShaderResourceView> bgSRV;
    device->CreateTexture2D(&bgDesc, nullptr, bgTex.GetAddressOf());
    device->CreateShaderResourceView(bgTex.Get(), nullptr, bgSRV.GetAddressOf());

    AVFrame* cpuBg = AllocTestBgrFrame(640, 480, 200);
    std::string bgErr;
    ASSERT_TRUE(stack.SetBackgroundImage(bgSRV.Get(), cpuBg, BackgroundFitMode::CropFill, &bgErr),
                "SetBackgroundImage failed");
    ASSERT_TRUE(stack.HasBackgroundImage(), "Stack must report having background image");

    StudioPreset preset = CreateFullGraphPreset();
    preset.backgroundMode = StudioBackgroundMode::Image;
    preset.lighting.lightingMode = LightingMode::SpatialApproximation;
    stack.SetPreset(preset);

    const int resolutions[][2] = {
        { 640, 360 },
        { 1280, 720 },
        { 1920, 1080 },
        { 320, 240 },
        { 1280, 720 }
    };

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    uint64_t seq = 1;
    for (const auto& res : resolutions) {
        int w = res[0];
        int h = res[1];

        uint64_t curSeq = seq++;
        int64_t curPts = 10000 * curSeq;
        mailbox->PostAnalysis(CreateFullAnalysisBundle(1, curSeq, curPts, w, h));

        AVFrame* frame = AllocTestBgrFrame(w, h, 120);
        StackExecutionReport rep{};
        AVFrame* out = stack.ProcessFrame(frame, curSeq, curPts, &rep, 1);

        ASSERT_TRUE(out != nullptr, "Frame must succeed at resolution");
        ASSERT_TRUE(rep.backgroundStatus == StageStatus::Applied, "Background must be Applied across churn");
        ASSERT_TRUE(stack.HasBackgroundImage(), "Background asset must remain preserved across churn");

        av_frame_free(&frame);
    }

    av_frame_free(&cpuBg);
    return true;
}

// ----------------------------------------------------------------------------
// Main Runner
// ----------------------------------------------------------------------------
int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "PrismForge Milestone 8: Combined Failure/Fallback & Retention" << std::endl;
    std::cout << "========================================================" << std::endl;

    RUN_TEST(TestFullGraphExecutionNativeAndSoftware);
    RUN_TEST(TestSoftwareGpuSeamFailuresTriggerSameFrameCpuFallback);
    RUN_TEST(TestNativeRouteSeamFailuresFailExplicitlyWithoutReadback);
    RUN_TEST(TestNoStaleNormalsOrShadowsAcrossSourceGenerationAndPTS);
    RUN_TEST(TestHardFourOutputLeaseCapacityAndAliasLifetime);
    RUN_TEST(TestAssetAndMetadataPreservationAcrossResolutionChurn);

    std::cout << "========================================================" << std::endl;
    std::cout << "[SUMMARY] All 6 Combined Failure/Fallback Tests PASSED!" << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
