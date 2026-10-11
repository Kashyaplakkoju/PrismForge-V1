#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace PrismForge::Capture {
// Owns at most one outstanding sample; cancellation wakes the camera worker.
// Reader callbacks hold this state independently of backend destruction.
struct MfReadState {
    std::mutex mutex;
    std::condition_variable ready;
    bool cancelled = false, available = false;
    HRESULT result = S_OK;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;
    ~MfReadState() { if (sample) sample->Release(); }
    void Cancel() {
        { std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
        ready.notify_all();
    }
    bool Take(HRESULT& hr, DWORD& outFlags, LONGLONG& pts, IMFSample*& outSample) {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this] { return cancelled || available; });
        if (cancelled) return false;
        hr = result; outFlags = flags; pts = timestamp;
        outSample = sample; sample = nullptr; available = false;
        return true;
    }
};
class MfReaderCallback final : public IMFSourceReaderCallback {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<MfReadState> state;
    ~MfReaderCallback() = default;
public:
    explicit MfReaderCallback(std::shared_ptr<MfReadState> value) : state(std::move(value)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
        *result = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG pts, IMFSample* sample) override {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled) return S_OK;
            if (state->sample) state->sample->Release();
            if (sample) sample->AddRef();
            state->sample = sample;
            state->result = hr; state->flags = flags; state->timestamp = pts;
            state->available = true;
        }
        state->ready.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { state->Cancel(); return S_OK; }
};
}
