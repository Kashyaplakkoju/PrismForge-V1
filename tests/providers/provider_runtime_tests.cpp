#include "providers/onnx_runner.h"
#include "providers/frame_request.h"
#include "providers/frame_preprocessor.h"
#include "providers/model_manifest.h"
#include "providers/result_adapter.h"
#include "providers/analysis_worker.h"
#include "effects/analysisbridge/analysis_mailbox.h"

#include "tiny_onnx_fixture.h"
#include "fake_delay_adapter.h"

#include <windows.h>
#include <iostream>
#include <vector>
#include <memory>
#include <cassert>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>

using namespace PrismForge::Providers;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Tests;

static bool WaitUntil(const std::function<bool()>& ready) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now()>=deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
static int g_passCount = 0;
static int g_failCount = 0;

#define EXPECT_TRUE(cond) do { \
    if (cond) { \
        g_passCount++; \
    } else { \
        std::cerr << "[FAIL] Line " << __LINE__ << ": " << #cond << std::endl; \
        g_failCount++; \
    } \
} while(0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_NEAR(a, b, tol) do { \
    float diff = std::abs(static_cast<float>(a) - static_cast<float>(b)); \
    if (diff <= (tol)) { \
        g_passCount++; \
    } else { \
        std::cerr << "[FAIL] Line " << __LINE__ << ": expected " << (a) << " ~= " << (b) \
                  << " (diff=" << diff << " > tol=" << (tol) << ")" << std::endl; \
        g_failCount++; \
    } \
} while(0)

// Helper: creates a solid color frame with specified dimensions
std::shared_ptr<const FrameRequest> CreateSolidFrame(
    uint64_t sourceGen, uint64_t seq, int64_t pts,
    int w, int h, uint8_t r, uint8_t g, uint8_t b,
    int strideExtra = 0, bool mirror = false
) {
    int stride = w * 3 + strideExtra;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * h, 0xEE); // poison fill
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    return FrameRequest::CreateFromPixels(
        sourceGen, seq, pts, w, h, stride,
        PixelFormat::RGB24, std::move(pixels),
        ColorRange::Full, ColorMatrix::Identity, FrameOrientation::Normal, mirror
    );
}

// ------------------------------------------------------------------------------------------------
// Test 1: Real ORT CPU inference using tiny deterministic ONNX fixture
// ------------------------------------------------------------------------------------------------
void Test01_RealOrtCpuInference() {
    std::cout << "\n[TEST 01] Real ORT CPU Inference (Tiny Fixture)" << std::endl;
    OnnxRunner runner;
    EXPECT_TRUE(runner.Initialize());

    auto modelBytes = TinyOnnxFixture::GetModelByteVector();
    RunnerStatusCode status = runner.LoadModelFromMemory(
        modelBytes.data(), modelBytes.size(), ExecutionProviderType::CPU
    );
    EXPECT_TRUE(status == RunnerStatusCode::Success);
    EXPECT_TRUE(runner.IsModelLoaded());

    auto info = runner.GetSessionInfo();
    EXPECT_TRUE(info.activeProvider == ExecutionProviderType::CPU);
    EXPECT_FALSE(info.fallbackOccurred);
    EXPECT_TRUE(info.inputName == "input");
    EXPECT_TRUE(info.outputName == "output");
    EXPECT_TRUE(info.inputElementCount == 1 * 3 * 16 * 16);
    EXPECT_TRUE(info.outputElementCount == 1 * 1 * 16 * 16);

    // Test pure Red: expected = 0.299
    std::vector<float> inRed(1 * 3 * 16 * 16, 0.0f);
    for (int i = 0; i < 16 * 16; ++i) inRed[i] = 1.0f; // R plane
    std::vector<float> outRed;
    EXPECT_TRUE(runner.RunInference(inRed, outRed) == RunnerStatusCode::Success);
    EXPECT_TRUE(outRed.size() == 256);
    EXPECT_NEAR(outRed[0], 0.299f, 1e-4f);

    // Test pure Green: expected = 0.587
    std::vector<float> inGreen(1 * 3 * 16 * 16, 0.0f);
    for (int i = 0; i < 16 * 16; ++i) inGreen[16 * 16 + i] = 1.0f; // G plane
    std::vector<float> outGreen;
    EXPECT_TRUE(runner.RunInference(inGreen, outGreen) == RunnerStatusCode::Success);
    EXPECT_NEAR(outGreen[0], 0.587f, 1e-4f);

    // Test pure Blue: expected = 0.114
    std::vector<float> inBlue(1 * 3 * 16 * 16, 0.0f);
    for (int i = 0; i < 16 * 16; ++i) inBlue[2 * 16 * 16 + i] = 1.0f; // B plane
    std::vector<float> outBlue;
    EXPECT_TRUE(runner.RunInference(inBlue, outBlue) == RunnerStatusCode::Success);
    EXPECT_NEAR(outBlue[0], 0.114f, 1e-4f);

    // Test file-based loading
    std::wstring tempFixturePath = L"test_fixture_temp.onnx";
    EXPECT_TRUE(TinyOnnxFixture::WriteFixtureFile(tempFixturePath));
    EXPECT_TRUE(runner.LoadModel(tempFixturePath, ExecutionProviderType::CPU) == RunnerStatusCode::Success);
    EXPECT_TRUE(runner.RunInference(inRed, outRed) == RunnerStatusCode::Success);
    EXPECT_NEAR(outRed[0], 0.299f, 1e-4f);
    DeleteFileW(tempFixturePath.c_str());
}

