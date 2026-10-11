#include "systemcamera.h"
#include "mf_capture.h"
#include "dshow_capture.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <deque>
#include <future>
#include <condition_variable>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace PrismForge::Capture {

namespace {

// FNV-1a 64-bit hash for stable opaque device ID generation
uint64_t Fnv1a64(const std::string& text) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string ToHex64(uint64_t val) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << val;
    return oss.str();
}

std::string ToLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.empty()) return false;
    std::string h = ToLower(haystack);
    std::string n = ToLower(needle);
    return h.find(n) != std::string::npos;
}

// PrismForge output virtual camera CLSIDs:
// DirectShow softcam: {D732B8C1-03D7-4D59-AF8E-4CB72B58B940}
// Media Foundation:   {2141C943-DD09-4915-BD14-264724DBA955}
constexpr const char* PRISMFORGE_DSHOW_CLSID = "D732B8C1-03D7-4D59-AF8E-4CB72B58B940";
constexpr const char* PRISMFORGE_MF_CLSID    = "2141C943-DD09-4915-BD14-264724DBA955";

} // namespace

std::string CaptureBackendToString(CaptureBackend backend) {
    switch (backend) {
        case CaptureBackend::MediaFoundation: return "Media Foundation";
        case CaptureBackend::DirectShow: return "DirectShow";
        default: return "Unknown";
    }
}

std::string CaptureStateToString(CaptureState state) {
    switch (state) {
        case CaptureState::Idle: return "Idle";
        case CaptureState::Starting: return "Starting";
        case CaptureState::Running: return "Running";
        case CaptureState::Stopping: return "Stopping";
        case CaptureState::Disconnected: return "Disconnected";
        case CaptureState::Error: return "Error";
        default: return "Unknown";
    }
}

std::string CaptureErrorCategoryToString(CaptureErrorCategory cat) {
    switch (cat) {
        case CaptureErrorCategory::None: return "None";
        case CaptureErrorCategory::Busy: return "Busy";
        case CaptureErrorCategory::PrivacyAccessDenied: return "PrivacyAccessDenied";
        case CaptureErrorCategory::DeviceRemoved: return "DeviceRemoved";
        case CaptureErrorCategory::UnsupportedFormat: return "UnsupportedFormat";
        case CaptureErrorCategory::InternalFailure: return "InternalFailure";
        default: return "Unknown";
    }
}

std::string CaptureMode::ToString() const {
    std::ostringstream oss;
    oss << width << "x" << height << " @" << std::fixed << std::setprecision(2) << Fps() << "fps (" << pixelFormat;
    if (interlaced) oss << ", interlaced";
    oss << ")";
    return oss.str();
}

std::string SystemCameraCatalog::NormalizeDevicePath(const std::string& rawPath) {
    if (rawPath.empty()) return "";

    std::string s = ToLower(rawPath);

    // Strip DirectShow moniker prefix "@device:pnp:"
    const std::string dshowPnp = "@device:pnp:";
    if (s.rfind(dshowPnp, 0) == 0) {
        s = s.substr(dshowPnp.length());
    }

    // Strip "\\?\"
    const std::string dosPrefix = "\\\\?\\";
    if (s.rfind(dosPrefix, 0) == 0) {
        s = s.substr(dosPrefix.length());
    }

    std::string lower = ToLower(s);

    // Look for category/interface GUID boundary "#{"
    size_t guidPos = lower.find("#{");
    if (guidPos != std::string::npos) {
        return lower.substr(0, guidPos);
    }

    // If no "#{", keep the full hardware instance path! Do NOT strip after '#'
    if (lower.find("usb#") == 0 || lower.find("pci#") == 0 || lower.find("root#") == 0) {
        return lower;
    }

    return "";
}

