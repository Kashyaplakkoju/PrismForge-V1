#pragma once
#include "face_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace PrismForge::Providers {
// Native, synchronous MediaPipe 0.10.32 CPU inference. The owner schedules these
// calls on its bounded analysis worker, never on the capture/render thread.
// The exact reviewed Windows x64 DLL hash is checked before any code is loaded.
class FaceLandmarkerRuntime {
public:
    FaceLandmarkerRuntime();
    ~FaceLandmarkerRuntime();
    FaceLandmarkerRuntime(const FaceLandmarkerRuntime&) = delete;
    FaceLandmarkerRuntime& operator=(const FaceLandmarkerRuntime&) = delete;
    bool Initialize(const std::wstring& runtimeDllPath, const std::wstring& modelPath,
                    std::string& error);
    bool DetectRGB(const uint8_t* data, size_t capacityBytes, int width, int height,
                   int strideBytes, FaceFrameTags tags, FaceObservation& observation,
                   std::string& error);
    // Clear tracking history while retaining the owned model/runtime. No inference.
    void Reset();
    // Report exceptional native teardown failure; callers must not advertise Ready.
    bool Reset(std::string& error);
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace PrismForge::Providers