// ------------------------------------------------------------------------------------------------
// Test 2: Real DirectML execution and honest fallback
// ------------------------------------------------------------------------------------------------
void Test02_RealOrtDirectMLInferenceAndFallback() {
    std::cout << "\n[TEST 02] Real DirectML Inference & Honest Fallback" << std::endl;
    OnnxRunner runner;
    auto modelBytes = TinyOnnxFixture::GetModelByteVector();

    RunnerStatusCode status = runner.LoadModelFromMemory(
        modelBytes.data(), modelBytes.size(), ExecutionProviderType::DirectML, 0
    );
    EXPECT_TRUE(status == RunnerStatusCode::Success);

    auto info = runner.GetSessionInfo();
    if (info.activeProvider == ExecutionProviderType::DirectML) {
        std::cout << "  -> DirectML initialized successfully on GPU device 0" << std::endl;
        EXPECT_FALSE(info.fallbackOccurred);

        // Run inference on DML
        std::vector<float> inGreen(1 * 3 * 16 * 16, 0.0f);
        for (int i = 0; i < 16 * 16; ++i) inGreen[16 * 16 + i] = 1.0f;
        std::vector<float> outGreen;
        EXPECT_TRUE(runner.RunInference(inGreen, outGreen) == RunnerStatusCode::Success);
        EXPECT_NEAR(outGreen[0], 0.587f, 1e-3f);
    } else {
        std::cout << "  -> DirectML unavailable; honest fallback to CPU verified" << std::endl;
        EXPECT_TRUE(info.fallbackOccurred);
        EXPECT_FALSE(info.fallbackReason.empty());
        EXPECT_TRUE(info.activeProvider == ExecutionProviderType::CPU);
    }
}

// ------------------------------------------------------------------------------------------------
// Test 3: Error handling: missing model, corrupt bytes, invalid configuration
// ------------------------------------------------------------------------------------------------
void Test03_ErrorHandlingMissingAndCorruptModel() {
    std::cout << "\n[TEST 03] Error Handling: Missing & Corrupt Model" << std::endl;
    OnnxRunner runner;

    // Nonexistent file
    EXPECT_TRUE(runner.LoadModel(L"nonexistent_model_12345.onnx") == RunnerStatusCode::ModelNotFound);
    EXPECT_FALSE(runner.IsModelLoaded());

    // Corrupt random bytes
    std::vector<uint8_t> corruptBytes = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    EXPECT_TRUE(runner.LoadModelFromMemory(corruptBytes.data(), corruptBytes.size()) == RunnerStatusCode::CorruptModel);
    EXPECT_FALSE(runner.IsModelLoaded());

    // Valid load followed by invalid load retains prior valid session
    auto modelBytes = TinyOnnxFixture::GetModelByteVector();
    EXPECT_TRUE(runner.LoadModelFromMemory(modelBytes.data(), modelBytes.size()) == RunnerStatusCode::Success);
    EXPECT_TRUE(runner.IsModelLoaded());
    uint64_t validEpoch = runner.GetModelEpoch();

    // Invalid load attempt
    EXPECT_TRUE(runner.LoadModelFromMemory(corruptBytes.data(), corruptBytes.size()) == RunnerStatusCode::CorruptModel);
    // Prior session still valid
    EXPECT_TRUE(runner.IsModelLoaded());
    EXPECT_TRUE(runner.GetModelEpoch() == validEpoch);
}

