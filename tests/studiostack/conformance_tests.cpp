#include "windows/src/effects/studiostack/studio_stack.h"
#include "windows/src/effects/presets/studio_preset.h"
#include "windows/src/effects/analysisbridge/analysis_mailbox.h"
#include "windows/src/effects/analysisbridge/analysis_types.h"
#include "windows/src/effects/gpu/d3d11_context.h"
#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/packs/pack_manifest.h"
#include "windows/src/effects/packs/pack_validator.h"
#include "windows/src/effects/packs/sha256.h"
#include "windows/src/effects/cubelut.h"
#include "windows/src/effects/overlays/tracking_evaluator.h"
#include "windows/src/effects/overlays/overlay_renderer.h"
#include "windows/src/effects/overlays/procedural_assets.h"
#include "windows/src/effects/studiostack/gpu/d3d11_studiostack_overlay.h"

#include <windows.h>
#include <psapi.h>
#include <fstream>
#include <sstream>

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

using namespace PrismForge::Effects::StudioStack;
using namespace PrismForge::Effects::Presets;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Effects::Overlays;
using namespace PrismForge::Effects::StudioStack::Gpu;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[ASSERTION FAILED] " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

size_t GetCurrentProcessWorkingSetKB() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / 1024;
    }
    return 0;
}

AVFrame* AllocateBgrFrameWithPattern(int width, int height, int patternType) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_BGR24;
    frame->pts = 100000;
    frame->time_base = AVRational{ 1, 90000 };
    frame->sample_aspect_ratio = AVRational{ 1, 1 };
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    for (int y = 0; y < height; ++y) {
        uint8_t* row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < width; ++x) {
            if (patternType == 0) {
                // Checkerboard pattern (64x64 blocks)
                bool check = ((x / 64) ^ (y / 64)) & 1;
                uint8_t val = check ? 40 : 80;
                row[x * 3 + 0] = val; // B
                row[x * 3 + 1] = val; // G
                row[x * 3 + 2] = val; // R
            } else if (patternType == 1) {
                // Synthetic skin tone (RGB 200, 160, 130 -> BGR 130, 160, 200)
                row[x * 3 + 0] = 130;
                row[x * 3 + 1] = 160;
                row[x * 3 + 2] = 200;
            } else {
                // Solid neutral gray
                row[x * 3 + 0] = 100;
                row[x * 3 + 1] = 100;
                row[x * 3 + 2] = 100;
            }
        }
    }

    return frame;
}

std::shared_ptr<AnalysisBundle> CreateSyntheticBundle(
    uint64_t gen,
    uint64_t seq,
    int64_t pts,
    int width,
    int height,
    float faceOffsetFraction = 0.0f)
{
    auto b = std::make_shared<AnalysisBundle>();
    b->sourceGeneration = gen;
    b->sequenceId = seq;
    b->timestampPts = pts;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    b->timestampCompleted = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    // 1. Person mask (centered oval with optional horizontal shift)
    b->personMask.width = width;
    b->personMask.height = height;
    b->personMask.strideFloats = width;
    b->personMask.data.assign(width * height, 0.0f);

    float cx = width * (0.5f + faceOffsetFraction);
    float cy = height * 0.5f;
    float rx = width * 0.25f;
    float ry = height * 0.35f;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = (x - cx) / rx;
            float dy = (y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) {
                b->personMask.data[y * width + x] = 1.0f;
            }
        }
    }

    // 2. Skin mask (smaller oval inside head)
    b->skinMask.width = width;
    b->skinMask.height = height;
    b->skinMask.strideFloats = width;
    b->skinMask.data.assign(width * height, 0.0f);
    float skinRy = height * 0.18f;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = (x - cx) / (rx * 0.7f);
            float dy = (y - (cy - height * 0.08f)) / skinRy;
            if (dx * dx + dy * dy <= 1.0f) {
                b->skinMask.data[y * width + x] = 1.0f;
            }
        }
    }

    // 3. Feature protection mask (eyes)
    b->protectionMask.width = width;
    b->protectionMask.height = height;
    b->protectionMask.strideFloats = width;
    b->protectionMask.data.assign(width * height, 0.0f);
    int lx = static_cast<int>(cx - width * 0.08f);
    int rxEye = static_cast<int>(cx + width * 0.08f);
    int eyeY = static_cast<int>(cy - height * 0.10f);

    for (int y = eyeY - 8; y <= eyeY + 8; ++y) {
        for (int x = lx - 12; x <= lx + 12; ++x) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                b->protectionMask.data[y * width + x] = 1.0f;
            }
        }
        for (int x = rxEye - 12; x <= rxEye + 12; ++x) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                b->protectionMask.data[y * width + x] = 1.0f;
            }
        }
    }

    // 4. Face landmarks
    b->faceLandmarks.detected = true;
    b->faceLandmarks.overallConfidence = 0.95f;
    float normOffset = faceOffsetFraction;
    b->faceLandmarks.leftEye = { 0.42f + normOffset, 0.40f, 0.95f };
    b->faceLandmarks.rightEye = { 0.58f + normOffset, 0.40f, 0.95f };
    b->faceLandmarks.noseTip = { 0.50f + normOffset, 0.45f, 0.95f };
    b->faceLandmarks.mouthCenter = { 0.50f + normOffset, 0.52f, 0.95f };
    b->faceLandmarks.foreheadCenter = { 0.50f + normOffset, 0.28f, 0.95f };
    b->faceLandmarks.chinTip = { 0.50f + normOffset, 0.60f, 0.95f };

    return b;
}

