#include "face_analysis_service.h"
#include "face_landmarker_runtime.h"
#include "face_geometry.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#pragma comment(lib, "bcrypt.lib")

namespace PrismForge::Providers {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxRgbBytes = 32ULL * 1024 * 1024;
constexpr std::array<unsigned char, 32> kTopologyDigest = {
    0x8b,0xac,0x80,0x44,0x33,0x97,0xe1,0x13,0xf4,0x1a,0x8b,0x56,0x5e,0xa7,0x2c,0x59,
    0x39,0x0b,0xc0,0x31,0xd9,0xde,0xfa,0xb2,0x89,0xdb,0xa7,0xbc,0x0c,0x54,0xe6,0x18};
bool LoadPinnedTopology(const std::wstring& path, FaceTopology& topology, std::string& error) {
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || (path[2] != L'\\' && path[2] != L'/') || path.find(L'\0') != std::wstring::npos ||
        path.find(L':', 2) != std::wstring::npos) {
        error = "Face topology requires an absolute local file path"; return false;
    }
    struct FileOwner { HANDLE handle = INVALID_HANDLE_VALUE; ~FileOwner() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } file;
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{}; BY_HANDLE_FILE_INFORMATION information{};
    if (file.handle == INVALID_HANDLE_VALUE || GetFileType(file.handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file.handle, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !GetFileSizeEx(file.handle, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        error = "Face topology is missing, not a regular file, or too large"; return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object, bytes(65536); // Outlive the hash handle that uses their storage.
    struct HashOwner { BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~HashOwner() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } hashOwner{algorithm, hash};
    DWORD length = 0, objectBytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &length, 0) < 0 || length != sizeof(objectBytes) || !objectBytes || objectBytes > 1024 * 1024) {
        error = "Could not initialize topology integrity check"; return false;
    }
    object.resize(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        error = "Could not create topology integrity check"; return false;
    }
    uint64_t consumed = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.handle, bytes.data(), DWORD(bytes.size()), &got, nullptr)) {
            error = "Could not read pinned topology"; return false;
        }
        if (!got) break;
        consumed += got;
        if (consumed > uint64_t(size.QuadPart) || BCryptHashData(hash, bytes.data(), got, 0) < 0) {
            error = "Could not hash bounded topology"; return false;
        }
    }
    std::array<unsigned char, 32> digest{};
    if (consumed != uint64_t(size.QuadPart) || BCryptFinishHash(hash, digest.data(), DWORD(digest.size()), 0) < 0 || digest != kTopologyDigest) {
        error = "Face topology hash mismatch"; return false;
    }
    // Hold the verified file read handle while the parser reads its resolved path;
    // share-write and share-delete are denied throughout the verification/parse.
    DWORD need = GetFinalPathNameByHandleW(file.handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!need || need > 32768) { error = "Could not resolve pinned topology path"; return false; }
    std::vector<wchar_t> resolved(size_t(need) + 1);
    DWORD got = GetFinalPathNameByHandleW(file.handle, resolved.data(), DWORD(resolved.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!got || got >= resolved.size()) { error = "Could not resolve pinned topology path"; return false; }
    const std::wstring stablePath(resolved.data(), got);
    if (stablePath.size() < 7 || stablePath.compare(0, 4, L"\\\\?\\") != 0 || stablePath[5] != L':') {
        error = "Face topology must resolve to a local drive file"; return false;
    }
    return LoadFaceTopology(stablePath, topology, error);
}
struct OwnedJob {
    FaceFrameTags tags;
    uint64_t epoch = 0, ticket = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};
