#include <windows.h>
#include "onnxruntime_c_api.h"

#if defined(PRISM_RUNTIME_WRONG_EXPORT)
extern "C" __declspec(dllexport) int UnrelatedExport() { return 1; }
#elif defined(PRISM_RUNTIME_WRONG_VERSION)
static const OrtApi* ORT_API_CALL WrongApi(uint32_t) noexcept { return nullptr; }
static const char* ORT_API_CALL Version() noexcept { return "incompatible-test-fixture"; }
#pragma comment(linker, "/export:OrtGetApiBase")
extern "C" const OrtApiBase* ORT_API_CALL OrtGetApiBase() NO_EXCEPTION {
    static const OrtApiBase base = { WrongApi, Version }; return &base;
}
#elif defined(PRISM_RUNTIME_MISSING_DEPENDENCY)
extern "C" __declspec(dllexport) int RequiredTestDependency() { return 1; }
#elif defined(PRISM_RUNTIME_REQUIRES_DEPENDENCY)
extern "C" __declspec(dllimport) int RequiredTestDependency();
#pragma comment(linker, "/export:OrtGetApiBase")
extern "C" const OrtApiBase* ORT_API_CALL OrtGetApiBase() NO_EXCEPTION {
    RequiredTestDependency(); return nullptr;
}
#else
#include "providers/onnx_runner.h"
#include "providers/model_manifest.h"
#include "tiny_onnx_fixture.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>
#include <limits>
#include <functional>
using namespace PrismForge::Providers;
using namespace PrismForge::Tests;
namespace {
int assertions = 0, failures = 0, groups = 0;
#define CHECK(condition) do { ++assertions; if (!(condition)) { ++failures; std::cerr << "FAIL:" << __LINE__ << " " << #condition << '\n'; } } while (0)
void Group(const char* title) { ++groups; std::cout << "[GROUP] " << title << '\n'; }
std::wstring DllDirectory() {
    std::wstring value(32768, L'\0'); DWORD count = GetDllDirectoryW(static_cast<DWORD>(value.size()), value.data());
    if (count >= value.size()) return L"<unexpected>"; value.resize(count); return value;
}
std::wstring CurrentDirectory() {
    std::wstring value(32768, L'\0'); DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(value.size()), value.data());
    if (!count || count >= value.size()) return L"<unexpected>"; value.resize(count); return value;
}
std::wstring ExecutablePath() {
    std::wstring value(32768, L'\0'); DWORD count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) return L""; value.resize(count); return value;
}
int ChildProbe(const std::wstring& path, const std::string& expected) {
    auto before = DllDirectory(); auto current = CurrentDirectory();
    OnnxRunner runner;
    CHECK(!runner.IsRuntimeAvailable());
    CHECK(!runner.Initialize(path));
    CHECK(!runner.IsRuntimeAvailable());
    CHECK(runner.GetRuntimeDiagnostic().find(expected) != std::string::npos);
    std::vector<float> output{1.0f};
    CHECK(runner.RunInference(std::vector<float>(768), output) == RunnerStatusCode::MissingRuntime);
    CHECK(output.empty());
    CHECK(DllDirectory() == before);
    CHECK(CurrentDirectory() == current);
    auto manifest = TinyOnnxFixture::CreateManifest();
    CHECK(runner.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize,
        ExecutionProviderType::CPU, 0, "missing", &manifest) == RunnerStatusCode::MissingRuntime);
    std::cout << "[CHILD] assertions=" << assertions << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
int Spawn(const std::wstring& arguments) {
    std::wstring executable = ExecutablePath();
    std::wstring command = L"\"" + executable + L"\" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return -1;
    DWORD result = WaitForSingleObject(process.hProcess, 10000);
    if (result != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 1000); }
    DWORD code = 124; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