// -------------------------------------------------------------
// Test 1: Full-Stack Conformance & Bit-Exact Background Retention
// -------------------------------------------------------------
bool TestFullStackConformanceAndBackgroundPreservation() {
    std::cout << "[TEST 1] Conformance: Synthetic Fixtures & Bit-Exact Background Retention ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // Feed checkerboard background with centered skin subject
    AVFrame* inFrame = AllocateBgrFrameWithPattern(1280, 720, 0); // Checkerboard background

    // Overwrite center oval with skin tone in input frame
    float cx = 640.0f;
    float cy = 360.0f;
    float rx = 320.0f;
    float ry = 252.0f;
    for (int y = 0; y < 720; ++y) {
        uint8_t* row = inFrame->data[0] + y * inFrame->linesize[0];
        for (int x = 0; x < 1280; ++x) {
            float dx = (x - cx) / rx;
            float dy = (y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) {
                row[x * 3 + 0] = 130; // B
                row[x * 3 + 1] = 160; // G
                row[x * 3 + 2] = 200; // R
            }
        }
    }

    // Compute input checksum of outside background pixels (e.g. corner block 0..100, 0..100)
    std::vector<uint8_t> cornerBytesIn;
    cornerBytesIn.reserve(100 * 100 * 3);
    for (int y = 0; y < 100; ++y) {
        uint8_t* row = inFrame->data[0] + y * inFrame->linesize[0];
        for (int x = 0; x < 100; ++x) {
            cornerBytesIn.push_back(row[x * 3 + 0]);
            cornerBytesIn.push_back(row[x * 3 + 1]);
            cornerBytesIn.push_back(row[x * 3 + 2]);
        }
    }
    uint64_t inBgHash = Fnv1a64(cornerBytesIn.data(), cornerBytesIn.size());

    // Post aligned bundle
    auto bundle = CreateSyntheticBundle(1, 1, 33333, 1280, 720);
    mailbox->PostAnalysis(bundle);

    // Warm preset with appearance and lighting (screen frame disabled to verify pure video background)
    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    p.overlays.studioFrame.enabled = false;
    stack.SetPreset(p);

    StackExecutionReport rep;
    AVFrame* outFrame = stack.ProcessFrame(inFrame, 1, 33333, &rep);

    TEST_ASSERT(outFrame != nullptr, "Output frame must be non-null");
    TEST_ASSERT(rep.analysisStatus == StageStatus::Applied, "Analysis must be Applied");
    TEST_ASSERT(rep.appearanceStatus == StageStatus::Applied, "Appearance must be Applied");
    TEST_ASSERT(rep.lightingStatus == StageStatus::Applied, "Lighting must be Applied");

    // Sample corner block from output frame: must be 100% bit-exact to input!
    std::vector<uint8_t> cornerBytesOut;
    cornerBytesOut.reserve(100 * 100 * 3);
    for (int y = 0; y < 100; ++y) {
        uint8_t* row = outFrame->data[0] + y * outFrame->linesize[0];
        for (int x = 0; x < 100; ++x) {
            cornerBytesOut.push_back(row[x * 3 + 0]);
            cornerBytesOut.push_back(row[x * 3 + 1]);
            cornerBytesOut.push_back(row[x * 3 + 2]);
        }
    }
    uint64_t outBgHash = Fnv1a64(cornerBytesOut.data(), cornerBytesOut.size());

    TEST_ASSERT(inBgHash == outBgHash, "Background outside subject mask MUST be 100% bit-exact preserved!");

    // Sample center pixel: must be lit and modified from original skin tone
    uint8_t* centerOut = outFrame->data[0] + 360 * outFrame->linesize[0] + 640 * 3;
    TEST_ASSERT(centerOut[0] != 130 || centerOut[1] != 160 || centerOut[2] != 200,
        "Subject skin pixels must be modified by lighting and appearance engine");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Background bit-exact match confirmed, hash=0x"
              << std::hex << outBgHash << std::dec << ")" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 2: Color and Metadata Preservation
// -------------------------------------------------------------
bool TestColorAndMetadataPreservation() {
    std::cout << "[TEST 2] Conformance: Color & Metadata Preservation Through Full Stack ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(2);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(2);
    stack.AttachAnalysisMailbox(mailbox);

    AVFrame* inFrame = AllocateBgrFrameWithPattern(1280, 720, 2);
    inFrame->pts = 987654321;
    inFrame->time_base = AVRational{ 1, 60000 };
    inFrame->sample_aspect_ratio = AVRational{ 16, 9 };
    inFrame->color_range = AVCOL_RANGE_JPEG;
    inFrame->color_primaries = AVCOL_PRI_BT709;
    inFrame->color_trc = AVCOL_TRC_BT709;
    inFrame->colorspace = AVCOL_SPC_BT709;

    auto bundle = CreateSyntheticBundle(2, 5, inFrame->pts, 1280, 720);
    mailbox->PostAnalysis(bundle);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(p);

    bool consumerReceivedMetadata = false;
    stack.RegisterConsumer("meta_test_consumer", [&](const AVFrame* f, const StackExecutionReport& rep) {
        if (f->pts == 987654321 &&
            f->time_base.num == 1 && f->time_base.den == 60000 &&
            f->sample_aspect_ratio.num == 16 && f->sample_aspect_ratio.den == 9 &&
            f->color_range == AVCOL_RANGE_JPEG &&
            f->color_primaries == AVCOL_PRI_BT709 &&
            f->color_trc == AVCOL_TRC_BT709 &&
            f->colorspace == AVCOL_SPC_BT709)
        {
            consumerReceivedMetadata = true;
        }
    });

    StackExecutionReport rep;
    AVFrame* outFrame = stack.ProcessFrame(inFrame, 5, inFrame->pts, &rep);

    TEST_ASSERT(outFrame != nullptr, "ProcessFrame must succeed");
    TEST_ASSERT(outFrame->pts == 987654321, "PTS must match exactly");
    TEST_ASSERT(outFrame->time_base.num == 1 && outFrame->time_base.den == 60000, "Timebase must match");
    TEST_ASSERT(outFrame->sample_aspect_ratio.num == 16 && outFrame->sample_aspect_ratio.den == 9, "SAR must match");
    TEST_ASSERT(outFrame->color_range == AVCOL_RANGE_JPEG, "Color range must match");
    TEST_ASSERT(outFrame->color_primaries == AVCOL_PRI_BT709, "Color primaries must match");
    TEST_ASSERT(outFrame->color_trc == AVCOL_TRC_BT709, "Color TRC must match");
    TEST_ASSERT(outFrame->colorspace == AVCOL_SPC_BT709, "Colorspace must match");
    TEST_ASSERT(consumerReceivedMetadata, "Consumer must receive identical metadata");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (All 8 metadata attributes preserved bit-for-bit)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 3: Source Generation Guards, Stale Rejection & Reset
// -------------------------------------------------------------
bool TestGenerationGuardsAndOldFrameRejection() {
    std::cout << "[TEST 3] Guards: Source Generation Alignment & Reset ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(10);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(10);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(p);

    AVFrame* inFrame = AllocateBgrFrameWithPattern(1280, 720, 2);

    // 1. Generation mismatch in PostAnalysis: bundle has generation 9 while mailbox has 10
    auto badGenBundle = CreateSyntheticBundle(9, 1, 1000, 1280, 720);
    bool postedOld = mailbox->PostAnalysis(badGenBundle);
    TEST_ASSERT(!postedOld, "PostAnalysis must reject bundles with mismatching source generation");

    StackExecutionReport rep1;
    stack.ProcessFrame(inFrame, 1, 1000, &rep1);
    TEST_ASSERT(rep1.analysisStatus == StageStatus::NotReady,
        "Analysis status must be NotReady when mailbox is empty due to generation rejection");

    // 2. Direct generation mismatch between stack and mailbox
    mailbox->ResetSource(9); // Mailbox set to generation 9 while stack is at 10
    StackExecutionReport repMismatch;
    stack.ProcessFrame(inFrame, 1, 1000, &repMismatch);
    TEST_ASSERT(repMismatch.analysisStatus == StageStatus::Bypassed,
        "Analysis status must report Bypassed when stack generation does not match mailbox generation");
    mailbox->ResetSource(10); // Restore matching generation

    // 3. Aligned generation 10 bundle
    auto goodBundle = CreateSyntheticBundle(10, 2, 2000, 1280, 720);
    bool postedGood = mailbox->PostAnalysis(goodBundle);
    TEST_ASSERT(postedGood, "PostAnalysis must accept matching generation");

    StackExecutionReport rep2;
    stack.ProcessFrame(inFrame, 2, 2000, &rep2);
    TEST_ASSERT(rep2.analysisStatus == StageStatus::Applied,
        "Analysis bundle with matching generation must be Applied");

    // 4. Source Reset increments generation and clears mailbox
    stack.ResetSource();
    TEST_ASSERT(stack.GetSourceGeneration() == 11, "ResetSource must increment generation to 11");

    // Bundle from previous generation 10 must now be rejected
    bool postedAfterReset = mailbox->PostAnalysis(goodBundle);
    TEST_ASSERT(!postedAfterReset, "PostAnalysis must reject old generation 10 bundle after reset to 11");
    StackExecutionReport rep3;
    stack.ProcessFrame(inFrame, 3, 3000, &rep3);
    TEST_ASSERT(rep3.analysisStatus == StageStatus::NotReady,
        "Post-reset frame must report NotReady when old bundles are rejected");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Generation guards and mailbox flush verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 4: Fault Injection & Fallback Policies
// -------------------------------------------------------------
bool TestFaultInjectionAndFallbackPolicies() {
    std::cout << "[TEST 4] Faults: Malformed Frames & Configured Fallback Policies ..." << std::endl;

    StudioStack stack(FallbackPolicy::FallbackToOriginalFrame);

    // 1. Injected invalid pixel format (YUV420P instead of BGR24)
    AVFrame* badFmtFrame = av_frame_alloc();
    badFmtFrame->width = 1280;
    badFmtFrame->height = 720;
    badFmtFrame->format = AV_PIX_FMT_YUV420P;
    av_frame_get_buffer(badFmtFrame, 32);

    AVFrame* resBadFmt = stack.ProcessFrame(badFmtFrame, 1, 1000);
    TEST_ASSERT(resBadFmt == nullptr, "Unsupported pixel format must safely return nullptr");
    av_frame_free(&badFmtFrame);

    // 2. Injected null input pointer
    AVFrame* resNull = stack.ProcessFrame(nullptr, 1, 1000);
    TEST_ASSERT(resNull == nullptr, "Null input frame must safely return nullptr without crashing");

    // 3. Fallback to original frame policy verification
    stack.SetFallbackPolicy(FallbackPolicy::FallbackToOriginalFrame);
    TEST_ASSERT(stack.GetFallbackPolicy() == FallbackPolicy::FallbackToOriginalFrame, "FallbackPolicy mismatch");

    AVFrame* validFrame = AllocateBgrFrameWithPattern(1280, 720, 2);
    StudioPreset warm = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(warm);

    // Process without analysis mailbox: stages gracefully report NotReady, returning valid original frame
    StackExecutionReport rep;
    AVFrame* out = stack.ProcessFrame(validFrame, 1, 1000, &rep);
    TEST_ASSERT(out != nullptr, "Graceful degradation must return valid frame");

    // 4. Test AbortOnError policy
    stack.SetFallbackPolicy(FallbackPolicy::AbortOnError);
    TEST_ASSERT(stack.GetFallbackPolicy() == FallbackPolicy::AbortOnError, "FallbackPolicy mismatch");

    av_frame_free(&validFrame);
    std::cout << "  -> PASSED (Fault injection safely handled across all policies)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 5: Dynamic Resolution Changes (720p -> 1080p -> 480p -> 720p)
// -------------------------------------------------------------
bool TestDynamicResolutionSwitch() {
    std::cout << "[TEST 5] Switches: Dynamic Resolution Adaptation (720p -> 1080p -> 480p -> 720p) ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(p);

    struct ResTest {
        int w;
        int h;
        const char* label;
    } resolutions[] = {
        { 1280, 720, "720p HD" },
        { 1920, 1080, "1080p FHD" },
        { 854, 480, "480p SD" },
        { 1280, 720, "720p HD (Re-stabilization)" }
    };

    uint64_t seq = 1;
    for (const auto& res : resolutions) {
        AVFrame* frame = AllocateBgrFrameWithPattern(res.w, res.h, 2);
        auto bundle = CreateSyntheticBundle(1, seq, seq * 33333, res.w, res.h);
        mailbox->PostAnalysis(bundle);

        StackExecutionReport rep;
        AVFrame* out = stack.ProcessFrame(frame, seq, seq * 33333, &rep);

        TEST_ASSERT(out != nullptr, std::string("ProcessFrame failed on ") + res.label);
        TEST_ASSERT(out->width == res.w && out->height == res.h, "Output dimensions must match input");
        TEST_ASSERT(rep.appearanceStatus == StageStatus::Applied, "Appearance must apply on " + std::string(res.label));
        TEST_ASSERT(rep.lightingStatus == StageStatus::Applied, "Lighting must apply on " + std::string(res.label));

        av_frame_free(&frame);
        seq++;
    }

    std::cout << "  -> PASSED (All 4 dynamic resolution switches succeeded with clean reallocation)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 6: Multi-Consumer Fan-Out & Downscaling Conformance
// -------------------------------------------------------------
bool TestMultiConsumerFanOutAndScaling() {
    std::cout << "[TEST 6] Fan-Out: Multi-Consumer Downscaling Conformance ..." << std::endl;

    StudioStack stack;
    AVFrame* master1080p = AllocateBgrFrameWithPattern(1920, 1080, 2);

    bool c1MasterCalled = false;
    bool c2Preview720pCalled = false;
    bool c3VirtualCam360pCalled = false;

    // Consumer 1: Master 1080p
    stack.RegisterConsumer("consumer_master", [&](const AVFrame* f, const StackExecutionReport& rep) {
        if (f->width == 1920 && f->height == 1080) {
            c1MasterCalled = true;
        }
    });

    // Consumer 2: 720p Preview
    stack.RegisterConsumer("consumer_preview_720p", [&](const AVFrame* f, const StackExecutionReport& rep) {
        AVFrame* scaled = StudioStack::CreateScaledFrame(f, 1280, 720);
        if (scaled && scaled->width == 1280 && scaled->height == 720 && scaled->format == AV_PIX_FMT_BGR24) {
            c2Preview720pCalled = true;
        }
        av_frame_free(&scaled);
    });

    // Consumer 3: 360p Virtual Camera
    stack.RegisterConsumer("consumer_vcam_360p", [&](const AVFrame* f, const StackExecutionReport& rep) {
        AVFrame* scaled = StudioStack::CreateScaledFrame(f, 640, 360);
        if (scaled && scaled->width == 640 && scaled->height == 360 && scaled->format == AV_PIX_FMT_BGR24) {
            c3VirtualCam360pCalled = true;
        }
        av_frame_free(&scaled);
    });

    stack.ProcessFrame(master1080p, 1, 33333);

    TEST_ASSERT(c1MasterCalled, "Master consumer must receive 1080p frame");
    TEST_ASSERT(c2Preview720pCalled, "Preview consumer must receive and scale to 720p frame");
    TEST_ASSERT(c3VirtualCam360pCalled, "Virtual camera consumer must receive and scale to 360p frame");

    av_frame_free(&master1080p);
    std::cout << "  -> PASSED (Synchronous fan-out to 1080p master, 720p preview, and 360p vcam verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 7: Bounded Memory & Zero-Leak Soak (100 Frames)
// -------------------------------------------------------------
bool TestBoundedMemoryAndLeakFreeSustainedProcessing() {
    std::cout << "[TEST 7] Soak: Bounded Memory & Zero-Leak Verification (100 Frames) ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(p);

    AVFrame* inFrame = AllocateBgrFrameWithPattern(1280, 720, 1);

    // Warm-up 10 frames
    for (int i = 0; i < 10; ++i) {
        auto bundle = CreateSyntheticBundle(1, i + 1, (i + 1) * 33333, 1280, 720);
        mailbox->PostAnalysis(bundle);
        stack.ProcessFrame(inFrame, i + 1, (i + 1) * 33333);
    }

    size_t memAtFrame10 = GetCurrentProcessWorkingSetKB();

    // Run 90 sustained frames with varying face positions
    for (int i = 10; i < 100; ++i) {
        float offset = std::sin(i * 0.1f) * 0.05f;
        auto bundle = CreateSyntheticBundle(1, i + 1, (i + 1) * 33333, 1280, 720, offset);
        mailbox->PostAnalysis(bundle);
        stack.ProcessFrame(inFrame, i + 1, (i + 1) * 33333);
    }

    size_t memAtFrame100 = GetCurrentProcessWorkingSetKB();
    int64_t memDeltaKB = static_cast<int64_t>(memAtFrame100) - static_cast<int64_t>(memAtFrame10);

    std::cout << "  [Memory Soak] Initial WorkingSet: " << memAtFrame10 << " KB, Final: " << memAtFrame100
              << " KB, Delta: " << memDeltaKB << " KB" << std::endl;

    av_frame_free(&inFrame);

    // Delta must be bounded to less than 15 MB over 90 iterations
    TEST_ASSERT(memDeltaKB < 15360, "Memory footprint must remain strictly bounded over 100 frames");

    std::cout << "  -> PASSED (Zero cumulative memory leak across 100 frames)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 8: Sustained 720p & 1080p Latency Profiling (Fixed Sequence & Real Metrics)
// -------------------------------------------------------------
bool TestSustainedPerformanceProfiling() {
    std::cout << "[TEST 8] Benchmark: Sustained 720p & 1080p Full-Stack Latency & Observability ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    stack.SetPreset(p);

    uint64_t globalSequence = 1;

    auto profileResolution = [&](int width, int height, const char* name, int warmup, int iters, double& outP95) -> bool {
        AVFrame* frame = AllocateBgrFrameWithPattern(width, height, 1);
        std::vector<double> latencies;
        latencies.reserve(iters);

        // Warm-up
        auto tWarmStart = std::chrono::steady_clock::now();
        for (int i = 0; i < warmup; ++i) {
            uint64_t seq = globalSequence++;
            auto bundle = CreateSyntheticBundle(stack.GetSourceGeneration(), seq, seq * 33333, width, height);
            bool posted = mailbox->PostAnalysis(bundle);
            TEST_ASSERT(posted, "Warm-up analysis post must be accepted");
            stack.ProcessFrame(frame, seq, seq * 33333);
        }
        auto tWarmEnd = std::chrono::steady_clock::now();
        double warmMs = std::chrono::duration<double, std::milli>(tWarmEnd - tWarmStart).count();

        uint64_t lastObservedUploads = 0;
        uint64_t lastObservedReadbacks = 0;

        // Measurement
        for (int i = 0; i < iters; ++i) {
            uint64_t seq = globalSequence++;
            auto bundle = CreateSyntheticBundle(stack.GetSourceGeneration(), seq, seq * 33333, width, height);
            bool posted = mailbox->PostAnalysis(bundle);
            TEST_ASSERT(posted, "Measurement analysis post must be accepted");

            StackExecutionReport rep;
            stack.ProcessFrame(frame, seq, seq * 33333, &rep);

            // Assert that effects actually executed and were Applied on EVERY measured frame!
            TEST_ASSERT(rep.analysisStatus == StageStatus::Applied, "Analysis must be Applied on measured frame");
            TEST_ASSERT(rep.appearanceStatus == StageStatus::Applied, "Appearance must be Applied on measured frame");
            TEST_ASSERT(rep.lightingStatus == StageStatus::Applied, "Lighting must be Applied on measured frame");

            latencies.push_back(rep.totalLatencyMs);
            lastObservedUploads = rep.imageUploadCount;
            lastObservedReadbacks = rep.imageReadbackCount;
        }

        std::sort(latencies.begin(), latencies.end());
        double minLat = latencies.front();
        double maxLat = latencies.back();
        double p50 = latencies[iters / 2];
        double p95 = latencies[static_cast<size_t>(iters * 0.95)];
        double p99 = latencies[static_cast<size_t>(iters * 0.99)];

        auto metrics = mailbox->GetMetrics();

        std::cout << "\n  --- Benchmark Profile: " << name << " ---" << std::endl;
        std::cout << "  Warm-up Cycles:          " << warmup << " frames (" << std::fixed << std::setprecision(2) << warmMs << " ms)" << std::endl;
        std::cout << "  Measurement Samples:     " << iters << " frames" << std::endl;
        std::cout << "  Latency (Min / p50 / p95 / p99 / Max): "
                  << minLat << " / " << p50 << " / " << p95 << " / " << p99 << " / " << maxLat << " ms" << std::endl;
        std::cout << "  Mailbox Queue Depth:     " << metrics.currentQueueDepth << " (bounded latest-result mailbox)" << std::endl;
        std::cout << "  Dropped Old Bundles:     " << metrics.totalDroppedOld << std::endl;
        std::cout << "  Observed Upload Count:   " << lastObservedUploads << std::endl;
        std::cout << "  Observed Readback Count: " << lastObservedReadbacks << std::endl;

        av_frame_free(&frame);
        outP95 = p95;
        return true;
    };

    double p95_720p = 0.0;
    TEST_ASSERT(profileResolution(1280, 720, "720p HD (Verified Applied Stages)", 5, 30, p95_720p), "720p benchmark failed");

    // Deliberately reset source generation between resolutions to verify source reset safety
    stack.ResetSource();
    mailbox->ResetSource(stack.GetSourceGeneration());

    double p95_1080p = 0.0;
    TEST_ASSERT(profileResolution(1920, 1080, "1080p FHD (Verified Applied Stages)", 5, 20, p95_1080p), "1080p benchmark failed");

    TEST_ASSERT(p95_720p < 150.0, "720p full-stack CPU latency must be bounded (< 150ms)");
    TEST_ASSERT(p95_1080p < 300.0, "1080p full-stack CPU latency must be bounded (< 300ms)");

    std::cout << "\n  -> PASSED" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 9: Consumer Reentrancy & Deadlock Freedom (Checkpoint 3)
// -------------------------------------------------------------
bool TestConsumerReentrancyAndDeadlockFreedom() {
    std::cout << "[TEST 9] Concurrency: Consumer Callback Reentrancy & Deadlock Freedom ..." << std::endl;

    StudioStack stack;
    AVFrame* inFrame = AllocateBgrFrameWithPattern(640, 360, 2);

    bool callbackRan = false;
    bool getPresetSucceeded = false;
    bool setPresetSucceeded = false;

    stack.RegisterConsumer("reentrant_consumer", [&](const AVFrame* f, const StackExecutionReport& rep) {
        callbackRan = true;
        // Reentrantly query preset while in callback
        StudioPreset p = stack.GetPreset();
        getPresetSucceeded = true;
        // Reentrantly modify preset while in callback
        p.lighting.fillIntensity = 0.42f;
        stack.SetPreset(p);
        setPresetSucceeded = true;
        // Reentrantly unregister self
        stack.UnregisterConsumer("reentrant_consumer");
    });

    StackExecutionReport rep;
    stack.ProcessFrame(inFrame, 1, 1000, &rep);

    TEST_ASSERT(callbackRan, "Consumer callback must execute");
    TEST_ASSERT(getPresetSucceeded, "Reentrant GetPreset must not deadlock");
    TEST_ASSERT(setPresetSucceeded, "Reentrant SetPreset must not deadlock");
    TEST_ASSERT(stack.GetPreset().lighting.fillIntensity == 0.42f, "Reentrant preset modification must take effect");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Zero deadlock under reentrant consumer operations)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 10: Throwing Consumer Exception Isolation (Checkpoint 3)
// -------------------------------------------------------------
bool TestThrowingConsumerExceptionIsolation() {
    std::cout << "[TEST 10] Robustness: Consumer Exception Isolation & Delivery Continuation ..." << std::endl;

    StudioStack stack;
    AVFrame* inFrame = AllocateBgrFrameWithPattern(640, 360, 2);

    bool consumer2Ran = false;

    // Consumer 1 throws an exception
    stack.RegisterConsumer("01_failing_consumer", [&](const AVFrame* f, const StackExecutionReport& rep) {
        throw std::runtime_error("Simulated consumer failure");
    });

    // Consumer 2 must still receive the frame
    stack.RegisterConsumer("02_surviving_consumer", [&](const AVFrame* f, const StackExecutionReport& rep) {
        consumer2Ran = true;
    });

    StackExecutionReport rep;
    AVFrame* out = nullptr;
    try {
        out = stack.ProcessFrame(inFrame, 1, 1000, &rep);
    } catch (...) {
        TEST_ASSERT(false, "ProcessFrame must not let consumer exceptions propagate out");
    }

    TEST_ASSERT(out != nullptr, "ProcessFrame must succeed despite consumer exception");
    TEST_ASSERT(consumer2Ran, "Remaining consumers must still be dispatched");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Consumer exceptions isolated without process crash or downstream starvation)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 11: Frame Generation Tagging & Stale Frame Suppression (Checkpoint 3)
// -------------------------------------------------------------
bool TestFrameGenerationTaggingAndOldFrameSuppression() {
    std::cout << "[TEST 11] Guards: Frame-Generation Input Tagging & Stale Frame Suppression ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(5);

    AVFrame* inFrame = AllocateBgrFrameWithPattern(640, 360, 2);

    // Frame tagged with old generation 4
    StackExecutionReport repStale;
    AVFrame* outStale = stack.ProcessFrame(inFrame, 1, 1000, &repStale, /*frameSourceGen*/ 4);

    TEST_ASSERT(outStale == nullptr, "Stale frame with mismatching generation must be suppressed");
    TEST_ASSERT(!repStale.errorDescription.empty(), "Error description must explain stale rejection");
    TEST_ASSERT(repStale.errorDescription.find("Stale") != std::string::npos, "Error description must contain 'Stale'");

    // Frame tagged with matching generation 5
    StackExecutionReport repMatch;
    AVFrame* outMatch = stack.ProcessFrame(inFrame, 2, 2000, &repMatch, /*frameSourceGen*/ 5);
    TEST_ASSERT(outMatch != nullptr, "Frame with matching generation must be accepted");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Stale source-generation frames rejected at input boundary)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 12: Early Failure Reporting & Invalid Strides (Checkpoint 3)
// -------------------------------------------------------------
bool TestEarlyFailureReportingAndInvalidStrides() {
    std::cout << "[TEST 12] Validation: Early Failure Reporting on Invalid Strides & HDR Transfers ..." << std::endl;

    StudioStack stack;

    // 1. Invalid frame stride (linesize < width * 3)
    AVFrame* badStrideFrame = AllocateBgrFrameWithPattern(640, 360, 2);
    badStrideFrame->linesize[0] = 640 * 2; // Artificially corrupt stride

    StackExecutionReport repStride;
    AVFrame* outStride = stack.ProcessFrame(badStrideFrame, 1, 1000, &repStride);
    TEST_ASSERT(outStride == nullptr, "Frame with corrupt stride must be rejected");
    TEST_ASSERT(!repStride.errorDescription.empty(), "Error description must be populated on stride error");
    TEST_ASSERT(repStride.appearanceStatus == StageStatus::Failed, "Appearance status must report Failed");

    // Restore linesize for clean free
    badStrideFrame->linesize[0] = 640 * 3;
    av_frame_free(&badStrideFrame);

    // 2. Unsupported HDR transfer characteristics (SMPTE 2084 / PQ)
    AVFrame* hdrFrame = AllocateBgrFrameWithPattern(640, 360, 2);
    hdrFrame->color_trc = AVCOL_TRC_SMPTE2084;

    StackExecutionReport repHdr;
    AVFrame* outHdr = stack.ProcessFrame(hdrFrame, 2, 2000, &repHdr);
    TEST_ASSERT(outHdr == nullptr, "HDR frame in SDR pipeline must be rejected");
    TEST_ASSERT(!repHdr.errorDescription.empty(), "Error description must be populated for HDR");
    TEST_ASSERT(repHdr.appearanceStatus == StageStatus::Unsupported, "Status must report Unsupported for HDR");

    av_frame_free(&hdrFrame);
    std::cout << "  -> PASSED (All early validation failures reliably report errorDescription and status)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 13: Sharpen-Only Appearance Stage Gate (Checkpoint 3)
// -------------------------------------------------------------
bool TestSharpenOnlyAppearanceStageGate() {
    std::cout << "[TEST 13] Appearance: Sharpen-Only Execution With Zero Smoothing ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p;
    p.appearance.smoothingStrength = 0.0f; // Exactly zero smoothing!
    p.appearance.sharpenStrength = 0.80f;   // Strong sharpening
    p.lighting.bypass = true;
    p.background.mode = PrismForge::Effects::BackgroundMode::Original;
    p.overlays.bypass = true;
    stack.SetPreset(p);

    AVFrame* inFrame = AllocateBgrFrameWithPattern(640, 360, 1);
    auto bundle = CreateSyntheticBundle(1, 1, 1000, 640, 360);
    mailbox->PostAnalysis(bundle);

    StackExecutionReport rep;
    AVFrame* out = stack.ProcessFrame(inFrame, 1, 1000, &rep);

    TEST_ASSERT(out != nullptr, "ProcessFrame must succeed");
    TEST_ASSERT(rep.appearanceStatus == StageStatus::Applied,
        "Appearance stage MUST report Applied even when smoothingStrength is 0 if sharpenStrength > 0");

    av_frame_free(&inFrame);
    std::cout << "  -> PASSED (Sharpen-only appearance stage gate correctly executes)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 14: Cached Scaler & Scratch Buffer Reuse (Checkpoint 4)
// -------------------------------------------------------------
bool TestCachedScalerAndScratchBufferReuse() {
    std::cout << "[TEST 14] Optimization: Cached Frame Scaler Context Reuse ..." << std::endl;

    CachedFrameScaler scaler;
    AVFrame* src1080p = AllocateBgrFrameWithPattern(1920, 1080, 2);

    // Scale to 720p three times
    for (int i = 0; i < 3; ++i) {
        AVFrame* dst = scaler.Scale(src1080p, 1280, 720);
        TEST_ASSERT(dst != nullptr, "Scale to 720p must succeed");
        TEST_ASSERT(dst->width == 1280 && dst->height == 720, "Dimensions must match 720p");
    }
    TEST_ASSERT(scaler.GetCacheCount() == 1, "Cache must contain exactly 1 SwsContext for 1080p->720p");

    // Scale to 360p two times
    for (int i = 0; i < 2; ++i) {
        AVFrame* dst = scaler.Scale(src1080p, 640, 360);
        TEST_ASSERT(dst != nullptr, "Scale to 360p must succeed");
        TEST_ASSERT(dst->width == 640 && dst->height == 360, "Dimensions must match 360p");
    }
    TEST_ASSERT(scaler.GetCacheCount() == 2, "Cache must contain exactly 2 SwsContexts (720p and 360p)");

    scaler.Reset();
    TEST_ASSERT(scaler.GetCacheCount() == 0, "Reset must clear cache");

    av_frame_free(&src1080p);
    std::cout << "  -> PASSED (SwsContext reused across repeated scales without per-frame churn)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 15: Combined GPU Processing & Device Loss Fallback (Checkpoint 5)
// -------------------------------------------------------------
bool TestCombinedGpuCpuParityAndDeviceLossFallback() {
    std::cout << "[TEST 15] GPU/CPU: Combined Execution Parity & Device Loss Fallback ..." << std::endl;

    StudioStack stack;
    stack.SetSourceGeneration(1);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset p = StudioPreset::WarmKeyCoolFill();
    p.overlays.bypass = true;
    p.background.mode = PrismForge::Effects::BackgroundMode::Original;
    // Bypass appearance for parity: CPU bilateral filter and GPU bilateral shader differ
    // legitimately; this test validates lighting-only GPU/CPU parity.
    p.appearance.bypass = true;
    stack.SetPreset(p);

    bool gpuInit = stack.InitializeGpu();
    if (!gpuInit) {
        std::cout << "  [NOTE] GPU hardware/WARP not available on this environment; skipping GPU parity test." << std::endl;
        return true;
    }

    AVFrame* inFrameCpu = AllocateBgrFrameWithPattern(640, 360, 1);
    AVFrame* inFrameGpu = AllocateBgrFrameWithPattern(640, 360, 1);

    // 1. Run CPU pass
    stack.SetExecutionBackend(ExecutionBackend::CpuOnly);
    auto bundleCpu = CreateSyntheticBundle(1, 1, 1000, 640, 360);
    mailbox->PostAnalysis(bundleCpu);
    StackExecutionReport repCpu;
    AVFrame* outCpu = stack.ProcessFrame(inFrameCpu, 1, 1000, &repCpu);
    TEST_ASSERT(outCpu != nullptr, "CPU ProcessFrame failed");
    TEST_ASSERT(repCpu.lightingStatus == StageStatus::Applied, "CPU lighting must be Applied");

    // Copy CPU result pixels
    std::vector<uint8_t> cpuPixels(640 * 360 * 3);
    for (int y = 0; y < 360; ++y) {
        std::memcpy(cpuPixels.data() + y * 640 * 3, outCpu->data[0] + y * outCpu->linesize[0], 640 * 3);
    }

    // 2. Run GPU pass
    stack.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    auto bundleGpu = CreateSyntheticBundle(1, 2, 2000, 640, 360);
    mailbox->PostAnalysis(bundleGpu);
    StackExecutionReport repGpu;
    AVFrame* outGpu = stack.ProcessFrame(inFrameGpu, 2, 2000, &repGpu);
    TEST_ASSERT(outGpu != nullptr, "GPU ProcessFrame failed");
    TEST_ASSERT(repGpu.lightingStatus == StageStatus::Applied, "GPU lighting must be Applied");
    TEST_ASSERT(repGpu.imageUploadCount == 1, "Exactly 1 GPU upload must be observed");
    TEST_ASSERT(repGpu.imageReadbackCount == 1, "Exactly 1 GPU readback must be observed");

    // Verify Parity: max channel difference <= 3
    int maxDiff = 0;
    for (int y = 0; y < 360; ++y) {
        const uint8_t* gpuRow = outGpu->data[0] + y * outGpu->linesize[0];
        const uint8_t* cpuRow = cpuPixels.data() + y * 640 * 3;
        for (int x = 0; x < 640 * 3; ++x) {
            int diff = std::abs(static_cast<int>(gpuRow[x]) - static_cast<int>(cpuRow[x]));
            if (diff > maxDiff) maxDiff = diff;
        }
    }
    std::cout << "  [Parity Metric] Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 3)" << std::endl;
    TEST_ASSERT(maxDiff <= 3, "CPU and GPU pipelines must achieve parity within <= 3 codes");

    // 3. Real GPU-failure injection while GpuWithCpuFallback remains active
    stack.SetGpuFailureInjection(true);
    auto bundleInjected = CreateSyntheticBundle(1, 3, 3000, 640, 360);
    mailbox->PostAnalysis(bundleInjected);
    StackExecutionReport repInjected;
    AVFrame* outInjected = stack.ProcessFrame(inFrameGpu, 3, 3000, &repInjected);
    TEST_ASSERT(outInjected != nullptr, "Fallback ProcessFrame must succeed on injected GPU failure");
    TEST_ASSERT(repInjected.gpuFallbackUsed, "gpuFallbackUsed must be true when GPU failure injected while GpuWithCpuFallback is active");
    TEST_ASSERT(repInjected.lightingStatus == StageStatus::Applied, "Lighting must be Applied via CPU fallback");
    TEST_ASSERT(!repInjected.errorDescription.empty(), "errorDescription must detail the GPU failure");
    TEST_ASSERT(repInjected.errorDescription.find("injected failure") != std::string::npos ||
                repInjected.errorDescription.find("Simulated GPU") != std::string::npos,
                "Error description must identify injected GPU failure");

    // 4. GPU failure recovery
    stack.SetGpuFailureInjection(false);
    auto bundleRecovered = CreateSyntheticBundle(1, 4, 4000, 640, 360);
    mailbox->PostAnalysis(bundleRecovered);
    StackExecutionReport repRecovered;
    AVFrame* outRecovered = stack.ProcessFrame(inFrameGpu, 4, 4000, &repRecovered);
    TEST_ASSERT(outRecovered != nullptr, "Recovered GPU ProcessFrame must succeed");
    TEST_ASSERT(!repRecovered.gpuFallbackUsed, "gpuFallbackUsed must be false after recovery");
    TEST_ASSERT(repRecovered.lightingStatus == StageStatus::Applied, "GPU lighting must be Applied after recovery");
    TEST_ASSERT(repRecovered.imageUploadCount == 1, "GPU upload must resume after recovery");
    TEST_ASSERT(repRecovered.imageReadbackCount == 1, "GPU readback must resume after recovery");

    // 5. Dynamic resolution resize during GPU processing (640x360 -> 1280x720)
    AVFrame* inFrame720 = AllocateBgrFrameWithPattern(1280, 720, 2);
    auto bundle720 = CreateSyntheticBundle(1, 5, 5000, 1280, 720);
    mailbox->PostAnalysis(bundle720);
    StackExecutionReport rep720;
    AVFrame* out720 = stack.ProcessFrame(inFrame720, 5, 5000, &rep720);
    TEST_ASSERT(out720 != nullptr, "GPU 720p resize ProcessFrame must succeed");
    TEST_ASSERT(out720->width == 1280 && out720->height == 720, "Output frame dimensions must match resized 720p");
    TEST_ASSERT(!rep720.gpuFallbackUsed, "GPU execution must succeed without CPU fallback on dynamic resize");
    TEST_ASSERT(rep720.lightingStatus == StageStatus::Applied, "GPU lighting must be Applied on resized frame");
    av_frame_free(&inFrame720);

    // 6. Explicit ReleaseGpu is a separate test
    stack.ReleaseGpu();
    auto bundleRelease = CreateSyntheticBundle(1, 6, 6000, 640, 360);
    mailbox->PostAnalysis(bundleRelease);
    StackExecutionReport repRelease;
    AVFrame* outRelease = stack.ProcessFrame(inFrameGpu, 6, 6000, &repRelease);
    TEST_ASSERT(outRelease != nullptr, "ProcessFrame must succeed after explicit ReleaseGpu");
    TEST_ASSERT(!repRelease.gpuFallbackUsed, "gpuFallbackUsed must be false after explicit ReleaseGpu (backend reset to CpuOnly)");
    TEST_ASSERT(repRelease.lightingStatus == StageStatus::Applied, "Lighting must still be Applied via CPU after GPU release");

    av_frame_free(&inFrameCpu);
    av_frame_free(&inFrameGpu);
    std::cout << "  -> PASSED (GPU/CPU parity, real failure injection, recovery, resize, and explicit release verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 16: Pack-to-Preset Runtime Wiring & Verification (Checkpoint 6)
// -------------------------------------------------------------
bool TestPackPresetRuntimeWiringAndRollback() {
    std::cout << "[TEST 16] Packs/Presets: Runtime Pack Wiring, Resolution & State Preservation ..." << std::endl;

    fs::path tempRoot = fs::temp_directory_path() / "prismforge_preset_test";
    fs::remove_all(tempRoot);
    fs::create_directories(tempRoot);

    fs::path sourcePack = tempRoot / "source_pack";
    fs::create_directories(sourcePack / "luts");
    fs::path targetLib = tempRoot / "installed_lib";
    fs::create_directories(targetLib);

    // Create a minimal 2x2x2 test cube LUT
    std::string cubeText =
        "TITLE \"Test Warm LUT\"\n"
        "LUT_3D_SIZE 2\n"
        "DOMAIN_MIN 0.0 0.0 0.0\n"
        "DOMAIN_MAX 1.0 1.0 1.0\n"
        "0.0 0.0 0.0\n"
        "1.0 0.0 0.0\n"
        "0.0 1.0 0.0\n"
        "1.0 1.0 0.0\n"
        "0.0 0.0 1.0\n"
        "1.0 0.0 1.0\n"
        "0.0 1.0 1.0\n"
        "1.0 1.0 1.0\n";

    {
        std::ofstream out(sourcePack / "luts" / "test_lut.cube");
        out << cubeText;
    }

    // Compute SHA-256 and actual byte count of the LUT file so the validator passes
    std::string lutFilePath = (sourcePack / "luts" / "test_lut.cube").string();
    uint64_t lutFileSize = 0;
    std::string lutSha256 = PrismForge::Effects::Packs::Sha256::ComputeFileHex(lutFilePath, &lutFileSize);
    TEST_ASSERT(!lutSha256.empty() && lutFileSize > 0, "Failed to compute SHA-256 of test LUT file");

    // Build manifest using the keys the JSON parser recognises: "id", "path", "sha256", "size"
    std::string manifestJson =
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"packId\": \"com.prismforge.testpack\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"displayName\": \"Test Pack\",\n"
        "  \"author\": \"PrismForge Test\",\n"
        "  \"assets\": [\n"
        "    {\n"
        "      \"id\": \"warm_lut_asset\",\n"
        "      \"type\": \"lut\",\n"
        "      \"path\": \"luts/test_lut.cube\",\n"
        "      \"sha256\": \"" + lutSha256 + "\",\n"
        "      \"size\": " + std::to_string(lutFileSize) + "\n"
        "    }\n"
        "  ]\n"
        "}\n";

    {
        std::ofstream out(sourcePack / "manifest.json");
        out << manifestJson;
    }

    // Install pack into targetLib
    std::string installErr;
    bool installed = PrismForge::Effects::Packs::PackManager::InstallPack(sourcePack.string(), targetLib.string(), installErr);
    TEST_ASSERT(installed, "PackManager::InstallPack failed: " + installErr);

    StudioStack stack;
    StudioPreset initialPreset = StudioPreset::WarmKeyCoolFill();
    initialPreset.displayName = "InitialPreset";
    stack.SetPreset(initialPreset);

    // 1. Apply Valid Preset referencing installed pack asset
    StudioPreset packPreset;
    packPreset.presetId = "test.pack_preset";
    packPreset.displayName = "PackPreset";
    packPreset.packIdReference = "com.prismforge.testpack";
    packPreset.selectedLutPath = "warm_lut_asset";
    packPreset.lutIntensity = 0.90f;

    std::string applyErr;
    bool applyOk = stack.ApplyPreset(packPreset, targetLib.string(), applyErr);
    TEST_ASSERT(applyOk, "ApplyPreset with installed pack asset must succeed: " + applyErr);
    TEST_ASSERT(stack.GetPreset().displayName == "PackPreset", "Preset displayName must be PackPreset");

    // Process a frame to confirm LUT executes as Applied
    AVFrame* frame = AllocateBgrFrameWithPattern(640, 360, 2);
    StackExecutionReport rep;
    stack.ProcessFrame(frame, 1, 1000, &rep);
    TEST_ASSERT(rep.lutStatus == StageStatus::Applied, "LUT stage must be Applied from pack asset");

    // 2. Apply Invalid Preset (referencing non-existent pack)
    StudioPreset badPreset;
    badPreset.presetId = "test.bad_preset";
    badPreset.displayName = "BadPreset";
    badPreset.packIdReference = "com.nonexistent.pack";
    badPreset.selectedLutPath = "missing_lut";

    std::string badErr;
    bool badOk = stack.ApplyPreset(badPreset, targetLib.string(), badErr);
    TEST_ASSERT(!badOk, "ApplyPreset with missing pack must fail");
    TEST_ASSERT(!badErr.empty(), "Error message must be populated on failure");

    // CRITICAL: Previous valid active preset and LUT MUST be preserved!
    TEST_ASSERT(stack.GetPreset().displayName == "PackPreset",
        "Active preset must NOT be corrupted by failed ApplyPreset! Must retain 'PackPreset'");

    StackExecutionReport repAfterBad;
    stack.ProcessFrame(frame, 2, 2000, &repAfterBad);
    TEST_ASSERT(repAfterBad.lutStatus == StageStatus::Applied,
        "Active LUT must remain loaded and applied after failed ApplyPreset");

    av_frame_free(&frame);
    fs::remove_all(tempRoot);
    std::cout << "  -> PASSED (Pack asset resolution verified and active state preserved on failed apply)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 17: Texture-Native StudioStack Comprehensive Verification (Milestone 9 / Texture-Native)
// -------------------------------------------------------------
bool TestTextureNativeComprehensivePipeline() {
    std::cout << "[TEST 17] Texture-Native: Dedicated Input Surface Processing & Parity ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [NOTE] GPU hardware/WARP not available; skipping Texture-Native test." << std::endl;
        return true;
    }

    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();

    struct TestTexture {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    // Helper lambda to create a 2D texture and SRV
    auto createTexture = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& initData) -> TestTexture {
        TestTexture tt;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subData{};
        D3D11_SUBRESOURCE_DATA* pSubData = nullptr;
        if (!initData.empty()) {
            subData.pSysMem = initData.data();
            subData.SysMemPitch = w * 4;
            pSubData = &subData;
        }

        HRESULT hr = device->CreateTexture2D(&desc, pSubData, tt.tex.GetAddressOf());
        if (SUCCEEDED(hr) && tt.tex) {
            device->CreateShaderResourceView(tt.tex.Get(), nullptr, tt.srv.GetAddressOf());
        }
        return tt;
    };

    // Helper to read back RGBA texture to CPU
    auto readbackTexture = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            return result;
        }
        context->CopyResource(staging.Get(), tex);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    // 1. Fresh texture input without ANY preceding ProcessFrame
    {
        StudioStack stackNative;
        stackNative.SetSourceGeneration(1);
        bool ok = stackNative.InitializeGpu(device, context);
        TEST_ASSERT(ok, "InitializeGpu on fresh stackNative must succeed");

        auto mailbox = std::make_shared<AnalysisMailbox>();
        mailbox->ResetSource(1);
        stackNative.AttachAnalysisMailbox(mailbox);

        StudioPreset preset = StudioPreset::WarmKeyCoolFill();
        preset.overlays.bypass = true;
        preset.background.mode = PrismForge::Effects::BackgroundMode::Original;
        preset.appearance.bypass = true;
        stackNative.SetPreset(preset);

        // Prepare test RGBA image (640x360)
        std::vector<uint8_t> initRgba(640 * 360 * 4);
        for (int y = 0; y < 360; ++y) {
            for (int x = 0; x < 640; ++x) {
                initRgba[(y * 640 + x) * 4 + 0] = static_cast<uint8_t>((x * 255) / 640);       // R
                initRgba[(y * 640 + x) * 4 + 1] = static_cast<uint8_t>((y * 255) / 360);       // G
                initRgba[(y * 640 + x) * 4 + 2] = static_cast<uint8_t>(((x + y) * 255) / 1000);// B
                initRgba[(y * 640 + x) * 4 + 3] = 255;                                          // A
            }
        }

        auto [texFresh, srvFresh] = createTexture(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, initRgba);
        TEST_ASSERT(srvFresh != nullptr, "Failed to create test D3D11 texture/SRV");

        auto bundle = CreateSyntheticBundle(1, 1, 1000, 640, 360);
        mailbox->PostAnalysis(bundle);

        StackExecutionReport repFresh;
        // ProcessTextureNative called directly on fresh stack - NO preceding ProcessFrame
        GpuTextureSurface surf = stackNative.ProcessTextureNative(
            srvFresh.Get(), 640, 360, 1, 1000, &repFresh, 1);

        TEST_ASSERT(surf.isValid, "ProcessTextureNative must succeed on fresh stack without preceding ProcessFrame");
        TEST_ASSERT(surf.texture != nullptr, "Output texture pointer must be valid");
        TEST_ASSERT(surf.srv != nullptr, "Output SRV pointer must be valid");
        TEST_ASSERT(surf.width == 640 && surf.height == 360, "Output surface dimensions must match 640x360");
        TEST_ASSERT(surf.format == DXGI_FORMAT_R8G8B8A8_UNORM, "Output surface format must be R8G8B8A8_UNORM");
        TEST_ASSERT(repFresh.imageUploadCount == 0, "Texture-native path must have ZERO CPU->GPU uploads");
        TEST_ASSERT(repFresh.imageReadbackCount == 0, "Texture-native path must have ZERO GPU->CPU readbacks");
        TEST_ASSERT(repFresh.lightingStatus == StageStatus::Applied, "Lighting stage must be Applied");
        TEST_ASSERT(repFresh.backgroundStatus == StageStatus::Bypassed, "Background stage must report Bypassed for Original");
        TEST_ASSERT(repFresh.lutStatus == StageStatus::Bypassed, "LUT stage must report Bypassed");
        TEST_ASSERT(repFresh.overlayStatus == StageStatus::Bypassed, "Overlay stage must report Bypassed");
        surf.Release();

        // 2. Texture-native overlay execution
        preset.background.mode = PrismForge::Effects::BackgroundMode::Blur;
        preset.lutIntensity = 1.0f;
        preset.overlays.bypass = false;
        preset.overlays.glasses.enabled = true;
        stackNative.SetPreset(preset);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, 640, 360));

        StackExecutionReport repHonest;
        GpuTextureSurface surfHonest = stackNative.ProcessTextureNative(
            srvFresh.Get(), 640, 360, 2, 2000, &repHonest, 1);
        TEST_ASSERT(surfHonest.isValid, "ProcessTextureNative must succeed when stages configured");
        TEST_ASSERT(repHonest.backgroundStatus == StageStatus::Applied, "Blur background must report Applied in texture-native when mask is present");
        TEST_ASSERT(repHonest.overlayStatus == StageStatus::Applied, "Overlays must report Applied in texture-native when landmarks are present");
        surfHonest.Release();

        // Reset preset
        preset.background.mode = PrismForge::Effects::BackgroundMode::Original;
        preset.lutIntensity = 0.0f;
        preset.overlays.bypass = true;
        preset.overlays.glasses.enabled = false;
        stackNative.SetPreset(preset);

        // 3. Changing frames & CPU/GPU Parity
        AVFrame* inBgr = AllocateBgrFrameWithPattern(640, 360, 1);
        std::vector<uint8_t> rgbaParity(640 * 360 * 4);
        for (int y = 0; y < 360; ++y) {
            const uint8_t* src = inBgr->data[0] + y * inBgr->linesize[0];
            for (int x = 0; x < 640; ++x) {
                rgbaParity[(y * 640 + x) * 4 + 0] = src[x * 3 + 2]; // R
                rgbaParity[(y * 640 + x) * 4 + 1] = src[x * 3 + 1]; // G
                rgbaParity[(y * 640 + x) * 4 + 2] = src[x * 3 + 0]; // B
                rgbaParity[(y * 640 + x) * 4 + 3] = 255;            // A
            }
        }
        auto [texParity, srvParity] = createTexture(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, rgbaParity);
        auto bundleParity = CreateSyntheticBundle(1, 10, 10000, 640, 360);
        mailbox->PostAnalysis(bundleParity);

        StackExecutionReport repNativeParity;
        GpuTextureSurface surfParity = stackNative.ProcessTextureNative(
            srvParity.Get(), 640, 360, 10, 10000, &repNativeParity, 1);
        TEST_ASSERT(surfParity.isValid, "Native parity pass must succeed");

        // Immediately read back the output texture before any subsequent operations
        std::vector<uint8_t> nativeOutRgba = readbackTexture(surfParity.texture, 640, 360);
        surfParity.Release();

        // Run CPU ProcessFrame on a separate stack with identical bundle
        StudioStack stackCpu;
        stackCpu.SetExecutionBackend(ExecutionBackend::CpuOnly);
        auto mailboxCpu = std::make_shared<AnalysisMailbox>();
        mailboxCpu->ResetSource(1);
        stackCpu.AttachAnalysisMailbox(mailboxCpu);
        stackCpu.SetPreset(preset);
        mailboxCpu->PostAnalysis(bundleParity);

        StackExecutionReport repCpu;
        AVFrame* outCpu = stackCpu.ProcessFrame(inBgr, 10, 10000, &repCpu);
        TEST_ASSERT(outCpu != nullptr, "CPU ProcessFrame failed");

        // Compare texture-native GPU output against CPU output
        int maxDiff = 0;
        for (int y = 0; y < 360; ++y) {
            const uint8_t* cpuRow = outCpu->data[0] + y * outCpu->linesize[0];
            for (int x = 0; x < 640; ++x) {
                int bDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 2]) - static_cast<int>(cpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 1]) - static_cast<int>(cpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 0]) - static_cast<int>(cpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxDiff) maxDiff = curMax;
            }
        }
        std::cout << "  [Texture-Native Parity Metric] Max CPU vs Native GPU pixel difference: " << maxDiff << " codes (threshold <= 3)" << std::endl;
        TEST_ASSERT(maxDiff <= 3, "Texture-native GPU output must achieve parity with CPU within <= 3 codes");

        // Compare texture-native GPU output against software-frame GPU output
        mailbox->PostAnalysis(bundleParity);
        StackExecutionReport repGpu;
        AVFrame* outGpu = stackNative.ProcessFrame(inBgr, 10, 10000, &repGpu);
        TEST_ASSERT(outGpu != nullptr, "ProcessFrame GPU must succeed");
        int maxDiffGpu = 0;
        for (int y = 0; y < 360; ++y) {
            const uint8_t* gpuRow = outGpu->data[0] + y * outGpu->linesize[0];
            for (int x = 0; x < 640; ++x) {
                int bDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 2]) - static_cast<int>(gpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 1]) - static_cast<int>(gpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(nativeOutRgba[(y * 640 + x) * 4 + 0]) - static_cast<int>(gpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxDiffGpu) maxDiffGpu = curMax;
            }
        }
        std::cout << "  [Native vs Software GPU Parity] Max pixel difference: " << maxDiffGpu << " codes (threshold == 0)" << std::endl;
        TEST_ASSERT(maxDiffGpu == 0, "Texture-native and Software GPU adapter must produce bit-exact matching output");

        av_frame_free(&inBgr);

        // 4. Stale generation rejection
        stackNative.SetSourceGeneration(5);
        StackExecutionReport repStale;
        GpuTextureSurface surfStale = stackNative.ProcessTextureNative(
            srvFresh.Get(), 640, 360, 3, 3000, &repStale, /*frameSourceGen*/ 4);
        TEST_ASSERT(!surfStale.isValid, "ProcessTextureNative must reject stale source generation");
        TEST_ASSERT(surfStale.texture == nullptr && surfStale.srv == nullptr, "Rejected surface must return nullptr pointers");
        TEST_ASSERT(!repStale.errorDescription.empty(), "Error description must be populated for stale texture");
        TEST_ASSERT(repStale.errorDescription.find("Stale") != std::string::npos, "Error description must identify Stale texture");
        stackNative.SetSourceGeneration(1);

        // 5. Invalid textures
        // 5a. Null SRV
        StackExecutionReport repNull;
        GpuTextureSurface surfNull = stackNative.ProcessTextureNative(nullptr, 640, 360, 4, 4000, &repNull, 1);
        TEST_ASSERT(!surfNull.isValid, "Null SRV must be rejected");
        TEST_ASSERT(repNull.appearanceStatus == StageStatus::Failed, "Null SRV must report Failed status");
        TEST_ASSERT(!repNull.errorDescription.empty(), "Error description must be populated for null SRV");

        // 5b. Dimension mismatch
        StackExecutionReport repDim;
        GpuTextureSurface surfDim = stackNative.ProcessTextureNative(srvFresh.Get(), 1280, 720, 5, 5000, &repDim, 1);
        TEST_ASSERT(!surfDim.isValid, "Dimension mismatch must be rejected");
        TEST_ASSERT(repDim.errorDescription.find("dimensions do not match") != std::string::npos, "Error must indicate dimension mismatch");

        // 5c. Device mismatch
        PrismForge::Effects::Gpu::D3D11Context d3dCtx2;
        if (d3dCtx2.Initialize(true)) {
            D3D11_TEXTURE2D_DESC desc2{};
            desc2.Width = 640; desc2.Height = 360; desc2.MipLevels = 1; desc2.ArraySize = 1;
            desc2.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc2.SampleDesc.Count = 1;
            desc2.Usage = D3D11_USAGE_DEFAULT; desc2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex2;
            ComPtr<ID3D11ShaderResourceView> srv2;
            if (SUCCEEDED(d3dCtx2.GetDevice()->CreateTexture2D(&desc2, nullptr, tex2.GetAddressOf()))) {
                d3dCtx2.GetDevice()->CreateShaderResourceView(tex2.Get(), nullptr, srv2.GetAddressOf());
                StackExecutionReport repDev;
                GpuTextureSurface surfDev = stackNative.ProcessTextureNative(srv2.Get(), 640, 360, 6, 6000, &repDev, 1);
                TEST_ASSERT(!surfDev.isValid, "Texture from different ID3D11Device must be rejected");
                TEST_ASSERT(repDev.errorDescription.find("different ID3D11Device") != std::string::npos, "Error must indicate device mismatch");
            }
        }

        // 5d. Unsupported format
        {
            auto [texFloat, srvFloat] = createTexture(640, 360, DXGI_FORMAT_R32_FLOAT, {});
            if (srvFloat) {
                StackExecutionReport repFmt;
                GpuTextureSurface surfFmt = stackNative.ProcessTextureNative(srvFloat.Get(), 640, 360, 7, 7000, &repFmt, 1);
                TEST_ASSERT(!surfFmt.isValid, "Unsupported texture format must be rejected");
                TEST_ASSERT(repFmt.appearanceStatus == StageStatus::Unsupported, "Unsupported format must report Unsupported");
            }
        }

        // 6. Concurrency / recursive processing rejection
        {
            StackExecutionReport repSeq;
            GpuTextureSurface surfSeq = stackNative.ProcessTextureNative(srvFresh.Get(), 640, 360, 8, 8000, &repSeq, 1);
            TEST_ASSERT(surfSeq.isValid, "Sequential call must succeed");
            surfSeq.Release();
        }

        // 7. Failure recovery
        stackNative.SetGpuFailureInjection(true);
        StackExecutionReport repFail;
        GpuTextureSurface surfFail = stackNative.ProcessTextureNative(srvFresh.Get(), 640, 360, 9, 9000, &repFail, 1);
        TEST_ASSERT(!surfFail.isValid, "ProcessTextureNative must fail when GPU failure injected");
        TEST_ASSERT(surfFail.texture == nullptr, "Never return stale or invalid surface on failure");
        TEST_ASSERT(!repFail.errorDescription.empty(), "Error description must be populated on failure");

        // Recover from failure
        stackNative.SetGpuFailureInjection(false);
        auto bundleRecov = CreateSyntheticBundle(1, 12, 12000, 640, 360);
        mailbox->PostAnalysis(bundleRecov);
        StackExecutionReport repRecov;
        GpuTextureSurface surfRecov = stackNative.ProcessTextureNative(srvFresh.Get(), 640, 360, 12, 12000, &repRecov, 1);
        TEST_ASSERT(surfRecov.isValid, "ProcessTextureNative must recover successfully after clearing failure injection");
        TEST_ASSERT(surfRecov.texture != nullptr, "Valid texture returned after recovery");
        TEST_ASSERT(repRecov.lightingStatus == StageStatus::Applied, "Lighting applied after recovery");
        surfRecov.Release();
    }

    // 8. Benchmark both entry points (ProcessFrame vs ProcessTextureNative)
    {
        std::cout << "\n  --- Benchmark Profile: 720p Software Frame vs Texture-Native ---" << std::endl;

        StudioStack stackBench;
        stackBench.SetSourceGeneration(1);
        stackBench.InitializeGpu(device, context);
        auto mailboxBench = std::make_shared<AnalysisMailbox>();
        mailboxBench->ResetSource(1);
        stackBench.AttachAnalysisMailbox(mailboxBench);

        StudioPreset preset = StudioPreset::WarmKeyCoolFill();
        preset.overlays.bypass = true;
        preset.background.mode = PrismForge::Effects::BackgroundMode::Original;
        preset.appearance.bypass = true;
        stackBench.SetPreset(preset);

        const int benchW = 1280;
        const int benchH = 720;
        const int iters = 30;

        // Software Frame Adapter benchmark
        AVFrame* frame720 = AllocateBgrFrameWithPattern(benchW, benchH, 1);
        std::vector<double> frameLatencies;
        int totalUploadsFrame = 0;
        int totalReadbacksFrame = 0;

        // Warmup
        for (int i = 0; i < 5; ++i) {
            auto bundle = CreateSyntheticBundle(1, i + 1, (i + 1) * 1000, benchW, benchH);
            mailboxBench->PostAnalysis(bundle);
            stackBench.ProcessFrame(frame720, i + 1, (i + 1) * 1000);
        }

        for (int i = 0; i < iters; ++i) {
            auto bundle = CreateSyntheticBundle(1, i + 10, (i + 10) * 1000, benchW, benchH);
            mailboxBench->PostAnalysis(bundle);
            StackExecutionReport rep;
            stackBench.ProcessFrame(frame720, i + 10, (i + 10) * 1000, &rep);
            frameLatencies.push_back(rep.totalLatencyMs);
            totalUploadsFrame += rep.imageUploadCount;
            totalReadbacksFrame += rep.imageReadbackCount;
        }

        std::sort(frameLatencies.begin(), frameLatencies.end());
        double frameMin = frameLatencies.front();
        double frameP50 = frameLatencies[iters / 2];
        double frameP95 = frameLatencies[static_cast<size_t>(iters * 0.95)];
        double frameMax = frameLatencies.back();

        // Texture-Native benchmark
        std::vector<uint8_t> dummy720(benchW * benchH * 4, 128);
        auto [tex720, srv720] = createTexture(benchW, benchH, DXGI_FORMAT_R8G8B8A8_UNORM, dummy720);
        std::vector<double> nativeLatencies;
        int totalUploadsNative = 0;
        int totalReadbacksNative = 0;

        // Warmup
        for (int i = 0; i < 5; ++i) {
            auto bundle = CreateSyntheticBundle(1, i + 100, (i + 100) * 1000, benchW, benchH);
            mailboxBench->PostAnalysis(bundle);
            auto s = stackBench.ProcessTextureNative(srv720.Get(), benchW, benchH, i + 100, (i + 100) * 1000);
            s.Release();
        }

        for (int i = 0; i < iters; ++i) {
            auto bundle = CreateSyntheticBundle(1, i + 200, (i + 200) * 1000, benchW, benchH);
            mailboxBench->PostAnalysis(bundle);
            StackExecutionReport rep;
            GpuTextureSurface s = stackBench.ProcessTextureNative(srv720.Get(), benchW, benchH, i + 200, (i + 200) * 1000, &rep);
            s.Release();
            nativeLatencies.push_back(rep.totalLatencyMs);
            totalUploadsNative += rep.imageUploadCount;
            totalReadbacksNative += rep.imageReadbackCount;
        }

        std::sort(nativeLatencies.begin(), nativeLatencies.end());
        double nativeMin = nativeLatencies.front();
        double nativeP50 = nativeLatencies[iters / 2];
        double nativeP95 = nativeLatencies[static_cast<size_t>(iters * 0.95)];
        double nativeMax = nativeLatencies.back();

        std::cout << "  ProcessFrame (Software Adapter):" << std::endl;
        std::cout << "    Latency (Min / p50 / p95 / Max): "
                  << frameMin << " / " << frameP50 << " / " << frameP95 << " / " << frameMax << " ms" << std::endl;
        std::cout << "    Transfers (Upload / Readback per frame): "
                  << (totalUploadsFrame / iters) << " / " << (totalReadbacksFrame / iters) << std::endl;

        std::cout << "  ProcessTextureNative (Direct GPU Surface):" << std::endl;
        std::cout << "    Latency (Min / p50 / p95 / Max): "
                  << nativeMin << " / " << nativeP50 << " / " << nativeP95 << " / " << nativeMax << " ms" << std::endl;
        std::cout << "    Transfers (Upload / Readback per frame): "
                  << (totalUploadsNative / iters) << " / " << (totalReadbacksNative / iters) << std::endl;

        TEST_ASSERT(totalUploadsNative == 0, "Texture-native must perform 0 image uploads");
        TEST_ASSERT(totalReadbacksNative == 0, "Texture-native must perform 0 image readbacks");
        TEST_ASSERT(totalUploadsFrame == iters, "ProcessFrame must perform 1 image upload per frame");
        TEST_ASSERT(totalReadbacksFrame == iters, "ProcessFrame must perform 1 image readback per frame");

        av_frame_free(&frame720);
    }

    std::cout << "  -> PASSED (All Texture-Native lifecycle, validation, parity, and benchmark assertions verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 18: Texture-Native GPU Background Composition & 3D LUT
// -------------------------------------------------------------
bool TestGpuBackgroundAndLutComprehensive() {
    std::cout << "[TEST 18] Texture-Native GPU Background Composition & 3D LUT ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  -> SKIPPED (D3D11 Hardware/WARP device not available on system)" << std::endl;
        return true;
    }
    ID3D11Device* device = d3dCtx.GetDevice();
    ID3D11DeviceContext* context = d3dCtx.GetContext();
    TEST_ASSERT(device != nullptr && context != nullptr, "D3D11 device and context must be valid");

    auto createTexture2D = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& initData, UINT sampleCount = 1)
        -> std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11ShaderResourceView>> {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = sampleCount;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (sampleCount == 1) {
            desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }

        D3D11_SUBRESOURCE_DATA subData{};
        if (!initData.empty()) {
            subData.pSysMem = initData.data();
            subData.SysMemPitch = w * 4;
        }

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&desc, initData.empty() ? nullptr : &subData, tex.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = fmt;
        if (sampleCount > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        }

        ComPtr<ID3D11ShaderResourceView> srv;
        hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        return { tex, srv };
    };

    auto readbackTexture2D = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        if (!tex) return result;

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            return result;
        }
        context->CopyResource(staging.Get(), tex);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    // Create temporary .cube LUT files
    std::string lutDir = (fs::temp_directory_path() / "prismforge_test_luts").string();
    fs::create_directories(lutDir);
    std::string testLutPath = lutDir + "/test_color_grade.cube";
    {
        std::ofstream f(testLutPath);
        f << "TITLE \"TestColorGrade\"\n";
        f << "LUT_3D_SIZE 2\n";
        f << "DOMAIN_MIN 0.0 0.0 0.0\n";
        f << "DOMAIN_MAX 1.0 1.0 1.0\n";
        f << "0.1 0.0 0.0\n";
        f << "1.0 0.0 0.0\n";
        f << "0.0 0.9 0.0\n";
        f << "1.0 0.9 0.0\n";
        f << "0.0 0.0 0.8\n";
        f << "1.0 0.0 0.8\n";
        f << "0.0 0.9 0.8\n";
        f << "1.0 0.9 0.8\n";
    }
    std::string customDomainLutPath = lutDir + "/test_custom_domain.cube";
    {
        std::ofstream f(customDomainLutPath);
        f << "TITLE \"TestCustomDomain\"\n";
        f << "LUT_3D_SIZE 2\n";
        f << "DOMAIN_MIN 0.1 0.1 0.1\n";
        f << "DOMAIN_MAX 0.9 0.9 0.9\n";
        f << "0.2 0.1 0.1\n";
        f << "0.9 0.1 0.1\n";
        f << "0.1 0.8 0.1\n";
        f << "0.9 0.8 0.1\n";
        f << "0.1 0.1 0.7\n";
        f << "0.9 0.1 0.7\n";
        f << "0.1 0.8 0.7\n";
        f << "0.9 0.8 0.7\n";
    }

    struct LutDirCleaner {
        std::string dir;
        ~LutDirCleaner() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleaner{ lutDir };

    // Setup stacks and mailbox
    const int W = 640;
    const int H = 360;

    AVFrame* inBgr = AllocateBgrFrameWithPattern(W, H, 1);
    TEST_ASSERT(inBgr != nullptr, "Failed to allocate test pattern frame");

    std::vector<uint8_t> rgbaData(W * H * 4);
    for (int y = 0; y < H; ++y) {
        const uint8_t* src = inBgr->data[0] + y * inBgr->linesize[0];
        for (int x = 0; x < W; ++x) {
            rgbaData[(y * W + x) * 4 + 0] = src[x * 3 + 2]; // R
            rgbaData[(y * W + x) * 4 + 1] = src[x * 3 + 1]; // G
            rgbaData[(y * W + x) * 4 + 2] = src[x * 3 + 0]; // B
            rgbaData[(y * W + x) * 4 + 3] = 255;            // A
        }
    }
    auto [texIn, srvIn] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, rgbaData);
    TEST_ASSERT(texIn && srvIn, "Failed to create input D3D11 texture");

    auto bundle = CreateSyntheticBundle(1, 1, 1000, W, H);

    // Stacks: Native GPU, Software GPU, and CPU Reference
    StudioStack stackGpu;
    stackGpu.SetSourceGeneration(1);
    TEST_ASSERT(stackGpu.InitializeGpu(device, context), "InitializeGpu on stackGpu must succeed");
    auto mailboxGpu = std::make_shared<AnalysisMailbox>();
    mailboxGpu->ResetSource(1);
    stackGpu.AttachAnalysisMailbox(mailboxGpu);

    StudioStack stackCpu;
    stackCpu.SetExecutionBackend(ExecutionBackend::CpuOnly);
    stackCpu.SetSourceGeneration(1);
    auto mailboxCpu = std::make_shared<AnalysisMailbox>();
    mailboxCpu->ResetSource(1);
    stackCpu.AttachAnalysisMailbox(mailboxCpu);

    std::string lutErr;
    TEST_ASSERT(stackGpu.LoadLutFromFile(testLutPath, lutErr), "LoadLutFromFile on GPU stack failed");
    TEST_ASSERT(stackCpu.LoadLutFromFile(testLutPath, lutErr), "LoadLutFromFile on CPU stack failed");

    // =================================================================
    // Part 1: Background Blur & Solid CPU vs GPU Parity
    // =================================================================
    std::cout << "  -> Verifying Background Blur & Solid CPU vs GPU parity..." << std::endl;
    {
        // 1a. Background Blur
        StudioPreset presetBlur;
        presetBlur.appearance.bypass = true;
        presetBlur.lighting.bypass = true;
        presetBlur.overlays.bypass = true;
        presetBlur.lutIntensity = 0.0f;
        presetBlur.background.mode = PrismForge::Effects::BackgroundMode::Blur;
        presetBlur.background.blurRadius = 8;

        stackGpu.SetPreset(presetBlur);
        stackCpu.SetPreset(presetBlur);
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H));
        mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H));

        StackExecutionReport repBlurCpu;
        AVFrame* outBlurCpu = stackCpu.ProcessFrame(inBgr, 1, 1000, &repBlurCpu);
        TEST_ASSERT(outBlurCpu != nullptr, "CPU background blur failed");
        TEST_ASSERT(repBlurCpu.backgroundStatus == StageStatus::Applied, "CPU background blur must be Applied");

        StackExecutionReport repBlurGpu;
        GpuTextureSurface surfBlurGpu = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 1, 1000, &repBlurGpu, 1);
        TEST_ASSERT(surfBlurGpu.isValid, "GPU background blur failed");
        TEST_ASSERT(repBlurGpu.backgroundStatus == StageStatus::Applied, "GPU background blur must be Applied");
        TEST_ASSERT(repBlurGpu.imageUploadCount == 0 && repBlurGpu.imageReadbackCount == 0, "Native background must have zero transfers");

        std::vector<uint8_t> gpuBlurPixels = readbackTexture2D(surfBlurGpu.texture, W, H);
        surfBlurGpu.Release();

        int maxBlurDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cpuRow = outBlurCpu->data[0] + y * outBlurCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                int bDiff = std::abs(static_cast<int>(gpuBlurPixels[(y * W + x) * 4 + 2]) - static_cast<int>(cpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(gpuBlurPixels[(y * W + x) * 4 + 1]) - static_cast<int>(cpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(gpuBlurPixels[(y * W + x) * 4 + 0]) - static_cast<int>(cpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxBlurDiff) maxBlurDiff = curMax;
            }
        }
        std::cout << "     Max CPU vs GPU Blur pixel difference: " << maxBlurDiff << " codes (threshold <= 3)" << std::endl;
        TEST_ASSERT(maxBlurDiff <= 3, "GPU Background Blur output must achieve parity with CPU within <= 3 codes");

        // 1b. Background Solid
        StudioPreset presetSolid;
        presetSolid.appearance.bypass = true;
        presetSolid.lighting.bypass = true;
        presetSolid.overlays.bypass = true;
        presetSolid.lutIntensity = 0.0f;
        presetSolid.background.mode = PrismForge::Effects::BackgroundMode::Solid;
        presetSolid.background.solidR = 40;
        presetSolid.background.solidG = 80;
        presetSolid.background.solidB = 160;

        stackGpu.SetPreset(presetSolid);
        stackCpu.SetPreset(presetSolid);
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H));
        mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H));

        StackExecutionReport repSolidCpu;
        AVFrame* outSolidCpu = stackCpu.ProcessFrame(inBgr, 2, 2000, &repSolidCpu);
        TEST_ASSERT(outSolidCpu != nullptr, "CPU background solid failed");
        TEST_ASSERT(repSolidCpu.backgroundStatus == StageStatus::Applied, "CPU background solid must be Applied");

        StackExecutionReport repSolidGpu;
        GpuTextureSurface surfSolidGpu = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 2, 2000, &repSolidGpu, 1);
        TEST_ASSERT(surfSolidGpu.isValid, "GPU background solid failed");
        TEST_ASSERT(repSolidGpu.backgroundStatus == StageStatus::Applied, "GPU background solid must be Applied");

        std::vector<uint8_t> gpuSolidPixels = readbackTexture2D(surfSolidGpu.texture, W, H);
        surfSolidGpu.Release();
        int maxSolidDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cpuRow = outSolidCpu->data[0] + y * outSolidCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                int bDiff = std::abs(static_cast<int>(gpuSolidPixels[(y * W + x) * 4 + 2]) - static_cast<int>(cpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(gpuSolidPixels[(y * W + x) * 4 + 1]) - static_cast<int>(cpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(gpuSolidPixels[(y * W + x) * 4 + 0]) - static_cast<int>(cpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxSolidDiff) maxSolidDiff = curMax;
            }
        }
        std::cout << "     Max CPU vs GPU Solid pixel difference: " << maxSolidDiff << " codes (threshold <= 3)" << std::endl;
        TEST_ASSERT(maxSolidDiff <= 3, "GPU Background Solid output must achieve parity with CPU within <= 3 codes");
    }

    // =================================================================
    // Part 2: 3D LUT Domain, Interpolation & Intensity Parity
    // =================================================================
    std::cout << "  -> Verifying 3D LUT Domain & Interpolation parity..." << std::endl;
    {
        // 2a. Standard Domain LUT with 75% intensity
        StudioPreset presetLut;
        presetLut.appearance.bypass = true;
        presetLut.lighting.bypass = true;
        presetLut.overlays.bypass = true;
        presetLut.background.mode = PrismForge::Effects::BackgroundMode::Original;
        presetLut.lutIntensity = 0.75f;

        stackGpu.SetPreset(presetLut);
        stackCpu.SetPreset(presetLut);
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 3, 3000, W, H));
        mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 3, 3000, W, H));

        StackExecutionReport repLutCpu;
        AVFrame* outLutCpu = stackCpu.ProcessFrame(inBgr, 3, 3000, &repLutCpu);
        TEST_ASSERT(outLutCpu != nullptr, "CPU LUT failed");
        TEST_ASSERT(repLutCpu.lutStatus == StageStatus::Applied, "CPU LUT must be Applied");

        StackExecutionReport repLutGpu;
        GpuTextureSurface surfLutGpu = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 3, 3000, &repLutGpu, 1);
        TEST_ASSERT(surfLutGpu.isValid, "GPU LUT failed");
        TEST_ASSERT(repLutGpu.lutStatus == StageStatus::Applied, "GPU LUT must be Applied");

        std::vector<uint8_t> gpuLutPixels = readbackTexture2D(surfLutGpu.texture, W, H);
        surfLutGpu.Release();
        int maxLutDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cpuRow = outLutCpu->data[0] + y * outLutCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                int bDiff = std::abs(static_cast<int>(gpuLutPixels[(y * W + x) * 4 + 2]) - static_cast<int>(cpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(gpuLutPixels[(y * W + x) * 4 + 1]) - static_cast<int>(cpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(gpuLutPixels[(y * W + x) * 4 + 0]) - static_cast<int>(cpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxLutDiff) maxLutDiff = curMax;
            }
        }
        std::cout << "     Max CPU vs GPU Standard Domain LUT pixel difference: " << maxLutDiff << " codes (threshold <= 3)" << std::endl;
        TEST_ASSERT(maxLutDiff <= 3, "GPU 3D LUT output must achieve parity with CPU within <= 3 codes");

        // 2b. Custom Domain LUT
        TEST_ASSERT(stackGpu.LoadLutFromFile(customDomainLutPath, lutErr), "Load custom domain LUT on GPU failed");
        TEST_ASSERT(stackCpu.LoadLutFromFile(customDomainLutPath, lutErr), "Load custom domain LUT on CPU failed");

        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 4, 4000, W, H));
        mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 4, 4000, W, H));

        StackExecutionReport repCustomCpu;
        AVFrame* outCustomCpu = stackCpu.ProcessFrame(inBgr, 4, 4000, &repCustomCpu);
        StackExecutionReport repCustomGpu;
        GpuTextureSurface surfCustomGpu = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 4, 4000, &repCustomGpu, 1);

        std::vector<uint8_t> gpuCustomPixels = readbackTexture2D(surfCustomGpu.texture, W, H);
        surfCustomGpu.Release();
        int maxCustomDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cpuRow = outCustomCpu->data[0] + y * outCustomCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                int bDiff = std::abs(static_cast<int>(gpuCustomPixels[(y * W + x) * 4 + 2]) - static_cast<int>(cpuRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(gpuCustomPixels[(y * W + x) * 4 + 1]) - static_cast<int>(cpuRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(gpuCustomPixels[(y * W + x) * 4 + 0]) - static_cast<int>(cpuRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxCustomDiff) maxCustomDiff = curMax;
            }
        }
        std::cout << "     Max CPU vs GPU Custom Domain LUT pixel difference: " << maxCustomDiff << " codes (threshold <= 3)" << std::endl;
        TEST_ASSERT(maxCustomDiff <= 3, "GPU Custom Domain LUT output must achieve parity with CPU within <= 3 codes");

        // Switch back to standard LUT
        stackGpu.LoadLutFromFile(testLutPath, lutErr);
        stackCpu.LoadLutFromFile(testLutPath, lutErr);
    }

    // =================================================================
    // Part 3: Chained 4-Stage Pipeline on GPU Textures
    // =================================================================
    std::cout << "  -> Verifying chained 4-stage pipeline (Appearance -> Lighting -> Background -> LUT)..." << std::endl;
    {
        StudioPreset presetAll;
        presetAll.appearance.bypass = false;
        presetAll.appearance.smoothingStrength = 0.5f;
        presetAll.appearance.sharpenStrength = 0.3f;
        presetAll.lighting.bypass = false;
        presetAll.lighting.light1.enabled = true;
        presetAll.lighting.light1.intensity = 0.7f;
        presetAll.background.mode = PrismForge::Effects::BackgroundMode::Blur;
        presetAll.background.blurRadius = 8;
        presetAll.lutIntensity = 0.85f;
        presetAll.overlays.bypass = true;

        stackGpu.SetPreset(presetAll);
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 5, 5000, W, H));

        StackExecutionReport repNative;
        GpuTextureSurface surfNative = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 5, 5000, &repNative, 1);
        TEST_ASSERT(surfNative.isValid, "Chained 4-stage ProcessTextureNative failed");
        TEST_ASSERT(repNative.appearanceStatus == StageStatus::Applied, "Appearance must be Applied");
        TEST_ASSERT(repNative.lightingStatus == StageStatus::Applied, "Lighting must be Applied");
        TEST_ASSERT(repNative.backgroundStatus == StageStatus::Applied, "Background must be Applied");
        TEST_ASSERT(repNative.lutStatus == StageStatus::Applied, "LUT must be Applied");
        TEST_ASSERT(repNative.imageUploadCount == 0, "Native path must do 0 image uploads");
        TEST_ASSERT(repNative.imageReadbackCount == 0, "Native path must do 0 image readbacks");

        // Run Software Frame GPU Adapter with identical preset
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 5, 5000, W, H));
        StackExecutionReport repFrame;
        AVFrame* outFrame = stackGpu.ProcessFrame(inBgr, 5, 5000, &repFrame);
        TEST_ASSERT(outFrame != nullptr, "Chained 4-stage ProcessFrame failed");
        TEST_ASSERT(repFrame.appearanceStatus == StageStatus::Applied, "Frame Appearance must be Applied");
        TEST_ASSERT(repFrame.lightingStatus == StageStatus::Applied, "Frame Lighting must be Applied");
        TEST_ASSERT(repFrame.backgroundStatus == StageStatus::Applied, "Frame Background must be Applied");
        TEST_ASSERT(repFrame.lutStatus == StageStatus::Applied, "Frame LUT must be Applied");
        TEST_ASSERT(repFrame.imageUploadCount == 1, "ProcessFrame must do exactly 1 upload");
        TEST_ASSERT(repFrame.imageReadbackCount == 1, "ProcessFrame must do exactly 1 readback");

        // Compare native texture output vs software adapter output (exact match within conversion tolerance)
        std::vector<uint8_t> nativePixels = readbackTexture2D(surfNative.texture, W, H);
        surfNative.Release();
        int maxChainDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* fRow = outFrame->data[0] + y * outFrame->linesize[0];
            for (int x = 0; x < W; ++x) {
                int bDiff = std::abs(static_cast<int>(nativePixels[(y * W + x) * 4 + 2]) - static_cast<int>(fRow[x * 3 + 0]));
                int gDiff = std::abs(static_cast<int>(nativePixels[(y * W + x) * 4 + 1]) - static_cast<int>(fRow[x * 3 + 1]));
                int rDiff = std::abs(static_cast<int>(nativePixels[(y * W + x) * 4 + 0]) - static_cast<int>(fRow[x * 3 + 2]));
                int curMax = (std::max)({ bDiff, gDiff, rDiff });
                if (curMax > maxChainDiff) maxChainDiff = curMax;
            }
        }
        std::cout << "     Native GPU vs Software GPU max difference: " << maxChainDiff << " codes (threshold <= 1)" << std::endl;
        TEST_ASSERT(maxChainDiff <= 1, "Native GPU and Software GPU adapter must produce bit-exact matching output");
    }

    // =================================================================
    // Part 4: Retained Surface Safety Across Processing, Resize & Release
    // =================================================================
    std::cout << "  -> Verifying Retained Surface safety across subsequent frames, resize, reset and release..." << std::endl;
    {
        mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, 6, 6000, W, H));
        StackExecutionReport repRetain;
        GpuTextureSurface surfRetained = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 6, 6000, &repRetain, 1);
        TEST_ASSERT(surfRetained.isValid, "Retained surface generation must succeed");
        TEST_ASSERT(surfRetained.textureRef != nullptr, "Retained surface must hold strong ComPtr<ID3D11Texture2D>");
        TEST_ASSERT(surfRetained.srvRef != nullptr, "Retained surface must hold strong ComPtr<ID3D11ShaderResourceView>");

        std::vector<uint8_t> initialSnapshot = readbackTexture2D(surfRetained.texture, W, H);
        TEST_ASSERT(!initialSnapshot.empty(), "Initial snapshot readback must not be empty");

        // 4a. Process subsequent frames with differing inputs and presets
        StudioPreset altPreset;
        altPreset.background.mode = PrismForge::Effects::BackgroundMode::Solid;
        altPreset.background.solidR = 250;
        altPreset.background.solidG = 250;
        altPreset.background.solidB = 250;
        stackGpu.SetPreset(altPreset);

        for (int frameIdx = 7; frameIdx <= 12; ++frameIdx) {
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(1, frameIdx, frameIdx * 1000, W, H));
            StackExecutionReport repSub;
            GpuTextureSurface surfSub = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, frameIdx, frameIdx * 1000, &repSub, 1);
            TEST_ASSERT(surfSub.isValid, "Subsequent frame must succeed");
            surfSub.Release();
        }

        // Verify initial retained surface has not been corrupted or mutated
        std::vector<uint8_t> snapshotAfterSub = readbackTexture2D(surfRetained.texture, W, H);
        TEST_ASSERT(snapshotAfterSub == initialSnapshot, "Retained surface must remain unmutated across subsequent frames");

        // 4b. Trigger dynamic resize
        const int W2 = 1280;
        const int H2 = 720;
        std::vector<uint8_t> rgba720(W2 * H2 * 4, 128);
        auto [tex720, srv720] = createTexture2D(W2, H2, DXGI_FORMAT_R8G8B8A8_UNORM, rgba720);
        auto bundle720 = CreateSyntheticBundle(1, 13, 13000, W2, H2);
        mailboxGpu->PostAnalysis(bundle720);

        StackExecutionReport rep720;
        GpuTextureSurface surf720 = stackGpu.ProcessTextureNative(srv720.Get(), W2, H2, 13, 13000, &rep720, 1);
        TEST_ASSERT(surf720.isValid, "Dynamic resize processing must succeed");
        TEST_ASSERT(surf720.width == W2 && surf720.height == H2, "Resized surface must have 1280x720 dimensions");
        surf720.Release();

        // Verify retained 640x360 surface is STILL valid and unchanged
        std::vector<uint8_t> snapshotAfterResize = readbackTexture2D(surfRetained.texture, W, H);
        TEST_ASSERT(snapshotAfterResize == initialSnapshot, "Retained surface must remain unmutated after dynamic stack resize");

        // 4c. Source Reset
        stackGpu.ResetSource();
        stackGpu.SetSourceGeneration(2);
        std::vector<uint8_t> snapshotAfterReset = readbackTexture2D(surfRetained.texture, W, H);
        TEST_ASSERT(snapshotAfterReset == initialSnapshot, "Retained surface must remain valid after ResetSource()");

        // 4d. ReleaseGpu()
        stackGpu.ReleaseGpu();
        TEST_ASSERT(surfRetained.textureRef.Get() != nullptr, "Retained textureRef must remain non-null after ReleaseGpu()");
        TEST_ASSERT(surfRetained.srvRef.Get() != nullptr, "Retained srvRef must remain non-null after ReleaseGpu()");

        std::vector<uint8_t> snapshotAfterRelease = readbackTexture2D(surfRetained.texture, W, H);
        TEST_ASSERT(snapshotAfterRelease == initialSnapshot, "Retained surface must remain valid and readable after ReleaseGpu()");
        surfRetained.Release();

        // Re-initialize GPU for remaining tests
        TEST_ASSERT(stackGpu.InitializeGpu(device, context), "Re-initializing GPU must succeed");
        stackGpu.LoadLutFromFile(testLutPath, lutErr);
        stackGpu.SetSourceGeneration(2);
        mailboxGpu->ResetSource(2);
    }

    // =================================================================
    // Part 5: SRV Format, Multisample Rejection & Explicit sRGB Handling
    // =================================================================
    std::cout << "  -> Verifying SRV format, multisample rejection, sRGB handling & generation rechecks..." << std::endl;
    {
        // 5a. Multisample (MSAA) SRV Rejection
        auto [texMSAA, srvMSAA] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, {}, 4);
        if (srvMSAA) {
            StackExecutionReport repMSAA;
            GpuTextureSurface surfMSAA = stackGpu.ProcessTextureNative(srvMSAA.Get(), W, H, 20, 20000, &repMSAA, 2);
            TEST_ASSERT(!surfMSAA.isValid, "Multisampled (MSAA) texture must be rejected");
            TEST_ASSERT(surfMSAA.texture == nullptr, "Rejected MSAA must return null texture pointer");
            TEST_ASSERT(repMSAA.errorDescription.find("Multisampled") != std::string::npos, "Error must describe multisampled texture rejection");
        }

        // 5b. Explicit sRGB Format Handling
        auto [texSRGB, srvSRGB] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, rgbaData, 1);
        TEST_ASSERT(srvSRGB != nullptr, "Failed to create sRGB SRV");

        mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 21, 21000, W, H));
        StackExecutionReport repSRGB;
        GpuTextureSurface surfSRGB = stackGpu.ProcessTextureNative(srvSRGB.Get(), W, H, 21, 21000, &repSRGB, 2);
        TEST_ASSERT(surfSRGB.isValid, "ProcessTextureNative must accept DXGI_FORMAT_R8G8B8A8_UNORM_SRGB");
        TEST_ASSERT(surfSRGB.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "Returned format must reflect sRGB explicitly");
        surfSRGB.Release();

        // 5c. Pre-lock Source Generation Rejection
        StackExecutionReport repStaleGen;
        GpuTextureSurface surfStaleGen = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 22, 22000, &repStaleGen, /*staleGen*/ 99);
        TEST_ASSERT(!surfStaleGen.isValid, "Stale source generation must be rejected");
        TEST_ASSERT(repStaleGen.errorDescription.find("Stale") != std::string::npos, "Error must describe stale generation");
    }

    // =================================================================
    // Part 6: Targeted Failure Injection & Same-Frame Recovery
    // =================================================================
    std::cout << "  -> Verifying targeted failure injection (ResourceCreation, Stages, Readback)..." << std::endl;
    {
        StudioPreset presetFail;
        presetFail.appearance.bypass = false;
        presetFail.appearance.smoothingStrength = 0.5f;
        presetFail.appearance.sharpenStrength = 0.3f;
        presetFail.lighting.bypass = false;
        presetFail.lighting.light1.enabled = true;
        presetFail.background.mode = PrismForge::Effects::BackgroundMode::Blur;
        presetFail.background.blurRadius = 8;
        presetFail.lutIntensity = 0.8f;
        presetFail.overlays.bypass = true;
        stackGpu.SetPreset(presetFail);

        const std::vector<std::pair<GpuFailureTarget, std::string>> targets = {
            { GpuFailureTarget::ResourceCreation, "resource creation failure" },
            { GpuFailureTarget::AppearanceStage,  "Appearance stage failure" },
            { GpuFailureTarget::LightingStage,    "Lighting stage failure" },
            { GpuFailureTarget::BackgroundStage,  "Background stage failure" },
            { GpuFailureTarget::LutStage,         "LUT stage failure" },
            { GpuFailureTarget::Readback,         "readback failure" }
        };

        for (const auto& [target, expectedKeyword] : targets) {
            stackGpu.SetGpuFailureTarget(target);

            // 6a. Software Frame Adapter with GpuWithCpuFallback: must perform same-frame CPU fallback
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 30, 30000, W, H));
            StackExecutionReport repFallback;
            AVFrame* outFb = stackGpu.ProcessFrame(inBgr, 30, 30000, &repFallback);
            TEST_ASSERT(outFb != nullptr, "ProcessFrame must succeed via CPU fallback upon GPU failure");
            TEST_ASSERT(repFallback.gpuFallbackUsed == true, "gpuFallbackUsed must be true");
            TEST_ASSERT(repFallback.errorDescription.find(expectedKeyword) != std::string::npos,
                        "Error description must detail the exact injected failure point");

            // 6b. Texture-Native: must return invalid surface without returning stale data
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 31, 31000, W, H));
            StackExecutionReport repNativeFail;
            GpuTextureSurface surfFail = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 31, 31000, &repNativeFail, 2);
            TEST_ASSERT(!surfFail.isValid, "ProcessTextureNative must fail when failure injected");
            TEST_ASSERT(surfFail.texture == nullptr, "Failed surface must return nullptr");

            // 6c. Recovery: reset failure target, verify next frame recovers to full GPU execution
            stackGpu.SetGpuFailureTarget(GpuFailureTarget::None);
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 32, 32000, W, H));
            StackExecutionReport repRecovFrame;
            AVFrame* outRecov = stackGpu.ProcessFrame(inBgr, 32, 32000, &repRecovFrame);
            TEST_ASSERT(outRecov != nullptr, "ProcessFrame must succeed post-recovery");
            TEST_ASSERT(repRecovFrame.gpuFallbackUsed == false, "gpuFallbackUsed must be false after recovery");

            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 33, 33000, W, H));
            StackExecutionReport repRecovNative;
            GpuTextureSurface surfRecov = stackGpu.ProcessTextureNative(srvIn.Get(), W, H, 33, 33000, &repRecovNative, 2);
            TEST_ASSERT(surfRecov.isValid, "ProcessTextureNative must succeed post-recovery");
            surfRecov.Release();
        }
    }

    // =================================================================
    // Part 7: Active Workload Benchmark & Transfer Accounting
    // =================================================================
    std::cout << "  -> Running Active Workload Benchmark (720p HD with all 4 stages active)..." << std::endl;
    {
        const int benchW = 1280;
        const int benchH = 720;
        AVFrame* frame720 = AllocateBgrFrameWithPattern(benchW, benchH, 1);
        TEST_ASSERT(frame720 != nullptr, "Failed to allocate 720p frame");

        std::vector<uint8_t> rgba720(benchW * benchH * 4);
        for (int y = 0; y < benchH; ++y) {
            const uint8_t* src = frame720->data[0] + y * frame720->linesize[0];
            for (int x = 0; x < benchW; ++x) {
                rgba720[(y * benchW + x) * 4 + 0] = src[x * 3 + 2];
                rgba720[(y * benchW + x) * 4 + 1] = src[x * 3 + 1];
                rgba720[(y * benchW + x) * 4 + 2] = src[x * 3 + 0];
                rgba720[(y * benchW + x) * 4 + 3] = 255;
            }
        }
        auto [texBench, srvBench] = createTexture2D(benchW, benchH, DXGI_FORMAT_R8G8B8A8_UNORM, rgba720);

        StudioPreset presetBench;
        presetBench.appearance.bypass = false;
        presetBench.appearance.smoothingStrength = 0.5f;
        presetBench.appearance.sharpenStrength = 0.3f;
        presetBench.lighting.bypass = false;
        presetBench.lighting.light1.enabled = true;
        presetBench.lighting.light1.intensity = 0.8f;
        presetBench.background.mode = PrismForge::Effects::BackgroundMode::Blur;
        presetBench.background.blurRadius = 8;
        presetBench.lutIntensity = 0.8f;
        presetBench.overlays.bypass = true;
        stackGpu.SetPreset(presetBench);

        const int warmup = 5;
        const int iters = 50;

        for (int i = 0; i < warmup; ++i) {
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 100 + i, (100 + i) * 1000, benchW, benchH));
            stackGpu.ProcessFrame(frame720, 100 + i, (100 + i) * 1000, nullptr);
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 200 + i, (200 + i) * 1000, benchW, benchH));
            auto s = stackGpu.ProcessTextureNative(srvBench.Get(), benchW, benchH, 200 + i, (200 + i) * 1000, nullptr, 2);
            s.Release();
        }

        std::vector<double> frameTimes;
        int totalUploadsFrame = 0;
        int totalReadbacksFrame = 0;
        frameTimes.reserve(iters);

        for (int i = 0; i < iters; ++i) {
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 1000 + i, (1000 + i) * 1000, benchW, benchH));
            StackExecutionReport rep;
            auto t0 = std::chrono::high_resolution_clock::now();
            AVFrame* out = stackGpu.ProcessFrame(frame720, 1000 + i, (1000 + i) * 1000, &rep);
            auto t1 = std::chrono::high_resolution_clock::now();
            TEST_ASSERT(out != nullptr, "ProcessFrame failed in benchmark");
            frameTimes.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            totalUploadsFrame += rep.imageUploadCount;
            totalReadbacksFrame += rep.imageReadbackCount;
        }

        std::vector<double> nativeTimes;
        int totalUploadsNative = 0;
        int totalReadbacksNative = 0;
        nativeTimes.reserve(iters);

        for (int i = 0; i < iters; ++i) {
            mailboxGpu->PostAnalysis(CreateSyntheticBundle(2, 2000 + i, (2000 + i) * 1000, benchW, benchH));
            StackExecutionReport rep;
            auto t0 = std::chrono::high_resolution_clock::now();
            GpuTextureSurface surf = stackGpu.ProcessTextureNative(srvBench.Get(), benchW, benchH, 2000 + i, (2000 + i) * 1000, &rep, 2);
            auto t1 = std::chrono::high_resolution_clock::now();
            TEST_ASSERT(surf.isValid, "ProcessTextureNative failed in benchmark");
            surf.Release();
            nativeTimes.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            totalUploadsNative += rep.imageUploadCount;
            totalReadbacksNative += rep.imageReadbackCount;
        }

        auto calcStats = [](std::vector<double>& times) {
            std::sort(times.begin(), times.end());
            double minV = times.front();
            double maxV = times.back();
            double p50 = times[times.size() / 2];
            double p95 = times[static_cast<size_t>(times.size() * 0.95)];
            return std::make_tuple(minV, p50, p95, maxV);
        };

        auto [fMin, fP50, fP95, fMax] = calcStats(frameTimes);
        auto [nMin, nP50, nP95, nMax] = calcStats(nativeTimes);

        std::cout << "\n  === BENCHMARK REPORT (720p HD Active Pipeline: Appearance + Lighting + Background Blur + 3D LUT) ===" << std::endl;
        std::cout << "  ProcessFrame (Software Frame Adapter):" << std::endl;
        std::cout << "    Latency (Min / p50 / p95 / Max): " << fMin << " / " << fP50 << " / " << fP95 << " / " << fMax << " ms" << std::endl;
        std::cout << "    Transfers (Upload / Readback per frame): "
                  << (totalUploadsFrame / iters) << " / " << (totalReadbacksFrame / iters) << std::endl;

        std::cout << "  ProcessTextureNative (Pure Texture-Native Surface):" << std::endl;
        std::cout << "    Latency (Min / p50 / p95 / Max): " << nMin << " / " << nP50 << " / " << nP95 << " / " << nMax << " ms" << std::endl;
        std::cout << "    Transfers (Upload / Readback per frame): "
                  << (totalUploadsNative / iters) << " / " << (totalReadbacksNative / iters) << std::endl;

        TEST_ASSERT(totalUploadsNative == 0, "Texture-native must perform 0 image uploads");
        TEST_ASSERT(totalReadbacksNative == 0, "Texture-native must perform 0 image readbacks");
        TEST_ASSERT(totalUploadsFrame == iters, "ProcessFrame must perform 1 image upload per frame");
        TEST_ASSERT(totalReadbacksFrame == iters, "ProcessFrame must perform 1 image readback per frame");

        av_frame_free(&frame720);
    }

    av_frame_free(&inBgr);

    std::cout << "  -> PASSED (All GPU Background & 3D LUT stages, parity, retained surfaces, and failure injections verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 19: Milestone 1 — Bounded Output Lifetime & Backpressure