// ------------------------------------------------------------------------------------------------
// Test 4: Tensor shape and size mismatch
// ------------------------------------------------------------------------------------------------
void Test04_TensorShapeMismatch() {
    std::cout << "\n[TEST 04] Tensor Shape Mismatch" << std::endl;
    OnnxRunner runner;
    auto modelBytes = TinyOnnxFixture::GetModelByteVector();
    runner.LoadModelFromMemory(modelBytes.data(), modelBytes.size());

    std::vector<float> badInput(100, 1.0f); // Model expects 768 elements (3*16*16)
    std::vector<float> output;
    EXPECT_TRUE(runner.RunInference(badInput, output) == RunnerStatusCode::TensorShapeMismatch);

    // Follow-up valid run succeeds
    std::vector<float> goodInput(768, 1.0f);
    EXPECT_TRUE(runner.RunInference(goodInput, output) == RunnerStatusCode::Success);
}

// ------------------------------------------------------------------------------------------------
// Test 5: Frame Preprocessor with odd dimensions, poison padding, and 1px frame
// ------------------------------------------------------------------------------------------------
void Test05_PreprocessorOddDimensionsAndPoisonPadding() {
    std::cout << "\n[TEST 05] Preprocessor: Odd Dims, Poison Padding & 1px" << std::endl;
    FramePreprocessor prep;
    NormalizationConfig norm;
    norm.mean = { 0.0f, 0.0f, 0.0f };
    norm.std = { 1.0f, 1.0f, 1.0f };
    norm.scaleTo01 = true;

    // 1px input
    auto frame1px = CreateSolidFrame(1, 1, 1000, 1, 1, 255, 0, 0);
    std::vector<float> tensor1px;
    GeometryTransform trans1px;
    EXPECT_TRUE(prep.Preprocess(*frame1px, 16, 16, TensorLayout::NCHW, norm, AspectMode::LetterboxPad, tensor1px, trans1px));
    EXPECT_TRUE(tensor1px.size() == 1 * 3 * 16 * 16);

    // Odd frame (15x7) with poison padding (10 extra bytes per row)
    auto frameOdd = CreateSolidFrame(1, 2, 2000, 15, 7, 0, 255, 0, 10);
    std::vector<float> tensorOdd;
    GeometryTransform transOdd;
    EXPECT_TRUE(prep.Preprocess(*frameOdd, 16, 16, TensorLayout::NCHW, norm, AspectMode::LetterboxPad, tensorOdd, transOdd));

    // Sample inside active letterbox region: Green should be 1.0f, Red and Blue 0.0f
    size_t activeX = transOdd.validTensorX + transOdd.validTensorW / 2;
    size_t activeY = transOdd.validTensorY + transOdd.validTensorH / 2;
    size_t gIdx = 1 * (16 * 16) + activeY * 16 + activeX;
    EXPECT_NEAR(tensorOdd[gIdx], 1.0f, 1e-4f);

    // Horizontal mirror test
    auto frameMirror = CreateSolidFrame(1, 3, 3000, 16, 16, 255, 0, 0, 0, true);
    std::vector<float> tensorMirror;
    GeometryTransform transMirror;
    EXPECT_TRUE(prep.Preprocess(*frameMirror, 16, 16, TensorLayout::NCHW, norm, AspectMode::LetterboxPad, tensorMirror, transMirror));
    EXPECT_TRUE(transMirror.mirror == true);
}