bool SystemCameraCatalog::IsPrismForgeOutput(const std::string& pathOrClsid) {
    if (pathOrClsid.empty()) return false;
    // Windows virtual-camera symbolic links need not contain our source CLSID.
    // Match only our registered friendly name and its Windows-added decoration.
    const std::string name = ToLower(pathOrClsid);
    if (name == "prismforge virtual camera" ||
        name == "prismforge virtual camera (windows virtual camera)") {
        return true;
    }
    if (ContainsCaseInsensitive(pathOrClsid, PRISMFORGE_DSHOW_CLSID)) {
        return true;
    }
    if (ContainsCaseInsensitive(pathOrClsid, PRISMFORGE_MF_CLSID)) {
        return true;
    }
    return false;
}

std::vector<CaptureDevice> SystemCameraCatalog::DeduplicateAndMerge(
    const std::vector<CaptureDevice>& mfDevices,
    const std::vector<CaptureDevice>& dsDevices)
{
    std::vector<CaptureDevice> merged;

    // 1. Add all Media Foundation devices
    for (const auto& mfDev : mfDevices) {
        CaptureDevice dev = mfDev;
        std::string seed = !dev.devicePath.empty() ? dev.devicePath : (dev.routes.empty() ? dev.displayName : dev.routes[0].routeId);
        dev.id = "syscam_" + ToHex64(Fnv1a64(seed));
        merged.push_back(std::move(dev));
    }

    // 2. Process DirectShow devices
    for (const auto& dsDev : dsDevices) {
        bool mergedWithExisting = false;

        if (!dsDev.devicePath.empty()) {
            for (auto& existing : merged) {
                if (!existing.devicePath.empty() && existing.devicePath == dsDev.devicePath) {
                    for (const auto& r : dsDev.routes) {
                        bool routeExists = false;
                        for (const auto& er : existing.routes) {
                            if (er == r) {
                                routeExists = true;
                                break;
                            }
                        }
                        if (!routeExists) {
                            existing.routes.push_back(r);
                        }
                    }
                    mergedWithExisting = true;
                    break;
                }
            }
        }

        if (!mergedWithExisting) {
            CaptureDevice newDev = dsDev;
            if (newDev.devicePath.empty()) {
                for (auto& existing : merged) {
                    if (existing.displayName == newDev.displayName) {
                        if (existing.displayName.find("(Media Foundation)") == std::string::npos) {
                            existing.displayName += " (Media Foundation)";
                        }
                        newDev.displayName += " (DirectShow)";
                        break;
                    }
                }
            }

            std::string seed = !newDev.devicePath.empty() ? newDev.devicePath : (newDev.routes.empty() ? newDev.displayName : newDev.routes[0].routeId);
            newDev.id = "syscam_" + ToHex64(Fnv1a64(seed));

            int suffix = 1;
            std::string baseId = newDev.id;
            while (true) {
                bool idCollision = false;
                for (const auto& existing : merged) {
                    if (existing.id == newDev.id) {
                        idCollision = true;
                        break;
                    }
                }
                if (!idCollision) break;
                newDev.id = baseId + "_" + std::to_string(suffix++);
            }

            merged.push_back(std::move(newDev));
        }
    }

    return merged;
}

namespace {
    static SystemCameraCatalog::CustomEnumerator s_customEnumerator = nullptr;
    static std::mutex s_enumeratorMutex;
}

void SystemCameraCatalog::SetCustomEnumeratorForTesting(CustomEnumerator enumerator) {
    std::lock_guard<std::mutex> lock(s_enumeratorMutex);
    s_customEnumerator = std::move(enumerator);
}

EnumerationReport SystemCameraCatalog::Enumerate() {
    CustomEnumerator custom;
    { std::lock_guard<std::mutex> lock(s_enumeratorMutex); custom = s_customEnumerator; }
    if (custom) return custom(); // Test seam callbacks execute outside catalogue lock.

    EnumerationReport report;

    std::vector<CaptureDevice> mfDevices;
    auto mfRes = MfDeviceEnumerator::EnumerateDevices(mfDevices);
    report.backendResults.push_back(mfRes);

    std::vector<CaptureDevice> dsDevices;
    auto dsRes = DShowDeviceEnumerator::EnumerateDevices(dsDevices);
    report.backendResults.push_back(dsRes);

    report.devices = DeduplicateAndMerge(mfDevices, dsDevices);
    return report;
}