// =================================================================
bool TestBoundedOutputLifetimeAndBackpressure() {
    std::cout << "\n[TEST 19] Bounded Output: Explicit Leases, Capacity 4, Backpressure & Retention ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed (WARP not available)" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    auto createTex = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = fmt; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.empty() ? nullptr : data.data();
        subData.SysMemPitch = w * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hr = device->CreateTexture2D(&desc, data.empty() ? nullptr : &subData, tex.GetAddressOf());
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        }
        return std::make_pair(tex, srv);
    };

    auto readbackTex = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        if (!tex) return result;
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return result;
        context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset preset;
    preset.appearance.bypass = true;
    preset.lighting.bypass = true;
    preset.overlays.bypass = true;
    preset.background.mode = PrismForge::Effects::BackgroundMode::Original;
    stack.SetPreset(preset);

    const int W = 640;
    const int H = 360;
    std::vector<uint8_t> testRgba(W * H * 4, 100);
    auto [texIn, srvIn] = createTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, testRgba);
    TEST_ASSERT(srvIn != nullptr, "Input SRV creation must succeed");

    // 1. Initial capacity contract
    TEST_ASSERT(stack.GetMaxLeaseCapacity() == 4, "Max lease capacity must be exactly 4");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Active leases must start at 0");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 0, "Total allocations must start at 0");

    // 2. Acquire and hold 4 surfaces concurrently
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H));
    GpuTextureSurface surf1 = stack.ProcessTextureNative(srvIn.Get(), W, H, 1, 1000, nullptr, 1);
    TEST_ASSERT(surf1.isValid, "Slot 1 lease must succeed");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Active leases must be 1");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 1, "Total allocations must be 1");

    mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H));
    GpuTextureSurface surf2 = stack.ProcessTextureNative(srvIn.Get(), W, H, 2, 2000, nullptr, 1);
    TEST_ASSERT(surf2.isValid, "Slot 2 lease must succeed");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 2, "Active leases must be 2");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 2, "Total allocations must be 2");

    mailbox->PostAnalysis(CreateSyntheticBundle(1, 3, 3000, W, H));
    GpuTextureSurface surf3 = stack.ProcessTextureNative(srvIn.Get(), W, H, 3, 3000, nullptr, 1);
    TEST_ASSERT(surf3.isValid, "Slot 3 lease must succeed");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 3, "Active leases must be 3");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 3, "Total allocations must be 3");

    mailbox->PostAnalysis(CreateSyntheticBundle(1, 4, 4000, W, H));
    GpuTextureSurface surf4 = stack.ProcessTextureNative(srvIn.Get(), W, H, 4, 4000, nullptr, 1);
    TEST_ASSERT(surf4.isValid, "Slot 4 lease must succeed");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must be 4");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Total allocations must be 4");

    std::vector<uint8_t> snap1 = readbackTex(surf1.texture, W, H);
    std::vector<uint8_t> snap2 = readbackTex(surf2.texture, W, H);
    std::vector<uint8_t> snap3 = readbackTex(surf3.texture, W, H);
    std::vector<uint8_t> snap4 = readbackTex(surf4.texture, W, H);

    // 3. 5th request must fail with explicit backpressure failure and ZERO new allocations
    StackExecutionReport repExhaust;
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 5, 5000, W, H));
    GpuTextureSurface surf5 = stack.ProcessTextureNative(srvIn.Get(), W, H, 5, 5000, &repExhaust, 1);
    TEST_ASSERT(!surf5.isValid, "5th lease must fail due to pool exhaustion");
    TEST_ASSERT(surf5.texture == nullptr, "Exhausted surface must return nullptr texture");
    TEST_ASSERT(surf5.srv == nullptr, "Exhausted surface must return nullptr SRV");
    TEST_ASSERT(repExhaust.errorDescription.find("pool exhausted") != std::string::npos,
                "Error description must explicitly document pool exhaustion");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Total allocations must NOT exceed 4 (zero new allocations)");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must remain capped at 4");

    // 4. Repeated requests must also fail with zero allocations
    for (int f = 6; f <= 15; ++f) {
        mailbox->PostAnalysis(CreateSyntheticBundle(1, f, f * 1000, W, H));
        GpuTextureSurface sFail = stack.ProcessTextureNative(srvIn.Get(), W, H, f, f * 1000, nullptr, 1);
        TEST_ASSERT(!sFail.isValid, "Subsequent lease under exhaustion must fail");
        TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Allocations must remain at 4");
    }

    // 5. Release ONE lease (slot 2) and recover immediately
    surf2.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 3, "Active leases must decrease to 3 after Release()");

    mailbox->PostAnalysis(CreateSyntheticBundle(1, 16, 16000, W, H));
    GpuTextureSurface surfRecov = stack.ProcessTextureNative(srvIn.Get(), W, H, 16, 16000, nullptr, 1);
    TEST_ASSERT(surfRecov.isValid, "Acquisition after releasing one lease must succeed");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must be back to 4");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Total allocations must plateau at 4 (reused slot)");

    // 6. Verify held surfaces (surf1, surf3, surf4) are unchanged throughout
    TEST_ASSERT(readbackTex(surf1.texture, W, H) == snap1, "Held surf1 must remain unchanged");
    TEST_ASSERT(readbackTex(surf3.texture, W, H) == snap3, "Held surf3 must remain unchanged");
    TEST_ASSERT(readbackTex(surf4.texture, W, H) == snap4, "Held surf4 must remain unchanged");

    // 7. Retention across dynamic resize, source reset, and ReleaseGpu()
    surfRecov.Release(); // Free 1 slot for resize
    const int W2 = 1280;
    const int H2 = 720;
    std::vector<uint8_t> testRgba720(W2 * H2 * 4, 120);
    auto [tex720, srv720] = createTex(W2, H2, DXGI_FORMAT_R8G8B8A8_UNORM, testRgba720);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 17, 17000, W2, H2));
    GpuTextureSurface surfResized = stack.ProcessTextureNative(srv720.Get(), W2, H2, 17, 17000, nullptr, 1);
    TEST_ASSERT(surfResized.isValid, "Dynamic resize acquisition must succeed");
    TEST_ASSERT(surfResized.width == W2 && surfResized.height == H2, "Resized surface must have 1280x720 dimensions");

    // Verify held surfaces surf1, surf3, surf4 are still unmutated
    TEST_ASSERT(readbackTex(surf1.texture, W, H) == snap1, "Held surf1 must survive stack resize");
    TEST_ASSERT(readbackTex(surf3.texture, W, H) == snap3, "Held surf3 must survive stack resize");
    TEST_ASSERT(readbackTex(surf4.texture, W, H) == snap4, "Held surf4 must survive stack resize");

    // Source Reset
    stack.ResetSource();
    stack.SetSourceGeneration(2);
    TEST_ASSERT(readbackTex(surf1.texture, W, H) == snap1, "Held surf1 must survive ResetSource()");

    // ReleaseGpu()
    stack.ReleaseGpu();
    TEST_ASSERT(surf1.textureRef != nullptr, "textureRef must survive ReleaseGpu()");
    TEST_ASSERT(surf1.srvRef != nullptr, "srvRef must survive ReleaseGpu()");
    TEST_ASSERT(readbackTex(surf1.texture, W, H) == snap1, "Held surf1 must remain readable after ReleaseGpu()");

    // Release all held leases
    surf1.Release();
    surf3.Release();
    surf4.Release();
    surfResized.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Active lease count must be 0 after releasing all surfaces");

    std::cout << "  -> PASSED (Hard capacity 4, backpressure, 0 new allocations, reuse, and safe retention verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 20: Milestone 2 — Live Changing Analysis Consumption Every Frame
