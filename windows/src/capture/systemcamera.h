#pragma once

#include "rtsp/ffmpeg.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace PrismForge::Capture {

// Available platform capture backends
enum class CaptureBackend : int {
    MediaFoundation = 0,
    DirectShow = 1
};

std::string CaptureBackendToString(CaptureBackend backend);

// Represents an API-specific route to access a video capture device
struct DeviceRoute {
    CaptureBackend backend = CaptureBackend::MediaFoundation;
    std::string routeId;    // Opaque UTF-8 string: MF symbolic link or DirectShow moniker display name / device path
    std::string routeName;  // Human-readable route descriptor

    bool operator==(const DeviceRoute& other) const {
        return backend == other.backend && routeId == other.routeId;
    }
};

// Represents a unique logical video capture device visible to Windows
struct CaptureDevice {
    std::string id;          // Deterministic stable opaque ID (hash of hardware instance key or primary route)
    std::string displayName; // User-facing friendly name (UTF-8)
    std::string devicePath;  // Verified normalized hardware instance path (e.g. "usb#vid_046d&pid_0825...")
    std::vector<DeviceRoute> routes;

    bool HasBackend(CaptureBackend b) const {
        for (const auto& r : routes) {
            if (r.backend == b) return true;
        }
        return false;
    }

    const DeviceRoute* GetRoute(CaptureBackend b) const {
        for (const auto& r : routes) {
            if (r.backend == b) return &r;
        }
        return nullptr;
    }
};

// Video mode capabilities and format specification
struct CaptureMode {
    int width = 0;
    int height = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 1; // Rational frame rate (e.g. 30000/1001 for 29.97 fps)
    std::string pixelFormat; // e.g. "NV12", "YUY2", "RGB24", "RGB32", "MJPG", "H264"
    bool interlaced = false;

    double Fps() const {
        return (fpsDenominator > 0 && fpsNumerator > 0)
            ? static_cast<double>(fpsNumerator) / static_cast<double>(fpsDenominator)
            : 0.0;
    }

    std::string ToString() const;

    bool operator==(const CaptureMode& other) const {
        return width == other.width &&
               height == other.height &&
               fpsNumerator == other.fpsNumerator &&
               fpsDenominator == other.fpsDenominator &&
               pixelFormat == other.pixelFormat &&
               interlaced == other.interlaced;
    }

    bool operator!=(const CaptureMode& other) const {
        return !(*this == other);
    }
};

// Lifecycle states of a camera capture session
enum class CaptureState : int {
    Idle = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Disconnected = 4,
    Error = 5
};

std::string CaptureStateToString(CaptureState state);

// Standardized error classifications
enum class CaptureErrorCategory : int {
    None = 0,
    Busy = 1,                   // Another process or application is using the camera
    PrivacyAccessDenied = 2,    // Windows camera privacy settings or ACL denied access
    DeviceRemoved = 3,          // Hardware was unplugged or disconnected
    UnsupportedFormat = 4,      // Device mode or format not supported / negotiable
    InternalFailure = 5         // Driver or subsystem internal failure
};

std::string CaptureErrorCategoryToString(CaptureErrorCategory cat);

// Asynchronous status snapshot, safe to copy across threads
struct CaptureStatus {
    CaptureState state = CaptureState::Idle;
    long hr = 0; // Windows HRESULT or error code
    CaptureErrorCategory errorCategory = CaptureErrorCategory::None;
    std::string errorMessage;
    CaptureMode nativeMode{};
    CaptureMode deliveredMode{};
    uint64_t framesCaptured = 0;
    uint64_t droppedFrames = 0;
};

// Status for individual backend enumeration
struct BackendEnumerationResult {
    CaptureBackend backend = CaptureBackend::MediaFoundation;
    bool success = false;
    long hr = 0;
    std::string errorMessage;
    size_t deviceCount = 0;
};

// Complete enumeration report across all backends
struct EnumerationReport {
    std::vector<CaptureDevice> devices;
    std::vector<BackendEnumerationResult> backendResults;

