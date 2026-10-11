#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>

namespace PrismForge::Providers {
struct ModelManifest;
enum class ExecutionProviderType { CPU = 0, DirectML = 1 };
enum class RunnerStatusCode {
    Success = 0, MissingRuntime = 1, ModelNotFound = 2, CorruptModel = 3,
    InvalidConfiguration = 4, InferenceFailed = 5, TensorShapeMismatch = 6,
    NonFiniteOutput = 7, Cancelled = 8, OutOfMemory = 9
};
struct RunnerSessionInfo {
    bool isLoaded = false;
    ExecutionProviderType requestedProvider = ExecutionProviderType::CPU;
    ExecutionProviderType activeProvider = ExecutionProviderType::CPU;
    bool fallbackOccurred = false;
    std::string fallbackReason;
    int deviceId = 0; // selected DML adapter; -1 for CPU
    std::string modelIdentifier;
    uint64_t modelEpoch = 0;
    std::string inputName, outputName;
    std::vector<int64_t> inputShape, outputShape;
    size_t inputElementCount = 0, outputElementCount = 0;
};

// Optional, per-instance C API loader. No process-wide Ort::InitApi state or ORT import.
// Supported contract: exactly one float32 input/output, fixed rank4, batch1, bounded tensors.
class OnnxRunner {
public:
    OnnxRunner();
    ~OnnxRunner();
    OnnxRunner(const OnnxRunner&) = delete;
    OnnxRunner& operator=(const OnnxRunner&) = delete;

    // Explicit paths are authoritative and must be absolute; never fall back from a bad one.
    // Default searches only executable-local and executable-relative worktree SDK paths.
    bool Initialize(const std::wstring& customDllPath = L"");
    bool IsRuntimeAvailable() const;
    std::string GetRuntimeDiagnostic() const;
    RunnerStatusCode LoadModel(const std::wstring& modelPath,
        ExecutionProviderType provider = ExecutionProviderType::CPU, int deviceId = 0,
        const ModelManifest* contract = nullptr);
    RunnerStatusCode LoadModelFromMemory(const void* modelData, size_t modelDataSize,
        ExecutionProviderType provider = ExecutionProviderType::CPU, int deviceId = 0,
        const std::string& modelIdentifier = "memory_buffer", const ModelManifest* contract = nullptr);
    void UnloadModel();
    // Native RunOptionsSetTerminate. Providers can finish an already executing kernel before
    // observing termination. No per-frame monitor thread. Caller signals atomic + this method.
    void CancelActiveRun() noexcept;
    bool IsInferenceRunning() const noexcept;
    RunnerStatusCode RunInference(const std::vector<float>& inputData, std::vector<float>& outputData,
        const std::atomic<bool>* cancelSignal = nullptr, uint64_t expectedModelEpoch = 0);
    RunnerSessionInfo GetSessionInfo() const;
    bool IsModelLoaded() const;
    uint64_t GetModelEpoch() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace PrismForge::Providers