std::vector<CaptureMode> SystemCameraCatalog::QueryCapabilities(const CaptureDevice& device) {
    const DeviceRoute* mfRoute = device.GetRoute(CaptureBackend::MediaFoundation);
    if (mfRoute && !mfRoute->routeId.empty()) {
        auto modes = MfDeviceEnumerator::QueryCapabilities(mfRoute->routeId);
        if (!modes.empty()) return modes;
    }

    const DeviceRoute* dsRoute = device.GetRoute(CaptureBackend::DirectShow);
    if (dsRoute && !dsRoute->routeId.empty()) {
        return DShowDeviceEnumerator::QueryCapabilities(dsRoute->routeId);
    }

    return {};
}

// Codex integration: backend ownership and callback/status delivery are serialized.
namespace {
thread_local const void* activeCaptureCallback = nullptr;
struct CallbackScope {
    const void* previous;
    explicit CallbackScope(const void* owner) : previous(activeCaptureCallback) { activeCaptureCallback = owner; }
    ~CallbackScope() { activeCaptureCallback = previous; }
};
}

struct SystemCameraSource::Impl {
    struct Attempt {
        std::atomic<bool> cancelled{false};
        std::atomic<uint64_t> frames{0};
        std::atomic<bool> consumerFailed{false};
        std::atomic<bool> acceptingFrames{true};
        CaptureMode requested;
        bool hasRequested = false;
        CaptureStatus pendingStatus;
        bool statusQueued = false;
        bool terminalPosted = false;
    };
    FrameCallback frameCallback;
    StatusCallback statusCallback;
    BackendFactory factory;
    std::unique_ptr<ICaptureBackend> backend; // Accessed only by the owner thread.
    CaptureDevice device;
    size_t routeIndex = 0;
    std::shared_ptr<Attempt> attempt;
    mutable std::mutex stateMutex;
    CaptureStatus status;
    std::mutex frameMutex;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    std::deque<std::shared_ptr<Attempt>> updates;
    bool stopRequested = false;
    bool exiting = false;
    std::thread owner;

