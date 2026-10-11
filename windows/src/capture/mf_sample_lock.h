#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <climits>
#include <cstdint>
#include "systemcamera.h"

namespace PrismForge::Capture {
struct MfSampleView {
    const uint8_t* data = nullptr; // Lowest-address row, not scanline zero.
    size_t size = 0;
    int pitch = 0;
};

// Validate the actual accessible allocation before normalizing a signed 2D view.
inline bool MakeMfSampleView(const uint8_t* scanline0, const uint8_t* allocation,
    size_t allocationSize, int pitch, const CaptureMode& mode, MfSampleView& out)
{
    out = {};
    if (!scanline0 || !allocation || pitch == 0 || pitch == INT_MIN ||
        mode.width <= 0 || mode.height <= 0 || mode.width > 16384 || mode.height > 16384) return false;
    const auto stride = static_cast<size_t>(pitch < 0 ? -pitch : pitch);
    size_t rowBytes = 0;
    if (mode.pixelFormat == "RGB24") rowBytes = static_cast<size_t>(mode.width) * 3;
    else if (mode.pixelFormat == "RGB32") rowBytes = static_cast<size_t>(mode.width) * 4;
    else if (mode.pixelFormat == "YUY2" && mode.width % 2 == 0) rowBytes = static_cast<size_t>(mode.width) * 2;
    else if (mode.pixelFormat == "NV12" && mode.width % 2 == 0 && mode.height % 2 == 0 && pitch > 0) rowBytes = mode.width;
    else return false;
    if (stride < rowBytes || stride > SIZE_MAX / static_cast<size_t>(mode.height + mode.height / 2)) return false;
    const auto first = reinterpret_cast<uintptr_t>(scanline0);
    const auto start = reinterpret_cast<uintptr_t>(allocation);
    if (first < start || first - start > allocationSize) return false;
    size_t offset = static_cast<size_t>(first - start);
    const size_t lastRowOffset = stride * static_cast<size_t>(mode.height - 1);
    if (pitch < 0) {
        if (offset < lastRowOffset) return false;
        offset -= lastRowOffset;
    }
    size_t needed = lastRowOffset + rowBytes;
    if (mode.pixelFormat == "NV12") {
        needed = stride * mode.height + stride * (mode.height / 2 - 1) + rowBytes;
    }
    if (needed > allocationSize - offset) return false;
    out.data = allocation + offset;
    out.size = allocationSize - offset;
    out.pitch = pitch;
    return true;
}

class MfSampleLock {
    IMFMediaBuffer* buffer = nullptr; // Borrowed until this lock is destroyed.
    Microsoft::WRL::ComPtr<IMF2DBuffer2> surface;
    bool locked2D = false, lockedLinear = false;
public:
    MfSampleView view;
    void Unlock() {
        if (locked2D) surface->Unlock2D();
        if (lockedLinear) buffer->Unlock();
        locked2D = lockedLinear = false;
        surface.Reset(); buffer = nullptr; view = {};
    }
    ~MfSampleLock() { Unlock(); }
    MfSampleLock() = default;
    MfSampleLock(const MfSampleLock&) = delete;
    MfSampleLock& operator=(const MfSampleLock&) = delete;
    bool Lock(IMFMediaBuffer* input, const CaptureMode& mode, LONG defaultStride, bool compressed) {
        if (!input || buffer) return false;
        if (!compressed && (mode.width <= 0 || mode.width > 16384 || mode.height <= 0 || mode.height > 16384)) return false;
        buffer = input;
        if (!compressed && SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(surface.GetAddressOf())))) {
            BYTE* row = nullptr; BYTE* start = nullptr; DWORD length = 0; LONG stride = 0;
            if (SUCCEEDED(surface->Lock2DSize(MF2DBuffer_LockFlags_Read, &row, &stride, &start, &length))) {
                locked2D = true;
                return MakeMfSampleView(row, start, length, stride, mode, view);
            }
        }
        BYTE* data = nullptr; DWORD maximum = 0, current = 0;
        if (FAILED(input->Lock(&data, &maximum, &current))) return false;
        lockedLinear = true;
        if (!data || !current || current > maximum) return false;
        if (compressed) { view = {data, current, 0}; return true; }
        // A linear buffer's origin is the beginning of the packed allocation.
        // The negotiated default stride supplies its orientation and row padding.
        if (!defaultStride || defaultStride == LONG_MIN) return false;
        const size_t stride = static_cast<size_t>(defaultStride < 0 ? -defaultStride : defaultStride);
        if (stride > SIZE_MAX / static_cast<size_t>(mode.height)) return false;
        size_t firstOffset = defaultStride < 0 ? stride * static_cast<size_t>(mode.height - 1) : 0;
        if (mode.height <= 0 || mode.height > 16384 || firstOffset > current) return false;
        return MakeMfSampleView(data + firstOffset, data, current, defaultStride, mode, view);
    }
};
}
