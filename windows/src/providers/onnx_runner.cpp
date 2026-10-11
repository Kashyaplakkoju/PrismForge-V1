#include "onnx_runner.h"
#include "model_manifest.h"
#include <windows.h>
#include "onnxruntime_c_api.h"
#include <filesystem>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace PrismForge::Providers {
namespace {
using GetApiBaseFn = const OrtApiBase* (ORT_API_CALL*)();
using AppendDmlFn = OrtStatus* (ORT_API_CALL*)(OrtSessionOptions*, int);
// Public OrtDmlApi prefix, resolved through GetExecutionProviderApi("DML", ORT_API_VERSION).
// Only its documented first entry is used; no static provider export dependency.
struct DmlApiPrefix { AppendDmlFn SessionOptionsAppendExecutionProvider_DML; };

template<class T> using Handle = std::unique_ptr<T, void (ORT_API_CALL*)(T*)>;
struct OrtFailure : std::runtime_error {
    OrtErrorCode code;
    OrtFailure(OrtErrorCode c, std::string message) : std::runtime_error(std::move(message)), code(c) {}
};
void Check(const OrtApi* api, OrtStatus* status) {
    if (!status) return;
    const auto code = api->GetErrorCode(status);
    std::string message;
    try { message = api->GetErrorMessage(status); }
    catch (...) { api->ReleaseStatus(status); throw; }
    api->ReleaseStatus(status);
    throw OrtFailure(code, std::move(message));
}
bool ElementCount(const std::vector<int64_t>& shape, size_t& count) {
    if (shape.size() != 4 || shape[0] != 1) return false;
    count = 1;
    for (auto dimension : shape) {
        if (dimension <= 0 || dimension > 8192 ||
            count > (16u * 1024u * 1024u) / static_cast<size_t>(dimension)) return false;
        count *= static_cast<size_t>(dimension);
    }
    return true;
}
std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    return std::filesystem::path(path).parent_path();
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return "<invalid path>";
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}
}

struct OnnxRunner::Impl {
    mutable std::mutex sessionMutex;
    mutable std::mutex cancelMutex;
    HMODULE module = nullptr;
    const OrtApi* api = nullptr;
    AppendDmlFn appendDml = nullptr;
    OrtEnv* env = nullptr;
    OrtMemoryInfo* cpuMemory = nullptr;
    OrtSession* session = nullptr;
    OrtRunOptions* activeOptions = nullptr; // protected by cancelMutex; owned by RunInference stack
    std::atomic<bool> runtimeAvailable{false};
    std::atomic<bool> running{false};
    std::atomic<bool> terminated{false};
    RunnerSessionInfo info;
    std::string diagnostic = "Runtime has not been initialized";
    uint64_t epoch = 0;
    std::wstring requestedRuntimePath;

    ~Impl() {
        // Wrapper must outlive its callers; worker shared state owns it across callback destruction.
        // Every runtime object is released before its per-instance module reference.
        if (api) {
            if (session) api->ReleaseSession(session);
            if (cpuMemory) api->ReleaseMemoryInfo(cpuMemory);
            if (env) api->ReleaseEnv(env);
        }
        if (module) FreeLibrary(module);
    }