// ------------------------------------------------------------------------------------------------
// Test 6: Inverse Mask Projection mapping back to original frame dimensions
// ------------------------------------------------------------------------------------------------
void Test06_InverseMaskMapping() {
    std::cout << "\n[TEST 06] Inverse Mask Projection" << std::endl;
    FramePreprocessor prep;
    NormalizationConfig norm;
    auto frame = CreateSolidFrame(1, 1, 1000, 100, 50, 128, 128, 128);

    std::vector<float> tensor;
    GeometryTransform trans;
    EXPECT_TRUE(prep.Preprocess(*frame, 16, 16, TensorLayout::NCHW, norm, AspectMode::LetterboxPad, tensor, trans));

    // Construct a mock mask on the 16x16 tensor where all active pixels are 0.8f
    std::vector<float> tensorMask(16 * 16, 0.8f);

    MaskBuffer outMask;
    EXPECT_TRUE(prep.MapMaskBack(tensorMask.data(), 16, 16, trans, outMask));
    EXPECT_TRUE(outMask.IsValid());
    EXPECT_TRUE(outMask.width == 100);
    EXPECT_TRUE(outMask.height == 50);
    EXPECT_TRUE(outMask.strideFloats == 100);

    // Center pixel should be close to 0.8f
    float centerVal = outMask.data[25 * outMask.strideFloats + 50];
    EXPECT_NEAR(centerVal, 0.8f, 1e-2f);
}

// ------------------------------------------------------------------------------------------------
// Test 7: ResultAdapter and Truthful Capabilities
// ------------------------------------------------------------------------------------------------
void Test07_TruthfulCapabilitiesAndResultAdapter() {
    std::cout << "\n[TEST 07] ResultAdapter & Truthful Capabilities" << std::endl;
    ResultAdapter adapter;
    auto manifest = TinyOnnxFixture::CreateManifest();
    auto frame = CreateSolidFrame(42, 105, 5000000, 64, 48, 200, 100, 50);
    manifest.aspectMode=AspectMode::Stretch;

    GeometryTransform transform;
    FramePreprocessor prep;
    std::vector<float> tensor;
    EXPECT_TRUE(prep.Preprocess(*frame,16,16,manifest.inputLayout,manifest.normConfig,AspectMode::Stretch,tensor,transform));

    std::vector<float> rawOutput(256, 0.65f); // 16x16 output floats
    std::shared_ptr<AnalysisBundle> bundle;

    EXPECT_TRUE(adapter.AdaptResult(rawOutput, manifest, transform, *frame, bundle));
    EXPECT_TRUE(bundle != nullptr);

    // Exact provenance check
    EXPECT_TRUE(bundle->sourceGeneration == 42);
    EXPECT_TRUE(bundle->sequenceId == 105);
    EXPECT_TRUE(bundle->timestampPts == 5000000);
    EXPECT_TRUE(bundle->timestampCompleted > 0);

    // Truthful capabilities check
    EXPECT_TRUE(bundle->HasPersonMask());
    EXPECT_FALSE(bundle->HasSkinMask());
    EXPECT_FALSE(bundle->HasProtectionMask());
    EXPECT_FALSE(bundle->HasNormals());
    EXPECT_FALSE(bundle->HasDepth());
    EXPECT_FALSE(bundle->HasFaceLandmarks());

    // Non-finite output rejection
    std::vector<float> badOutput(256, 0.5f);
    badOutput[10] = std::numeric_limits<float>::quiet_NaN();
    std::shared_ptr<AnalysisBundle> badBundle;
    EXPECT_FALSE(adapter.AdaptResult(badOutput, manifest, transform, *frame, badBundle));
}