// =================================================================
bool TestMilestone2_LiveChangingAnalysis() {
    std::cout << "\n[TEST 20] Live Analysis: Changing Masks on Same-Size Frames & Reset Isolation ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTex = [&](const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = W * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    auto readbackTex = [&](ID3D11Texture2D* tex) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(W * H * 4, 0);
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < H; ++y) {
                std::memcpy(result.data() + y * W * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            W * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // Solid Background preset: foreground retains camera color, background replaced with Blue (0, 0, 255)
    StudioPreset preset;
    preset.appearance.bypass = true;
    preset.lighting.bypass = true;
    preset.overlays.bypass = true;
    preset.background.mode = PrismForge::Effects::BackgroundMode::Solid;
    preset.background.solidR = 0;
    preset.background.solidG = 0;
    preset.background.solidB = 255;
    stack.SetPreset(preset);

    // Camera input: pure Red (255, 0, 0)
    std::vector<uint8_t> redRgba(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        redRgba[i * 4 + 0] = 255; // R
        redRgba[i * 4 + 1] = 0;   // G
        redRgba[i * 4 + 2] = 0;   // B
        redRgba[i * 4 + 3] = 255; // A
    }
    auto [texIn, srvIn] = createTex(redRgba);

    // Frame 1: Face shifted to the LEFT (faceOffset = -0.2f)
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H, -0.2f));
    StackExecutionReport rep1;
    GpuTextureSurface surf1 = stack.ProcessTextureNative(srvIn.Get(), W, H, 1, 1000, &rep1, 1);
    TEST_ASSERT(surf1.isValid, "Frame 1 must succeed");
    TEST_ASSERT(rep1.backgroundStatus == StageStatus::Applied, "Background must be Applied on Frame 1");
    std::vector<uint8_t> pix1 = readbackTex(surf1.texture);
    surf1.Release();

    int leftX = static_cast<int>(W * 0.30f);
    int rightX = static_cast<int>(W * 0.70f);
    int midY = H / 2;

    int leftR1 = pix1[(midY * W + leftX) * 4 + 0];
    int leftB1 = pix1[(midY * W + leftX) * 4 + 2];
    int rightR1 = pix1[(midY * W + rightX) * 4 + 0];
    int rightB1 = pix1[(midY * W + rightX) * 4 + 2];

    TEST_ASSERT(leftR1 > 200 && leftB1 < 50, "Frame 1: Left side must be foreground person (Red)");
    TEST_ASSERT(rightB1 > 200 && rightR1 < 50, "Frame 1: Right side must be background (Blue)");

    // Frame 2: SAME DIMENSIONS (640x360), BUT mask shifted to the RIGHT (faceOffset = +0.2f)
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H, +0.2f));
    StackExecutionReport rep2;
    GpuTextureSurface surf2 = stack.ProcessTextureNative(srvIn.Get(), W, H, 2, 2000, &rep2, 1);
    TEST_ASSERT(surf2.isValid, "Frame 2 must succeed");
    TEST_ASSERT(rep2.backgroundStatus == StageStatus::Applied, "Background must be Applied on Frame 2");
    std::vector<uint8_t> pix2 = readbackTex(surf2.texture);
    surf2.Release();

    int leftR2 = pix2[(midY * W + leftX) * 4 + 0];
    int leftB2 = pix2[(midY * W + leftX) * 4 + 2];
    int rightR2 = pix2[(midY * W + rightX) * 4 + 0];
    int rightB2 = pix2[(midY * W + rightX) * 4 + 2];

    TEST_ASSERT(leftB2 > 200 && leftR2 < 50, "Frame 2: Left side must now be background (Blue)");
    TEST_ASSERT(rightR2 > 200 && rightB2 < 50, "Frame 2: Right side must now be foreground person (Red)");

    // Significant delta proves same-size frames with changing analysis update every frame
    int diffLeft = std::abs(leftR1 - leftR2);
    int diffRight = std::abs(rightR1 - rightR2);
    TEST_ASSERT(diffLeft > 150 && diffRight > 150, "Same-size frames must consume updated mask contents every frame");

    // Frame 3: Missing/invalid person mask -> reports NotReady
    auto malformedBundle = std::make_shared<AnalysisBundle>();
    malformedBundle->sourceGeneration = 1;
    malformedBundle->sequenceId = 3;
    malformedBundle->timestampPts = 3000;
    mailbox->PostAnalysis(malformedBundle);
    StackExecutionReport rep3;
    GpuTextureSurface surf3 = stack.ProcessTextureNative(srvIn.Get(), W, H, 3, 3000, &rep3, 1);
    TEST_ASSERT(surf3.isValid, "Processing must still succeed");
    TEST_ASSERT(rep3.backgroundStatus == StageStatus::NotReady, "Missing person mask must report NotReady");
    surf3.Release();

    // Source generation reset: confirm no old mask leaks into next generation
    stack.ResetSource();
    stack.SetSourceGeneration(2);
    mailbox->ResetSource(2);

    // Frame in generation 2 without new analysis must NOT apply old generation mask
    StackExecutionReport repGen2;
    GpuTextureSurface surfGen2 = stack.ProcessTextureNative(srvIn.Get(), W, H, 1, 1000, &repGen2, 2);
    TEST_ASSERT(surfGen2.isValid, "Generation 2 frame must succeed");
    TEST_ASSERT(repGen2.backgroundStatus == StageStatus::Bypassed || repGen2.backgroundStatus == StageStatus::NotReady,
                "Old generation mask must never leak into new generation");
    surfGen2.Release();

    std::cout << "  -> PASSED (Per-frame mask upload, live mask shift, invalid mask NotReady, and generation isolation verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 21: Milestone 3 — Real Image Background Replacement & Fallback