    bool EnsureRuntime(const std::wstring& explicitPath) {
        // sessionMutex is held by every caller, including initialization and candidate construction.
        if (runtimeAvailable.load(std::memory_order_acquire)) return true;
        if (!explicitPath.empty()) requestedRuntimePath = explicitPath;
        std::vector<std::filesystem::path> candidates;
        if (!requestedRuntimePath.empty()) {
            std::filesystem::path path(requestedRuntimePath);
            if (!path.is_absolute()) { diagnostic = "Runtime path must be absolute"; return false; }
            DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                path /= L"onnxruntime.dll";
            candidates.push_back(path.lexically_normal());
        } else {
            auto directory = ExecutableDirectory();
            if (directory.empty()) { diagnostic = "Executable directory unavailable"; return false; }
            candidates.push_back(directory / L"onnxruntime.dll");
            candidates.push_back((directory / L"../../build/provider-deps/ort-sdk/bin/onnxruntime.dll").lexically_normal());
            candidates.push_back((directory / L"../provider-deps/ort-sdk/bin/onnxruntime.dll").lexically_normal());
        }
        for (const auto& path : candidates) {
            HMODULE candidate = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!candidate) {
                diagnostic = "Runtime load failed: " + Utf8(path.wstring()) +
                    " (Win32=" + std::to_string(GetLastError()) + ")";
                continue;
            }
            auto getBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(candidate, "OrtGetApiBase"));
            const OrtApiBase* base = getBase ? getBase() : nullptr;
            const OrtApi* candidateApi = (base && base->GetApi) ? base->GetApi(ORT_API_VERSION) : nullptr;
            if (!candidateApi) {
                diagnostic = getBase ? "ORT API version unavailable" : "OrtGetApiBase export missing";
                FreeLibrary(candidate);
                continue;
            }
            OrtEnv* candidateEnv = nullptr;
            OrtMemoryInfo* candidateMemory = nullptr;
            try {
                Check(candidateApi, candidateApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                    "PrismForgeAnalysis", &candidateEnv));
                Check(candidateApi, candidateApi->CreateCpuMemoryInfo(OrtArenaAllocator,
                    OrtMemTypeDefault, &candidateMemory));
                const void* dmlApi = nullptr;
                OrtStatus* dmlStatus = candidateApi->GetExecutionProviderApi("DML", ORT_API_VERSION, &dmlApi);
                if (!dmlStatus && dmlApi)
                    appendDml = static_cast<const DmlApiPrefix*>(dmlApi)->SessionOptionsAppendExecutionProvider_DML;
                else if (dmlStatus) candidateApi->ReleaseStatus(dmlStatus);
                if (!appendDml)
                    appendDml = reinterpret_cast<AppendDmlFn>(GetProcAddress(candidate,
                        "OrtSessionOptionsAppendExecutionProvider_DML"));
                diagnostic = "Loaded " + Utf8(path.wstring()) + ", API=" + std::to_string(ORT_API_VERSION);
                module = candidate;
                api = candidateApi;
                env = candidateEnv;
                cpuMemory = candidateMemory;
                runtimeAvailable.store(true, std::memory_order_release);
                return true;
            } catch (const std::exception& error) {
                diagnostic = std::string("ORT initialization failed: ") + error.what();
            } catch (...) { diagnostic = "ORT initialization failed"; }
            if (candidateMemory) candidateApi->ReleaseMemoryInfo(candidateMemory);
            if (candidateEnv) candidateApi->ReleaseEnv(candidateEnv);
            appendDml = nullptr;
            FreeLibrary(candidate);
        }
        return false;
    }

    bool DirectMLAvailable() {
        char** providers = nullptr;
        int count = 0;
        Check(api, api->GetAvailableProviders(&providers, &count));
        bool found = false;
        for (int i = 0; i < count; ++i)
            if (providers[i] && std::string(providers[i]) == "DmlExecutionProvider") found = true;
        Check(api, api->ReleaseAvailableProviders(providers, count));
        return found;
    }

    void TensorMetadata(OrtSession* candidate, bool input, std::string& name,
        std::vector<int64_t>& shape, size_t& count) {
        OrtAllocator* allocator = nullptr;
        Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
        char* allocatedName = nullptr;
        Check(api, input ? api->SessionGetInputName(candidate, 0, allocator, &allocatedName) :
            api->SessionGetOutputName(candidate, 0, allocator, &allocatedName));
        if (!allocatedName) throw OrtFailure(ORT_INVALID_ARGUMENT, "Empty tensor name");
        try { name = allocatedName; }
        catch (...) { allocator->Free(allocator, allocatedName); throw; }
        allocator->Free(allocator, allocatedName);
        if (name.empty()) throw OrtFailure(ORT_INVALID_ARGUMENT, "Empty tensor name");
        OrtTypeInfo* rawType = nullptr;
        Check(api, input ? api->SessionGetInputTypeInfo(candidate, 0, &rawType) :
            api->SessionGetOutputTypeInfo(candidate, 0, &rawType));
        Handle<OrtTypeInfo> type(rawType, api->ReleaseTypeInfo);
        ONNXType onnxType;
        Check(api, api->GetOnnxTypeFromTypeInfo(type.get(), &onnxType));
        if (onnxType != ONNX_TYPE_TENSOR) throw OrtFailure(ORT_INVALID_ARGUMENT, "Non-tensor value unsupported");
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        Check(api, api->CastTypeInfoToTensorInfo(type.get(), &tensor));
        ONNXTensorElementDataType elementType;
        Check(api, api->GetTensorElementType(tensor, &elementType));
        if (elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw OrtFailure(ORT_INVALID_ARGUMENT, "Only float32 tensors supported");
        size_t rank = 0;
        Check(api, api->GetDimensionsCount(tensor, &rank));
        if (rank != 4) throw OrtFailure(ORT_INVALID_ARGUMENT, "Only fixed rank4 batch1 supported");
        shape.resize(rank);
        Check(api, api->GetDimensions(tensor, shape.data(), rank));
        if (!ElementCount(shape, count))
            throw OrtFailure(ORT_INVALID_ARGUMENT, "Dynamic, oversized or non-batch1 tensor unsupported");
    }

    RunnerStatusCode Load(const std::wstring* path, const void* data, size_t bytes,
        ExecutionProviderType requested, int device, const std::string& identifier,
        const ModelManifest* contract) {
        if (device < 0 || (requested != ExecutionProviderType::CPU && requested != ExecutionProviderType::DirectML) ||
            (contract && !contract->Validate())) return RunnerStatusCode::InvalidConfiguration;
        if (!EnsureRuntime(L"")) return RunnerStatusCode::MissingRuntime;
        try {
            RunnerSessionInfo candidateInfo;
            candidateInfo.requestedProvider = requested;
            candidateInfo.modelIdentifier = identifier;
            auto create = [&](ExecutionProviderType selected) {
                OrtSessionOptions* rawOptions = nullptr;
                Check(api, api->CreateSessionOptions(&rawOptions));
                Handle<OrtSessionOptions> options(rawOptions, api->ReleaseSessionOptions);
                Check(api, api->SetSessionGraphOptimizationLevel(options.get(), ORT_ENABLE_ALL));
                Check(api, api->SetSessionExecutionMode(options.get(), ORT_SEQUENTIAL));
                if (selected == ExecutionProviderType::DirectML) {
                    if (!appendDml || !DirectMLAvailable())
                        throw OrtFailure(ORT_FAIL, "DirectML provider API or advertised provider unavailable");
                    Check(api, api->DisableMemPattern(options.get()));
                    Check(api, appendDml(options.get(), device));
                } else Check(api, api->SetIntraOpNumThreads(options.get(), 2));
                OrtSession* candidate = nullptr;
                Check(api, path ? api->CreateSession(env, path->c_str(), options.get(), &candidate) :
                    api->CreateSessionFromArray(env, data, bytes, options.get(), &candidate));
                return Handle<OrtSession>(candidate, api->ReleaseSession);
            };
            Handle<OrtSession> candidate(nullptr, api->ReleaseSession);
            if (requested == ExecutionProviderType::DirectML) {
                try {
                    candidate = create(ExecutionProviderType::DirectML);
                    candidateInfo.activeProvider = ExecutionProviderType::DirectML;
                    candidateInfo.deviceId = device;
                } catch (const OrtFailure& error) {
                    candidateInfo.fallbackOccurred = true;
                    candidateInfo.fallbackReason = std::string("DirectML session failed: ") + error.what();
                    candidate = create(ExecutionProviderType::CPU);
                    candidateInfo.activeProvider = ExecutionProviderType::CPU;
                    candidateInfo.deviceId = -1;
                }
            } else {
                candidate = create(ExecutionProviderType::CPU);
                candidateInfo.activeProvider = ExecutionProviderType::CPU;
                candidateInfo.deviceId = -1;
            }
            size_t inputs = 0, outputs = 0, overridable = 0;
            Check(api, api->SessionGetInputCount(candidate.get(), &inputs));
            Check(api, api->SessionGetOutputCount(candidate.get(), &outputs));
            Check(api, api->SessionGetOverridableInitializerCount(candidate.get(), &overridable));
            if (inputs != 1 || outputs != 1 || overridable != 0) return RunnerStatusCode::InvalidConfiguration;
            TensorMetadata(candidate.get(), true, candidateInfo.inputName, candidateInfo.inputShape,
                candidateInfo.inputElementCount);
            TensorMetadata(candidate.get(), false, candidateInfo.outputName, candidateInfo.outputShape,
                candidateInfo.outputElementCount);
            if (contract && (contract->inputTensorName != candidateInfo.inputName ||
                contract->outputTensorName != candidateInfo.outputName ||
                contract->inputDims != candidateInfo.inputShape || contract->outputDims != candidateInfo.outputShape))
                return RunnerStatusCode::InvalidConfiguration;
            candidateInfo.isLoaded = true;
            candidateInfo.modelEpoch = epoch + 1;
            // Move metadata before session swap: allocation failure must leave the prior pair intact.
            info = std::move(candidateInfo);
            OrtSession* prior = session;
            session = candidate.release();
            ++epoch;
            if (prior) api->ReleaseSession(prior);
            return RunnerStatusCode::Success;
        } catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
        catch (const OrtFailure& failure) {
            return failure.code == ORT_INVALID_ARGUMENT ? RunnerStatusCode::InvalidConfiguration : RunnerStatusCode::CorruptModel;
        } catch (...) { return RunnerStatusCode::CorruptModel; }
    }
};