// ------------------------------------------------------------------------------------------------
// Test 8: Bounded Worker Single In-Flight + Latest Replacement (No FIFO growth)
// ------------------------------------------------------------------------------------------------
void Test08_BoundedWorkerSingleInFlightAndLatestReplacement() {
    std::cout << "\n[TEST 08] Bounded Worker: Single In-Flight & Latest Replacement" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    FakeDelayAdapter fake;
    fake.hold.store(true);
    fake.artificialDelayMs = 50; // 50ms delay per frame
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });

    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Submit frame 1 (enters in-flight)
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 255, 0, 0));
    EXPECT_TRUE(WaitUntil([&] { return fake.callCount.load()==1; }));

    // Rapidly submit frames 2 through 10 (each replaces the previous pending frame)
    for (uint64_t seq = 2; seq <= 10; ++seq) {
        worker.SubmitFrame(CreateSolidFrame(1, seq, seq * 1000, 16, 16, 0, 255, 0));
    }

    // Wait for in-flight (frame 1) and pending (frame 10) to finish
    fake.hold.store(false);
    EXPECT_TRUE(WaitUntil([&] { return worker.GetMetrics().totalCompleted==2; }));
    worker.Stop();

    auto metrics = worker.GetMetrics();
    std::cout << "  Submitted: " << metrics.totalSubmitted
              << ", ReplacedPending: " << metrics.totalReplacedPending
              << ", Completed: " << metrics.totalCompleted << std::endl;

    EXPECT_TRUE(metrics.totalSubmitted == 10);
    EXPECT_TRUE(metrics.totalReplacedPending == 8); // Frames 2..9 replaced
    EXPECT_TRUE(metrics.totalCompleted == 2);       // Exactly frame 1 and frame 10 completed

    // Verify mailbox has latest frame 10
    AnalysisAlignmentStatus alignStatus;
    auto aligned = mailbox->GetAlignedAnalysis(1, 10, 10000, alignStatus);
    EXPECT_TRUE(alignStatus == AnalysisAlignmentStatus::Aligned);
    EXPECT_TRUE(aligned != nullptr);
    EXPECT_TRUE(aligned->sequenceId == 10);
}

// ------------------------------------------------------------------------------------------------
// Test 9: Immediate release of replaced pending input
// ------------------------------------------------------------------------------------------------
void Test09_ImmediateReleaseOfReplacedPendingFrame() {
    std::cout << "\n[TEST 09] Immediate Release of Replaced Pending Frame" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    FakeDelayAdapter fake;
    fake.hold.store(true);
    fake.artificialDelayMs = 100;
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Start in-flight frame
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 10, 10, 10));
    EXPECT_TRUE(WaitUntil([&] { return fake.callCount.load()==1; }));

    // Submit frame 2
    auto frame2 = CreateSolidFrame(1, 2, 2000, 16, 16, 20, 20, 20);
    worker.SubmitFrame(frame2);
    // frame2 is now held in m_pendingFrame. Refcount should be at least 2 (frame2 local + m_pendingFrame)
    EXPECT_TRUE(frame2.use_count() >= 2);

    // Submit frame 3: should replace frame2 immediately
    auto frame3 = CreateSolidFrame(1, 3, 3000, 16, 16, 30, 30, 30);
    worker.SubmitFrame(frame3);

    // frame2 is no longer referenced by worker; refcount drops to 1 (local frame2 only)!
    EXPECT_TRUE(frame2.use_count() == 1);

    worker.Stop();
}

// ------------------------------------------------------------------------------------------------
// Test 10: Source Reset drops pending frame and cancels obsolete generation
// ------------------------------------------------------------------------------------------------
void Test10_SourceResetDropsPendingAndRejectsStale() {
    std::cout << "\n[TEST 10] Source Reset & Obsolete Generation Rejection" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    FakeDelayAdapter fake;
    fake.artificialDelayMs = 60;
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Submit frame on gen 1
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 100, 100, 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Reset source to gen 2 mid-flight
    worker.ResetSource(2);

    // Submit frame on gen 2
    worker.SubmitFrame(CreateSolidFrame(2, 1, 2000, 16, 16, 200, 200, 200));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    worker.Stop();

    // Verify gen 1 never delivered to mailbox
    AnalysisAlignmentStatus statusGen1;
    auto b1 = mailbox->GetAlignedAnalysis(1, 1, 1000, statusGen1);
    EXPECT_TRUE(b1 == nullptr);

    // Gen 2 delivered
    AnalysisAlignmentStatus statusGen2;
    auto b2 = mailbox->GetAlignedAnalysis(2, 1, 2000, statusGen2);
    EXPECT_TRUE(b2 != nullptr);
    EXPECT_TRUE(b2->sourceGeneration == 2);
}