    Impl() : owner([this] { Run(); }) {}
    bool InsideCallback() const { return activeCaptureCallback == this; }
    void Publish(CaptureStatus next) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            status = next;
        }
        if (statusCallback) {
            CallbackScope scope(this);
            try { statusCallback(next); } catch (...) {}
        }
    }
    std::shared_ptr<Attempt> Current() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return attempt;
    }
    void SetCurrent(std::shared_ptr<Attempt> value) {
        std::lock_guard<std::mutex> lock(stateMutex);
        attempt = std::move(value);
    }
    bool Call(std::function<bool()> operation) {
        if (InsideCallback()) return false; // No synchronous owner/callback self-wait.
        auto result = std::make_shared<std::promise<bool>>();
        auto future = result->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            wake.wait(lock, [this] { return exiting || commands.size() < 64; });
            if (exiting) return false;
            commands.emplace_back([operation = std::move(operation), result] {
                try { result->set_value(operation()); }
                catch (...) { result->set_exception(std::current_exception()); }
            });
        }
        wake.notify_all();
        return future.get();
    }
    void RequestStop() {
        if (auto current = Current()) current->cancelled = true;
        else {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (status.state == CaptureState::Idle) return;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopRequested = true; // Coalesced; callback cancellation never waits or joins.
        }
        wake.notify_all();
    }
    void StopOwned(bool publishIdle = true) {
        auto previous = Current();
        if (previous) previous->cancelled = true;
        SetCurrent(nullptr);
        if (previous) {
            std::lock_guard<std::mutex> lock(queueMutex);
            updates.erase(std::remove(updates.begin(), updates.end(), previous), updates.end());
        }
        if (backend) {
            backend->Cancel();
            backend->Close();
            backend.reset();
        }
        if (publishIdle) Publish(CaptureStatus{});
    }
    void PostStatus(const std::shared_ptr<Attempt>& value, CaptureStatus next) {
        if (value->cancelled) return;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (value->cancelled || value->terminalPosted) return;
            value->terminalPosted = next.state == CaptureState::Error || next.state == CaptureState::Disconnected;
            if (value->terminalPosted) value->acceptingFrames = false;
            // First-frame notifications must retain the negotiated mode even when
            // they coalesce with a native backend's earlier Starting notification.
            if (next.nativeMode.width == 0) next.nativeMode = value->pendingStatus.nativeMode;
            if (next.deliveredMode.width == 0) next.deliveredMode = value->pendingStatus.deliveredMode;
            value->pendingStatus = std::move(next);
            if (!value->statusQueued) {
                value->statusQueued = true;
                updates.push_back(value); // One pending snapshot per live attempt.
            }
        }
        wake.notify_all();
    }
    bool OpenRoute(const CaptureMode* requested) {
        auto next = std::make_shared<Attempt>();
        next->hasRequested = requested != nullptr;
        if (requested) next->requested = *requested;
        SetCurrent(next);
        auto frameListener = frameCallback;
        auto onFrame = [this, next, frameListener](AVFrame* frame) {
            std::lock_guard<std::mutex> delivery(frameMutex);
            if (next->cancelled || next->consumerFailed || !next->acceptingFrames) return;
            CallbackScope scope(this);
            try {
                if (frameListener) frameListener(frame);
                const auto count = ++next->frames;
                if (count == 1) {
                    CaptureStatus running;
                    running.state = CaptureState::Running;
                    PostStatus(next, running);
                }
            } catch (...) {
                next->consumerFailed = true;
                CaptureStatus failure;
                failure.state = CaptureState::Error;
                failure.errorCategory = CaptureErrorCategory::InternalFailure;
                failure.errorMessage = "Camera frame consumer threw an exception.";
                failure.droppedFrames = 1;
                PostStatus(next, failure);
            }
        };
        auto onStatus = [this, next](const CaptureStatus& value) { PostStatus(next, value); };
        const auto& route = device.routes[routeIndex];
        const auto* mode = next->hasRequested ? &next->requested : nullptr;
        if (factory) backend = factory(route, mode, onFrame, onStatus);
        else if (route.backend == CaptureBackend::MediaFoundation) backend = std::make_unique<MfCaptureBackend>(onFrame, onStatus);
        else backend = std::make_unique<DShowCaptureBackend>(onFrame, onStatus);
        if (!backend || !backend->Open(route, mode)) {
            CaptureStatus error;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                error = next->pendingStatus;
            }
            if (error.state != CaptureState::Error) {
                error.state = CaptureState::Error;
                error.errorCategory = CaptureErrorCategory::InternalFailure;
                error.errorMessage = "Camera route could not start.";
            }
            StopOwned(false);
            Publish(error);
            return false;
        }
        return true;
    }
    static bool CanFallback(const CaptureStatus& s) {
        return s.state == CaptureState::Error && s.errorCategory != CaptureErrorCategory::PrivacyAccessDenied &&
            s.errorCategory != CaptureErrorCategory::DeviceRemoved;
    }
    bool StartOwned(CaptureDevice selected, const CaptureMode* requested) {
        StopOwned(false);
        device = std::move(selected);
        routeIndex = 0;
        std::stable_sort(device.routes.begin(), device.routes.end(), [](const DeviceRoute& a, const DeviceRoute& b) {
            return int(a.backend) < int(b.backend);
        });
        if (device.routes.empty()) {
            CaptureStatus error;
            error.state = CaptureState::Error;
            error.errorCategory = CaptureErrorCategory::DeviceRemoved;
            error.errorMessage = "Selected camera has no capture routes.";
            Publish(error);
            return false;
        }
        CaptureStatus starting;
        starting.state = CaptureState::Starting;
        Publish(starting);
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopRequested) return false;
        }
        do {
            { std::lock_guard<std::mutex> lock(queueMutex); if (stopRequested) return false; }
            if (OpenRoute(requested)) return true;
            CaptureStatus failure;
            { std::lock_guard<std::mutex> lock(stateMutex); failure = status; }
            if (!CanFallback(failure)) break;
        } while (++routeIndex < device.routes.size());
        return false;
    }
    void HandleStatus(const std::shared_ptr<Attempt>& value, CaptureStatus next) {
        if (value->cancelled || Current() != value) return;
        next.framesCaptured = value->frames;
        CaptureStatus previous;
        { std::lock_guard<std::mutex> lock(stateMutex); previous = status; }
        if (next.nativeMode.width == 0) next.nativeMode = previous.nativeMode;
        if (next.deliveredMode.width == 0) next.deliveredMode = previous.deliveredMode;
        if (next.state == CaptureState::Running && value->frames == 0) next.state = CaptureState::Starting;
        if (CanFallback(next) && value->frames == 0 && !value->consumerFailed && routeIndex + 1 < device.routes.size()) {
            CaptureMode requested = value->requested;
            const bool hasRequested = value->hasRequested;
            StopOwned(false);
            ++routeIndex;
            CaptureStatus starting;
            starting.state = CaptureState::Starting;
            Publish(starting);
            do {
                { std::lock_guard<std::mutex> lock(queueMutex); if (stopRequested) return; }
                if (OpenRoute(hasRequested ? &requested : nullptr)) return;
                CaptureStatus failure;
                { std::lock_guard<std::mutex> lock(stateMutex); failure = status; }
                if (!CanFallback(failure)) return;
            } while (++routeIndex < device.routes.size());
            return;
        }
        Publish(next);
        if (next.state == CaptureState::Error || next.state == CaptureState::Disconnected) StopOwned(false);
    }
    void Run() {
        for (;;) {
            std::function<void()> command;
            std::shared_ptr<Attempt> update;
            CaptureStatus next;
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                wake.wait(lock, [this] { return exiting || stopRequested || !commands.empty() || !updates.empty(); });
                if (exiting) break;
                if (stopRequested) { stopRequested = false; stop = true; }
                else if (!commands.empty()) { command = std::move(commands.front()); commands.pop_front(); }
                else {
                    update = std::move(updates.front()); updates.pop_front();
                    next = update->pendingStatus;
                    update->statusQueued = false;
                }
            }
            wake.notify_all();
            if (stop) StopOwned();
            else if (command) command();
            else HandleStatus(update, next);
        }
        StopOwned(false);
    }
};