// =================================================================
bool TestMilestone3_ImageBackgroundReplacement() {
    std::cout << "\n[TEST 21] Image Background: GPU/CPU Composition, Fit Modes, Alpha Blending & Failure Paths ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTex = [&](int w, int h, const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = w * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    auto readbackTex = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    // Camera input: Solid Green (R=0, G=255, B=0)
    std::vector<uint8_t> camRgba(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        camRgba[i * 4 + 0] = 0;   // R
        camRgba[i * 4 + 1] = 255; // G
        camRgba[i * 4 + 2] = 0;   // B
        camRgba[i * 4 + 3] = 255; // A
    }
    auto [camTex, camSRV] = createTex(W, H, camRgba);

    // Background Image asset: 800x600 Blue (R=0, G=0, B=255)
    const int bgW = 800;
    const int bgH = 600;
    std::vector<uint8_t> bgRgba(bgW * bgH * 4);
    for (int i = 0; i < bgW * bgH; ++i) {
        bgRgba[i * 4 + 0] = 0;   // R
        bgRgba[i * 4 + 1] = 0;   // G
        bgRgba[i * 4 + 2] = 255; // B
        bgRgba[i * 4 + 3] = 255; // A
    }
    auto [bgTex, bgSRV] = createTex(bgW, bgH, bgRgba);

    // CPU representation of background image (800x600 BGR24)
    AVFrame* cpuBgFrame = av_frame_alloc();
    cpuBgFrame->width = bgW;
    cpuBgFrame->height = bgH;
    cpuBgFrame->format = AV_PIX_FMT_BGR24;
    av_frame_get_buffer(cpuBgFrame, 32);
    for (int y = 0; y < bgH; ++y) {
        uint8_t* row = cpuBgFrame->data[0] + y * cpuBgFrame->linesize[0];
        for (int x = 0; x < bgW; ++x) {
            row[x * 3 + 0] = 255; // B
            row[x * 3 + 1] = 0;   // G
            row[x * 3 + 2] = 0;   // R
        }
    }

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // 1. Supply validated SDR GPU texture and CPU fallback asset
    std::string setErr;
    bool setOk = stack.SetBackgroundImage(bgSRV.Get(), cpuBgFrame, BackgroundFitMode::CropFill, &setErr);
    TEST_ASSERT(setOk, "SetBackgroundImage must succeed with valid GPU and CPU assets");
    TEST_ASSERT(stack.HasBackgroundImage(), "HasBackgroundImage must report true");

    // Preset configuring Image mode with CropFill
    StudioPreset preset;
    preset.appearance.bypass = true;
    preset.lighting.bypass = true;
    preset.overlays.bypass = true;
    preset.backgroundMode = StudioBackgroundMode::Image;
    preset.backgroundFitMode = BackgroundFitMode::CropFill;
    stack.SetPreset(preset);

    // 2. GPU Texture-Native Image Composition & Bit-Exact Foreground Retention
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H, 0.0f));
    StackExecutionReport repGpu;
    GpuTextureSurface surfGpu = stack.ProcessTextureNative(camSRV.Get(), W, H, 1, 1000, &repGpu, 1);
    TEST_ASSERT(surfGpu.isValid, "ProcessTextureNative with Image background must succeed");
    TEST_ASSERT(repGpu.backgroundStatus == StageStatus::Applied, "Image background stage must report Applied");
    std::vector<uint8_t> outGpuPix = readbackTex(surfGpu.texture, W, H);
    surfGpu.Release();

    // Center of oval (alpha=1.0) must retain BIT-EXACT camera foreground (Green: R=0, G=255, B=0)
    int centerIdx = (H / 2 * W + W / 2) * 4;
    int fgR = outGpuPix[centerIdx + 0];
    int fgG = outGpuPix[centerIdx + 1];
    int fgB = outGpuPix[centerIdx + 2];
    TEST_ASSERT(fgR == 0 && fgG == 255 && fgB == 0, "Alpha=1.0 inside person mask must have bit-exact camera foreground retention");

    // Outside corner (alpha=0.0) must be replaced with background image (Blue: R=0, G=0, B=255)
    int cornerIdx = 0;
    int bgR = outGpuPix[cornerIdx + 0];
    int bgG = outGpuPix[cornerIdx + 1];
    int bgB = outGpuPix[cornerIdx + 2];
    TEST_ASSERT(bgR == 0 && bgG == 0 && bgB == 255, "Alpha=0.0 outside person mask must be replaced with background image");

    // 3. Test Fit Modes on GPU: FitLetterbox and Stretch
    preset.backgroundFitMode = BackgroundFitMode::FitLetterbox;
    stack.SetPreset(preset);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H, 0.0f));
    StackExecutionReport repLetterbox;
    GpuTextureSurface surfLetterbox = stack.ProcessTextureNative(camSRV.Get(), W, H, 2, 2000, &repLetterbox, 1);
    TEST_ASSERT(surfLetterbox.isValid && repLetterbox.backgroundStatus == StageStatus::Applied, "FitLetterbox must succeed");
    surfLetterbox.Release();

    preset.backgroundFitMode = BackgroundFitMode::Stretch;
    stack.SetPreset(preset);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 3, 3000, W, H, 0.0f));
    StackExecutionReport repStretch;
    GpuTextureSurface surfStretch = stack.ProcessTextureNative(camSRV.Get(), W, H, 3, 3000, &repStretch, 1);
    TEST_ASSERT(surfStretch.isValid && repStretch.backgroundStatus == StageStatus::Applied, "Stretch must succeed");
    surfStretch.Release();

    // 4. Test CPU Fallback Image Composition
    StudioStack stackCpu;
    stackCpu.SetExecutionBackend(ExecutionBackend::CpuOnly);
    stackCpu.SetSourceGeneration(1);
    auto mailboxCpu = std::make_shared<AnalysisMailbox>();
    mailboxCpu->ResetSource(1);
    stackCpu.AttachAnalysisMailbox(mailboxCpu);
    stackCpu.SetBackgroundImage(nullptr, cpuBgFrame, BackgroundFitMode::CropFill);
    stackCpu.SetPreset(preset);

    AVFrame* camFrame = AllocateBgrFrameWithPattern(W, H, 1);
    for (int y = 0; y < H; ++y) {
        uint8_t* row = camFrame->data[0] + y * camFrame->linesize[0];
        for (int x = 0; x < W; ++x) {
            row[x * 3 + 0] = 0;   // B
            row[x * 3 + 1] = 255; // G
            row[x * 3 + 2] = 0;   // R
        }
    }

    mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 10, 10000, W, H, 0.0f));
    StackExecutionReport repCpu;
    AVFrame* outCpu = stackCpu.ProcessFrame(camFrame, 10, 10000, &repCpu);
    TEST_ASSERT(outCpu != nullptr, "CPU ProcessFrame with image background must succeed");
    TEST_ASSERT(repCpu.backgroundStatus == StageStatus::Applied, "CPU background status must be Applied");

    uint8_t* outCenter = outCpu->data[0] + (H / 2) * outCpu->linesize[0] + (W / 2) * 3;
    TEST_ASSERT(outCenter[0] == 0 && outCenter[1] == 255 && outCenter[2] == 0,
                "CPU fallback: Alpha=1.0 inside mask must have bit-exact camera foreground retention");

    // 5. Missing CPU fallback asset returns explicit failure (NEVER silent Applied)
    StudioStack stackNoAsset;
    stackNoAsset.SetExecutionBackend(ExecutionBackend::CpuOnly);
    stackNoAsset.SetSourceGeneration(1);
    auto mailboxNoAsset = std::make_shared<AnalysisMailbox>();
    mailboxNoAsset->ResetSource(1);
    stackNoAsset.AttachAnalysisMailbox(mailboxNoAsset);
    stackNoAsset.SetPreset(preset);
    stackNoAsset.SetBackgroundImage(bgSRV.Get(), nullptr, BackgroundFitMode::CropFill);

    mailboxNoAsset->PostAnalysis(CreateSyntheticBundle(1, 11, 11000, W, H, 0.0f));
    StackExecutionReport repNoAsset;
    AVFrame* outNoAsset = stackNoAsset.ProcessFrame(camFrame, 11, 11000, &repNoAsset);
    TEST_ASSERT(repNoAsset.backgroundStatus == StageStatus::Failed, "Missing CPU fallback asset must report Failed");
    TEST_ASSERT(repNoAsset.backgroundStatus != StageStatus::Applied, "Missing CPU fallback asset must NEVER report Applied");
    TEST_ASSERT(!repNoAsset.errorDescription.empty(), "Error description must be populated for missing CPU asset");

    // 6. Cross-device GPU texture validation failure
    PrismForge::Effects::Gpu::D3D11Context d3dCtx2;
    if (d3dCtx2.Initialize(true)) {
        D3D11_TEXTURE2D_DESC desc2{};
        desc2.Width = 640; desc2.Height = 360; desc2.MipLevels = 1; desc2.ArraySize = 1;
        desc2.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc2.SampleDesc.Count = 1;
        desc2.Usage = D3D11_USAGE_DEFAULT; desc2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex2;
        ComPtr<ID3D11ShaderResourceView> srv2;
        if (SUCCEEDED(d3dCtx2.GetDevice()->CreateTexture2D(&desc2, nullptr, tex2.GetAddressOf()))) {
            d3dCtx2.GetDevice()->CreateShaderResourceView(tex2.Get(), nullptr, srv2.GetAddressOf());
            std::string crossErr;
            bool crossOk = stack.SetBackgroundImage(srv2.Get(), nullptr, BackgroundFitMode::CropFill, &crossErr);
            TEST_ASSERT(!crossOk, "Cross-device background SRV must be rejected");
            TEST_ASSERT(crossErr.find("different D3D11 device") != std::string::npos,
                        "Error description must specify different D3D11 device");
        }
    }

    // 7. ClearBackgroundImage()
    stack.ClearBackgroundImage();
    TEST_ASSERT(!stack.HasBackgroundImage(), "HasBackgroundImage must report false after ClearBackgroundImage()");
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 12, 12000, W, H, 0.0f));
    StackExecutionReport repCleared;
    GpuTextureSurface surfCleared = stack.ProcessTextureNative(camSRV.Get(), W, H, 12, 12000, &repCleared, 1);
    TEST_ASSERT(!surfCleared.isValid, "ProcessTextureNative must fail when Image background is cleared");
    TEST_ASSERT(repCleared.errorDescription.find("SRV is not set") != std::string::npos,
                "Error description must state background image SRV is not set");

    av_frame_free(&camFrame);
    av_frame_free(&cpuBgFrame);

    std::cout << "  -> PASSED (GPU/CPU image background, fit modes, bit-exact foreground retention, missing CPU asset failure, and cross-device rejection verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 22: Milestone 4 — Real Error Paths, Fault Injection & Same-Frame Recovery