// ------------------------------------------------------------------------------------------------
// Test 11: Backwards PTS and Sequence Rejection
// ------------------------------------------------------------------------------------------------
void Test11_BackwardsPtsAndSequenceRejection() {
    std::cout << "\n[TEST 11] Backwards PTS & Sequence Rejection" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Submit seq 100
    worker.SubmitFrame(CreateSolidFrame(1, 100, 10000, 16, 16, 50, 50, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(worker.SubmitFrame(CreateSolidFrame(1,101,9999,16,16,50,50,50)));
    // Submit seq 99 (backwards in sequence)
    worker.SubmitFrame(CreateSolidFrame(1, 99, 9000, 16, 16, 50, 50, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    worker.Stop();
    auto metrics = worker.GetMetrics();
    EXPECT_TRUE(metrics.totalDroppedOutOfOrder >= 1);
}

// ------------------------------------------------------------------------------------------------
// Test 12: Model reload mid-flight discards prior epoch completion
// ------------------------------------------------------------------------------------------------
void Test12_ModelReloadMidFlight() {
    std::cout << "\n[TEST 12] Model Reload Mid-Flight" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    FakeDelayAdapter fake;
    fake.artificialDelayMs = 50;
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Submit frame
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 10, 10, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Reload model while frame 1 is in flight
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    worker.Stop();

    // Frame 1 belonged to epoch 1, so it must not have been posted to mailbox
    AnalysisAlignmentStatus status;
    auto bundle = mailbox->GetAlignedAnalysis(1, 1, 1000, status);
    EXPECT_TRUE(bundle == nullptr);
}

// ------------------------------------------------------------------------------------------------
// Test 13: Worker lifecycle, stop during inference, and self-join safety
// ------------------------------------------------------------------------------------------------
void Test13_WorkerLifecycleAndSelfJoinSafety() {
    std::cout << "\n[TEST 13] Worker Lifecycle & Self-Join Safety" << std::endl;
    AnalysisWorker worker;
    worker.ResetSource(1);
    FakeDelayAdapter fake;
    fake.artificialDelayMs = 100;
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);

    // Start -> Stop -> Restart cycle
    EXPECT_TRUE(worker.Start());
    EXPECT_TRUE(worker.IsRunning());
    worker.Stop();
    EXPECT_FALSE(worker.IsRunning());
    EXPECT_TRUE(worker.Start());
    EXPECT_TRUE(worker.IsRunning());

    // Submit slow frame and immediately stop (stop during inference)
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 50, 50, 50));
    EXPECT_TRUE(WaitUntil([&] { return fake.callCount.load()==1; }));
    auto t0 = std::chrono::steady_clock::now();
    worker.Stop();
    auto stopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  Stop duration: " << stopDuration << " ms" << std::endl;
    EXPECT_TRUE(stopDuration < 150); // Clean prompt join

    // Test calling Stop() from within result callback (self-join safety)
    AnalysisWorker selfJoinWorker;
    selfJoinWorker.ResetSource(1);
    selfJoinWorker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    selfJoinWorker.SetResultCallback([&selfJoinWorker](std::shared_ptr<const AnalysisBundle>) {
        selfJoinWorker.Stop(); // Calling Stop() from worker thread!
    });
    selfJoinWorker.Start();
    selfJoinWorker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 50, 50, 50));
    EXPECT_TRUE(WaitUntil([&] { return !selfJoinWorker.IsRunning(); }));
    EXPECT_FALSE(selfJoinWorker.IsRunning());
}

// ------------------------------------------------------------------------------------------------
// Test 14: Retained bundle lifetime beyond worker destruction
// ------------------------------------------------------------------------------------------------
void Test14_RetainedBundleLifetimeBeyondWorkerDestruction() {
    std::cout << "\n[TEST 14] Retained Bundle Lifetime After Worker Destruction" << std::endl;
    std::shared_ptr<const AnalysisBundle> retainedBundle;

    {
        auto worker = std::make_unique<AnalysisWorker>();
        worker->ResetSource(1);
        auto manifest = TinyOnnxFixture::CreateManifest();
        worker->LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
        worker->SetResultCallback([&retainedBundle](std::shared_ptr<const AnalysisBundle> b) {
            retainedBundle = b;
        });
        worker->Start();
        worker->SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 100, 100, 100));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        worker->Stop();
        // worker destroyed here
    }

    // Verify retained bundle is fully intact and readable
    EXPECT_TRUE(retainedBundle != nullptr);
    EXPECT_TRUE(retainedBundle->HasPersonMask());
    EXPECT_TRUE(retainedBundle->personMask.width == 16);
    EXPECT_TRUE(retainedBundle->personMask.height == 16);
    float sample = retainedBundle->personMask.data[0];
    EXPECT_TRUE(std::isfinite(sample));
}