OnnxRunner::OnnxRunner() : m_impl(std::make_unique<Impl>()) {}
OnnxRunner::~OnnxRunner() { UnloadModel(); }
bool OnnxRunner::Initialize(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex);
    try { return m_impl->EnsureRuntime(path); }
    catch (...) { m_impl->diagnostic = "Runtime loader allocation or path failure"; return false; }
}
bool OnnxRunner::IsRuntimeAvailable() const { return m_impl->runtimeAvailable.load(std::memory_order_acquire); }
std::string OnnxRunner::GetRuntimeDiagnostic() const {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex); return m_impl->diagnostic;
}
RunnerStatusCode OnnxRunner::LoadModel(const std::wstring& path, ExecutionProviderType provider,
    int device, const ModelManifest* contract) {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return RunnerStatusCode::ModelNotFound;
    const uint64_t bytes = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    if (bytes > 256u * 1024u * 1024u) return RunnerStatusCode::InvalidConfiguration;
    try { return m_impl->Load(&path, nullptr, 0, provider, device, Utf8(path), contract); }
    catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
    catch (...) { return RunnerStatusCode::InvalidConfiguration; }
}
RunnerStatusCode OnnxRunner::LoadModelFromMemory(const void* data, size_t size, ExecutionProviderType provider,
    int device, const std::string& identifier, const ModelManifest* contract) {
    if (!data || !size || size > 256u * 1024u * 1024u) return RunnerStatusCode::InvalidConfiguration;
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex);
    try { return m_impl->Load(nullptr, data, size, provider, device, identifier, contract); }
    catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
    catch (...) { return RunnerStatusCode::InvalidConfiguration; }
}
void OnnxRunner::UnloadModel() {
    CancelActiveRun();
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex);
    if (m_impl->session) m_impl->api->ReleaseSession(m_impl->session);
    m_impl->session = nullptr;
    m_impl->info = RunnerSessionInfo{};
    ++m_impl->epoch; // invalidate any preprocessing snapshot, even if there was no session
}
void OnnxRunner::CancelActiveRun() noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_impl->cancelMutex);
        if (!m_impl->activeOptions) return;
        m_impl->terminated.store(true, std::memory_order_release);
        OrtStatus* status = m_impl->api->RunOptionsSetTerminate(m_impl->activeOptions);
        if (status) m_impl->api->ReleaseStatus(status);
    } catch (...) { /* noexcept cancellation/destruction boundary */ }
}
bool OnnxRunner::IsInferenceRunning() const noexcept { return m_impl->running.load(std::memory_order_acquire); }
RunnerStatusCode OnnxRunner::RunInference(const std::vector<float>& input, std::vector<float>& output,
    const std::atomic<bool>* signal, uint64_t expectedEpoch) {
    // Errors never expose stale/partially copied output.
    output.clear();
    if (signal && signal->load(std::memory_order_acquire)) return RunnerStatusCode::Cancelled;
    std::lock_guard<std::mutex> sessionLock(m_impl->sessionMutex);
    if (!m_impl->runtimeAvailable.load()) return RunnerStatusCode::MissingRuntime;
    if (!m_impl->session) return RunnerStatusCode::ModelNotFound;
    if (expectedEpoch && expectedEpoch != m_impl->info.modelEpoch) return RunnerStatusCode::Cancelled;
    if (input.size() != m_impl->info.inputElementCount) return RunnerStatusCode::TensorShapeMismatch;
    if (!std::all_of(input.begin(), input.end(), [](float value) { return std::isfinite(value); }))
        return RunnerStatusCode::InvalidConfiguration;
    const OrtApi* api = m_impl->api;
    try {
        OrtValue* rawInput = nullptr;
        Check(api, api->CreateTensorWithDataAsOrtValue(m_impl->cpuMemory,
            const_cast<float*>(input.data()), input.size() * sizeof(float), m_impl->info.inputShape.data(),
            m_impl->info.inputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &rawInput));
        Handle<OrtValue> inputValue(rawInput, api->ReleaseValue);
        OrtRunOptions* rawOptions = nullptr;
        Check(api, api->CreateRunOptions(&rawOptions));
        Handle<OrtRunOptions> options(rawOptions, api->ReleaseRunOptions);
        struct Registration {
            Impl* impl;
            Registration(Impl* state, OrtRunOptions* options) : impl(state) {
                std::lock_guard<std::mutex> lock(impl->cancelMutex);
                impl->terminated.store(false, std::memory_order_release);
                impl->activeOptions = options;
                impl->running.store(true, std::memory_order_release);
            }
            ~Registration() {
                std::lock_guard<std::mutex> lock(impl->cancelMutex);
                impl->running.store(false, std::memory_order_release);
                impl->activeOptions = nullptr;
            }
        } registration(m_impl.get(), options.get());
        if (signal && signal->load(std::memory_order_acquire)) return RunnerStatusCode::Cancelled;
        const char* inputName = m_impl->info.inputName.c_str();
        const char* outputName = m_impl->info.outputName.c_str();
        const OrtValue* inputPointer = inputValue.get();
        OrtValue* rawOutput = nullptr;
        OrtStatus* runStatus = api->Run(m_impl->session, options.get(), &inputName, &inputPointer, 1,
            &outputName, 1, &rawOutput);
        Handle<OrtValue> outputValue(rawOutput, api->ReleaseValue);
        Check(api, runStatus);
        if (m_impl->terminated.load() || (signal && signal->load())) return RunnerStatusCode::Cancelled;
        if (!outputValue) return RunnerStatusCode::InferenceFailed;
        OrtTensorTypeAndShapeInfo* rawTensorInfo = nullptr;
        Check(api, api->GetTensorTypeAndShape(outputValue.get(), &rawTensorInfo));
        Handle<OrtTensorTypeAndShapeInfo> tensorInfo(rawTensorInfo, api->ReleaseTensorTypeAndShapeInfo);
        size_t count = 0, rank = 0;
        ONNXTensorElementDataType type;
        Check(api, api->GetTensorElementType(tensorInfo.get(), &type));
        Check(api, api->GetDimensionsCount(tensorInfo.get(), &rank));
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != 4) return RunnerStatusCode::TensorShapeMismatch;
        std::vector<int64_t> shape(rank);
        Check(api, api->GetDimensions(tensorInfo.get(), shape.data(), rank));
        if (shape != m_impl->info.outputShape || !ElementCount(shape, count) || count != m_impl->info.outputElementCount)
            return RunnerStatusCode::TensorShapeMismatch;
        void* data = nullptr;
        Check(api, api->GetTensorMutableData(outputValue.get(), &data));
        if (!data) return RunnerStatusCode::InferenceFailed;
        const auto* floats = static_cast<const float*>(data);
        if (!std::all_of(floats, floats + count, [](float value) { return std::isfinite(value); }))
            return RunnerStatusCode::NonFiniteOutput;
        std::vector<float> completed(floats, floats + count);
        if (m_impl->terminated.load() || (signal && signal->load())) return RunnerStatusCode::Cancelled;
        output.swap(completed);
        return RunnerStatusCode::Success;
    } catch (const std::bad_alloc&) { return RunnerStatusCode::OutOfMemory; }
    catch (...) {
        if (m_impl->terminated.load() || (signal && signal->load())) return RunnerStatusCode::Cancelled;
        return RunnerStatusCode::InferenceFailed;
    }
}
RunnerSessionInfo OnnxRunner::GetSessionInfo() const {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex); return m_impl->info;
}
bool OnnxRunner::IsModelLoaded() const {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex); return m_impl->session && m_impl->info.isLoaded;
}
uint64_t OnnxRunner::GetModelEpoch() const {
    std::lock_guard<std::mutex> lock(m_impl->sessionMutex); return m_impl->epoch;
}
} // namespace PrismForge::Providers