    bool HasErrors() const {
        for (const auto& br : backendResults) {
            if (!br.success) return true;
        }
        return false;
    }
};

// Device catalog and enumeration service
class SystemCameraCatalog {
public:
    // Enumerate all visible capture devices across Media Foundation and DirectShow.
    // Automatically filters out PrismForge's virtual output devices and deduplicates cross-API routes.
    static EnumerationReport Enumerate();

    // Query native modes by temporarily activating the selected driver, then releasing it
    static std::vector<CaptureMode> QueryCapabilities(const CaptureDevice& device);

    // Normalizes raw device path to extract hardware instance identity (stripping prefixes and category GUIDs)
    static std::string NormalizeDevicePath(const std::string& rawPath);

    // Checks whether an identifier / CLSID represents PrismForge's own virtual camera outputs
    static bool IsPrismForgeOutput(const std::string& pathOrClsid);

    // Deduplicates raw devices from both backends and merges matching hardware identities
    static std::vector<CaptureDevice> DeduplicateAndMerge(
        const std::vector<CaptureDevice>& mfDevices,
        const std::vector<CaptureDevice>& dsDevices);

    // Test seam: inject custom enumeration for deterministic testing
    using CustomEnumerator = std::function<EnumerationReport()>;
    static void SetCustomEnumeratorForTesting(CustomEnumerator enumerator);
};

// Active camera capture source session
class SystemCameraSource {
public:
    // Frame callback: receives a borrowed, immutable AVFrame valid only during the synchronous callback.
    // The consumer must not retain the AVFrame* beyond the return of the callback.
    using FrameCallback = std::function<void(AVFrame* borrowedFrame)>;

    // Status callback: receives asynchronous status updates by value.
    using StatusCallback = std::function<void(const CaptureStatus& status)>;

    SystemCameraSource();
    ~SystemCameraSource();

    SystemCameraSource(const SystemCameraSource&) = delete;
    SystemCameraSource& operator=(const SystemCameraSource&) = delete;

    // Registers callbacks before starting
    void SetFrameCallback(FrameCallback callback);
    void SetStatusCallback(StatusCallback callback);

    // Starts capture on the selected device. If requestedMode is null, a sensible default native mode is negotiated.
    // Returns true if starting process was initiated; capture is only Running once frames start flowing.
    bool Start(const CaptureDevice& device, const CaptureMode* requestedMode = nullptr);

    // Stops capture, cancels pending reads, drains callbacks, and cleans up resources.
    // External Stop drains and joins. Callback Stop requests cancellation without waiting.
    // Synchronous Start/configuration and source destruction from a callback are unsupported.
    void Stop();

    // Queries current capture state (thread-safe)
    CaptureState GetState() const;

    // Queries current capture status snapshot (thread-safe)
    CaptureStatus GetStatus() const;

    // Test seam: backend factory hook for injecting synthetic/mock capture backends
    class ICaptureBackend;
    using BackendFactory = std::function<std::unique_ptr<ICaptureBackend>(
        const DeviceRoute& route,
        const CaptureMode* requestedMode,
        FrameCallback frameCb,
        StatusCallback statusCb)>;
    void SetBackendFactoryForTesting(BackendFactory factory);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Base interface for capture backends (Media Foundation, DirectShow, Mock)
class SystemCameraSource::ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    // Starts capture worker loop
    virtual bool Open(const DeviceRoute& route, const CaptureMode* requestedMode) = 0;

    // Cancels and signals worker loop to terminate immediately
    virtual void Cancel() = 0;

    // Waits for worker to finish and cleans up
    virtual void Close() = 0;

    // Obtains negotiated delivered capture mode
    virtual CaptureMode GetDeliveredMode() const = 0;

    // Obtains negotiated native capture mode
    virtual CaptureMode GetNativeMode() const = 0;

    // Checks whether the calling thread is the backend's worker thread
    virtual bool IsWorkerThread() const = 0;
};

} // namespace PrismForge::Capture
