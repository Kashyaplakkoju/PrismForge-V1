#include "providers/face_analysis_service.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace P = PrismForge::Providers;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
struct Suite {
    int checks = 0, groups = 0, failures = 0;
    void Check(bool good, const std::string& message) { ++checks; if (!good) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
    void Require(bool good, const std::string& message) { Check(good, message); if (!good) throw std::runtime_error(message); }
    template<class F> void Group(const char* name, F function) { ++groups; std::cout << "GROUP " << name << '\n'; try { function(); } catch (const std::exception& error) { std::cerr << "GROUP STOP: " << error.what() << '\n'; } }
};
struct FrameOwner {
    AVFrame* value = nullptr;
    FrameOwner() = default;
    FrameOwner(const FrameOwner&) = delete;
    FrameOwner& operator=(const FrameOwner&) = delete;
    FrameOwner(FrameOwner&& other) noexcept : value(other.value) { other.value = nullptr; }
    ~FrameOwner() { av_frame_free(&value); }
};
struct InvalidTopologyFixture {
    std::wstring path;
    bool created = false;
    InvalidTopologyFixture() {
        wchar_t directory[MAX_PATH + 1]{};
        const DWORD count = GetTempPathW(MAX_PATH + 1, directory);
        if (!count || count > MAX_PATH) throw std::runtime_error("cannot resolve test temp directory");
        path = std::wstring(directory, count) + L"PrismForge-invalid-face-topology-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".obj";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create bounded invalid topology fixture");
        created = true;
        const char contents[] = "# Deliberately unreviewed, bounded test topology\n";
        DWORD written = 0;
        const bool saved = WriteFile(file, contents, DWORD(sizeof(contents) - 1), &written, nullptr) && written == sizeof(contents) - 1;
        CloseHandle(file);
        if (!saved) { DeleteFileW(path.c_str()); created = false; throw std::runtime_error("cannot write bounded invalid topology fixture"); }
    }
    ~InvalidTopologyFixture() { if (created) DeleteFileW(path.c_str()); }
};
FrameOwner Blank(int width = 192, int height = 192) {
    FrameOwner result; result.value = av_frame_alloc();
    if (!result.value) throw std::runtime_error("av_frame_alloc failed");
    result.value->format = AV_PIX_FMT_BGR24; result.value->width = width; result.value->height = height;
    if (av_frame_get_buffer(result.value, 32) < 0) throw std::runtime_error("owned frame allocation failed");
    for (int y = 0; y < height; ++y) std::fill_n(result.value->data[0] + size_t(y) * result.value->linesize[0], size_t(width) * 3, uint8_t(0));
    return result;
}
FrameOwner Decode(const wchar_t* filename) {
    auto check = [](HRESULT value) { if (FAILED(value)) throw std::runtime_error("WIC face fixture decode failed"); };
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapClipper> clip; ComPtr<IWICFormatConverter> converter; ComPtr<IWICBitmapSource> source;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    check(factory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
    check(decoder->GetFrame(0, &frame)); UINT width = 0, height = 0; check(frame->GetSize(&width, &height));
    source = frame;
    // Read only the supplied camera preview in memory; no fixture writes/uploads.
    if ((width == 1376 || width == 1377) && (height == 915 || height == 917)) {
        check(factory->CreateBitmapClipper(&clip)); WICRect rectangle{126, 287, 792, 443};
        check(clip->Initialize(frame.Get(), &rectangle)); source = clip; width = 792; height = 443;
    }
    if (!width || !height || width > 8192 || height > 8192 || uint64_t(width) * height * 3 > 32ULL * 1024 * 1024) throw std::runtime_error("fixture dimensions exceed bounds");
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    FrameOwner result = Blank(int(width), int(height));
    const uint64_t capacity = uint64_t(result.value->linesize[0]) * height;
    if (capacity > UINT_MAX) throw std::runtime_error("fixture stride exceeds WIC bound");
    check(converter->CopyPixels(nullptr, UINT(result.value->linesize[0]), UINT(capacity), result.value->data[0])); return result;
}
bool WaitReady(P::FaceAnalysisService& service, std::chrono::milliseconds limit = std::chrono::seconds(15)) {
    const auto until = Clock::now() + limit;
    do {
        const auto state = service.GetState();
        if (state.ready) return true;
        if (state.phase == P::FaceAnalysisPhase::Failed || state.phase == P::FaceAnalysisPhase::Stopped) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (Clock::now() < until);
    return false;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { std::cerr << "Usage: face_service_tests runtime.dll model.task canonical.obj [face-fixture.png]\n"; return 2; }
    Suite suite; P::FaceAnalysisOptions options{argv[1], argv[2], argv[3]};
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComOwner { bool owned; ~ComOwner() { if (owned) CoUninitialize(); } } comOwner{SUCCEEDED(com)};
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 2;
    FrameOwner blank = Blank();
    suite.Group("native-free disabled controls and pinned-topology rejection", [&] {
        P::FaceAnalysisService off(options);
        const auto start = Clock::now(); off.ResetSource(1); off.Configure(false);
        suite.Check(!off.Analyse(blank.value, 1, 1, 0), "disabled service returns empty");
        suite.Check(std::chrono::duration<double, std::milli>(Clock::now() - start).count() < 100, "disabled controls do not load a native model");
        suite.Check(off.GetState().submittedFrames == 0 && !off.GetState().inFlight, "disabled frames are not queued");
        off.Stop(); suite.Check(off.GetState().phase == P::FaceAnalysisPhase::Stopped, "disabled service stops and joins");
        P::FaceAnalysisOptions bad = options; bad.topologyPath = argv[2];
        P::FaceAnalysisService rejected(bad); rejected.ResetSource(1); rejected.Configure(true);
        suite.Check(!WaitReady(rejected), "unreviewed topology never reaches ready");
        // The model task is larger than1MiB, so this fixture must fail the
        // topology size boundary before hashing. Test the hash boundary separately.
        suite.Check(rejected.GetState().lastError == "Face topology is missing, not a regular file, or too large", "oversized topology failure is explicit");
        suite.Check(!rejected.Analyse(blank.value, 1, 1, 0), "failed assets cannot produce geometry"); rejected.Stop();
        InvalidTopologyFixture wrongHash; bad.topologyPath = wrongHash.path;
        P::FaceAnalysisService hashRejected(bad); hashRejected.ResetSource(1); hashRejected.Configure(true);
        suite.Check(!WaitReady(hashRejected), "bounded unreviewed topology hash never reaches ready");
        suite.Check(hashRejected.GetState().lastError == "Face topology hash mismatch", "bounded topology failure is the actual hash mismatch");
        suite.Check(!hashRejected.Analyse(blank.value, 1, 1, 0), "hash mismatch cannot publish geometry"); hashRejected.Stop();
    });
    P::FaceAnalysisService service(options); service.ResetSource(2); service.Configure(true);
    bool ready = false;
    suite.Group("actual CPU worker initialization and owned-plane validation", [&] {
        ready = WaitReady(service); suite.Require(ready, "actual native worker initializes: " + service.GetState().lastError);
        suite.Check(service.GetState().phase == P::FaceAnalysisPhase::Ready && service.GetState().sourceGeneration == 2, "ready state belongs to requested source");
        const uint64_t before = service.GetState().submittedFrames;
        suite.Check(!service.Analyse(nullptr, 2, 1, 0), "null frame rejected");
        AVFrame borrowed{}; borrowed.format = AV_PIX_FMT_BGR24; borrowed.width = blank.value->width; borrowed.height = blank.value->height;
        borrowed.linesize[0] = blank.value->linesize[0]; borrowed.data[0] = blank.value->data[0];
        suite.Check(!service.Analyse(&borrowed, 2, 1, 0), "unowned plane rejected without native submission");
        borrowed.extended_data = borrowed.data;
        suite.Check(!service.Analyse(&borrowed, 2, 1, 0), "unowned frame with valid extended_data also rejected");
        const int oldStride = blank.value->linesize[0]; blank.value->linesize[0] = 1;
        suite.Check(!service.Analyse(blank.value, 2, 1, 0), "short stride rejected"); blank.value->linesize[0] = oldStride;
        const int oldWidth = blank.value->width; blank.value->width = std::numeric_limits<int>::max();
        suite.Check(!service.Analyse(blank.value, 2, 1, 0), "dimensions rejected before unsafe multiplication"); blank.value->width = oldWidth;
        uint8_t* oldData = blank.value->data[0]; blank.value->data[0] = blank.value->buf[0]->data + blank.value->buf[0]->size - 2;
        suite.Check(!service.Analyse(blank.value, 2, 1, 0), "short owned capacity rejected"); blank.value->data[0] = oldData;
        suite.Check(!service.Analyse(blank.value, 2, 0, 0) && !service.Analyse(blank.value, 2, 1, -1) && !service.Analyse(blank.value, 1, 1, 0), "invalid and old source tags rejected");
        suite.Check(service.GetState().submittedFrames == before, "invalid frames never enter worker queue");
        // A valid packed plane may be owned by another standard buffer slot.
        blank.value->buf[1] = blank.value->buf[0]; blank.value->buf[0] = nullptr;
        suite.Check(!service.Analyse(blank.value, 2, 1, 0, std::chrono::milliseconds(1000)), "owned plane in a nonzero reference slot is accepted and completes actual no-face inference");
        blank.value->buf[0] = blank.value->buf[1]; blank.value->buf[1] = nullptr;
        suite.Check(!service.Analyse(blank.value, 2, 2, 16000, std::chrono::milliseconds(1000)), "native blank image produces empty geometry");
        suite.Check(service.GetState().completedFrames == 2 && service.GetState().lastCompletedSequence == 2 && !service.GetState().lastDetected, "actual no-face completed separately from timeout");
        suite.Check(!service.Analyse(blank.value, 2, 2, 16000) && service.GetState().submittedFrames == before + 2, "duplicate input sequence never resubmitted");
    });
    if (ready && argc > 4) suite.Group("real camera fixture exact geometry, cancellation and source reset", [&] {
        FrameOwner face = Decode(argv[4]);
        service.ResetSource(2); suite.Require(WaitReady(service), "fresh real-image tracking after owner validation");
        auto result = service.Analyse(face.value, 2, 2, 16000, std::chrono::milliseconds(1000));
        suite.Require(bool(result), "actual face worker returns geometry: " + service.GetState().lastError);
        suite.Check(result->sourceGeneration == 2 && result->sequenceId == 2 && result->timestampPts == 16000, "first result uses original exact tags");
        suite.Check(result->HasDepth() && result->HasNormals() && result->HasPersonMask() && result->faceLandmarks.detected, "actual geometry carries owned normal/depth/face support");
        suite.Require(result->HasSkinMask() && result->HasProtectionMask(), "face-effect bundle carries bounded geometric touch-up eligibility and protection");
        suite.Check(!result->HasFaceLandmarks(), "geometric detection does not fabricate calibrated landmark confidence");
        suite.Require(result->skinMask.width == result->personMask.width && result->skinMask.height == result->personMask.height &&
            result->protectionMask.width == result->personMask.width && result->protectionMask.height == result->personMask.height,
            "touch-up masks use the same bounded facial raster, never body segmentation");
        size_t protectedFeatures = 0, eligibleSurface = 0;
        for (int y = 0; y < result->personMask.height; ++y) for (int x = 0; x < result->personMask.width; ++x) {
            const float coverage = result->personMask.data[size_t(y) * result->personMask.strideFloats + x];
            const float eligibility = result->skinMask.data[size_t(y) * result->skinMask.strideFloats + x];
            const float protection = result->protectionMask.data[size_t(y) * result->protectionMask.strideFloats + x];
            suite.Check(eligibility >= 0 && eligibility <= coverage && protection >= 0 && protection <= 1,
                "geometric eligibility never extends beyond face coverage and feature protection is bounded");
            if (protection == 1) { ++protectedFeatures; suite.Check(eligibility == 0, "protected eye/lip/brow raster has no smoothing eligibility"); }
            if (eligibility > 0) ++eligibleSurface;
        }
        suite.Check(protectedFeatures > 0 && eligibleSurface > 0, "real face has both protected features and eligible facial surface");
        size_t covered = 0; for (float value : result->personMask.data) if (value > 0) ++covered;
        suite.Check(covered > 100 && covered < result->personMask.data.size() / 2, "coverage is confined to the face");
        const auto retained = result; const auto retainedNormal = result->normalMap.data; const auto retainedDepth = result->depthMap.data;
        auto equalPts = service.Analyse(face.value, 2, 3, 16000, std::chrono::milliseconds(1000));
        suite.Require(bool(equalPts), "equal original PTS accepted with a new sequence");
        suite.Check(equalPts->sequenceId == 3 && equalPts->timestampPts == 16000 && retained->normalMap.data == retainedNormal && retained->depthMap.data == retainedDepth, "retained result remains immutable and timestamps are not retagged");
        const auto timeoutStart = Clock::now();
        auto timedOut = service.Analyse(face.value, 2, 4, 33000, std::chrono::milliseconds(0));
        suite.Check(!timedOut, "zero-budget submission returns no aged geometry");
        suite.Check(std::chrono::duration<double, std::milli>(Clock::now() - timeoutStart).count() < 100, "zero wait budget never waits for native inference");
        suite.Check(service.GetState().timedOutFrames >= 1, "timeout is separately counted");
        auto following = service.Analyse(face.value, 2, 5, 50000, std::chrono::milliseconds(1000));
        suite.Require(bool(following), "subsequent exact frame works after timeout");
        suite.Check(following->sequenceId == 5 && following->timestampPts == 50000, "timed-out sequence is never retagged as the following frame");
        for (uint64_t seq = 6; seq < 26; ++seq) {
            suite.Check(!service.Analyse(face.value, 2, seq, int64_t(seq * 16667), std::chrono::milliseconds(0)), "overload returns empty rather than old output");
            const auto state = service.GetState(); suite.Check(state.submittedFrames >= seq && !state.hasPending, "timeout removes pending owned input");
        }
        auto drained = service.Analyse(face.value, 2, 26, 500000, std::chrono::milliseconds(1000));
        suite.Require(bool(drained), "worker recovers after bounded overload");
        suite.Check(drained->sequenceId == 26, "overload never delivers older queued geometry");
        const auto resetStart = Clock::now(); service.ResetSource(3);
        suite.Check(std::chrono::duration<double, std::milli>(Clock::now() - resetStart).count() < 100, "source reset does not join or call native code");
        suite.Check(!service.Analyse(face.value, 2, 27, 600000), "prior generation rejected immediately");
        suite.Require(WaitReady(service), "worker finishes source tracking reset");
        auto newSource = service.Analyse(face.value, 3, 1, 0, std::chrono::milliseconds(1000));
        suite.Require(bool(newSource), "new source permits its own sequence/PTS origin");
        suite.Check(newSource->sourceGeneration == 3 && newSource->sequenceId == 1 && newSource->timestampPts == 0, "source reset exact tags with original time origin");
        service.ResetSource(2); suite.Check(service.GetState().sourceGeneration == 3, "generation cannot roll backward");
        service.ResetSource(3); suite.Require(WaitReady(service), "same generation resets for source resolution changes");
        auto resizedOrigin = service.Analyse(face.value, 3, 1, 0, std::chrono::milliseconds(1000));
        suite.Require(bool(resizedOrigin), "same generation reset clears consumed sequence/PTS history");
        suite.Check(resizedOrigin->sourceGeneration == 3 && resizedOrigin->sequenceId == 1 && resizedOrigin->timestampPts == 0, "same generation reset preserves exact source tags");
        service.Configure(false);
        suite.Check(!service.Analyse(face.value, 3, 2, 16000) && !service.GetState().enabled && !service.GetState().ready, "off invalidates any old geometry immediately");
        service.Configure(true); suite.Require(WaitReady(service), "toggle reuses model only after worker tracking reset");
        auto afterToggle = service.Analyse(face.value, 3, 1, 0, std::chrono::milliseconds(1000));
        suite.Require(bool(afterToggle), "re-enabled worker accepts clean tracking origin");
        suite.Check(afterToggle->sourceGeneration == 3 && afterToggle->sequenceId == 1 && afterToggle->timestampPts == 0, "re-enabled result exact");
        // This observes a real worker-owned input in flight. It does not infer
        // cancellation behavior from the0ms tests, which only cancel pending.
        service.ResetSource(4); suite.Require(WaitReady(service), "fresh source prepared for actual in-flight cancellation");
        std::shared_ptr<const PrismForge::Effects::AnalysisBridge::AnalysisBundle> cancelledResult;
        std::atomic<bool> callerFinished{false};
        const auto droppedBeforeCancellation = service.GetState().droppedFrames;
        std::thread caller([&] {
            cancelledResult = service.Analyse(face.value, 4, 1, 0, std::chrono::milliseconds(1000));
            callerFinished.store(true, std::memory_order_release);
        });
        struct CallerJoin { std::thread& thread; ~CallerJoin() { if (thread.joinable()) thread.join(); } } callerJoin{caller};
        bool observedInFlight = false;
        const auto observationDeadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < observationDeadline && !callerFinished.load(std::memory_order_acquire)) {
            const auto current = service.GetState();
            if (current.inFlight && current.sourceGeneration == 4) { observedInFlight = true; break; }
            std::this_thread::yield();
        }
        suite.Check(observedInFlight, "actual native worker input observed in flight before reset");
        const auto cancellationStart = Clock::now(); service.ResetSource(5);
        caller.join();
        suite.Check(!cancelledResult, "source reset cancels the observed in-flight caller rather than returning old geometry");
        suite.Check(std::chrono::duration<double, std::milli>(Clock::now() - cancellationStart).count() < 100,
            "reset wakes the effects caller without joining native inference");
        suite.Require(WaitReady(service), "old native inference finishes and new-source tracking reset completes");
        suite.Check(service.GetState().droppedFrames > droppedBeforeCancellation && service.GetState().lastCompletedSequence == 0,
            "obsolete in-flight result is dropped rather than published into new source");
        auto afterCancellation = service.Analyse(face.value, 5, 1, 0, std::chrono::milliseconds(1000));
        suite.Require(bool(afterCancellation), "exact new source inference succeeds after actual in-flight cancellation");
        suite.Check(afterCancellation->sourceGeneration == 5 && afterCancellation->sequenceId == 1 && afterCancellation->timestampPts == 0,
            "new-source result preserves exact original tags after in-flight cancellation");
        std::cout << "OBSERVED_INFLIGHT_SOURCE_CANCELLATION " << observedInFlight << "\n";
        std::cout << "ACTUAL_FACE_WORKER_MS " << service.GetState().lastInferenceMilliseconds << " FACE_PIXELS " << covered << " NOT_LIVE_FPS\n";
    });
    else if (argc <= 4) std::cout << "SKIP real face service: optional user fixture not provided\n";
    service.Stop();
    suite.Check(service.GetState().phase == P::FaceAnalysisPhase::Stopped && !service.GetState().ready && !service.GetState().hasPending, "cooperative Stop joins and clears queue");
    suite.Check(!service.Analyse(blank.value, 3, 99, 999999), "stopped service returns no geometry");
    std::cout << "FACE_SERVICE checks=" << suite.checks << " groups=" << suite.groups << " failures=" << suite.failures << "; actual native CPU worker and offline supplied image; no product FPS claim\n";
    return suite.failures ? 1 : 0;
}