SystemCameraSource::SystemCameraSource() : m_impl(std::make_unique<Impl>()) {}
SystemCameraSource::~SystemCameraSource() {
    // Destruction is an external-owner operation, never a borrowed callback operation.
    Stop();
    { std::lock_guard<std::mutex> lock(m_impl->queueMutex); m_impl->exiting = true; }
    m_impl->wake.notify_all();
    m_impl->owner.join();
}
void SystemCameraSource::SetFrameCallback(FrameCallback callback) {
    m_impl->Call([this, callback = std::move(callback)]() mutable { m_impl->frameCallback = std::move(callback); return true; });
}
void SystemCameraSource::SetStatusCallback(StatusCallback callback) {
    m_impl->Call([this, callback = std::move(callback)]() mutable { m_impl->statusCallback = std::move(callback); return true; });
}
void SystemCameraSource::SetBackendFactoryForTesting(BackendFactory factory) {
    m_impl->Call([this, factory = std::move(factory)]() mutable { m_impl->factory = std::move(factory); return true; });
}
bool SystemCameraSource::Start(const CaptureDevice& selected, const CaptureMode* requested) {
    const CaptureMode mode = requested ? *requested : CaptureMode{};
    return m_impl->Call([this, selected, mode, hasMode = requested != nullptr] {
        return m_impl->StartOwned(selected, hasMode ? &mode : nullptr);
    });
}
void SystemCameraSource::Stop() {
    if (m_impl->InsideCallback()) { m_impl->RequestStop(); return; }
    if (auto attempt = m_impl->Current()) attempt->cancelled = true;
    m_impl->Call([this] { m_impl->StopOwned(); return true; });
}
CaptureState SystemCameraSource::GetState() const { return GetStatus().state; }
CaptureStatus SystemCameraSource::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_impl->stateMutex);
    auto snapshot = m_impl->status;
    if (m_impl->attempt) snapshot.framesCaptured = m_impl->attempt->frames.load();
    return snapshot;
}

} // namespace PrismForge::Capture
