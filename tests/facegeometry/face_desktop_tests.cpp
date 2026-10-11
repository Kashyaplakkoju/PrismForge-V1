#include "effects/desktopstudiopipeline.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using Microsoft::WRL::ComPtr;
using namespace PrismForge::Effects;
namespace P = PrismForge::Providers;
namespace S = PrismForge::Effects::StudioStack;
namespace {
int assertions = 0, groups = 0;
void Require(bool condition, const std::string& description) {
    ++assertions;
    if (!condition) throw std::runtime_error(description);
}
struct FrameOwner {
    AVFrame* frame = nullptr;
    explicit FrameOwner(AVFrame* value = nullptr) : frame(value) {}
    ~FrameOwner() { av_frame_free(&frame); }
    FrameOwner(const FrameOwner&) = delete;
    FrameOwner& operator=(const FrameOwner&) = delete;
};
std::vector<uint8_t> Pixels(const AVFrame* frame) {
    Require(frame && frame->format == AV_PIX_FMT_BGR24 && frame->width > 0 && frame->height > 0,
        "test output is a valid BGR24 frame");
    std::vector<uint8_t> bytes(size_t(frame->width) * size_t(frame->height) * 3);
    for (int y = 0; y < frame->height; ++y)
        std::memcpy(bytes.data() + size_t(y) * size_t(frame->width) * 3,
            frame->data[0] + size_t(y) * size_t(frame->linesize[0]), size_t(frame->width) * 3);
    return bytes;
}
AVFrame* DecodeFixture(const wchar_t* filename) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmapClipper> clip;
    auto checked = [](HRESULT result) {
        if (FAILED(result)) throw std::runtime_error("WIC could not decode the supplied face fixture");
    };
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    checked(factory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
    checked(decoder->GetFrame(0, &decoded));
    UINT width = 0, height = 0;
    checked(decoded->GetSize(&width, &height));
    Require(width && height && width <= 8192 && height <= 8192 && uint64_t(width) * height * 3 <= 32ULL * 1024 * 1024,
        "user fixture dimensions are bounded before pixel allocation");
    ComPtr<IWICBitmapSource> source = decoded;
    // The supplied screenshot contains PrismForge controls. Analyse its camera
    // preview only, in memory; the source image is never changed or saved.
    if ((width == 1376 || width == 1377) && (height == 915 || height == 917)) {
        checked(factory->CreateBitmapClipper(&clip));
        WICRect region{126, 287, 792, 443};
        checked(clip->Initialize(decoded.Get(), &region));
        source = clip; width = 792; height = 443;
    }
    checked(factory->CreateFormatConverter(&converter));
    checked(converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom));
    FrameOwner owned(av_frame_alloc());
    Require(owned.frame != nullptr, "allocate an owned fixture frame");
    auto* frame = owned.frame;
    frame->width = int(width); frame->height = int(height); frame->format = AV_PIX_FMT_BGR24;
    Require(av_frame_get_buffer(frame, 64) >= 0, "allocate bounded owned fixture pixels");
    checked(converter->CopyPixels(nullptr, UINT(frame->linesize[0]), UINT(size_t(frame->linesize[0]) * height), frame->data[0]));
    frame->pts = 100000; frame->time_base = {1, 1000000}; frame->duration = 16667;
    frame->color_range = AVCOL_RANGE_JPEG; frame->colorspace = AVCOL_SPC_RGB;
    frame->color_primaries = AVCOL_PRI_BT709; frame->color_trc = AVCOL_TRC_BT709;
    owned.frame = nullptr;
    return frame;
}
AVFrame* ClonePixels(const AVFrame* source, uint8_t fill = 0, bool blank = false) {
    FrameOwner owned(av_frame_alloc());
    Require(owned.frame != nullptr, "allocate independent current camera frame");
    auto* frame = owned.frame;
    frame->width = source->width; frame->height = source->height; frame->format = AV_PIX_FMT_BGR24;
    Require(av_frame_get_buffer(frame, 64) >= 0 && av_frame_copy_props(frame, source) >= 0,
        "independent frame owns pixels and source metadata");
    for (int y = 0; y < frame->height; ++y) {
        auto* destination = frame->data[0] + size_t(y) * size_t(frame->linesize[0]);
        if (blank) std::memset(destination, fill, size_t(frame->width) * 3);
        else std::memcpy(destination, source->data[0] + size_t(y) * size_t(source->linesize[0]), size_t(frame->width) * 3);
    }
    owned.frame = nullptr;
    return frame;
}
DesktopStudioSettings FaceSettings() {
    DesktopStudioSettings settings;
    settings.light = DesktopLightMode::Custom; settings.strength = 1;
    settings.faceLighting = true; settings.lightingSubjectOnly = true;
    settings.keyLight.enabled = true; settings.keyLight.posX = .9f; settings.keyLight.posY = 0;
    settings.keyLight.intensity = 1; settings.keyLight.colorTemperature = 6500;
    settings.fillLight.enabled = false; settings.fillLight.intensity = 0;
    settings.background = DesktopBackgroundMode::Off; settings.studioFrame = false;
    return settings;
}
void VerifyExactFrame(DesktopStudioPipeline& pipeline, const AVFrame* source, uint64_t generation,
    const AVFrame* output, bool hardware, bool fallback = false) {
    const auto report = pipeline.GetLastReport();
    const auto state = pipeline.GetFaceAnalysisState();
    Require(report.lightingStatus == S::StageStatus::Applied && report.effectiveLightingMode == Lighting::LightingMode::NormalGuided,
        "real facial lighting applied in normal-guided mode");
    Require(report.normalPreparationCount > 0 && report.maskPreparationCount > 0,
        "actual normals and geometric face coverage were prepared");
    Require(report.sourceGeneration == generation && report.frameSequence > 0 && report.timestampPts == source->pts,
        "report carries current camera generation, sequence and original microPTS");
    Require(state.phase == P::FaceAnalysisPhase::Ready && state.enabled && state.ready && state.lastDetected,
        "actual native model detected the supplied face");
    Require(state.sourceGeneration == generation && state.lastCompletedSequence == report.frameSequence && state.lastCompletedPts == source->pts,
        "the native face completion belongs to exactly the processed frame");
    Require(state.completedFrames > 0 && state.inferenceErrors == 0, "real face inference completed without an error");
    Require(output && output != source && output->width == source->width && output->height == source->height &&
        output->pts == source->pts && output->time_base.num == 1 && output->time_base.den == 1000000,
        "processed output retains current source geometry and original timestamp metadata");
    Require(report.backgroundStatus == S::StageStatus::Bypassed && pipeline.GetSegmentationState().phase == P::SegmentationPhase::Disabled,
        "face-only lighting does not activate body segmentation or background composition");
    Require(!report.fallbackToOriginal && report.gpuFallbackUsed == fallback, "truthful current facial-lighting fallback outcome");
    if (hardware) {
        Require(report.imageUploadCount == 1 && report.normalUploadCount > 0,
            "the actual hardware path uploaded camera pixels and normals");
        if (!fallback) Require(report.imageReadbackCount == 1, "completed GPU result was actually read back");
    } else Require(report.imageUploadCount == 0 && report.imageReadbackCount == 0 && report.normalUploadCount == 0,
        "CPU facial-lighting path performs no GPU transfers");
}
AVFrame* WaitApplied(DesktopStudioPipeline& pipeline, AVFrame* frame, uint64_t generation, bool hardware, bool fallback = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        frame->pts += 16667;
        auto* output = pipeline.Process(frame, generation);
        const auto state = pipeline.GetFaceAnalysisState();
        if (state.phase == P::FaceAnalysisPhase::Failed) throw std::runtime_error("Real face service failed: " + state.message + "; " + state.lastError);
        if (pipeline.GetLastReport().lightingStatus == S::StageStatus::Applied) {
            VerifyExactFrame(pipeline, frame, generation, output, hardware, fallback);
            return output;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto state = pipeline.GetFaceAnalysisState();
    throw std::runtime_error("Real face lighting did not apply in three seconds; " + state.message + "; " + state.lastError);
}
size_t VerifyLocalized(const AVFrame* output, const AVFrame* source) {
    const auto expected = Pixels(source), actual = Pixels(output);
    Require(actual.size() == expected.size(), "facial lighting preserves all camera pixel dimensions");
    size_t changed = 0;
    for (size_t i = 0; i < actual.size(); i += 3)
        if (actual[i] != expected[i] || actual[i + 1] != expected[i + 1] || actual[i + 2] != expected[i + 2]) ++changed;
    Require(changed > 100 && changed < size_t(source->width) * size_t(source->height) / 3,
        "actual shading changes a localized face rather than tinting the full frame");
    const int cornerWidth = std::min(32, source->width), cornerHeight = std::min(32, source->height);
    for (int y = 0; y < cornerHeight; ++y)
        Require(std::memcmp(output->data[0] + size_t(y) * size_t(output->linesize[0]),
            source->data[0] + size_t(y) * size_t(source->linesize[0]), size_t(cornerWidth) * 3) == 0,
            "the known room corner outside the face remains byte-exact");
    std::cout << "LOCALIZED_FACE_CHANGED_PIXELS " << changed << " TOTAL " << size_t(source->width) * size_t(source->height) << '\n';
    return changed;
}
void CompareIndependentCpu(const AVFrame* cpu, const AVFrame* gpu, const AVFrame* input) {
    const auto cpuBytes = Pixels(cpu), gpuBytes = Pixels(gpu), inputBytes = Pixels(input);
    Require(cpuBytes.size() == gpuBytes.size() && cpuBytes.size() == inputBytes.size(), "independent model/renderer outputs share current dimensions");
    double total = 0, changedTotal = 0; size_t changedChannels = 0; int maximum = 0;
    std::vector<int> changedDifferences;
    for (size_t i = 0; i < cpuBytes.size(); ++i) {
        const int difference = std::abs(int(cpuBytes[i]) - int(gpuBytes[i]));
        total += difference; maximum = std::max(maximum, difference);
        if (cpuBytes[i] != inputBytes[i] || gpuBytes[i] != inputBytes[i]) {
            changedTotal += difference; ++changedChannels; changedDifferences.push_back(difference);
        }
    }
    Require(changedChannels > 100, "independent CPU counterpart actually shades a face");
    std::sort(changedDifferences.begin(), changedDifferences.end());
    const double mean = total / double(cpuBytes.size());
    const double changedMean = changedTotal / double(changedChannels);
    const int changedP95 = changedDifferences[(changedDifferences.size() - 1) * 95 / 100];
    std::cout << "INDEPENDENT_NATIVE_MESH_CPU_COMPARISON global_mean=" << mean << " changed_mean=" << changedMean
        << " changed_p95=" << changedP95 << " max=" << maximum << " channels=" << changedChannels
        << " (independent tracking; not an identical-geometry shader oracle)\n";
    // The two real native VIDEO trackers may converge at different sequences.
    // Bound actual pixel agreement over changed pixels, without pretending the
    // independent meshes are the exact same synthetic oracle input.
    Require(mean <= .5 && changedMean <= 4 && changedP95 <= 16,
        "actual independent CPU counterpart agrees within measured tracking/edge tolerance");
}
void NoFaceAfterSourceChange(DesktopStudioPipeline& pipeline, AVFrame* blank, uint64_t generation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool completed = false;
    do {
        blank->pts += 16667;
        auto* output = pipeline.Process(blank, generation);
        auto report = pipeline.GetLastReport(); auto state = pipeline.GetFaceAnalysisState();
        Require(output == blank && Pixels(output) == Pixels(blank), "blank current source keeps its exact input, without retained face output");
        Require(report.sourceGeneration == generation && report.timestampPts == blank->pts && report.lightingStatus == S::StageStatus::NotReady,
            "no-face or pending source is current and reports facial lighting not ready");
        Require(report.normalPreparationCount == 0 && report.normalUploadCount == 0, "a blank source never uses a retained normal map");
        Require(pipeline.GetSegmentationState().phase == P::SegmentationPhase::Disabled, "no-face fallback never starts body segmentation");
        if (state.phase == P::FaceAnalysisPhase::Failed) throw std::runtime_error(state.message);
        if (state.phase == P::FaceAnalysisPhase::Ready && state.lastCompletedSequence == report.frameSequence && state.lastCompletedPts == blank->pts) {
            Require(state.sourceGeneration == generation && !state.lastDetected && state.inferenceErrors == 0,
                "actual native blank-frame inference completed for the current source with no face");
            completed = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    Require(completed, "the real blank-source no-face completion arrived in three seconds");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: face_desktop_tests fixture.png\n"; return 2; }
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComOwner { bool owned; ~ComOwner() { if (owned) CoUninitialize(); } } com{SUCCEEDED(result)};
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) { std::cerr << "COM initialization failed\n"; return 2; }
    std::cout << std::fixed << std::setprecision(6);
    try {
        FrameOwner fixture(DecodeFixture(argv[1]));
        DesktopStudioPipeline cpu(false), gpu(true);
        const auto settings = FaceSettings(); cpu.ConfigureStudio(settings); gpu.ConfigureStudio(settings);
        FrameOwner cpuFace, gpuFace;
        std::vector<uint8_t> retainedPixels;
#define GROUP(name, code) ++groups; std::cout << "GROUP " name << std::endl; { code; } std::cout << "PASS " name << std::endl;
        GROUP("actual CPU native face geometry drives localized current lighting", {
            auto* output = WaitApplied(cpu, fixture.frame, 7, false);
            VerifyLocalized(output, fixture.frame);
            cpuFace.frame = av_frame_clone(output); Require(cpuFace.frame != nullptr, "retain actual CPU facial-lighting output");
        });
        GROUP("actual hardware native face geometry drives localized current lighting", {
            auto* output = WaitApplied(gpu, fixture.frame, 7, true);
            VerifyLocalized(output, fixture.frame);
            gpuFace.frame = av_frame_clone(output); Require(gpuFace.frame != nullptr, "retain actual hardware facial-lighting output");
            retainedPixels = Pixels(gpuFace.frame);
            CompareIndependentCpu(cpuFace.frame, gpuFace.frame, fixture.frame);
        });
        GROUP("actual GPU readback failure uses current CPU face fallback and recovers", {
            FrameOwner current(ClonePixels(fixture.frame)); current.frame->pts = 800000;
            for (int y = 0; y < current.frame->height; ++y) {
                auto* row = current.frame->data[0] + size_t(y) * size_t(current.frame->linesize[0]);
                for (int x = 0; x < current.frame->width * 3; ++x) row[x] = uint8_t(std::min(255, int(row[x]) + 4));
            }
            auto* cpuOutput = WaitApplied(cpu, current.frame, 8, false);
            FrameOwner expected(av_frame_clone(cpuOutput)); Require(expected.frame != nullptr, "retain independently processed current CPU frame");
            gpu.InjectGpuReadbackFailureForTesting(true);
            auto* output = WaitApplied(gpu, current.frame, 8, true, true);
            VerifyLocalized(output, current.frame); CompareIndependentCpu(expected.frame, output, current.frame);
            Require(Pixels(output) != retainedPixels, "failure never replays the older unmodified facial-lighting frame");
            gpu.InjectGpuReadbackFailureForTesting(false);
            output = WaitApplied(gpu, current.frame, 8, true); VerifyLocalized(output, current.frame);
        });
        GROUP("actual no-face inference rejects old-source face geometry", {
            FrameOwner blank(ClonePixels(fixture.frame, 48, true)); blank.frame->pts = 900000;
            NoFaceAfterSourceChange(cpu, blank.frame, 9); NoFaceAfterSourceChange(gpu, blank.frame, 9);
        });
        GROUP("master bypass and lighting off stop native intake and preserve settings", {
            for (auto* pipeline : {&cpu, &gpu}) {
                DesktopColorSettings bypass; bypass.bypassed = true; pipeline->Configure(bypass);
                fixture.frame->pts = 1000000;
                Require(pipeline->Process(fixture.frame, 10) == fixture.frame, "master bypass returns the exact current camera frame");
                Require(pipeline->GetStudioSettings().faceLighting && pipeline->GetStudioSettings().keyLight.posX == .9f,
                    "master bypass preserves face mode and custom light placement");
                Require(pipeline->GetFaceAnalysisState().phase == P::FaceAnalysisPhase::Disabled && !pipeline->GetFaceAnalysisState().enabled,
                    "master bypass disables face intake immediately");
                Require(pipeline->GetLastReport().normalPreparationCount == 0 && pipeline->GetLastReport().imageUploadCount == 0,
                    "master bypass performs no geometry preparation or camera upload");
                pipeline->Configure({}); auto disabled = settings; disabled.light = DesktopLightMode::Off; pipeline->ConfigureStudio(disabled);
                Require(pipeline->Process(fixture.frame, 10) == fixture.frame, "lighting Off returns the exact current camera input");
                Require(pipeline->GetFaceAnalysisState().phase == P::FaceAnalysisPhase::Disabled, "lighting Off keeps face analysis disabled");
                pipeline->ConfigureStudio(settings);
            }
        });
        GROUP("source reset rebinds exact geometry while retained fanout pixels stay owned", {
            fixture.frame->pts = 1100000;
            auto* first = gpu.Process(fixture.frame, 11); const auto initial = gpu.GetLastReport();
            Require(initial.sourceGeneration == 11 && initial.frameSequence == 1 && initial.timestampPts == fixture.frame->pts,
                "the new camera's first report has its own exact generation, sequence one and original PTS");
            if (initial.lightingStatus != S::StageStatus::Applied)
                Require(first == fixture.frame && initial.lightingStatus == S::StageStatus::NotReady, "source-reset waiting never substitutes old facial output");
            auto* output = WaitApplied(gpu, fixture.frame, 11, true); VerifyLocalized(output, fixture.frame);
            Require(Pixels(gpuFace.frame) == retainedPixels, "an owned processed frame survives native source reset and later output-buffer reuse");
        });
#undef GROUP
        std::cout << "FACE_DESKTOP_PASS groups=" << groups << " assertions=" << assertions
            << "; actual native face image and real CPU/GPU execution; offline fixture only, not live FPS or gesture latency\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FACE_DESKTOP_FAIL groups=" << groups << " assertions=" << assertions << " reason=" << error.what() << '\n';
        return 1;
    }
}