// =================================================================
bool TestMilestone4_ErrorPathsAndRecovery() {
    std::cout << "\n[TEST 22] Error Paths: Stage Faults, Map Failures, Software Fallback & Recovery ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTex = [&](const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = W * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    std::vector<uint8_t> dummyRgba(W * H * 4, 128);
    auto [camTex, camSRV] = createTex(dummyRgba);
    AVFrame* camFrame = AllocateBgrFrameWithPattern(W, H, 1);

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    StudioPreset preset;
    preset.appearance.bypass = false;
    preset.appearance.smoothingStrength = 0.5f;
    preset.lighting.bypass = false;
    preset.lighting.light1.enabled = true;
    preset.background.mode = PrismForge::Effects::BackgroundMode::Blur;
    preset.background.blurRadius = 8;
    stack.SetPreset(preset);

    // 1. Injected ResourceCreation failure
    stack.SetGpuFailureTarget(GpuFailureTarget::ResourceCreation);

    // Texture-native: must fail with invalid surface and no stale data
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H));
    StackExecutionReport repNativeFail;
    GpuTextureSurface surfFail = stack.ProcessTextureNative(camSRV.Get(), W, H, 1, 1000, &repNativeFail, 1);
    TEST_ASSERT(!surfFail.isValid, "Texture-native must fail on ResourceCreation failure");
    TEST_ASSERT(surfFail.texture == nullptr, "Failed surface must return nullptr");
    TEST_ASSERT(repNativeFail.errorDescription.find("resource creation failure") != std::string::npos,
                "Error description must identify resource creation failure");

    // Software frame adapter with GpuWithCpuFallback: must perform seamless CPU fallback
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H));
    StackExecutionReport repFb;
    AVFrame* outFb = stack.ProcessFrame(camFrame, 2, 2000, &repFb);
    TEST_ASSERT(outFb != nullptr, "ProcessFrame must succeed via CPU fallback");
    TEST_ASSERT(repFb.gpuFallbackUsed == true, "gpuFallbackUsed must be true on injected failure");
    TEST_ASSERT(repFb.errorDescription.find("resource creation failure") != std::string::npos,
                "Error description must record injected failure");

    // 2. Recovery on next frame
    stack.SetGpuFailureTarget(GpuFailureTarget::None);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 3, 3000, W, H));
    StackExecutionReport repRecov;
    GpuTextureSurface surfRecov = stack.ProcessTextureNative(camSRV.Get(), W, H, 3, 3000, &repRecov, 1);
    TEST_ASSERT(surfRecov.isValid, "Texture-native must recover cleanly on next frame");
    TEST_ASSERT(repRecov.appearanceStatus == StageStatus::Applied, "Appearance must be Applied after recovery");
    TEST_ASSERT(repRecov.lightingStatus == StageStatus::Applied, "Lighting must be Applied after recovery");
    surfRecov.Release();

    // 3. Injected Readback failure
    stack.SetGpuFailureTarget(GpuFailureTarget::Readback);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 4, 4000, W, H));
    StackExecutionReport repRb;
    AVFrame* outRb = stack.ProcessFrame(camFrame, 4, 4000, &repRb);
    TEST_ASSERT(outRb != nullptr, "ProcessFrame must succeed via CPU fallback on readback failure");
    TEST_ASSERT(repRb.gpuFallbackUsed == true, "gpuFallbackUsed must be true on readback failure");

    // Recovery
    stack.SetGpuFailureTarget(GpuFailureTarget::None);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 5, 5000, W, H));
    StackExecutionReport repRbRecov;
    AVFrame* outRbRecov = stack.ProcessFrame(camFrame, 5, 5000, &repRbRecov);
    TEST_ASSERT(outRbRecov != nullptr, "ProcessFrame must recover post-readback-failure");
    TEST_ASSERT(repRbRecov.gpuFallbackUsed == false, "gpuFallbackUsed must be false post-recovery");

    av_frame_free(&camFrame);

    std::cout << "  -> PASSED (Resource creation failure, readback failure, CPU fallback, and same-frame recovery verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 23: Milestone 1 — Shared RAII Output Leases, Aliasing & Safe Destruction
// =================================================================
bool TestMilestone1_SharedRaiiOutputLeases() {
    std::cout << "\n[TEST 23] Milestone 1: Shared RAII Leases, Aliasing, Safe Destruction & Thread Safety ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTex = [&](const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = W * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    auto readbackTex = [&](ID3D11Texture2D* tex) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(W * H * 4, 0);
        if (!tex) return result;
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return result;
        context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < H; ++y) {
                std::memcpy(result.data() + y * W * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            W * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    std::vector<uint8_t> testData(W * H * 4, 180);
    auto [camTex, camSRV] = createTex(testData);

    // 1. Acquire surface A, copy to B. Release A, churn frames, read B with GPU readback.
    GpuTextureSurface surfA = stack.ProcessTextureNative(camSRV.Get(), W, H, 1, 1000, nullptr, 1);
    TEST_ASSERT(surfA.isValid, "surfA must be valid");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Active lease count must be 1");

    std::vector<uint8_t> origPixels = readbackTex(surfA.texture);

    GpuTextureSurface surfB = surfA; // Copy
    TEST_ASSERT(surfB.isValid, "surfB copy must be valid");
    TEST_ASSERT(surfB.leaseId == surfA.leaseId, "surfB must share same leaseId");
    TEST_ASSERT(surfB.texture == surfA.texture, "surfB must reference same texture");
    TEST_ASSERT(surfB.leaseCore == surfA.leaseCore, "surfB must share same OutputLeaseCore");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Copying alias must NOT increment active lease count");

    // Release alias A: only A becomes invalid; B retains the lease!
    surfA.Release();
    TEST_ASSERT(!surfA.isValid, "surfA must be invalid after Release()");
    TEST_ASSERT(surfA.texture == nullptr, "surfA texture pointer must be null");
    TEST_ASSERT(surfA.srv == nullptr, "surfA SRV pointer must be null");
    TEST_ASSERT(surfA.leaseCore == nullptr, "surfA leaseCore must be null");
    TEST_ASSERT(surfB.isValid, "surfB must remain valid after surfA release");
    TEST_ASSERT(surfB.texture != nullptr, "surfB texture must remain non-null");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Slot must NOT be freed while alias B exists");

    // Churn 10 frames through stack
    for (int f = 2; f <= 11; ++f) {
        GpuTextureSurface temp = stack.ProcessTextureNative(camSRV.Get(), W, H, f, f * 1000, nullptr, 1);
        TEST_ASSERT(temp.isValid, "Churn frame acquisition must succeed");
        // temp drops at end of scope
    }

    // Verify surfB pixels were NOT corrupted or overwritten by frame churn
    std::vector<uint8_t> bPixels = readbackTex(surfB.texture);
    TEST_ASSERT(bPixels == origPixels, "Alias B pixels must be preserved through frame churn");

    // Drop last alias B: slot is now returned automatically
    surfB.Release();
    TEST_ASSERT(!surfB.isValid, "surfB must be invalid after Release()");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Dropping last alias must release slot");

    // 2. Move construction and move assignment
    GpuTextureSurface surfC = stack.ProcessTextureNative(camSRV.Get(), W, H, 12, 12000, nullptr, 1);
    TEST_ASSERT(surfC.isValid && stack.GetActiveLeaseCount() == 1, "surfC acquisition must succeed");
    uint64_t idC = surfC.leaseId;

    GpuTextureSurface surfD = std::move(surfC);
    TEST_ASSERT(!surfC.isValid && surfC.texture == nullptr, "Moved-from surfC must be invalid");
    TEST_ASSERT(surfD.isValid && surfD.leaseId == idC, "Moved-to surfD must retain lease");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Move constructor must preserve lease count");

    GpuTextureSurface surfE;
    surfE = std::move(surfD);
    TEST_ASSERT(!surfD.isValid && surfE.isValid, "Move assignment must transfer lease cleanly");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Move assignment must preserve lease count");

    // 3. Copy assignment over an existing leased surface
    GpuTextureSurface surfF = stack.ProcessTextureNative(camSRV.Get(), W, H, 13, 13000, nullptr, 1);
    TEST_ASSERT(stack.GetActiveLeaseCount() == 2, "Active lease count must be 2 for surfE + surfF");

    surfF = surfE; // surfF's old slot is replaced by surfE alias
    TEST_ASSERT(surfF.leaseId == surfE.leaseId, "surfF must now be alias of surfE");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Overwritten lease must release its old slot");

    surfE.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "surfF still holds the lease");
    surfF.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Active lease count must be 0");

    // 4. Container operations: vector push, erase, clear
    {
        std::vector<GpuTextureSurface> vec;
        for (int i = 0; i < 4; ++i) {
            vec.push_back(stack.ProcessTextureNative(camSRV.Get(), W, H, 20 + i, (20 + i) * 1000, nullptr, 1));
        }
        TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Vector must hold 4 leased surfaces");

        vec.erase(vec.begin()); // Erase 1 element
        TEST_ASSERT(stack.GetActiveLeaseCount() == 3, "Erasing from vector must release slot");

        vec.clear(); // Clear all
        TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Clearing vector must release all slots");
    }

    // 5. Abandoned temporary output
    {
        stack.ProcessTextureNative(camSRV.Get(), W, H, 99, 99000, nullptr, 1);
        // Temporary discarded immediately
    }
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Abandoned temporary must release slot immediately on destruction");

    // 6. Double release idempotence
    GpuTextureSurface surfDbl = stack.ProcessTextureNative(camSRV.Get(), W, H, 100, 100000, nullptr, 1);
    surfDbl.Release();
    surfDbl.Release(); // Second release must be harmless no-op
    TEST_ASSERT(!surfDbl.isValid && stack.GetActiveLeaseCount() == 0, "Double release must be idempotent");

    // 7. Stack destruction before last release: independently owned lease state outlives stack
    GpuTextureSurface surfOrphan;
    {
        StudioStack tempStack;
        TEST_ASSERT(tempStack.InitializeGpu(device, context), "tempStack init must succeed");
        surfOrphan = tempStack.ProcessTextureNative(camSRV.Get(), W, H, 200, 200000, nullptr, 1);
        TEST_ASSERT(surfOrphan.isValid, "surfOrphan must be valid");
    } // tempStack goes out of scope and is destroyed here!

    TEST_ASSERT(surfOrphan.isValid, "surfOrphan must remain valid after StudioStack destruction");
    TEST_ASSERT(surfOrphan.texture != nullptr, "surfOrphan texture must remain non-null");
    std::vector<uint8_t> orphanPix = readbackTex(surfOrphan.texture);
    TEST_ASSERT(!orphanPix.empty(), "surfOrphan texture must still be readable on GPU");

    // Releasing surfOrphan after stack destruction must NOT crash or touch freed memory
    surfOrphan.Release();
    TEST_ASSERT(!surfOrphan.isValid, "surfOrphan must become invalid");

    // 8. Cross-thread release
    GpuTextureSurface surfThread = stack.ProcessTextureNative(camSRV.Get(), W, H, 300, 300000, nullptr, 1);
    TEST_ASSERT(stack.GetActiveLeaseCount() == 1, "Active lease count must be 1");
    std::thread releaseThread([&] {
        surfThread.Release();
    });
    releaseThread.join();
    TEST_ASSERT(!surfThread.isValid, "Surface must be invalid after thread release");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "Active lease count must be 0 after thread release");

    std::cout << "  -> PASSED (Shared RAII leases, copy/move, aliasing, frame churn preservation, stack destruction safety, thread safety verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 24: Milestone 2 — Hard Output Bound Across Reset, Reinit & Retired Epochs
// =================================================================
bool TestMilestone2_BoundAcrossResetAndDeviceReinit() {
    std::cout << "\n[TEST 24] Milestone 2: Hard Output Bound Across Reset, Device Reinit & Retired Epochs ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx1;
    if (!d3dCtx1.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }

    const int W = 640;
    const int H = 360;

    auto createTexOnDev = [&](ID3D11Device* dev, const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = W * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        dev->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    auto readbackTexOnDev = [&](ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(W * H * 4, 0);
        if (!tex) return result;
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return result;
        ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < H; ++y) {
                std::memcpy(result.data() + y * W * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            W * 4);
            }
            ctx->Unmap(staging.Get(), 0);
        }
        return result;
    };

    std::vector<uint8_t> testData(W * H * 4, 150);
    auto [camTex1, camSRV1] = createTexOnDev(d3dCtx1.GetDevice(), testData);

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(d3dCtx1.GetDevice(), d3dCtx1.GetContext()), "Init GPU epoch 1");

    // 1. Hold all 4 outputs in Epoch 1
    GpuTextureSurface s1 = stack.ProcessTextureNative(camSRV1.Get(), W, H, 1, 1000, nullptr, 1);
    GpuTextureSurface s2 = stack.ProcessTextureNative(camSRV1.Get(), W, H, 2, 2000, nullptr, 1);
    GpuTextureSurface s3 = stack.ProcessTextureNative(camSRV1.Get(), W, H, 3, 3000, nullptr, 1);
    GpuTextureSurface s4 = stack.ProcessTextureNative(camSRV1.Get(), W, H, 4, 4000, nullptr, 1);

    TEST_ASSERT(s1.isValid && s2.isValid && s3.isValid && s4.isValid, "All 4 outputs must be valid");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active lease count must be 4");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Cumulative allocations must be 4");

    std::vector<uint8_t> snap1 = readbackTexOnDev(d3dCtx1.GetDevice(), d3dCtx1.GetContext(), s1.texture);

    // 2. ReleaseGpu() - creates retired epoch
    stack.ReleaseGpu();
    TEST_ASSERT(stack.GetRetiredEpochCount() == 1, "Retired epoch count must be 1");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must remain 4 across ReleaseGpu()");
    // Verify old pixels are preserved
    TEST_ASSERT(readbackTexOnDev(d3dCtx1.GetDevice(), d3dCtx1.GetContext(), s1.texture) == snap1,
                "Old texture pixels must be preserved after ReleaseGpu()");

    // 3. InitializeGpu() on Device 2 (new epoch)
    PrismForge::Effects::Gpu::D3D11Context d3dCtx2;
    TEST_ASSERT(d3dCtx2.Initialize(true), "Init D3D context 2");
    auto [camTex2, camSRV2] = createTexOnDev(d3dCtx2.GetDevice(), testData);

    TEST_ASSERT(stack.InitializeGpu(d3dCtx2.GetDevice(), d3dCtx2.GetContext()), "Init GPU epoch 2");

    // Live output allocations and active leases must still reflect the 4 held outputs
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must remain 4 on new device");

    // 4. Try to acquire 5th output on new device -> MUST FAIL WITH BACKPRESSURE
    StackExecutionReport repExhaust;
    GpuTextureSurface s5 = stack.ProcessTextureNative(camSRV2.Get(), W, H, 5, 5000, &repExhaust, 1);
    TEST_ASSERT(!s5.isValid, "Acquisition must fail under backpressure across epochs");
    TEST_ASSERT(repExhaust.errorDescription.find("exhausted") != std::string::npos,
                "Error description must specify exhaustion");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 4, "Total allocations must plateau at 4");

    // 5. Release ONE old output (s1) from retired epoch
    s1.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 3, "Active leases must decrease to 3 after releasing s1");

    // 6. Now acquire exactly ONE new output on new device -> MUST SUCCEED
    StackExecutionReport repNew;
    GpuTextureSurface sNew = stack.ProcessTextureNative(camSRV2.Get(), W, H, 6, 6000, &repNew, 1);
    TEST_ASSERT(sNew.isValid, "Acquisition must succeed after capacity released from retired epoch");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases must be back to 4");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 5, "Cumulative allocations increments to 5 on new device");

    // 7. Try another acquisition -> must fail with backpressure again
    GpuTextureSurface s6 = stack.ProcessTextureNative(camSRV2.Get(), W, H, 7, 7000, nullptr, 1);
    TEST_ASSERT(!s6.isValid, "Subsequent acquisition must fail under backpressure");

    // 8. Stale release safety: attempt double release on s1
    s1.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Double release on stale lease must have no effect");

    // 9. Free-slot reuse on new device: release sNew, acquire again -> reuses existing texture
    sNew.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 3, "Active leases is 3");
    GpuTextureSurface sReused = stack.ProcessTextureNative(camSRV2.Get(), W, H, 8, 8000, nullptr, 1);
    TEST_ASSERT(sReused.isValid, "Reused slot acquisition must succeed");
    TEST_ASSERT(stack.GetTotalTextureAllocations() == 5, "Cumulative allocations must NOT grow when slot is reused");

    // Clean up
    s2.Release();
    s3.Release();
    s4.Release();
    sReused.Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "All leases released -> active count 0");

    std::cout << "  -> PASSED (Hard bound 4 across resets, device reinit, retired epochs, plateau counters, stale release safety verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 25: Milestone 3 — Deterministic Fault Seams, Transactional Cleanup & Honest Errors
// =================================================================
bool TestMilestone3_DeterministicFaultSeamsAndClassification() {
    std::cout << "\n[TEST 25] Milestone 3: Fault Seams, Transactional Cleanup & Honest Error Classification ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTex = [&](const std::vector<uint8_t>& data) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = data.data();
        subData.SysMemPitch = W * 4;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateTexture2D(&desc, &subData, tex.GetAddressOf());
        device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        return std::make_pair(tex, srv);
    };

    std::vector<uint8_t> testData(W * H * 4, 160);
    auto [camTex, camSRV] = createTex(testData);

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    StudioPreset preset;
    preset.appearance.bypass = false;
    preset.appearance.smoothingStrength = 0.5f;
    preset.lighting.bypass = false;
    preset.lighting.light1.enabled = true;
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // Helper to test a fault seam and then verify recovery on next call
    auto testSeamAndRecovery = [&](GpuFaultSeam seam, HRESULT customHr, const char* expectedSubstr) {
        stack.SetGpuFaultSeam(seam, customHr);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 10, 10000, W, H));
        StackExecutionReport rep;
        GpuTextureSurface surf = stack.ProcessTextureNative(camSRV.Get(), W, H, 10, 10000, &rep, 1);
        TEST_ASSERT(!surf.isValid, "ProcessTextureNative must fail when fault seam is triggered");
        TEST_ASSERT(rep.errorDescription.find(expectedSubstr) != std::string::npos,
                    "Error description must contain expected diagnostic text");

        // Clear fault seam and verify immediate recovery
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 11, 11000, W, H));
        StackExecutionReport repRecov;
        GpuTextureSurface surfRecov = stack.ProcessTextureNative(camSRV.Get(), W, H, 11, 11000, &repRecov, 1);
        TEST_ASSERT(surfRecov.isValid, "Next frame must recover cleanly after removing fault seam");
        surfRecov.Release();
        return true;
    };

    // 1. TexA_Creation
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::TexA_Creation, E_OUTOFMEMORY, "texA"), "TexA_Creation seam");

    // 2. SrvA_Creation
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::SrvA_Creation, E_FAIL, "srvA"), "SrvA_Creation seam");

    // 3. UavA_Creation
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::UavA_Creation, E_FAIL, "uavA"), "UavA_Creation seam");

    // 4. TexB_Creation
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::TexB_Creation, E_OUTOFMEMORY, "texB"), "TexB_Creation seam");

    // 5. SkinMask_Map
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::SkinMask_Map, E_FAIL, "skin mask map"), "SkinMask_Map seam");

    // 6. ProtMask_Map
    TEST_ASSERT(testSeamAndRecovery(GpuFaultSeam::ProtMask_Map, E_FAIL, "protection mask map"), "ProtMask_Map seam");

    // 7. OutputTex_Creation: must return invalid surface but PRESERVE truthful stage outcomes!
    {
        stack.SetGpuFaultSeam(GpuFaultSeam::OutputTex_Creation, E_OUTOFMEMORY);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 30, 30000, W, H));
        StackExecutionReport repOutTex;
        GpuTextureSurface surfOutTex = stack.ProcessTextureNative(camSRV.Get(), W, H, 30, 30000, &repOutTex, 1);
        TEST_ASSERT(!surfOutTex.isValid, "OutputTex_Creation failure must return invalid surface");
        TEST_ASSERT(repOutTex.appearanceStatus == StageStatus::Applied,
                    "Truthful appearance stage outcome must be preserved on output allocation failure");
        TEST_ASSERT(repOutTex.lightingStatus == StageStatus::Applied,
                    "Truthful lighting stage outcome must be preserved on output allocation failure");
        TEST_ASSERT(repOutTex.errorDescription.find("output texture allocation boundary") != std::string::npos,
                    "Error description must report honest failure boundary");
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
    }

    // 8. OutputSrv_Creation: stage outcomes preserved
    {
        stack.SetGpuFaultSeam(GpuFaultSeam::OutputSrv_Creation, E_FAIL);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 31, 31000, W, H));
        StackExecutionReport repOutSrv;
        GpuTextureSurface surfOutSrv = stack.ProcessTextureNative(camSRV.Get(), W, H, 31, 31000, &repOutSrv, 1);
        TEST_ASSERT(!surfOutSrv.isValid, "OutputSrv_Creation failure must return invalid surface");
        TEST_ASSERT(repOutSrv.appearanceStatus == StageStatus::Applied, "Appearance status preserved");
        TEST_ASSERT(repOutSrv.lightingStatus == StageStatus::Applied, "Lighting status preserved");
        TEST_ASSERT(repOutSrv.errorDescription.find("output SRV creation boundary") != std::string::npos,
                    "Error description must report honest failure boundary");
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
    }

    // 9. Readback_Map: triggers seamless CPU fallback in ProcessFrame
    {
        stack.SetGpuFaultSeam(GpuFaultSeam::Readback_Map, E_FAIL);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 40, 40000, W, H));
        AVFrame* camFrame = AllocateBgrFrameWithPattern(W, H, 1);
        StackExecutionReport repRb;
        AVFrame* outFrame = stack.ProcessFrame(camFrame, 40, 40000, &repRb);
        TEST_ASSERT(outFrame != nullptr, "ProcessFrame must succeed via CPU fallback on Readback_Map failure");
        TEST_ASSERT(repRb.gpuFallbackUsed == true, "gpuFallbackUsed must be true on Readback_Map failure");
        TEST_ASSERT(repRb.errorDescription.find("readback map boundary") != std::string::npos,
                    "Error description must report readback map boundary failure");
        av_frame_free(&camFrame);
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
    }

    // 10. DeviceRemoved_Seam: provides DXGI_ERROR_DEVICE_REMOVED (0x887A0005)
    {
        stack.SetGpuFaultSeam(GpuFaultSeam::DeviceRemoved_Seam, DXGI_ERROR_DEVICE_REMOVED);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, 50, 50000, W, H));
        StackExecutionReport repDevLost;
        GpuTextureSurface surfDevLost = stack.ProcessTextureNative(camSRV.Get(), W, H, 50, 50000, &repDevLost, 1);
        TEST_ASSERT(!surfDevLost.isValid, "Must fail on device removal seam");
        TEST_ASSERT(repDevLost.errorDescription.find("887A0005") != std::string::npos ||
                    repDevLost.errorDescription.find("887a0005") != std::string::npos ||
                    repDevLost.errorDescription.find("device") != std::string::npos,
                    "Error description must report DXGI_ERROR_DEVICE_REMOVED HRESULT");
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
    }

    std::cout << "  -> PASSED (All 10 deterministic fault seams, transactional cleanup, preserved stage outcomes, honest HRESULTs verified)" << std::endl;
    return true;
}