bool CopyOwnedBgr(const AVFrame* frame, OwnedJob& job, std::string& error) {
    if (!frame || frame->format != AV_PIX_FMT_BGR24 || !frame->data[0] || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 8192 || frame->height > 8192 || frame->linesize[0] <= 0) {
        error = "Face analysis needs a bounded, owned BGR24 frame"; return false;
    }
    const uint64_t rowBytes = uint64_t(frame->width) * 3;
    const uint64_t rgbBytes = rowBytes * uint64_t(frame->height);
    const uint64_t spanBytes = uint64_t(frame->linesize[0]) * uint64_t(frame->height - 1) + rowBytes;
    if (uint64_t(frame->linesize[0]) < rowBytes || rgbBytes > kMaxRgbBytes) {
        error = "Face analysis frame stride or capacity exceeds bounds"; return false;
    }
    // BGR24 has one video plane. Check its bounded standard AVBufferRef owner
    // slots directly: av_frame_get_plane_buffer reads extended_data and can crash
    // on an unowned/malformed AVFrame before returning its documented null result.
    // We consume only data[0], so no extended_data or extended_buf dereference is
    // needed. Ref ownership must cover every accessed row, including padding.
    const uintptr_t data = reinterpret_cast<uintptr_t>(frame->data[0]);
    bool ownedSpan = false;
    for (const AVBufferRef* buffer : frame->buf) {
        if (!buffer || !buffer->data || !buffer->size) continue;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer->data);
        if (buffer->size > UINTPTR_MAX - begin || data < begin || data > begin + buffer->size) continue;
        if (spanBytes <= begin + buffer->size - data) { ownedSpan = true; break; }
    }
    if (!ownedSpan) { error = "Face analysis owned plane does not cover every input row"; return false; }
    job.width = frame->width; job.height = frame->height; job.rgb.resize(size_t(rgbBytes));
    for (int y = 0; y < frame->height; ++y) {
        const uint8_t* source = frame->data[0] + size_t(y) * size_t(frame->linesize[0]);
        uint8_t* destination = job.rgb.data() + size_t(y) * size_t(rowBytes);
        for (int x = 0; x < frame->width; ++x) {
            const size_t offset = size_t(x) * 3;
            destination[offset] = source[offset + 2]; destination[offset + 1] = source[offset + 1]; destination[offset + 2] = source[offset];
        }
    }
    return true;
}
}
struct FaceAnalysisService::Impl {
    FaceAnalysisOptions options;
    mutable std::mutex mutex;
    std::mutex intakeMutex, stopMutex;
    std::condition_variable changed;
    std::thread worker;
    bool stopping = false, enabled = false, workerExited = false;
    uint64_t epoch = 1, controlRevision = 0, nextTicket = 0;
    uint64_t generation = 0, lastSubmittedSequence = 0, waitingTicket = 0, completedTicket = 0;
    int64_t lastSubmittedPts = 0;
    std::unique_ptr<OwnedJob> pending;
    std::shared_ptr<const Effects::AnalysisBridge::AnalysisBundle> result;
    FaceAnalysisState state;
    explicit Impl(FaceAnalysisOptions value) : options(std::move(value)) { worker = std::thread([this] { Run(); }); }
    void ClearIntake() {
        if (pending) { ++state.droppedFrames; pending.reset(); }
        result.reset(); waitingTicket = completedTicket = 0;
        lastSubmittedSequence = 0; lastSubmittedPts = 0;
        state.hasPending = false; state.lastDetected = false;
        state.lastCompletedSequence = 0; state.lastCompletedPts = 0;
    }
    void Run() noexcept {
        std::unique_ptr<FaceLandmarkerRuntime> runtime;
        FaceTopology topology;
        bool initialized = false, failed = false;
        uint64_t handledControl = 0;
        try {
            runtime = std::make_unique<FaceLandmarkerRuntime>();
            for (;;) {
                std::unique_ptr<OwnedJob> job;
                bool shouldRun = false, reset = false;
                uint64_t requestedEpoch = 0, revision = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stopping || controlRevision != handledControl || (enabled && !failed && (!initialized || pending)); });
                    if (stopping) break;
                    reset = controlRevision != handledControl;
                    handledControl = revision = controlRevision;
                    requestedEpoch = epoch; shouldRun = enabled;
                    if (reset) failed = false;
                }
                if (reset && initialized) {
                    // Teardown is native and worker-only. A failed close retains
                    // the live graph; never advertise Ready for that task.
                    std::string resetError;
                    if (!runtime->Reset(resetError)) {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!stopping && epoch == requestedEpoch && controlRevision == revision) {
                            failed = true; state.phase = FaceAnalysisPhase::Failed; state.ready = false;
                            state.lastError = resetError; state.message = "Face geometry reset failed: " + resetError;
                            ClearIntake(); changed.notify_all();
                        }
                        continue;
                    }
                }
                if (!shouldRun) continue;
                if (!initialized) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stopping || !enabled || epoch != requestedEpoch) continue;
                        state.phase = FaceAnalysisPhase::Loading; state.ready = false; state.message = "Loading face geometry";
                    }
                    std::string error;
                    initialized = LoadPinnedTopology(options.topologyPath, topology, error) && runtime->Initialize(options.runtimeDllPath, options.modelPath, error);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stopping || !enabled || epoch != requestedEpoch) continue;
                        if (!initialized) {
                            failed = true; state.phase = FaceAnalysisPhase::Failed; state.ready = false;
                            state.lastError = error; state.message = "Face geometry unavailable: " + error;
                            ClearIntake(); changed.notify_all(); continue;
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping || !enabled || controlRevision != revision || epoch != requestedEpoch) continue;
                    state.phase = FaceAnalysisPhase::Ready; state.ready = true; state.message = "Face geometry ready (native CPU)";
                    changed.notify_all();
                    if (!pending) continue;
                    job = std::move(pending); state.hasPending = false; state.inFlight = true;
                }
                const auto start = Clock::now();
                FaceObservation observation; FaceGeometry geometry; std::string error;
                bool success = runtime->DetectRGB(job->rgb.data(), job->rgb.size(), job->width, job->height, job->width * 3, job->tags, observation, error);
                std::shared_ptr<Effects::AnalysisBridge::AnalysisBundle> bundle;
                if (success && observation.detected) {
                    // Preserve the full source aspect ratio; scale down both axes if
                    // an unusually tall source would exceed the 2048 raster bound.
                    int rasterWidth = 320;
                    int rasterHeight = std::max(1, int((int64_t(job->height) * rasterWidth + job->width / 2) / job->width));
                    if (rasterHeight > 2048) { rasterHeight = 2048; rasterWidth = std::max(1, int(int64_t(job->width) * rasterHeight / job->height)); }
                    success = BuildFaceGeometry(observation, topology, rasterWidth, rasterHeight, geometry, error);
                    if (success) {
                        bundle = std::make_shared<Effects::AnalysisBridge::AnalysisBundle>();
                        bundle->sourceGeneration = job->tags.generation; bundle->sequenceId = job->tags.sequence; bundle->timestampPts = job->tags.microPts;
                        bundle->timestampCompleted = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                        bundle->personMask = std::move(geometry.coverage); bundle->normalMap = std::move(geometry.normals);
                        bundle->depthMap = std::move(geometry.depth); bundle->faceLandmarks = std::move(geometry.anchors);
                        bundle->skinMask = std::move(geometry.touchupSurface);
                        bundle->protectionMask = std::move(geometry.featureProtection);
                    }
                }
                const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.inFlight = false;
                    if (stopping || !enabled || job->epoch != epoch || job->tags.generation != generation) { ++state.droppedFrames; changed.notify_all(); continue; }
                    ++state.completedFrames; state.lastInferenceMilliseconds = milliseconds;
                    state.lastCompletedSequence = job->tags.sequence; state.lastCompletedPts = job->tags.microPts;
                    state.lastDetected = success && observation.detected;
                    if (!success) { ++state.inferenceErrors; state.lastError = error; state.message = "Face geometry skipped: " + error; }
                    else { state.lastError.clear(); state.message = observation.detected ? "Face geometry ready (native CPU)" : "No face detected"; }
                    if (waitingTicket == job->ticket) { result = std::move(bundle); completedTicket = job->ticket; }
                    else ++state.droppedFrames;
                    changed.notify_all();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            state.phase = FaceAnalysisPhase::Failed; state.ready = false; state.inFlight = false;
            state.lastError = "Face analysis worker failed"; state.message = state.lastError;
            enabled = false; state.enabled = false; ClearIntake(); changed.notify_all();
        }
        if (runtime) runtime->Stop(); // Cooperative native teardown only here or on this worker's destructor.
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) { state.phase = FaceAnalysisPhase::Stopped; state.message = "Face geometry stopped"; }
        state.ready = state.inFlight = false; workerExited = true; changed.notify_all();
    }
};
FaceAnalysisService::FaceAnalysisService(const FaceAnalysisOptions& options) : impl_(std::make_unique<Impl>(options)) {}
FaceAnalysisService::~FaceAnalysisService() { Stop(); }
void FaceAnalysisService::Configure(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping || impl_->workerExited || impl_->enabled == enabled) return;
    impl_->enabled = enabled; ++impl_->epoch; ++impl_->controlRevision; impl_->ClearIntake();
    impl_->state.enabled = enabled; impl_->state.ready = false; impl_->state.lastError.clear();
    impl_->state.phase = enabled ? FaceAnalysisPhase::Loading : FaceAnalysisPhase::Disabled;
    impl_->state.message = enabled ? "Loading face geometry" : "Face geometry is off";
    impl_->changed.notify_all();
}
void FaceAnalysisService::ResetSource(uint64_t generation) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping || !generation || generation < impl_->generation) return;
    impl_->generation = generation; ++impl_->epoch; ++impl_->controlRevision; impl_->ClearIntake();
    impl_->state.sourceGeneration = generation; impl_->state.ready = false;
    if (impl_->enabled) { impl_->state.phase = FaceAnalysisPhase::Loading; impl_->state.message = "Resetting face geometry for camera source"; }
    impl_->changed.notify_all();
}
std::shared_ptr<const Effects::AnalysisBridge::AnalysisBundle> FaceAnalysisService::Analyse(
    const AVFrame* bgrFrame, uint64_t generation, uint64_t sequence, int64_t microPts, std::chrono::milliseconds budget) {
    // Serial intake also prevents concurrent callers from allocating unbounded
    // staging images. Configure/ResetSource use a different mutex and remain free.
    std::lock_guard<std::mutex> intake(impl_->intakeMutex);
    uint64_t requestedEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopping || !impl_->enabled || !impl_->state.ready || !generation || generation != impl_->generation || !sequence || microPts < 0 ||
            (impl_->lastSubmittedSequence && (sequence <= impl_->lastSubmittedSequence || microPts < impl_->lastSubmittedPts))) return {};
        requestedEpoch = impl_->epoch;
    }
    try {
        auto job = std::make_unique<OwnedJob>(); std::string error;
        if (!CopyOwnedBgr(bgrFrame, *job, error)) {
            std::lock_guard<std::mutex> lock(impl_->mutex); impl_->state.lastError = std::move(error); return {};
        }
        std::unique_lock<std::mutex> lock(impl_->mutex);
        if (impl_->stopping || !impl_->enabled || !impl_->state.ready || impl_->epoch != requestedEpoch || impl_->generation != generation) return {};
        if (impl_->nextTicket == std::numeric_limits<uint64_t>::max()) { impl_->state.lastError = "Face analysis ticket counter exhausted"; return {}; }
        if (impl_->pending) { ++impl_->state.droppedFrames; impl_->pending.reset(); }
        job->tags = {generation, sequence, microPts}; job->epoch = requestedEpoch; job->ticket = ++impl_->nextTicket;
        const uint64_t ticket = job->ticket;
        impl_->pending = std::move(job); impl_->waitingTicket = ticket; impl_->completedTicket = 0; impl_->result.reset();
        impl_->lastSubmittedSequence = sequence; impl_->lastSubmittedPts = microPts;
        ++impl_->state.submittedFrames; impl_->state.hasPending = true;
        impl_->changed.notify_one();
        const auto milliseconds = std::max<int64_t>(0, std::min<int64_t>(1000, budget.count()));
        const bool complete = milliseconds > 0 && impl_->changed.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] {
            return impl_->stopping || !impl_->enabled || impl_->epoch != requestedEpoch || !impl_->state.ready || impl_->completedTicket == ticket;
        });
        if (!complete) {
            ++impl_->state.timedOutFrames; impl_->waitingTicket = 0;
            if (impl_->pending && impl_->pending->ticket == ticket) { ++impl_->state.droppedFrames; impl_->pending.reset(); impl_->state.hasPending = false; }
            impl_->changed.notify_all(); return {};
        }
        if (impl_->stopping || !impl_->enabled || impl_->epoch != requestedEpoch || impl_->completedTicket != ticket) return {};
        auto result = std::move(impl_->result); impl_->waitingTicket = impl_->completedTicket = 0;
        if (result && (result->sourceGeneration != generation || result->sequenceId != sequence || result->timestampPts != microPts)) return {};
        return result;
    } catch (...) {
        std::lock_guard<std::mutex> lock(impl_->mutex); impl_->state.lastError = "Face intake allocation failed"; return {};
    }
}
FaceAnalysisState FaceAnalysisService::GetState() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->state; }
void FaceAnalysisService::Stop() {
    std::lock_guard<std::mutex> stop(impl_->stopMutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->stopping) {
            impl_->stopping = true; impl_->enabled = false; ++impl_->epoch; impl_->ClearIntake();
            impl_->state.enabled = impl_->state.ready = false; impl_->changed.notify_all();
        }
    }
    // This service has no callbacks and its worker never calls Stop or public
    // methods, so self-join is structurally excluded.
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
