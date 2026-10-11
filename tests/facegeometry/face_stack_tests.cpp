#include "windows/src/effects/studiostack/studio_stack.h"
#include "windows/src/effects/gpu/d3d11_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using namespace PrismForge::Effects;
using namespace PrismForge::Effects::AnalysisBridge;
using namespace PrismForge::Effects::Lighting;
using namespace PrismForge::Effects::Presets;
using Stack = PrismForge::Effects::StudioStack::StudioStack;
using namespace PrismForge::Effects::StudioStack;

namespace {
constexpr int W = 80, H = 48;
constexpr uint64_t Generation = 7;
constexpr int64_t Pts = 10000;
int g_checks = 0;
void Check(bool condition, const std::string& reason) {
    ++g_checks;
    if (!condition) throw std::runtime_error(reason);
}
struct FrameDelete { void operator()(AVFrame* f) const { av_frame_free(&f); } };
using OwnedFrame = std::unique_ptr<AVFrame, FrameDelete>;
OwnedFrame Frame() {
    OwnedFrame f(av_frame_alloc());
    Check(!!f, "frame allocation");
    f->width = W; f->height = H; f->format = AV_PIX_FMT_BGR24;
    f->pts = Pts; f->time_base = AVRational{1, 1000000};
    f->color_range = AVCOL_RANGE_JPEG; f->color_trc = AVCOL_TRC_IEC61966_2_1;
    Check(av_frame_get_buffer(f.get(), 32) == 0, "pixel allocation");
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        auto* p = f->data[0] + y * f->linesize[0] + x * 3;
        p[0] = static_cast<uint8_t>(70 + x / 4); p[1] = static_cast<uint8_t>(90 + y / 4); p[2] = 125;
    }
    return f;
}
std::vector<uint8_t> Pixels(const AVFrame* frame) {
    Check(frame && frame->format == AV_PIX_FMT_BGR24 && frame->width == W && frame->height == H,
        "valid returned frame");
    std::vector<uint8_t> data(W * H * 3);
    for (int y = 0; y < H; ++y) std::memcpy(data.data() + y * W * 3,
        frame->data[0] + y * frame->linesize[0], W * 3);
    return data;
}
const uint8_t* Pixel(const AVFrame* f, int x, int y) { return f->data[0] + y * f->linesize[0] + x * 3; }
StudioPreset Preset(bool background = false, bool shadows = false) {
    auto p = StudioPreset::Neutral(); p.bypassAll = false;
    p.lighting = LightingConfig::Neutral(); p.lighting.bypass = false;
    p.lighting.subjectOnlyMode = false; // The explicit face input must still restrict coverage.
    p.lighting.lightingMode = LightingMode::NormalGuided;
    p.lighting.ambientIntensity = 0.18f; p.lighting.fillIntensity = 0.0f;
    p.lighting.light1.enabled = true; p.lighting.light1.posX = -0.62f;
    p.lighting.light1.posY = -0.1f; p.lighting.light1.posZ = 0.5f;
    p.lighting.light1.intensity = 0.7f;
    p.lighting.shadowConfig.enabled = shadows;
    p.lighting.shadowConfig.shadowIntensity = 0.75f;
    if (background) {
        p.backgroundMode = StudioBackgroundMode::Solid;
        p.background.mode = BackgroundMode::Solid;
        p.background.solidB = 12; p.background.solidG = 24; p.background.solidR = 36;
    }
    return p;
}
std::shared_ptr<AnalysisBundle> Geometry(uint64_t sequence = 1, int64_t pts = Pts,
    bool allCovered = false, int width = W, int height = H) {
    auto b = std::make_shared<AnalysisBundle>();
    b->sourceGeneration = Generation; b->sequenceId = sequence; b->timestampPts = pts;
    auto& m = b->personMask; m.width = width; m.height = height; m.strideFloats = width;
    m.data.assign(static_cast<size_t>(width) * height, 0.0f);
    auto& n = b->normalMap; n.width = width; n.height = height; n.strideFloats = width * 3;
    n.data.assign(static_cast<size_t>(width) * height * 3, 0.0f);
    auto& d = b->depthMap; d.width = width; d.height = height; d.strideFloats = width;
    d.sourceGeneration = Generation; d.sequenceId = sequence; d.timestampPts = pts;
    d.data.assign(static_cast<size_t>(width) * height, -1.0f);
    d.confidence.assign(static_cast<size_t>(width) * height, 0.0f);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        bool supported = allCovered || (x >= width / 3 && x < width * 2 / 3 &&
            y >= height / 4 && y < height * 3 / 4);
        if (!supported) continue;
        const size_t at = static_cast<size_t>(y) * width + x;
        m.data[at] = 1.0f;
        n.data[at * 3] = -0.6f; n.data[at * 3 + 1] = 0.0f; n.data[at * 3 + 2] = 0.8f;
        d.data[at] = (x >= width / 2 - 2 && x < width / 2 + 2) ? 0.38f : 0.5f;
        d.confidence[at] = 1.0f;
    }
    return b;
}
std::shared_ptr<AnalysisBundle> Person(uint64_t sequence = 1, int64_t pts = Pts) {
    auto b = Geometry(sequence, pts, true);
    // Deliberately different mailbox normals: a stale face override must not borrow these.
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const size_t at = static_cast<size_t>(y) * W + x;
        b->personMask.data[at] = x >= 10 && x < W - 10 ? 1.0f : 0.0f;
        b->normalMap.data[at * 3] = 0.0f;
        b->normalMap.data[at * 3 + 2] = 1.0f;
    }
    return b;
}
std::shared_ptr<AnalysisMailbox> AttachPerson(Stack& s, uint64_t seq = 1, int64_t pts = Pts) {
    auto mailbox = std::make_shared<AnalysisMailbox>();
    s.AttachAnalysisMailbox(mailbox);
    Check(mailbox->PostAnalysis(Person(seq, pts)), "post distinct mailbox body mask");
    return mailbox;
}
void Configure(Stack& s, const StudioPreset& p) { s.SetSourceGeneration(Generation); s.SetPreset(p); }
int ByteOracle(int byte, double factor) {
    double c = byte / 255.0;
    double linear = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    linear = std::clamp(linear * factor, 0.0, 1.0);
    double result = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return std::clamp(static_cast<int>(result * 255.0 + 0.5), 0, 255);
}
void CheckConstantNormalOracle(const AVFrame* output, const AVFrame* input, const StudioPreset& p) {
    // Independent closed-form Lambert + sRGB oracle; not an invocation of a stack processor.
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const double u = 2.0 * (x + 0.5) / W - 1.0, v = 2.0 * (y + 0.5) / H - 1.0;
        const double dx = (p.lighting.light1.posX - u) * W / H;
        const double dy = p.lighting.light1.posY - v, dz = p.lighting.light1.posZ;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double lambert = std::max(0.0, (-0.6 * dx + 0.8 * dz) / distance);
        const double factor = p.lighting.ambientIntensity + p.lighting.light1.intensity * lambert /
            (1.0 + 0.4 * std::max(0.0, distance - dz));
        for (int channel = 0; channel < 3; ++channel) Check(
            std::abs(static_cast<int>(Pixel(output, x, y)[channel]) -
                ByteOracle(Pixel(input, x, y)[channel], factor)) <= 1,
            "closed-form face Lambert oracle");
    }
}
void CurrentAndCoverageCpu() {
    auto input = Frame(); const auto original = Pixels(input.get());
    auto p = Preset(); Stack s; Configure(s, p); AttachPerson(s);
    StackExecutionReport report;
    auto* out = s.ProcessFrame(input.get(), 1, Pts, &report, Generation, Geometry());
    Check(out && report.lightingStatus == StageStatus::Applied, "current face applied");
    Check(report.normalPreparationCount == 1 && report.maskPreparationCount == 1,
        "one current face normal and coverage preparation");
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        if (x >= W / 3 && x < W * 2 / 3 && y >= H / 4 && y < H * 3 / 4) continue;
        Check(std::memcmp(Pixel(out, x, y), Pixel(input.get(), x, y), 3) == 0,
            "every uncovered pixel preserved exactly, including body and background");
    }
    Check(Pixels(input.get()) == original, "caller pixels untouched");
    Check(std::memcmp(Pixel(out, 38, 24), Pixel(input.get(), 38, 24), 3) != 0,
        "actual face pixel changed");
    Check(out->pts == input->pts && out->time_base.num == 1 && out->time_base.den == 1000000,
        "frame metadata preserved");

    // A fully supported low-resolution raster must not flatten physical normal directions.
    Stack lowResolutionStack; Configure(lowResolutionStack, p);
    auto* smallOut = lowResolutionStack.ProcessFrame(input.get(), 1, Pts, &report, Generation,
        Geometry(1, Pts, true, 13, 9));
    Check(report.lightingStatus == StageStatus::Applied, "low-resolution current face applied");
    CheckConstantNormalOracle(smallOut, input.get(), p);
}
void StaleAndUnsupportedCpu() {
    auto input = Frame(); const auto original = Pixels(input.get());
    for (int mismatch = 0; mismatch < 14; ++mismatch) {
        auto face = Geometry(); auto p = Preset(false, mismatch >= 7);
        switch (mismatch) {
        case 0: --face->sourceGeneration; break;
        case 1: ++face->sequenceId; break;
        case 2: --face->timestampPts; break;
        case 3: face->personMask = {}; break;
        case 4: std::fill(face->personMask.data.begin(), face->personMask.data.end(), 0.0f); break;
        case 5: face->normalMap.data[(H / 2 * W + W / 2) * 3] = std::numeric_limits<float>::quiet_NaN(); break;
        case 6: face->normalMap.width--; break;
        case 7: face->depthMap = {}; break;
        case 8: --face->depthMap.sequenceId; break;
        case 9: --face->depthMap.timestampPts; break;
        case 10: face->depthMap.format = 1; break;
        case 11: face->depthMap.confidence[H / 2 * W + W / 2] = 0.1f; break;
        case 12: face->normalMap = {}; break;
        case 13: --face->depthMap.sourceGeneration; break;
        }
        Stack s; Configure(s, p); AttachPerson(s);
        StackExecutionReport report;
        const auto* out = s.ProcessFrame(input.get(), 1, Pts, &report, Generation, face);
        Check(out && report.lightingStatus == StageStatus::NotReady,
            "explicit stale/missing/malformed face is NotReady case " + std::to_string(mismatch));
        Check(report.normalPreparationCount == 0 && report.maskPreparationCount == 0,
            "invalid face cannot prepare mailbox normals/coverage");
        Check(Pixels(out) == original, "invalid face preserves every current source pixel");
    }
    // Default caller retains existing mailbox behavior, confirming the API is additive.
    Stack legacy; Configure(legacy, Preset()); AttachPerson(legacy);
    StackExecutionReport report;
    Check(legacy.ProcessFrame(input.get(), 1, Pts, &report, Generation) &&
        report.lightingStatus == StageStatus::Applied, "ordinary caller retains mailbox geometry route");
}
void IndependentBackgroundCpu() {
    auto input = Frame(); Stack s; Configure(s, Preset(true)); auto mailbox = AttachPerson(s);
    StackExecutionReport report;
    auto* out = s.ProcessFrame(input.get(), 1, Pts, &report, Generation, Geometry());
    Check(report.lightingStatus == StageStatus::Applied && report.backgroundStatus == StageStatus::Applied,
        "face lighting and body background applied together");
    Check(report.maskPreparationCount == 2 && report.maskReuseCount == 0,
        "face and full-person masks have separate preparations");
    Check(std::memcmp(Pixel(out, 18, 24), Pixel(input.get(), 18, 24), 3) == 0,
        "shoulder remains original rather than replaced by background");
    Check(Pixel(out, 1, 24)[0] == 12 && Pixel(out, 1, 24)[1] == 24 && Pixel(out, 1, 24)[2] == 36,
        "pixels beyond person receive background color");
    Check(mailbox->PostAnalysis(Person(2, Pts + 10000)), "new full-person analysis posted");
    auto missingFace = Geometry(2, Pts + 10000); missingFace->personMask = {};
    out = s.ProcessFrame(input.get(), 2, Pts + 10000, &report, Generation, missingFace);
    Check(report.lightingStatus == StageStatus::NotReady && report.backgroundStatus == StageStatus::Applied,
        "face disappears while background continues");
    Check(std::memcmp(Pixel(out, 38, 24), Pixel(input.get(), 38, 24), 3) == 0,
        "previous frame face cannot survive missing face support");
}
void ComparePixels(const AVFrame* a, const std::vector<uint8_t>& b, int tolerance, const std::string& name) {
    const auto pixels = Pixels(a); Check(pixels.size() == b.size(), name + " pixel dimensions");
    int maxError = 0;
    for (size_t i = 0; i < pixels.size(); ++i) maxError = std::max(maxError,
        std::abs(static_cast<int>(pixels[i]) - b[i]));
    std::cout << "  " << name << " max_byte_error=" << maxError << std::endl;
    Check(maxError <= tolerance, name + " pixel parity");
}
void GpuRoute(bool hardware) {
    PrismForge::Effects::Gpu::D3D11Context d3d;
    Check(d3d.Initialize(hardware), "D3D11 device initialized");
    Check(d3d.IsHardwareAdapter() == hardware, "requested actual hardware/WARP adapter");
    std::cout << "  adapter=" << d3d.GetAdapterName() << std::endl;
    auto input = Frame(); const auto untouched = Pixels(input.get());
    std::vector<uint8_t> noShadowPixels;
    for (bool shadows : {false, true}) {
        auto preset = Preset(true, shadows);
        Stack cpu; Configure(cpu, preset); AttachPerson(cpu);
        StackExecutionReport cpuReport;
        auto* expected = cpu.ProcessFrame(input.get(), 1, Pts, &cpuReport, Generation, Geometry());
        Check(cpuReport.lightingStatus == StageStatus::Applied && cpuReport.backgroundStatus == StageStatus::Applied,
            "CPU parity oracle actual stages");
        const auto expectedPixels = Pixels(expected);
        if (!shadows) noShadowPixels = expectedPixels;
        else {
            int darkerBytes = 0;
            for (size_t i = 0; i < expectedPixels.size(); ++i)
                if (expectedPixels[i] + 2 < noShadowPixels[i]) ++darkerBytes;
            Check(darkerBytes > 0, "supported face depth produces actual non-neutral shadow pixels");
            std::cout << "  actual_shadow_darker_bytes=" << darkerBytes << std::endl;
        }
        Stack gpu; Configure(gpu, preset); AttachPerson(gpu);
        gpu.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
        Check(gpu.InitializeGpu(d3d.GetDevice(), d3d.GetContext()), "stack GPU initialized");
        StackExecutionReport report;
        const auto* output = gpu.ProcessFrame(input.get(), 1, Pts, &report, Generation, Geometry());
        Check(output && !report.gpuFallbackUsed && report.lightingStatus == StageStatus::Applied &&
            report.backgroundStatus == StageStatus::Applied, "real GPU stages applied without fallback");
        Check(report.imageUploadCount == 1 && report.imageReadbackCount == 1 && report.normalUploadCount == 1 &&
            report.maskUploadCount == 2, "actual GPU frame, normal, independent face/body mask transfers");
        ComparePixels(output, expectedPixels, 2, shadows ? "GPU shadow face/body" : "GPU face/body");
        Check(std::memcmp(Pixel(output, 18, 24), Pixel(input.get(), 18, 24), 3) == 0,
            "GPU body preserved outside face coverage");
        Check(output->pts == input->pts && output->time_base.den == 1000000, "GPU source metadata preserved");

        // A GPU readback failure must restart from the current ORIGINAL CPU frame,
        // not apply the effects a second time to partially processed GPU pixels.
        gpu.SetGpuFailureTarget(GpuFailureTarget::Readback);
        output = gpu.ProcessFrame(input.get(), 1, Pts, &report, Generation, Geometry());
        Check(output && report.gpuFallbackUsed, "readback failure uses CPU fallback");
        ComparePixels(output, expectedPixels, 0, "same-frame CPU restart after readback fault");
        Check(Pixels(input.get()) == untouched, "readback fallback keeps caller original pixels");
    }
    // Independent analytic oracle also exercises coarse raster GPU normal direction.
    Stack full; Configure(full, Preset());
    full.SetExecutionBackend(ExecutionBackend::GpuWithCpuFallback);
    Check(full.InitializeGpu(d3d.GetDevice(), d3d.GetContext()), "coarse raster GPU stack initialized");
    StackExecutionReport report;
    auto* output = full.ProcessFrame(input.get(), 1, Pts, &report, Generation, Geometry(1, Pts, true, 13, 9));
    Check(!report.gpuFallbackUsed && report.lightingStatus == StageStatus::Applied, "coarse raster actual GPU lighting");
    CheckConstantNormalOracle(output, input.get(), Preset());
    AttachPerson(full);
    for (int mismatch = 0; mismatch < 7; ++mismatch) {
        auto stale = Geometry();
        if (mismatch == 0) --stale->sourceGeneration;
        else if (mismatch == 1) ++stale->sequenceId;
        else if (mismatch == 2) ++stale->timestampPts;
        else if (mismatch == 3) std::fill(stale->personMask.data.begin(), stale->personMask.data.end(), 0.0f);
        else if (mismatch == 4) stale->personMask = {};
        else if (mismatch == 5) stale->normalMap = {};
        else { full.SetPreset(Preset(false, true)); --stale->depthMap.sequenceId; }
        output = full.ProcessFrame(input.get(), 1, Pts, &report, Generation, stale);
        Check(!report.gpuFallbackUsed && report.lightingStatus == StageStatus::NotReady,
            "GPU rejects stale or unsupported face input without CPU fallback");
        Check(Pixels(output) == untouched, "GPU stale face preserves current original pixels");
    }
}
} // namespace
int main() {
    try {
        CurrentAndCoverageCpu(); std::cout << "PASS current face / exact uncovered pixels / analytic coarse normal oracle" << std::endl;
        StaleAndUnsupportedCpu(); std::cout << "PASS stale or unsupported geometry / additive legacy route" << std::endl;
        IndependentBackgroundCpu(); std::cout << "PASS independent body mask / face loss / no face history" << std::endl;
        GpuRoute(true); std::cout << "PASS hardware GPU pixels / depth / exact tags / same-frame fallback" << std::endl;
        GpuRoute(false); std::cout << "PASS WARP GPU pixels / depth / exact tags / same-frame fallback" << std::endl;
        std::cout << "FACE_STACK_PASS checks=" << g_checks << std::endl; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FACE_STACK_FAIL checks=" << g_checks << " error=" << error.what() << std::endl; return 1;
    }
}