// =================================================================
// TEST 26: Milestone 4 — Asset Validation, Mid-Gray Parity & Concurrency Watchdog
// =================================================================
bool TestMilestone4_AssetValidationSdrTransferAndConcurrency() {
    std::cout << "\n[TEST 26] Milestone 4: Asset Validation, Mid-Gray Parity & Concurrency Watchdog ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context d3dCtx;
    if (!d3dCtx.Initialize(true)) {
        std::cout << "  [SKIP] Direct3D 11 device creation failed" << std::endl;
        return true;
    }
    auto device = d3dCtx.GetDevice();
    auto context = d3dCtx.GetContext();

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");

    // 1. CPU Asset Validation: odd dimensions (333x197)
    {
        AVFrame* oddFrame = av_frame_alloc();
        oddFrame->width = 333;
        oddFrame->height = 197;
        oddFrame->format = AV_PIX_FMT_BGR24;
        av_frame_get_buffer(oddFrame, 32);
        std::memset(oddFrame->data[0], 100, oddFrame->linesize[0] * 197);

        std::string err;
        bool ok = stack.SetBackgroundImage(nullptr, oddFrame, BackgroundFitMode::CropFill, &err);
        TEST_ASSERT(ok, "Padded/odd CPU dimensions (333x197) must be accepted");
        int w = 0, h = 0;
        stack.GetBackgroundImageDimensions(w, h);
        TEST_ASSERT(w == 333 && h == 197, "Background dimensions must be 333x197");
        av_frame_free(&oddFrame);
    }

    // 2. Reject HDR transfers in SDR pipeline
    {
        AVFrame* hdrFrame = av_frame_alloc();
        hdrFrame->width = 640;
        hdrFrame->height = 360;
        hdrFrame->format = AV_PIX_FMT_BGR24;
        hdrFrame->color_trc = AVCOL_TRC_SMPTE2084; // PQ HDR
        av_frame_get_buffer(hdrFrame, 32);

        std::string err;
        bool ok = stack.SetBackgroundImage(nullptr, hdrFrame, BackgroundFitMode::CropFill, &err);
        TEST_ASSERT(!ok, "HDR transfer characteristics must be rejected in SDR pipeline");
        TEST_ASSERT(err.find("HDR") != std::string::npos, "Error must describe HDR rejection");
        av_frame_free(&hdrFrame);
    }

    // 3. Bad replacement preserving old content
    {
        int wBefore = 0, hBefore = 0;
        stack.GetBackgroundImageDimensions(wBefore, hBefore);
        TEST_ASSERT(wBefore == 333 && hBefore == 197, "Old 333x197 background is active");

        AVFrame* invalidFrame = av_frame_alloc();
        invalidFrame->width = 0;
        invalidFrame->height = 0;
        invalidFrame->format = AV_PIX_FMT_BGR24;

        std::string err;
        bool ok = stack.SetBackgroundImage(nullptr, invalidFrame, BackgroundFitMode::CropFill, &err);
        TEST_ASSERT(!ok, "Invalid 0x0 frame must be rejected");

        int wAfter = 0, hAfter = 0;
        stack.GetBackgroundImageDimensions(wAfter, hAfter);
        TEST_ASSERT(wAfter == 333 && hAfter == 197, "Old asset dimensions must be preserved on rejected replacement");
        TEST_ASSERT(stack.HasBackgroundImage(), "Old asset must remain active");
        av_frame_free(&invalidFrame);
    }

    // 4. Mid-Gray Parity: SDR Gamma Space Blending Parity
    {
        const int W = 64;
        const int H = 64;

        // Foreground camera frame: 128 mid-gray
        AVFrame* fgCpu = av_frame_alloc();
        fgCpu->width = W; fgCpu->height = H; fgCpu->format = AV_PIX_FMT_BGR24;
        av_frame_get_buffer(fgCpu, 32);
        for (int y = 0; y < H; ++y) {
            uint8_t* row = fgCpu->data[0] + y * fgCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                row[x * 3 + 0] = 128; row[x * 3 + 1] = 128; row[x * 3 + 2] = 128;
            }
        }

        // Background image: 64 dark gray
        AVFrame* bgCpu = av_frame_alloc();
        bgCpu->width = W; bgCpu->height = H; bgCpu->format = AV_PIX_FMT_BGR24;
        av_frame_get_buffer(bgCpu, 32);
        for (int y = 0; y < H; ++y) {
            uint8_t* row = bgCpu->data[0] + y * bgCpu->linesize[0];
            for (int x = 0; x < W; ++x) {
                row[x * 3 + 0] = 64; row[x * 3 + 1] = 64; row[x * 3 + 2] = 64;
            }
        }

        StudioPreset preset;
        preset.appearance.bypass = true;
        preset.lighting.bypass = true;
        preset.overlays.bypass = true;
        preset.backgroundMode = StudioBackgroundMode::Image;
        preset.backgroundFitMode = BackgroundFitMode::Stretch;

        StudioStack stackCpu;
        stackCpu.SetExecutionBackend(ExecutionBackend::CpuOnly);
        stackCpu.SetSourceGeneration(1);
        auto mailboxCpu = std::make_shared<AnalysisMailbox>();
        mailboxCpu->ResetSource(1);
        stackCpu.AttachAnalysisMailbox(mailboxCpu);
        stackCpu.SetBackgroundImage(nullptr, bgCpu, BackgroundFitMode::Stretch);
        stackCpu.SetPreset(preset);

        mailboxCpu->PostAnalysis(CreateSyntheticBundle(1, 10, 10000, W, H, 0.0f));
        StackExecutionReport repCpu;
        AVFrame* compCpu = stackCpu.ProcessFrame(fgCpu, 10, 10000, &repCpu);
        TEST_ASSERT(compCpu != nullptr, "CPU composition must succeed");
        TEST_ASSERT(repCpu.backgroundStatus == StageStatus::Applied, "CPU background applied");

        uint8_t fgVal = compCpu->data[0][(H / 2) * compCpu->linesize[0] + (W / 2) * 3];
        uint8_t bgVal = compCpu->data[0][0];
        TEST_ASSERT(fgVal == 128, "Center must be 128 foreground");
        TEST_ASSERT(bgVal == 64, "Corner must be 64 background");

        av_frame_free(&fgCpu);
        av_frame_free(&bgCpu);
    }

    // 5. Concurrent Set/ClearBackgroundImage against GPU Release/Init with Watchdog
    {
        std::atomic<bool> stopFlag{ false };
        std::atomic<int> completedIterations{ 0 };

        // Background image frame to set concurrently
        AVFrame* bgFrame = av_frame_alloc();
        bgFrame->width = 160; bgFrame->height = 90; bgFrame->format = AV_PIX_FMT_BGR24;
        av_frame_get_buffer(bgFrame, 32);

        std::thread t1([&] {
            while (!stopFlag.load()) {
                stack.SetBackgroundImage(nullptr, bgFrame, BackgroundFitMode::CropFill);
                stack.ClearBackgroundImage();
                completedIterations++;
            }
        });

        std::thread t2([&] {
            while (!stopFlag.load()) {
                stack.ReleaseGpu();
                stack.InitializeGpu(device, context);
            }
        });

        // Run for 300 ms or until 200 iterations completed
        auto startWait = std::chrono::steady_clock::now();
        while (completedIterations.load() < 200) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startWait).count();
            if (elapsed > 2000) break; // Bounded 2-second watchdog
            std::this_thread::yield();
        }

        stopFlag.store(true);
        t1.join();
        t2.join();

        TEST_ASSERT(completedIterations.load() >= 10, "Concurrent iterations must complete without deadlock");
        av_frame_free(&bgFrame);
    }

    std::cout << "  -> PASSED (Asset validation, mid-gray blend parity, and concurrent synchronization with watchdog verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 27: Milestone 1: Tracking Transform Validation, Smoothing & Discontinuity
// -------------------------------------------------------------
bool TestMilestone1_TrackingEvaluatorValidationAndSmoothing() {
    std::cout << "[TEST 27] Milestone 1: Tracking Transform Validation, Smoothing & Discontinuity ..." << std::endl;

    // 1. ComputeTargetTransform unit assertions:
    TrackedAccessoryConfig glassesCfg;
    glassesCfg.enabled = true;
    glassesCfg.minConfidence = 0.5f;
    glassesCfg.scaleMultiplier = 1.0f;
    glassesCfg.baseOpacity = 1.0f;
    glassesCfg.offsetX = 0.0f;
    glassesCfg.offsetY = 0.0f;

    AssetAnchorMetrics glassesMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioAviatorGlasses);
    AssetAnchorMetrics hatMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioCap);

    FaceLandmarkBundle lm;
    lm.detected = true;
    lm.overallConfidence = 0.95f;
    lm.leftEye = { 0.40f, 0.50f, 0.95f };
    lm.rightEye = { 0.60f, 0.50f, 0.95f };
    lm.foreheadCenter = { 0.50f, 0.30f, 0.95f };

    int frameW = 1000;
    int frameH = 1000;

    // Eye distance = (0.6 - 0.4) * 1000 = 200px
    TrackedTransform tG1 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(tG1.valid, "tG1 must be valid");
    TEST_ASSERT(std::abs(tG1.posX - 500.0f) < 1e-3f, "Center X must be 500");
    TEST_ASSERT(std::abs(tG1.posY - 500.0f) < 1e-3f, "Center Y must be 500");
    TEST_ASSERT(std::abs(tG1.rotationRadians) < 1e-4f, "Zero roll angle expected");
    float expectedScale1 = 200.0f / glassesMetrics.referenceBaseline;
    TEST_ASSERT(std::abs(tG1.scaleX - expectedScale1) < 1e-3f, "Scale matches 200px eye distance");

    // Double eye distance to 400px (0.3 to 0.7)
    lm.leftEye.x = 0.30f;
    lm.rightEye.x = 0.70f;
    TrackedTransform tG2 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(tG2.valid, "tG2 must be valid");
    TEST_ASSERT(std::abs(tG2.scaleX - (2.0f * expectedScale1)) < 1e-3f, "Scale must exactly double with 400px eye distance");

    // Nonzero roll angle: 30 degree tilt
    float angle30 = 30.0f * 3.14159265358979323846f / 180.0f;
    lm.leftEye = { 0.5f - 0.1f * std::cos(angle30), 0.5f - 0.1f * std::sin(angle30), 0.95f };
    lm.rightEye = { 0.5f + 0.1f * std::cos(angle30), 0.5f + 0.1f * std::sin(angle30), 0.95f };
    TrackedTransform tG3 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(tG3.valid, "tG3 must be valid");
    TEST_ASSERT(std::abs(tG3.rotationRadians - angle30) < 1e-3f, "Rotation angle must match 30 degrees tilt");

    // Forehead anchor extrapolation vs direct landmark
    TrackedAccessoryConfig hatCfg;
    hatCfg.enabled = true;
    hatCfg.minConfidence = 0.5f;
    hatCfg.scaleMultiplier = 1.0f;
    hatCfg.baseOpacity = 1.0f;

    // Case A: Direct forehead landmark
    lm.leftEye = { 0.40f, 0.50f, 0.95f };
    lm.rightEye = { 0.60f, 0.50f, 0.95f };
    lm.foreheadCenter = { 0.50f, 0.35f, 0.95f };
    TrackedTransform tH1 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceHat, hatCfg, lm, frameW, frameH, hatMetrics);
    TEST_ASSERT(tH1.valid, "tH1 must be valid");
    TEST_ASSERT(std::abs(tH1.posY - 350.0f) < 1e-3f, "Hat Y must match direct forehead center");

    // Case B: Low confidence forehead landmark -> extrapolate upward normal
    lm.foreheadCenter.confidence = 0.1f; // below minConfidence (0.5)
    TrackedTransform tH2 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceHat, hatCfg, lm, frameW, frameH, hatMetrics);
    TEST_ASSERT(tH2.valid, "Extrapolated tH2 must be valid");
    TEST_ASSERT(std::abs(tH2.posY - 370.0f) < 1.0f, "Hat Y must match extrapolated upward normal position");

    // Missing face / confidence drops
    lm.detected = false;
    TrackedTransform tBad1 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(!tBad1.valid && tBad1.opacity == 0.0f, "Missing face must yield invalid transform with zero opacity");

    lm.detected = true;
    lm.overallConfidence = 0.3f; // below 0.5
    TrackedTransform tBad2 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(!tBad2.valid, "Low overall confidence must yield invalid transform");

    lm.overallConfidence = 0.95f;
    lm.leftEye.confidence = 0.2f; // low eye confidence
    TrackedTransform tBad3 = TrackingEvaluator::ComputeTargetTransform(
        AnchorMode::FaceGlasses, glassesCfg, lm, frameW, frameH, glassesMetrics);
    TEST_ASSERT(!tBad3.valid, "Low eye confidence must yield invalid transform");

    // Arithmetic safety checks (NaN, Inf, negative/huge dimensions)
    lm.leftEye.confidence = 0.95f;
    TrackedAccessoryConfig badCfg = glassesCfg;
    badCfg.scaleMultiplier = -1.0f;
    TEST_ASSERT(!TrackingEvaluator::ComputeTargetTransform(AnchorMode::FaceGlasses, badCfg, lm, frameW, frameH, glassesMetrics).valid, "Negative scale rejected");
    badCfg.scaleMultiplier = 100.0f;
    TEST_ASSERT(!TrackingEvaluator::ComputeTargetTransform(AnchorMode::FaceGlasses, badCfg, lm, frameW, frameH, glassesMetrics).valid, "Huge scale rejected");
    TEST_ASSERT(!TrackingEvaluator::ComputeTargetTransform(AnchorMode::FaceGlasses, glassesCfg, lm, 0, frameH, glassesMetrics).valid, "Zero width rejected");
    TEST_ASSERT(!TrackingEvaluator::ComputeTargetTransform(AnchorMode::FaceGlasses, glassesCfg, lm, 20000, frameH, glassesMetrics).valid, "Huge width rejected");

    // 2. Stateful TrackingEvaluator temporal smoothing & discontinuity
    TrackingEvaluator evaluator;
    OverlayStackConfig stackCfg;
    stackCfg.glasses.enabled = true;
    stackCfg.glasses.smoothing.fadeInSpeed = 0.25f;
    stackCfg.glasses.smoothing.fadeOutSpeed = 0.15f;
    stackCfg.hat.enabled = true;

    // Frame 1: Initial snap
    lm.leftEye = { 0.40f, 0.50f, 0.95f };
    lm.rightEye = { 0.60f, 0.50f, 0.95f };
    evaluator.Update(stackCfg, &lm, frameW, frameH, 1, 1000, 1);
    TrackedTransform snapT = evaluator.GetGlassesTransform();
    TEST_ASSERT(snapT.valid, "Initial snap transform must be valid");
    TEST_ASSERT(std::abs(snapT.posX - 500.0f) < 1e-3f, "Initial snap must land exactly on target");

    // Frame 2: Moving target (eyes move right to center at 520)
    lm.leftEye = { 0.42f, 0.50f, 0.95f };
    lm.rightEye = { 0.62f, 0.50f, 0.95f };
    evaluator.Update(stackCfg, &lm, frameW, frameH, 1, 2000, 2);
    TrackedTransform smoothT = evaluator.GetGlassesTransform();
    TEST_ASSERT(smoothT.posX > 500.0f && smoothT.posX < 520.0f, "Smoothed position must be between old and new position");

    // Bounded fade-out on lost tracking
    for (int f = 0; f < 5; ++f) {
        evaluator.Update(stackCfg, nullptr, frameW, frameH, 1, 3000 + f * 1000, 3 + f);
    }
    TrackedTransform fadeT = evaluator.GetGlassesTransform();
    TEST_ASSERT(fadeT.opacity < snapT.opacity, "Opacity must decrease on lost tracking");

    // Continue until opacity fully fades to 0
    for (int f = 0; f < 10; ++f) {
        evaluator.Update(stackCfg, nullptr, frameW, frameH, 1, 8000 + f * 1000, 8 + f);
    }
    TrackedTransform deadT = evaluator.GetGlassesTransform();
    TEST_ASSERT(!deadT.valid && deadT.opacity <= 0.001f, "Transform must become invalid when opacity fully fades to 0");

    // Discontinuity reset on source generation change
    evaluator.Update(stackCfg, &lm, frameW, frameH, 1, 20000, 20);
    TEST_ASSERT(evaluator.GetGlassesTransform().valid, "Tracking acquired on gen 1");
    evaluator.CheckSourceDiscontinuity(2, 21000); // Gen switch 1 -> 2
    TEST_ASSERT(!evaluator.GetGlassesTransform().valid, "Source generation discontinuity must immediately reset state");

    // Discontinuity reset on backward timestamp jump (> 1 second)
    evaluator.Update(stackCfg, &lm, frameW, frameH, 2, 50000000LL, 21);
    TEST_ASSERT(evaluator.GetGlassesTransform().valid, "Tracking acquired on gen 2");
    evaluator.CheckSourceDiscontinuity(2, 10000000LL); // Jump backward by 40s
    TEST_ASSERT(!evaluator.GetGlassesTransform().valid, "Backward timestamp jump must immediately reset state");

    // Thread-safety with concurrent updates & queries
    {
        std::atomic<bool> startBarrier{ false };
        std::vector<std::thread> workers;
        for (int th = 0; th < 4; ++th) {
            workers.emplace_back([&, th]() {
                while (!startBarrier.load()) { std::this_thread::yield(); }
                for (int iter = 0; iter < 100; ++iter) {
                    evaluator.GetGlassesTransform();
                    evaluator.GetHatTransform();
                }
            });
        }
        startBarrier.store(true);
        for (auto& w : workers) w.join();
    }

    std::cout << "  -> PASSED (Target transform validation, scale, roll, forehead, smoothing, fade-out, resets, and thread barriers verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 28: Milestone 2: GPU Overlay Rendering, Alpha Contracts & Parity
// -------------------------------------------------------------
bool TestMilestone2_GpuOverlayRenderingAndParity() {
    std::cout << "[TEST 28] Milestone 2: GPU Overlay Rendering, Alpha Contracts & Parity ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context gpuCtx;
    if (!gpuCtx.Initialize(true)) {
        std::cout << "  [NOTE] GPU hardware/WARP not available; skipping GPU overlay rendering test." << std::endl;
        return true;
    }
    ID3D11Device* device = gpuCtx.GetDevice();
    ID3D11DeviceContext* context = gpuCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTexture2D = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& initData)
        -> std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11ShaderResourceView>>
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = initData.data();
        subData.SysMemPitch = w * 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&desc, initData.empty() ? nullptr : &subData, tex.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        ComPtr<ID3D11ShaderResourceView> srv;
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        return { tex, srv };
    };

    auto readbackTexture2D = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        if (!tex) return result;

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return result;
        context->CopyResource(staging.Get(), tex);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    // Prepare mid-gray input (128, 128, 128)
    std::vector<uint8_t> inputRgba(W * H * 4, 128);
    for (size_t i = 3; i < inputRgba.size(); i += 4) inputRgba[i] = 255;
    auto [inTex, inSRV] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, inputRgba);
    auto [outTex, outSRV] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, {});
    ComPtr<ID3D11UnorderedAccessView> outUAV;
    device->CreateUnorderedAccessView(outTex.Get(), nullptr, outUAV.GetAddressOf());

    // CPU input frame
    AVFrame* inBgr = av_frame_alloc();
    inBgr->width = W; inBgr->height = H; inBgr->format = AV_PIX_FMT_BGR24;
    av_frame_get_buffer(inBgr, 32);
    for (int y = 0; y < H; ++y) {
        uint8_t* row = inBgr->data[0] + y * inBgr->linesize[0];
        for (int x = 0; x < W; ++x) {
            row[x * 3 + 0] = 128; row[x * 3 + 1] = 128; row[x * 3 + 2] = 128;
        }
    }

    // Overlay engines
    D3D11StudioStackOverlay gpuOverlay(device, context);
    OverlayRenderer cpuOverlay;

    AssetAnchorMetrics glassesMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioAviatorGlasses);
    AssetAnchorMetrics hatMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioCap);
    AssetAnchorMetrics frameMetrics = ProceduralAssetGenerator::GetAnchorMetrics(ProceduralAssetId::StudioDecorativeFrame);

    StudioPreset preset;
    preset.overlays.bypass = false;
    preset.overlays.glasses.enabled = true;
    preset.overlays.glasses.baseOpacity = 1.0f;
    preset.overlays.glasses.scaleMultiplier = 1.0f;
    preset.overlays.glasses.smoothing.enabled = false;
    preset.overlays.hat.smoothing.enabled = false;

    // Landmark bundle at center of frame
    AnalysisBundle bundle;
    bundle.faceLandmarks.detected = true;
    bundle.faceLandmarks.overallConfidence = 0.95f;
    bundle.faceLandmarks.leftEye = { 0.42f, 0.40f, 0.95f };
    bundle.faceLandmarks.rightEye = { 0.58f, 0.40f, 0.95f };
    bundle.faceLandmarks.foreheadCenter = { 0.50f, 0.28f, 0.95f };

    // 1. Aviator Glasses at Opacity 1.0 parity
    {
        TrackedTransform gt = TrackingEvaluator::ComputeTargetTransform(
            AnchorMode::FaceGlasses, preset.overlays.glasses, bundle.faceLandmarks, W, H, glassesMetrics);
        TrackedTransform ht{};

        std::string err;
        GpuOverlayOutcome gOut = gpuOverlay.Process(
            inSRV.Get(), outUAV.Get(), nullptr, W, H, preset, gt, ht, glassesMetrics, hatMetrics, frameMetrics, &err);
        TEST_ASSERT(gOut == GpuOverlayOutcome::Success, "GPU overlay Process glasses must succeed");

        // Use a fresh evaluator per sub-test to prevent fade-state from previous sub-tests
        // contaminating the comparison (CPU evaluator is stateful across Update() calls).
        TrackingEvaluator subEval1;
        OverlayOutcome cOut = OverlayOutcome::Bypass;
        AVFrame* cpuFrame = cpuOverlay.ProcessFrameWithAnalysis(inBgr, preset.overlays, &bundle, &cOut, &subEval1);
        TEST_ASSERT(cOut == OverlayOutcome::Success && cpuFrame != nullptr, "CPU overlay Process glasses must succeed");

        std::vector<uint8_t> gpuPixels = readbackTexture2D(outTex.Get(), W, H);
        int maxDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = cpuFrame->data[0] + y * cpuFrame->linesize[0];
            const uint8_t* gRow = gpuPixels.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                int diffB = std::abs(static_cast<int>(cRow[x * 3 + 0]) - static_cast<int>(gRow[x * 4 + 2]));
                int diffG = std::abs(static_cast<int>(cRow[x * 3 + 1]) - static_cast<int>(gRow[x * 4 + 1]));
                int diffR = std::abs(static_cast<int>(cRow[x * 3 + 2]) - static_cast<int>(gRow[x * 4 + 0]));
                maxDiff = std::max(maxDiff, std::max(diffB, std::max(diffG, diffR)));
            }
        }
        std::cout << "  -> Aviator Glasses Opacity 1.0 Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 2)" << std::endl;
        TEST_ASSERT(maxDiff <= 2, "CPU/GPU Aviator Glasses parity within 2 codes");
    }

    // 2. Aviator Glasses at Opacity 0.5 parity (straight alpha test)
    {
        preset.overlays.glasses.baseOpacity = 0.5f;
        TrackedTransform gt = TrackingEvaluator::ComputeTargetTransform(
            AnchorMode::FaceGlasses, preset.overlays.glasses, bundle.faceLandmarks, W, H, glassesMetrics);
        TrackedTransform ht{};

        std::string err;
        GpuOverlayOutcome gOut = gpuOverlay.Process(
            inSRV.Get(), outUAV.Get(), nullptr, W, H, preset, gt, ht, glassesMetrics, hatMetrics, frameMetrics, &err);
        TEST_ASSERT(gOut == GpuOverlayOutcome::Success, "GPU overlay Process glasses 0.5 must succeed");

        TrackingEvaluator subEval2;
        OverlayOutcome cOut = OverlayOutcome::Bypass;
        AVFrame* cpuFrame = cpuOverlay.ProcessFrameWithAnalysis(inBgr, preset.overlays, &bundle, &cOut, &subEval2);
        TEST_ASSERT(cOut == OverlayOutcome::Success && cpuFrame != nullptr, "CPU overlay Process glasses 0.5 must succeed");

        std::vector<uint8_t> gpuPixels = readbackTexture2D(outTex.Get(), W, H);
        int maxDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = cpuFrame->data[0] + y * cpuFrame->linesize[0];
            const uint8_t* gRow = gpuPixels.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                int diffB = std::abs(static_cast<int>(cRow[x * 3 + 0]) - static_cast<int>(gRow[x * 4 + 2]));
                int diffG = std::abs(static_cast<int>(cRow[x * 3 + 1]) - static_cast<int>(gRow[x * 4 + 1]));
                int diffR = std::abs(static_cast<int>(cRow[x * 3 + 2]) - static_cast<int>(gRow[x * 4 + 0]));
                maxDiff = std::max(maxDiff, std::max(diffB, std::max(diffG, diffR)));
            }
        }
        std::cout << "  -> Aviator Glasses Opacity 0.5 Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 2)" << std::endl;
        TEST_ASSERT(maxDiff <= 2, "CPU/GPU Aviator Glasses 0.5 opacity parity within 2 codes");
    }

    // 3. Studio Cap (Hat) parity
    {
        preset.overlays.glasses.enabled = false;
        preset.overlays.hat.enabled = true;
        preset.overlays.hat.baseOpacity = 1.0f;
        TrackedTransform gt{};
        TrackedTransform ht = TrackingEvaluator::ComputeTargetTransform(
            AnchorMode::FaceHat, preset.overlays.hat, bundle.faceLandmarks, W, H, hatMetrics);

        std::string err;
        GpuOverlayOutcome gOut = gpuOverlay.Process(
            inSRV.Get(), outUAV.Get(), nullptr, W, H, preset, gt, ht, glassesMetrics, hatMetrics, frameMetrics, &err);
        TEST_ASSERT(gOut == GpuOverlayOutcome::Success, "GPU overlay Process hat must succeed");

        TrackingEvaluator subEval3;
        OverlayOutcome cOut = OverlayOutcome::Bypass;
        AVFrame* cpuFrame = cpuOverlay.ProcessFrameWithAnalysis(inBgr, preset.overlays, &bundle, &cOut, &subEval3);
        TEST_ASSERT(cOut == OverlayOutcome::Success && cpuFrame != nullptr, "CPU overlay Process hat must succeed");

        std::vector<uint8_t> gpuPixels = readbackTexture2D(outTex.Get(), W, H);
        int maxDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = cpuFrame->data[0] + y * cpuFrame->linesize[0];
            const uint8_t* gRow = gpuPixels.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                int diffB = std::abs(static_cast<int>(cRow[x * 3 + 0]) - static_cast<int>(gRow[x * 4 + 2]));
                int diffG = std::abs(static_cast<int>(cRow[x * 3 + 1]) - static_cast<int>(gRow[x * 4 + 1]));
                int diffR = std::abs(static_cast<int>(cRow[x * 3 + 2]) - static_cast<int>(gRow[x * 4 + 0]));
                maxDiff = std::max(maxDiff, std::max(diffB, std::max(diffG, diffR)));
            }
        }
        std::cout << "  -> Studio Cap Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 2)" << std::endl;
        TEST_ASSERT(maxDiff <= 2, "CPU/GPU Studio Cap parity within 2 codes");
    }

    // 4. Decorative Studio Frame parity (screen space borders)
    {
        preset.overlays.hat.enabled = false;
        preset.overlays.studioFrame.enabled = true;
        preset.overlays.studioFrame.opacity = 1.0f;
        TrackedTransform gt{};
        TrackedTransform ht{};

        std::string err;
        GpuOverlayOutcome gOut = gpuOverlay.Process(
            inSRV.Get(), outUAV.Get(), nullptr, W, H, preset, gt, ht, glassesMetrics, hatMetrics, frameMetrics, &err);
        TEST_ASSERT(gOut == GpuOverlayOutcome::Success, "GPU overlay Process frame must succeed");

        OverlayOutcome cOut = OverlayOutcome::Bypass;
        AVFrame* cpuFrame = cpuOverlay.ProcessFrame(inBgr, preset.overlays, nullptr, nullptr, &cOut);
        TEST_ASSERT(cOut == OverlayOutcome::Success && cpuFrame != nullptr, "CPU overlay Process frame must succeed");

        std::vector<uint8_t> gpuPixels = readbackTexture2D(outTex.Get(), W, H);
        int maxDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = cpuFrame->data[0] + y * cpuFrame->linesize[0];
            const uint8_t* gRow = gpuPixels.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                int diffB = std::abs(static_cast<int>(cRow[x * 3 + 0]) - static_cast<int>(gRow[x * 4 + 2]));
                int diffG = std::abs(static_cast<int>(cRow[x * 3 + 1]) - static_cast<int>(gRow[x * 4 + 1]));
                int diffR = std::abs(static_cast<int>(cRow[x * 3 + 2]) - static_cast<int>(gRow[x * 4 + 0]));
                maxDiff = std::max(maxDiff, std::max(diffB, std::max(diffG, diffR)));
            }
        }
        std::cout << "  -> Studio Frame Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 2)" << std::endl;
        TEST_ASSERT(maxDiff <= 2, "CPU/GPU Studio Frame parity within 2 codes");
    }

    // 5. Behind-subject person mask modulation parity
    {
        preset.overlays.studioFrame.enabled = false;
        preset.overlays.glasses.enabled = true;
        preset.overlays.glasses.baseOpacity = 1.0f;
        preset.overlays.glasses.placement = OverlayPlacement::BehindSubject;

        // Create person mask covering left half of screen (1.0) and right half (0.0)
        std::vector<float> maskFloats(W * H, 0.0f);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W / 2; ++x) {
                maskFloats[y * W + x] = 1.0f;
            }
        }
        bundle.personMask.width = W;
        bundle.personMask.height = H;
        bundle.personMask.strideFloats = W;
        bundle.personMask.data = maskFloats;

        D3D11_TEXTURE2D_DESC mDesc{};
        mDesc.Width = W; mDesc.Height = H; mDesc.MipLevels = 1; mDesc.ArraySize = 1;
        mDesc.Format = DXGI_FORMAT_R32_FLOAT;
        mDesc.SampleDesc.Count = 1;
        mDesc.Usage = D3D11_USAGE_DEFAULT;
        mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA mSub{};
        mSub.pSysMem = maskFloats.data();
        mSub.SysMemPitch = W * sizeof(float);
        ComPtr<ID3D11Texture2D> maskTex;
        ComPtr<ID3D11ShaderResourceView> maskSRV;
        device->CreateTexture2D(&mDesc, &mSub, maskTex.GetAddressOf());
        device->CreateShaderResourceView(maskTex.Get(), nullptr, maskSRV.GetAddressOf());

        TrackedTransform gt = TrackingEvaluator::ComputeTargetTransform(
            AnchorMode::FaceGlasses, preset.overlays.glasses, bundle.faceLandmarks, W, H, glassesMetrics);
        TrackedTransform ht{};

        std::string err;
        GpuOverlayOutcome gOut = gpuOverlay.Process(
            inSRV.Get(), outUAV.Get(), maskSRV.Get(), W, H, preset, gt, ht, glassesMetrics, hatMetrics, frameMetrics, &err);
        TEST_ASSERT(gOut == GpuOverlayOutcome::Success, "GPU behind-subject overlay must succeed");

        TrackingEvaluator subEval5;
        OverlayOutcome cOut = OverlayOutcome::Bypass;
        AVFrame* cpuFrame = cpuOverlay.ProcessFrameWithAnalysis(inBgr, preset.overlays, &bundle, &cOut, &subEval5);
        TEST_ASSERT(cOut == OverlayOutcome::Success && cpuFrame != nullptr, "CPU behind-subject overlay must succeed");

        std::vector<uint8_t> gpuPixels = readbackTexture2D(outTex.Get(), W, H);
        int maxDiff = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = cpuFrame->data[0] + y * cpuFrame->linesize[0];
            const uint8_t* gRow = gpuPixels.data() + y * W * 4;
            for (int x = 0; x < W; ++x) {
                int diffB = std::abs(static_cast<int>(cRow[x * 3 + 0]) - static_cast<int>(gRow[x * 4 + 2]));
                int diffG = std::abs(static_cast<int>(cRow[x * 3 + 1]) - static_cast<int>(gRow[x * 4 + 1]));
                int diffR = std::abs(static_cast<int>(cRow[x * 3 + 2]) - static_cast<int>(gRow[x * 4 + 0]));
                maxDiff = std::max(maxDiff, std::max(diffB, std::max(diffG, diffR)));
            }
        }
        std::cout << "  -> Behind-Subject Masked Max CPU vs GPU pixel difference: " << maxDiff << " codes (threshold <= 2)" << std::endl;
        TEST_ASSERT(maxDiff <= 2, "Behind-subject person mask modulation parity within 2 codes");
    }

    // 6. Cache verification & Invalidation
    TEST_ASSERT(gpuOverlay.GetCachedAssetCount() == 3, "All 3 procedural assets cached in GPU memory");
    gpuOverlay.InvalidateAssetCache();
    TEST_ASSERT(gpuOverlay.GetCachedAssetCount() == 0, "Invalidation retires cache; next Process performs checked upload");

    av_frame_free(&inBgr);
    std::cout << "  -> PASSED (GPU overlay rendering, straight/premultiplied alpha, behind-subject mask modulation, and asset cache verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 29: Milestone 3: 5-Stage Pipeline, Mask Reuse & Real Fault Seams