// Original protobuf test graphs; no external weights, ONNX packages or pretrained model download.
using Bytes = std::vector<uint8_t>;
void Varint(Bytes& out, uint64_t value) { do { uint8_t byte = value & 127; value >>= 7; out.push_back(byte | (value ? 128 : 0)); } while (value); }
void Integer(Bytes& out, unsigned field, uint64_t value) { Varint(out, field * 8); Varint(out, value); }
void Blob(Bytes& out, unsigned field, const Bytes& bytes) { Varint(out, field * 8 + 2); Varint(out, bytes.size()); out.insert(out.end(), bytes.begin(), bytes.end()); }
void String(Bytes& out, unsigned field, const std::string& text) { Blob(out, field, Bytes(text.begin(), text.end())); }
Bytes Value(const std::string& name, const std::vector<int64_t>& dimensions, int elementType = 1) {
    Bytes shape; for (auto dim : dimensions) { Bytes dimension; Integer(dimension, 1, dim); Blob(shape, 1, dimension); }
    Bytes tensor; Integer(tensor, 1, elementType); Blob(tensor, 2, shape);
    Bytes type; Blob(type, 1, tensor); Bytes value; String(value, 1, name); Blob(value, 2, type); return value;
}
Bytes Identity(const std::vector<int64_t>& shape, int type = 1, bool secondOutput = false) {
    Bytes node; String(node, 1, "input"); String(node, 2, "output"); String(node, 4, "Identity");
    Bytes graph; Blob(graph, 1, node); String(graph, 2, "OriginalIdentityContractFixture");
    Blob(graph, 11, Value("input", shape, type)); Blob(graph, 12, Value("output", shape, type));
    if (secondOutput) Blob(graph, 12, Value("input", shape, type));
    Bytes model; Integer(model, 1, 8); Blob(model, 7, graph); Bytes opset; Integer(opset, 2, 17); Blob(model, 8, opset); return model;
}
Bytes HeavyMatMul(int side, int stages) {
    Bytes graph; String(graph, 2, "OriginalCancellationMatMulFixture");
    for (int index = 0; index < stages; ++index) {
        Bytes node; String(node, 1, index ? "stage" + std::to_string(index - 1) : "input");
        String(node, 1, "weight"); String(node, 2, index + 1 == stages ? "output" : "stage" + std::to_string(index));
        String(node, 4, "MatMul"); Blob(graph, 1, node);
    }
    Bytes tensor; Integer(tensor, 1, side); Integer(tensor, 1, side); Integer(tensor, 2, 1); String(tensor, 8, "weight");
    std::vector<float> identity(static_cast<size_t>(side) * side, 0.0f);
    for (int i = 0; i < side; ++i) identity[static_cast<size_t>(i) * side + i] = 1.0f;
    const auto* raw = reinterpret_cast<const uint8_t*>(identity.data());
    Blob(tensor, 9, Bytes(raw, raw + identity.size() * sizeof(float))); Blob(graph, 5, tensor);
    Blob(graph, 11, Value("input", {1, 1, side, side})); Blob(graph, 12, Value("output", {1, 1, side, side}));
    Bytes model; Integer(model, 1, 8); Blob(model, 7, graph); Bytes opset; Integer(opset, 2, 17); Blob(model, 8, opset); return model;
}
void LoadTiny(OnnxRunner& runner, const ModelManifest* manifest = nullptr) {
    CHECK(runner.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize,
        ExecutionProviderType::CPU, 0, "original-tiny-fixture", manifest) == RunnerStatusCode::Success);
}
void ExpectUnchanged(OnnxRunner& runner, uint64_t epoch) {
    CHECK(runner.GetModelEpoch() == epoch); CHECK(runner.IsModelLoaded());
    std::vector<float> output;
    CHECK(runner.RunInference(std::vector<float>(768, 0.25f), output) == RunnerStatusCode::Success);
    CHECK(output.size() == 256 && std::abs(output[0] - 0.25f) < 1e-5f);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--failure-child") {
        std::string expected; for (wchar_t ch : std::wstring(argv[3])) expected.push_back(static_cast<char>(ch));
        return ChildProbe(argv[2], expected);
    }
    std::wstring runtime, fixtures;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::wstring(argv[i]) == L"--runtime") runtime = argv[i + 1];
        else if (std::wstring(argv[i]) == L"--fixtures") fixtures = argv[i + 1];
        else return 2;
    }
    if (runtime.empty() || fixtures.empty()) { std::cerr << "--runtime and --fixtures required\n"; return 2; }
    Group("optional runtime: absent/wrong export/version/dependency isolated child processes");
    const auto probe = [&](const wchar_t* file, const wchar_t* diagnostic) {
        auto path = (std::filesystem::path(fixtures) / file).wstring();
        if (std::wstring(file) == L"absent/onnxruntime.dll") CHECK(!std::filesystem::exists(path));
        else CHECK(std::filesystem::is_regular_file(path));
        int code = Spawn(L"--failure-child \"" + path + L"\" \"" + diagnostic + L"\"");
        CHECK(code == 0);
        std::wcout << L"[PROBE] " << file << L" child_exit=" << code << L'\n';
    };
    CHECK(std::filesystem::is_regular_file(std::filesystem::path(fixtures) / L"dependency-build/missing-dependency.dll"));
    CHECK(!std::filesystem::exists(std::filesystem::path(fixtures) / L"missing-dependency.dll"));
    probe(L"absent/onnxruntime.dll", L"Runtime load failed");
    probe(L"wrong-export.dll", L"export missing");
    probe(L"wrong-version.dll", L"version unavailable");
    probe(L"requires-dependency.dll", L"Runtime load failed");
    Group("explicit relative path rejected; missing load retry; global DLL and working directories stable");
    auto directoryBefore = DllDirectory(); auto workingBefore = CurrentDirectory();
    OnnxRunner runner;
    CHECK(!runner.Initialize(L"relative/onnxruntime.dll"));
    CHECK(runner.Initialize(runtime)); CHECK(runner.IsRuntimeAvailable());
    CHECK(DllDirectory() == directoryBefore && CurrentDirectory() == workingBefore);
    auto manifest = TinyOnnxFixture::CreateManifest(); LoadTiny(runner, &manifest);
    Group("independent per-instance API ownership survives another runner destruction");
    { OnnxRunner other; CHECK(other.Initialize(runtime)); LoadTiny(other, &manifest); }
    ExpectUnchanged(runner, runner.GetModelEpoch());
    Group("transactional manifest input/output names, dimensions and layout mismatch preserve old pair");
    const uint64_t epoch = runner.GetModelEpoch();
    for (int change = 0; change < 4; ++change) {
        auto bad = manifest;
        if (change == 0) bad.inputTensorName = "wrong-input";
        if (change == 1) bad.outputTensorName = "wrong-output";
        if (change == 2) bad.inputDims[2] = 8;
        if (change == 3) bad.inputLayout = TensorLayout::NHWC;
        CHECK(runner.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize,
            ExecutionProviderType::CPU, 0, "invalid", &bad) == RunnerStatusCode::InvalidConfiguration);
        ExpectUnchanged(runner, epoch);
    }
    Group("fixed rank4 batch1 float32 one-output model contract; old model survives rejected candidates");
    for (auto bytes : {Identity({1,3,16,16}, 7), Identity({2,3,16,16}), Identity({1,3,16}),
            Identity({1,3,16,16}, 1, true), Identity({0,3,16,16}), Identity({1,3,8193,16})}) {
        CHECK(runner.LoadModelFromMemory(bytes.data(), bytes.size()) == RunnerStatusCode::InvalidConfiguration);
        ExpectUnchanged(runner, epoch);
    }
    Group("epoch mismatch, cancellation, nonfinite input and malformed input clear stale output");
    std::vector<float> output{1.0f}; std::vector<float> input(768, 0.5f);
    CHECK(runner.RunInference(input, output, nullptr, epoch + 1) == RunnerStatusCode::Cancelled); CHECK(output.empty());
    std::atomic<bool> cancelled{true}; output = {1.0f};
    CHECK(runner.RunInference(input, output, &cancelled) == RunnerStatusCode::Cancelled); CHECK(output.empty());
    input[0] = std::numeric_limits<float>::infinity(); output = {1.0f};
    CHECK(runner.RunInference(input, output) == RunnerStatusCode::InvalidConfiguration); CHECK(output.empty());
    output = {1.0f}; CHECK(runner.RunInference({}, output) == RunnerStatusCode::TensorShapeMismatch); CHECK(output.empty());
    Group("real DirectML session or explicit CPU fallback; invalid adapter fallback reports CPU device");
    OnnxRunner dml; CHECK(dml.Initialize(runtime));
    CHECK(dml.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize,
        ExecutionProviderType::DirectML, 0, "DML-original-fixture", &manifest) == RunnerStatusCode::Success);
    auto info = dml.GetSessionInfo(); CHECK(info.isLoaded);
    CHECK(info.activeProvider == ExecutionProviderType::DirectML || (info.activeProvider == ExecutionProviderType::CPU && info.fallbackOccurred && !info.fallbackReason.empty()));
    CHECK(dml.RunInference(std::vector<float>(768, 0.5f), output) == RunnerStatusCode::Success);
    CHECK(output.size() == 256 && std::abs(output[0] - 0.5f) < 1e-5f);
    std::cout << "[PROVIDER] selected=" << (info.activeProvider == ExecutionProviderType::DirectML ? "DirectML" : "CPU") << " reason=" << info.fallbackReason << '\n';
    CHECK(dml.LoadModelFromMemory(TinyOnnxFixture::GetModelBytes(), TinyOnnxFixture::kModelByteSize,
        ExecutionProviderType::DirectML, std::numeric_limits<int>::max(), "invalid-adapter", &manifest) == RunnerStatusCode::Success);
    info = dml.GetSessionInfo(); CHECK(info.activeProvider == ExecutionProviderType::CPU && info.deviceId == -1 && info.fallbackOccurred && !info.fallbackReason.empty());
    Group("native cancellation after long CPU Run dispatch remains in flight; subsequent inference recovers");
    auto heavy = HeavyMatMul(1024, 16);
    CHECK(runner.LoadModelFromMemory(heavy.data(), heavy.size(), ExecutionProviderType::CPU, 0, "original-16-matmul-cancellation-fixture") == RunnerStatusCode::Success);
    std::vector<float> largeInput(1024 * 1024, 0.125f), largeOutput;
    RunnerStatusCode runStatus = RunnerStatusCode::InferenceFailed;
    std::atomic<bool> finished{false};
    std::thread inference([&] { runStatus = runner.RunInference(largeInput, largeOutput); finished.store(true); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!runner.IsInferenceRunning() && !finished.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(runner.IsInferenceRunning() && !finished.load());
    // Native Run has no public kernel-entered callback: sustained dispatch plus terminated Run result
    // proves RunOptions termination, but does not claim GPU kernel preemption or a fixed stop bound.
    auto sustain = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    while (std::chrono::steady_clock::now() < sustain && !finished.load()) std::this_thread::yield();
    CHECK(runner.IsInferenceRunning() && !finished.load());
    auto cancelStart = std::chrono::steady_clock::now(); runner.CancelActiveRun(); inference.join();
    auto cancelMillis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelStart).count();
    CHECK(runStatus == RunnerStatusCode::Cancelled); CHECK(largeOutput.empty()); CHECK(!runner.IsInferenceRunning());
    std::cout << "[CANCEL] native CPU dispatch termination observation_ms=" << cancelMillis << '\n';
    CHECK(runner.RunInference(largeInput, largeOutput) == RunnerStatusCode::Success);
    CHECK(largeOutput.size() == largeInput.size() && std::abs(largeOutput.back() - 0.125f) < 1e-5f);
    LoadTiny(runner, &manifest); ExpectUnchanged(runner, runner.GetModelEpoch());
    Group("unload epoch invalidation and unchanged global search configuration");
    auto lastEpoch = runner.GetModelEpoch(); runner.UnloadModel();
    CHECK(!runner.IsModelLoaded() && runner.GetModelEpoch() > lastEpoch);
    CHECK(DllDirectory() == directoryBefore && CurrentDirectory() == workingBefore);
    std::cout << "[SUMMARY] groups=" << groups << " assertions=" << assertions << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
#endif