// ------------------------------------------------------------------------------------------------
// Test 15: Fault injection and clean recovery on next frame
// ------------------------------------------------------------------------------------------------
void Test15_FaultRecoveryNextFrame() {
    std::cout << "\n[TEST 15] Fault Injection & Next Frame Recovery" << std::endl;
    auto mailbox = std::make_shared<AnalysisMailbox>();
    AnalysisWorker worker(mailbox);
    worker.ResetSource(1);

    FakeDelayAdapter fake;
    fake.failuresToInject = 1; // Inject exactly 1 inference error
    worker.SetMockInferenceFunction([&fake](const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel) {
        return fake.Execute(in, out, cancel);
    });
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Frame 1 fails
    worker.SubmitFrame(CreateSolidFrame(1, 1, 1000, 16, 16, 10, 10, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    EXPECT_TRUE(worker.GetMetrics().totalInferenceErrors == 1);

    // Frame 2 succeeds
    worker.SubmitFrame(CreateSolidFrame(1, 2, 2000, 16, 16, 20, 20, 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    worker.Stop();
    EXPECT_TRUE(worker.GetMetrics().totalCompleted == 1);

    AnalysisAlignmentStatus status;
    auto bundle = mailbox->GetAlignedAnalysis(1, 2, 2000, status);
    EXPECT_TRUE(bundle != nullptr);
    EXPECT_TRUE(bundle->sequenceId == 2);
}

// ------------------------------------------------------------------------------------------------
// Test 16: Bounded memory and plateau under changing geometry
// ------------------------------------------------------------------------------------------------
void Test16_MemoryPlateauUnderChangingGeometry() {
    std::cout << "\n[TEST 16] Queue Accounting Under Changing Geometry (resource capacities tested separately)" << std::endl;
    AnalysisWorker worker;
    worker.ResetSource(1);
    auto manifest = TinyOnnxFixture::CreateManifest();
    worker.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize, manifest);
    worker.Start();

    // Rapidly submit frames with alternating dimensions
    const int resolutions[][2] = {
        { 16, 16 }, { 64, 48 }, { 32, 64 }, { 128, 72 }, { 15, 7 }
    };

    for (uint64_t i = 1; i <= 60; ++i) {
        int w = resolutions[i % 5][0];
        int h = resolutions[i % 5][1];
        worker.SubmitFrame(CreateSolidFrame(1, i, i * 1000, w, h, 128, 128, 128));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    worker.Stop();

    auto m = worker.GetMetrics();
    std::cout << "  Submitted 60 frames under changing resolutions: Completed=" << m.totalCompleted
              << ", ReplacedPending=" << m.totalReplacedPending << std::endl;
    EXPECT_TRUE(m.totalCompleted + m.totalReplacedPending == 60);
}

int main(int argc, char** argv) {
    std::cout << "==========================================================" << std::endl;
    std::cout << "PrismForge: Analysis Provider Runtime & Recovery Test Suite" << std::endl;
    std::cout << "==========================================================" << std::endl;

    Test01_RealOrtCpuInference();
    Test02_RealOrtDirectMLInferenceAndFallback();
    Test03_ErrorHandlingMissingAndCorruptModel();
    Test04_TensorShapeMismatch();
    Test05_PreprocessorOddDimensionsAndPoisonPadding();
    Test06_InverseMaskMapping();
    Test07_TruthfulCapabilitiesAndResultAdapter();
    Test08_BoundedWorkerSingleInFlightAndLatestReplacement();
    Test09_ImmediateReleaseOfReplacedPendingFrame();
    Test10_SourceResetDropsPendingAndRejectsStale();
    Test11_BackwardsPtsAndSequenceRejection();
    Test12_ModelReloadMidFlight();
    Test13_WorkerLifecycleAndSelfJoinSafety();
    Test14_RetainedBundleLifetimeBeyondWorkerDestruction();
    Test15_FaultRecoveryNextFrame();
    Test16_MemoryPlateauUnderChangingGeometry();

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "RESULTS: " << g_passCount << " passed, " << g_failCount << " failed." << std::endl;
    std::cout << "==========================================================" << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}
