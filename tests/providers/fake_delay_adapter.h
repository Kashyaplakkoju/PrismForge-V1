#pragma once

#include "providers/onnx_runner.h"
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace PrismForge::Tests {

/// <summary>
/// EXPLICITLY LABELLED FAKE DELAY & FAULT INJECTION ADAPTER
/// 
/// Used exclusively for lifecycle, async queue bounded replacement, cancellation,
/// and error-recovery unit tests. NEVER claims real model inference.
/// </summary>
class FakeDelayAdapter {
public:
    int artificialDelayMs = 0;
    std::atomic<uint64_t> callCount{ 0 };
    std::atomic<int> failuresToInject{ 0 };
    bool injectNonFinite = false;
    bool injectOOM = false;
    std::atomic<bool> hold{ false };

    void Reset() {
        artificialDelayMs = 0;
        callCount.store(0);
        failuresToInject.store(0);
        injectNonFinite = false;
        injectOOM = false;
    }

    Providers::RunnerStatusCode Execute(
        const std::vector<float>& inputTensor,
        std::vector<float>& outputTensor,
        const std::atomic<bool>* cancelToken
    ) {
        callCount++;

        if (injectOOM) {
            throw std::bad_alloc();
        }

        if (failuresToInject.load() > 0) {
            failuresToInject--;
            return Providers::RunnerStatusCode::InferenceFailed;
        }

        while (hold.load()) {
            if (cancelToken && cancelToken->load()) return Providers::RunnerStatusCode::Cancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Interruptible delay loop
        if (artificialDelayMs > 0) {
            auto startTime = std::chrono::steady_clock::now();
            while (true) {
                if (cancelToken && cancelToken->load()) {
                    return Providers::RunnerStatusCode::Cancelled;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime
                ).count();
                if (elapsed >= artificialDelayMs) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        if (cancelToken && cancelToken->load()) {
            return Providers::RunnerStatusCode::Cancelled;
        }

        // Produce a deterministic mock 1-channel output (16x16 = 256 floats)
        size_t outSize = 256;
        outputTensor.resize(outSize);

        float fillVal = 0.75f;
        if (injectNonFinite) {
            fillVal = std::numeric_limits<float>::quiet_NaN();
        }

        for (size_t i = 0; i < outSize; ++i) {
            outputTensor[i] = fillVal;
        }

        return Providers::RunnerStatusCode::Success;
    }
};

} // namespace PrismForge::Tests