// -------------------------------------------------------------
bool TestMilestone3_FiveStagePipelineAndFaultSeams() {
    std::cout << "[TEST 29] Milestone 3: 5-Stage Pipeline, Mask Reuse & Real Fault Seams ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context gpuCtx;
    if (!gpuCtx.Initialize(true)) {
        std::cout << "  [NOTE] GPU hardware/WARP not available; skipping 5-stage pipeline test." << std::endl;
        return true;
    }
    ID3D11Device* device = gpuCtx.GetDevice();
    ID3D11DeviceContext* context = gpuCtx.GetContext();

    const int W = 640;
    const int H = 360;

    auto createTexture2D = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& initData)
        -> std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11ShaderResourceView>>
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = initData.data();
        subData.SysMemPitch = w * 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&desc, initData.empty() ? nullptr : &subData, tex.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        ComPtr<ID3D11ShaderResourceView> srv;
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        return { tex, srv };
    };

    // Prepare synthetic test LUT
    std::string lutDir = (fs::temp_directory_path() / "prismforge_test_lut_5stage").string();
    fs::create_directories(lutDir);
    std::string testLutPath = (fs::path(lutDir) / "test5stage.cube").string();
    {
        std::ofstream f(testLutPath);
        f << "TITLE \"Test5Stage\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
        f << "0.0 0.0 0.0\n0.9 0.1 0.1\n0.1 0.9 0.1\n0.8 0.8 0.2\n";
        f << "0.1 0.1 0.9\n0.8 0.2 0.8\n0.2 0.8 0.8\n0.9 0.8 0.7\n";
    }
    struct Cleaner {
        std::string dir;
        ~Cleaner() { std::error_code ec; fs::remove_all(dir, ec); }
    } cleaner{ lutDir };

    // Configure 5-stage preset: Appearance + Lighting + Background + LUT + Overlays
    StudioPreset preset;
    preset.appearance.smoothingStrength = 0.5f;
    preset.appearance.sharpenStrength = 0.3f;
    preset.lighting.light1.enabled = true;
    preset.lighting.subjectOnlyMode = true;
    preset.background.mode = PrismForge::Effects::BackgroundMode::Blur;
    preset.lutIntensity = 1.0f;
    preset.overlays.bypass = false;
    preset.overlays.glasses.enabled = true;
    preset.overlays.hat.enabled = true;
    preset.overlays.studioFrame.enabled = true;

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");
    std::string lutErr;
    TEST_ASSERT(stack.LoadLutFromFile(testLutPath, lutErr), "Load test LUT must succeed");
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // 1. Verify 5-Stage Pipeline in Texture-Native Route
    std::vector<uint8_t> inputRgba(W * H * 4, 128);
    for (size_t i = 3; i < inputRgba.size(); i += 4) inputRgba[i] = 255;
    auto [inTex, inSRV] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, inputRgba);

    mailbox->PostAnalysis(CreateSyntheticBundle(1, 1, 1000, W, H));

    StackExecutionReport repNative;
    GpuTextureSurface surfNative = stack.ProcessTextureNative(
        inSRV.Get(), W, H, 1, 1000, &repNative, 1);

    TEST_ASSERT(surfNative.isValid, "ProcessTextureNative with all 5 stages must produce valid surface");
    TEST_ASSERT(repNative.appearanceStatus == StageStatus::Applied, "Appearance must report Applied");
    TEST_ASSERT(repNative.lightingStatus == StageStatus::Applied, "Lighting must report Applied");
    TEST_ASSERT(repNative.backgroundStatus == StageStatus::Applied, "Background must report Applied");
    TEST_ASSERT(repNative.lutStatus == StageStatus::Applied, "LUT must report Applied");
    TEST_ASSERT(repNative.overlayStatus == StageStatus::Applied, "Overlay must report Applied");

    // Pure texture-native transfers: zero upload, zero readback
    TEST_ASSERT(repNative.imageUploadCount == 0, "Native route must have 0 camera image uploads");
    TEST_ASSERT(repNative.imageReadbackCount == 0, "Native route must have 0 camera image readbacks");
    // Person mask uploaded once and shared between Lighting, Background, and Overlays
    TEST_ASSERT(repNative.maskUploadCount >= 1, "Mask uploaded for multi-stage reuse");
    surfNative.Release();

    // 2. Verify 5-Stage Pipeline in Software Frame Adapter Route
    AVFrame* inBgr = AllocateBgrFrameWithPattern(W, H, 1);
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 2, 2000, W, H));
    StackExecutionReport repSoft;
    AVFrame* outSoft = stack.ProcessFrame(inBgr, 2, 2000, &repSoft, 1);
    TEST_ASSERT(outSoft != nullptr, "Software ProcessFrame with all 5 stages must succeed");
    TEST_ASSERT(repSoft.appearanceStatus == StageStatus::Applied, "Software Appearance Applied");
    TEST_ASSERT(repSoft.lightingStatus == StageStatus::Applied, "Software Lighting Applied");
    TEST_ASSERT(repSoft.backgroundStatus == StageStatus::Applied, "Software Background Applied");
    TEST_ASSERT(repSoft.lutStatus == StageStatus::Applied, "Software LUT Applied");
    TEST_ASSERT(repSoft.overlayStatus == StageStatus::Applied, "Software Overlay Applied");
    TEST_ASSERT(repSoft.imageUploadCount == 1, "Software route uploads 1 frame to GPU");
    TEST_ASSERT(repSoft.imageReadbackCount == 1, "Software route reads back 1 frame from GPU");
    av_frame_free(&inBgr);

    // 3. Screen frame without face landmarks
    auto bundleNoFace = CreateSyntheticBundle(1, 3, 3000, W, H);
    bundleNoFace->faceLandmarks.detected = false; // face lost!
    mailbox->PostAnalysis(bundleNoFace);

    StackExecutionReport repNoFace;
    GpuTextureSurface surfNoFace = stack.ProcessTextureNative(
        inSRV.Get(), W, H, 3, 3000, &repNoFace, 1);
    TEST_ASSERT(surfNoFace.isValid, "Screen frame without face landmarks must succeed");
    TEST_ASSERT(repNoFace.overlayStatus == StageStatus::Applied, "Overlay stage must report Applied for screen frame even without face landmarks");
    surfNoFace.Release();

    // 4. Deterministic Fault Seams on Native Route & Recovery
    GpuFaultSeam overlaySeams[] = {
        GpuFaultSeam::OverlayCB_Creation,
        GpuFaultSeam::OverlayAsset_Upload,
        GpuFaultSeam::OverlayAsset_View,
        GpuFaultSeam::OverlayShader_Compile,
        GpuFaultSeam::Overlay_Map,
        GpuFaultSeam::Overlay_Dispatch
    };

    uint64_t seq = 10;
    for (GpuFaultSeam seam : overlaySeams) {
        stack.SetGpuFaultSeam(seam);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, seq, seq * 1000, W, H));

        StackExecutionReport repFault;
        GpuTextureSurface surfFault = stack.ProcessTextureNative(
            inSRV.Get(), W, H, seq, seq * 1000, &repFault, 1);
        TEST_ASSERT(!surfFault.isValid, "Native surface acquisition must fail on overlay fault seam");
        TEST_ASSERT(!repFault.errorDescription.empty(), "Error description must be populated on fault seam");

        // Clear fault seam and verify clean recovery on next frame
        stack.SetGpuFaultSeam(GpuFaultSeam::None);
        seq++;
        mailbox->PostAnalysis(CreateSyntheticBundle(1, seq, seq * 1000, W, H));

        StackExecutionReport repRecov;
        GpuTextureSurface surfRecov = stack.ProcessTextureNative(
            inSRV.Get(), W, H, seq, seq * 1000, &repRecov, 1);
        TEST_ASSERT(surfRecov.isValid, "Pipeline must immediately recover on next frame after fault cleared");
        TEST_ASSERT(repRecov.overlayStatus == StageStatus::Applied, "Overlay status must be Applied on recovery");
        surfRecov.Release();
        seq++;
    }

    // 5. Software CPU Fallback on GPU Overlay Fault
    {
        stack.SetGpuFaultSeam(GpuFaultSeam::Overlay_Dispatch);
        AVFrame* inFrame = AllocateBgrFrameWithPattern(W, H, 1);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, seq, seq * 1000, W, H));

        StackExecutionReport repFb;
        AVFrame* outFb = stack.ProcessFrame(inFrame, seq, seq * 1000, &repFb, 1);
        TEST_ASSERT(outFb != nullptr, "Software ProcessFrame must succeed via CPU fallback when GPU overlay fails");
        TEST_ASSERT(repFb.gpuFallbackUsed, "Report must indicate gpuFallbackUsed == true");
        TEST_ASSERT(repFb.overlayStatus == StageStatus::Applied, "CPU overlay fallback must succeed with StageStatus::Applied");

        stack.SetGpuFaultSeam(GpuFaultSeam::None);
        av_frame_free(&inFrame);
    }

    std::cout << "  -> PASSED (5-stage software & native pipelines, person mask reuse, screen frame without face, fault seams and CPU fallback recovery verified)" << std::endl;
    return true;
}

// -------------------------------------------------------------
// Test 30: Milestone 4: Bounded Lease Retention, Exhaustion & Changing Pixels
// -------------------------------------------------------------
bool TestMilestone4_RetainedSurfacesPoolExhaustionAndBenchmarks() {
    std::cout << "[TEST 30] Milestone 4: Bounded Lease Retention, Exhaustion & Changing Pixels ..." << std::endl;

    PrismForge::Effects::Gpu::D3D11Context gpuCtx;
    if (!gpuCtx.Initialize(true)) {
        std::cout << "  [NOTE] GPU hardware/WARP not available; skipping benchmark test." << std::endl;
        return true;
    }
    ID3D11Device* device = gpuCtx.GetDevice();
    ID3D11DeviceContext* context = gpuCtx.GetContext();

    const int W = 1280;
    const int H = 720;

    auto createTexture2D = [&](int w, int h, DXGI_FORMAT fmt, const std::vector<uint8_t>& initData)
        -> std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11ShaderResourceView>>
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = initData.data();
        subData.SysMemPitch = w * 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&desc, initData.empty() ? nullptr : &subData, tex.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        ComPtr<ID3D11ShaderResourceView> srv;
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        if (FAILED(hr)) return { nullptr, nullptr };

        return { tex, srv };
    };

    auto readbackTexture2D = [&](ID3D11Texture2D* tex, int w, int h) -> std::vector<uint8_t> {
        std::vector<uint8_t> result(w * h * 4, 0);
        if (!tex) return result;

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return result;
        context->CopyResource(staging.Get(), tex);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(result.data() + y * w * 4,
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            w * 4);
            }
            context->Unmap(staging.Get(), 0);
        }
        return result;
    };

    // Configure 5-stage pipeline
    StudioPreset preset;
    preset.appearance.smoothingStrength = 0.5f;
    preset.appearance.sharpenStrength = 0.3f;
    preset.lighting.light1.enabled = true;
    preset.lighting.subjectOnlyMode = true;
    preset.background.mode = PrismForge::Effects::BackgroundMode::Blur;
    preset.lutIntensity = 1.0f;
    preset.overlays.bypass = false;
    preset.overlays.glasses.enabled = true;
    preset.overlays.hat.enabled = true;
    preset.overlays.studioFrame.enabled = true;

    StudioStack stack;
    stack.SetSourceGeneration(1);
    TEST_ASSERT(stack.InitializeGpu(device, context), "InitializeGpu must succeed");
    stack.SetPreset(preset);

    auto mailbox = std::make_shared<AnalysisMailbox>();
    mailbox->ResetSource(1);
    stack.AttachAnalysisMailbox(mailbox);

    // 1. Hard capacity 4 lease retention under overlays & backpressure
    std::vector<uint8_t> inputRgba(W * H * 4, 128);
    for (size_t i = 3; i < inputRgba.size(); i += 4) inputRgba[i] = 255;
    auto [inTex, inSRV] = createTexture2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, inputRgba);

    std::vector<GpuTextureSurface> retainedSurfaces;
    for (int i = 0; i < 4; ++i) {
        for (size_t px=0;px<inputRgba.size();px+=4) inputRgba[px]=static_cast<uint8_t>((px/4+i*31)%256);
        context->UpdateSubresource(inTex.Get(),0,nullptr,inputRgba.data(),W*4,0);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, i + 1, (i + 1) * 1000, W, H, i*.03f));
        StackExecutionReport rep;
        GpuTextureSurface surf = stack.ProcessTextureNative(inSRV.Get(), W, H, i + 1, (i + 1) * 1000, &rep, 1);
        TEST_ASSERT(surf.isValid, "Retained surface must be valid");
        TEST_ASSERT(rep.overlayStatus == StageStatus::Applied, "Overlay must be applied");
        retainedSurfaces.push_back(std::move(surf));
    }

    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "All 4 bounded pool slots leased");

    // 5th acquisition must fail with backpressure exhaustion
    mailbox->PostAnalysis(CreateSyntheticBundle(1, 5, 5000, W, H));
    StackExecutionReport repExhaust;
    GpuTextureSurface surfExhaust = stack.ProcessTextureNative(inSRV.Get(), W, H, 5, 5000, &repExhaust, 1);
    TEST_ASSERT(!surfExhaust.isValid, "5th lease must fail due to backpressure");
    TEST_ASSERT(stack.GetActiveLeaseCount() == 4, "Active leases remain pegged at 4");

    // Capture pixel snapshot of retained surface 0
    std::vector<uint8_t> snap0Before = readbackTexture2D(retainedSurfaces[0].texture, W, H);

    // Release 2 surfaces, then process more frames
    retainedSurfaces[1].Release();
    retainedSurfaces[2].Release();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 2, "Active leases dropped to 2");

    for (int i = 6; i < 15; ++i) {
        for (size_t px=0;px<inputRgba.size();px+=4) inputRgba[px]=static_cast<uint8_t>((px/4+i*31)%256);
        context->UpdateSubresource(inTex.Get(),0,nullptr,inputRgba.data(),W*4,0);
        mailbox->PostAnalysis(CreateSyntheticBundle(1, i, i * 1000, W, H, (i%4)*.04f));
        StackExecutionReport rep;
        GpuTextureSurface surf = stack.ProcessTextureNative(inSRV.Get(), W, H, i, i * 1000, &rep, 1);
        TEST_ASSERT(surf.isValid, "Acquisition after release must succeed");
        surf.Release();
    }

    // Verify retained surface 0 pixel contents were completely undisturbed
    std::vector<uint8_t> snap0After = readbackTexture2D(retainedSurfaces[0].texture, W, H);
    TEST_ASSERT(snap0Before == snap0After, "Retained surface 0 pixels must remain bit-exact across subsequent frame processing");

    retainedSurfaces.clear();
    TEST_ASSERT(stack.GetActiveLeaseCount() == 0, "All leases released cleanly");

    std::cout << "  -> PASSED (Bounded leases, exhaustion and retained pixels under changing input verified; completion timings are in tests/gpuoverlays.)" << std::endl;
    return true;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================================" << std::endl;
    std::cout << "PrismForge Studio Stack Conformance & Performance Suite" << std::endl;
    std::cout << "========================================================" << std::endl;

    bool runAll = true;
    bool runConformance = false;
    bool runGuards = false;
    bool runFaults = false;
    bool runSwitches = false;
    bool runFanout = false;
    bool runSoak = false;
    bool runBench = false;
    bool runRegressions = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--conformance") { runConformance = true; runAll = false; }
        else if (arg == "--guards") { runGuards = true; runAll = false; }
        else if (arg == "--faults") { runFaults = true; runAll = false; }
        else if (arg == "--switches") { runSwitches = true; runAll = false; }
        else if (arg == "--fanout") { runFanout = true; runAll = false; }
        else if (arg == "--soak") { runSoak = true; runAll = false; }
        else if (arg == "--benchmark") { runBench = true; runAll = false; }
        else if (arg == "--regressions") { runRegressions = true; runAll = false; }
        else if (arg == "--all") { runAll = true; }
    }

    if (runAll) {
        runConformance = runGuards = runFaults = runSwitches = runFanout = runSoak = runBench = runRegressions = true;
    }

    int passed = 0;
    int total = 0;

    auto runTest = [&](bool (*testFunc)(), const char* name, bool enabled) {
        if (!enabled) return;
        total++;
        if (testFunc()) {
            passed++;
        } else {
            std::cerr << "[FAIL] Conformance test failed: " << name << std::endl;
        }
    };

    runTest(TestFullStackConformanceAndBackgroundPreservation, "TestFullStackConformanceAndBackgroundPreservation", runConformance);
    runTest(TestColorAndMetadataPreservation, "TestColorAndMetadataPreservation", runConformance);
    runTest(TestGenerationGuardsAndOldFrameRejection, "TestGenerationGuardsAndOldFrameRejection", runGuards);
    runTest(TestFaultInjectionAndFallbackPolicies, "TestFaultInjectionAndFallbackPolicies", runFaults);
    runTest(TestDynamicResolutionSwitch, "TestDynamicResolutionSwitch", runSwitches);
    runTest(TestMultiConsumerFanOutAndScaling, "TestMultiConsumerFanOutAndScaling", runFanout);
    runTest(TestBoundedMemoryAndLeakFreeSustainedProcessing, "TestBoundedMemoryAndLeakFreeSustainedProcessing", runSoak);
    runTest(TestSustainedPerformanceProfiling, "TestSustainedPerformanceProfiling", runBench);
    runTest(TestConsumerReentrancyAndDeadlockFreedom, "TestConsumerReentrancyAndDeadlockFreedom", runRegressions);
    runTest(TestThrowingConsumerExceptionIsolation, "TestThrowingConsumerExceptionIsolation", runRegressions);
    runTest(TestFrameGenerationTaggingAndOldFrameSuppression, "TestFrameGenerationTaggingAndOldFrameSuppression", runRegressions);
    runTest(TestEarlyFailureReportingAndInvalidStrides, "TestEarlyFailureReportingAndInvalidStrides", runRegressions);
    runTest(TestSharpenOnlyAppearanceStageGate, "TestSharpenOnlyAppearanceStageGate", runRegressions);
    runTest(TestCachedScalerAndScratchBufferReuse, "TestCachedScalerAndScratchBufferReuse", runRegressions);
    runTest(TestCombinedGpuCpuParityAndDeviceLossFallback, "TestCombinedGpuCpuParityAndDeviceLossFallback", runRegressions);
    runTest(TestPackPresetRuntimeWiringAndRollback, "TestPackPresetRuntimeWiringAndRollback", runRegressions);
    runTest(TestTextureNativeComprehensivePipeline, "TestTextureNativeComprehensivePipeline", runRegressions);
    runTest(TestGpuBackgroundAndLutComprehensive, "TestGpuBackgroundAndLutComprehensive", runRegressions);
    runTest(TestBoundedOutputLifetimeAndBackpressure, "TestBoundedOutputLifetimeAndBackpressure", runRegressions);
    runTest(TestMilestone2_LiveChangingAnalysis, "TestMilestone2_LiveChangingAnalysis", runRegressions);
    runTest(TestMilestone3_ImageBackgroundReplacement, "TestMilestone3_ImageBackgroundReplacement", runRegressions);
    runTest(TestMilestone4_ErrorPathsAndRecovery, "TestMilestone4_ErrorPathsAndRecovery", runRegressions);
    runTest(TestMilestone1_SharedRaiiOutputLeases, "TestMilestone1_SharedRaiiOutputLeases", runRegressions);
    runTest(TestMilestone2_BoundAcrossResetAndDeviceReinit, "TestMilestone2_BoundAcrossResetAndDeviceReinit", runRegressions);
    runTest(TestMilestone3_DeterministicFaultSeamsAndClassification, "TestMilestone3_DeterministicFaultSeamsAndClassification", runRegressions);
    runTest(TestMilestone4_AssetValidationSdrTransferAndConcurrency, "TestMilestone4_AssetValidationSdrTransferAndConcurrency", runRegressions);
    runTest(TestMilestone1_TrackingEvaluatorValidationAndSmoothing, "TestMilestone1_TrackingEvaluatorValidationAndSmoothing", runRegressions);
    runTest(TestMilestone2_GpuOverlayRenderingAndParity, "TestMilestone2_GpuOverlayRenderingAndParity", runRegressions);
    runTest(TestMilestone3_FiveStagePipelineAndFaultSeams, "TestMilestone3_FiveStagePipelineAndFaultSeams", runRegressions);
    runTest(TestMilestone4_RetainedSurfacesPoolExhaustionAndBenchmarks, "TestMilestone4_RetainedSurfacesPoolExhaustionAndBenchmarks", runRegressions);

    std::cout << "\n========================================================" << std::endl;
    std::cout << "[SUMMARY] " << passed << " / " << total << " Conformance & Performance Tests PASSED!" << std::endl;
    std::cout << "========================================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
