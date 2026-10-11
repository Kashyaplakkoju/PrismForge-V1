#include "net/usbbridge.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace PrismForge::Usb;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; return false; } } while(false)

// Stateful fake ADB transport. Tests assert observable map ownership and recovery,
// not command counts or the reconciler's internal implementation.
struct FakeAdb {
    struct Forward { std::wstring serial; std::wstring destination; };
    bool online = true;
    bool ambiguous = false;
    bool invalidSerial = false;
    bool malformedListing = false;
    bool failReverseListing = false;
    bool failForwardListing = false;
    bool failForwardCreate = false;
    bool throwNext = false;
    std::wstring serial = L"phone-alpha";
    std::map<std::wstring, std::map<std::wstring, std::wstring>> reverse;
    std::map<std::wstring, Forward> forward;
    std::vector<Command> calls;
    std::atomic_bool* cancelOnCommand = nullptr;
    int cancelAt = -1;

    CommandResult Invoke(const Command& command, const std::atomic_bool& stop) {
        calls.push_back(command);
        if (cancelOnCommand && static_cast<int>(calls.size()) == cancelAt) *cancelOnCommand = true;
        if (stop.load()) return {false, "cancelled", true};
        if (throwNext) { throwNext = false; throw std::runtime_error("fake helper exception"); }
        if (command.size() == 2 && command[0] == L"-d") {
            if (command[1] == L"get-state") {
                if (ambiguous) return {false, "error: more than one device/emulator", false};
                return online ? CommandResult{true, "device\r\n", false}
                    : CommandResult{false, "error: device offline", false};
            }
            if (command[1] == L"get-serialno") return {true, invalidSerial ? "unknown\n" : Narrow(serial) + "\r\n", false};
        }
        if (command.size() == 2 && command[0] == L"forward" && command[1] == L"--list") {
            if (failForwardListing) return {false, "error: forward list failed", false};
            if (malformedListing) return {true, "serial tcp:8555\n", false};
            std::string output;
            for (const auto& entry : forward)
                output += Narrow(entry.second.serial) + " " + Narrow(entry.first) + " " + Narrow(entry.second.destination) + "\n";
            return {true, output, false};
        }
        if (command.size() < 4 || command[0] != L"-s") return {false, "unexpected fake command", false};
        const auto& selected = command[1];
        const auto& operation = command[2];
        if (!online || selected != serial) return {false, "error: device offline", false};
        if (command[3] == L"--list") {
            if (operation == L"reverse") {
                if (failReverseListing) return {false, "error: reverse list failed", false};
                if (malformedListing) return {true, "transport tcp:7969\n", false};
                std::string output;
                for (const auto& entry : reverse[selected])
                    output += Narrow(selected) + " " + Narrow(entry.first) + " " + Narrow(entry.second) + "\n";
                return {true, output, false};
            }
            if (operation == L"forward") {
                if (failForwardListing) return {false, "error: forward list failed", false};
                if (malformedListing) return {true, "serial tcp:8555\n", false};
                std::string output;
                for (const auto& entry : forward)
                    output += Narrow(entry.second.serial) + " " + Narrow(entry.first) + " " + Narrow(entry.second.destination) + "\n";
                return {true, output, false};
            }
        }
        if (command.size() == 5 && command[3] == L"--remove") {
            if (operation == L"reverse") {
                reverse[selected].erase(command[4]);
                return {true, {}, false};
            }
            if (operation == L"forward") {
                auto found = forward.find(command[4]);
                if (found != forward.end() && found->second.serial == selected) forward.erase(found);
                return {true, {}, false};
            }
        }
        if (command.size() == 6 && command[3] == L"--no-rebind") {
            if (operation == L"reverse") {
                auto& entries = reverse[selected];
                if (entries.count(command[4])) return {false, "error: cannot rebind existing reverse", false};
                entries.emplace(command[4], command[5]);
                return {true, {}, false};
            }
            if (operation == L"forward") {
                if (failForwardCreate) return {false, "error: forward creation failed", false};
                if (forward.count(command[4])) return {false, "error: cannot rebind existing forward", false};
                forward.emplace(command[4], Forward{selected, command[5]});
                return {true, {}, false};
            }
        }
        return {false, "unexpected fake command", false};
    }
    static std::string Narrow(const std::wstring& value) { std::string result; for (wchar_t c : value) result += static_cast<char>(c); return result; }
    Runner MakeRunner() { return [this](const Command& command, const std::atomic_bool& stop) { return Invoke(command, stop); }; }
    bool Complete(int hostPort) const {
        auto r = reverse.find(serial);
        auto f = forward.find(L"tcp:8555");
        if (r == reverse.end() || f == forward.end()) return false;
        auto control = r->second.find(L"tcp:7969");
        return control != r->second.end() && control->second == L"tcp:" + std::to_wstring(hostPort)
            && f->second.serial == serial && f->second.destination == L"tcp:8555";
    }
    bool HasRemoval() const {
        return std::any_of(calls.begin(), calls.end(), [](const Command& command) {
            return std::find(command.begin(), command.end(), L"--remove") != command.end();
        });
    }
};

bool LateAttachment() {
    FakeAdb adb; adb.online = false;
    std::atomic_bool stop{false};
    BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(!adb.Complete(17969));
    CHECK(!bridge.GetStatus().empty());
    adb.online = true; bridge.Tick(stop);
    CHECK(bridge.Ready()); CHECK(adb.Complete(17969));
    return true;
}

bool PartialFailureRecovers() {
    FakeAdb adb; adb.failForwardCreate = true;
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready());
    CHECK(adb.reverse[adb.serial][L"tcp:7969"] == L"tcp:17969");
    CHECK(adb.forward.empty());
    adb.failForwardCreate = false; bridge.Tick(stop);
    CHECK(bridge.Ready()); CHECK(adb.Complete(17969));
    bridge.Cleanup(stop);
    CHECK(adb.reverse[adb.serial].empty()); CHECK(adb.forward.empty());
    return true;
}

bool ReconnectRepairsDisappearedMaps() {
    FakeAdb adb; std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(bridge.Ready());
    adb.online = false; bridge.Tick(stop); CHECK(!bridge.Ready());
    adb.reverse.clear(); adb.forward.clear(); adb.online = true; bridge.Tick(stop);
    CHECK(bridge.Ready()); CHECK(adb.Complete(17969));
    return true;
}

bool AdoptMatchingMapsWithoutOwning() {
    FakeAdb adb;
    adb.reverse[adb.serial][L"tcp:7969"] = L"tcp:17969";
    adb.forward[L"tcp:8555"] = {adb.serial, L"tcp:8555"};
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(bridge.Ready()); CHECK(adb.Complete(17969));
    bridge.Cleanup(stop); CHECK(adb.Complete(17969)); CHECK(!adb.HasRemoval());
    return true;
}

bool ReverseConflictPreserved() {
    FakeAdb adb; adb.reverse[adb.serial][L"tcp:7969"] = L"tcp:29999";
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready());
    CHECK(adb.reverse[adb.serial][L"tcp:7969"] == L"tcp:29999");
    bridge.Cleanup(stop); CHECK(adb.reverse[adb.serial][L"tcp:7969"] == L"tcp:29999");
    return true;
}

bool ForwardConflictPreserved() {
    FakeAdb adb; adb.forward[L"tcp:8555"] = {L"foreign-device", L"tcp:38888"};
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready());
    CHECK(adb.forward[L"tcp:8555"].serial == L"foreign-device");
    CHECK(adb.forward[L"tcp:8555"].destination == L"tcp:38888");
    bridge.Cleanup(stop); CHECK(adb.forward[L"tcp:8555"].serial == L"foreign-device");
    return true;
}

bool OwnedCleanupPreservesOtherPorts() {
    FakeAdb adb; adb.reverse[adb.serial][L"tcp:7000"] = L"tcp:7001";
    adb.forward[L"tcp:9000"] = {adb.serial, L"tcp:9001"};
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(bridge.Ready()); bridge.Cleanup(stop);
    CHECK(!adb.Complete(17969));
    CHECK(adb.reverse[adb.serial].size() == 1);
    CHECK(adb.reverse[adb.serial][L"tcp:7000"] == L"tcp:7001");
    CHECK(adb.forward.size() == 1); CHECK(adb.forward[L"tcp:9000"].destination == L"tcp:9001");
    return true;
}

bool CleanupDoesNotRemoveReplacedMapping() {
    FakeAdb adb; std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(bridge.Ready());
    adb.reverse[adb.serial][L"tcp:7969"] = L"tcp:28888";
    adb.forward[L"tcp:8555"] = {adb.serial, L"tcp:38888"};
    bridge.Cleanup(stop);
    CHECK(adb.reverse[adb.serial][L"tcp:7969"] == L"tcp:28888");
    CHECK(adb.forward[L"tcp:8555"].destination == L"tcp:38888");
    CHECK(!adb.HasRemoval()); return true;
}

bool AmbiguousDeviceHasNoSideEffects() {
    FakeAdb adb; adb.ambiguous = true;
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(adb.reverse.empty()); CHECK(adb.forward.empty());
    CHECK(!bridge.GetStatus().empty());
    adb.ambiguous = false; bridge.Tick(stop); CHECK(bridge.Ready());
    return true;
}

bool InvalidIdentityRejected() {
    FakeAdb adb; adb.invalidSerial = true;
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(adb.reverse.empty()); CHECK(adb.forward.empty());
    return true;
}

bool CancelBeforeTickDoesNothing() {
    FakeAdb adb; std::atomic_bool stop{true}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(adb.calls.empty()); CHECK(adb.reverse.empty()); CHECK(adb.forward.empty());
    return true;
}

bool CancelBetweenCommandsDoesNotPublishReady() {
    FakeAdb adb; std::atomic_bool stop{false};
    adb.cancelOnCommand = &stop; adb.cancelAt = 2;
    BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(stop.load()); CHECK(!bridge.Ready()); CHECK(adb.reverse.empty()); CHECK(adb.forward.empty());
    return true;
}

bool FailedListingNeverClaimsReadyOrDeletes() {
    FakeAdb adb; adb.failReverseListing = true;
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(!adb.HasRemoval());
    adb.failReverseListing = false; adb.failForwardListing = true;
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(!adb.HasRemoval());
    adb.failForwardListing = false; bridge.Tick(stop); CHECK(bridge.Ready());
    return true;
}

bool HelperExceptionContainedAndRecoverable() {
    FakeAdb adb; adb.throwNext = true;
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready());
    bridge.Tick(stop); CHECK(bridge.Ready()); return true;
}

bool SerialChangeCannotUseOldForward() {
    FakeAdb adb; std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(bridge.Ready());
    adb.serial = L"phone-beta"; bridge.Tick(stop);
    CHECK(!bridge.Ready()); // Alpha's existing host port cannot be stolen for beta.
    CHECK(adb.forward[L"tcp:8555"].serial == L"phone-alpha");
    adb.forward.erase(L"tcp:8555"); bridge.Tick(stop);
    CHECK(bridge.Ready()); CHECK(adb.Complete(17969)); return true;
}

bool MalformedListingCannotMaskConflict() {
    FakeAdb adb; adb.malformedListing = true;
    adb.reverse[adb.serial][L"tcp:7969"] = L"tcp:29999";
    adb.forward[L"tcp:8555"] = {L"foreign-device", L"tcp:38888"};
    std::atomic_bool stop{false}; BridgeReconciler bridge(17969, adb.MakeRunner());
    bridge.Tick(stop); CHECK(!bridge.Ready()); CHECK(!adb.HasRemoval());
    CHECK(adb.reverse[adb.serial][L"tcp:7969"] == L"tcp:29999");
    CHECK(adb.forward[L"tcp:8555"].serial == L"foreign-device"); return true;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("wide string conversion failed");
    std::string text(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), length, nullptr, nullptr);
    return text;
}

struct ChildFixture {
    std::wstring executable;
    std::wstring directory;
    ChildFixture() {
        const auto base = adb::dir();
        if (base.empty()) throw std::runtime_error("test executable location unavailable");
        directory = base + L"\\fake helper path \u03b1";
        std::filesystem::create_directory(directory);
        std::vector<wchar_t> name(32768);
        const DWORD count = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!count || count >= name.size()) throw std::runtime_error("test executable path invalid");
        executable = directory + L"\\fake adb \u6d4b\u8bd5.exe";
        if (!CopyFileW(std::wstring(name.data(), count).c_str(), executable.c_str(), FALSE))
            throw std::runtime_error("fake helper copy failed");
    }
};

bool RealProcessQuotedArgumentsAndWorkingDirectory() {
    ChildFixture child; std::atomic_bool stop{false};
    CHECK(std::filesystem::current_path() != std::filesystem::path(child.directory));
    const std::vector<std::wstring> values{L"", L"space value", L"quote\"inside", L"ends\\", L"two\\\\\"x", L"\u6d4b\u8bd5"};
    Command command{L"--echo"}; command.insert(command.end(), values.begin(), values.end());
    const auto result = adb::Execute(child.executable, child.directory, command, stop);
    CHECK(result.succeeded); CHECK(!result.cancelled);
    std::string normalized = result.output; normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
    CHECK(normalized.find("CWD=" + Utf8(child.directory) + "\n") != std::string::npos);
    for (size_t i = 0; i != values.size(); ++i) {
        const auto expected = "ARG" + std::to_string(i) + "=" + Utf8(values[i]) + "\n";
        CHECK(normalized.find(expected) != std::string::npos);
    }
    CHECK(normalized.find("STDERR=visible\n") != std::string::npos);
    return true;
}

bool ApplicationDirectoryIgnoresParentWorkingDirectory() {
    ChildFixture child;
    const auto original = std::filesystem::current_path();
    const auto application = adb::dir();
    struct RestorePath { std::filesystem::path path; ~RestorePath() { std::error_code error; std::filesystem::current_path(path, error); } } restore{original};
    std::filesystem::current_path(child.directory);
    CHECK(adb::dir() == application);
    CHECK(PrismForge::ApplicationDirectory() == application);
    return true;
}

bool RealProcessNonzeroExitIsFailure() {
    ChildFixture child; std::atomic_bool stop{false};
    const auto result = adb::Execute(child.executable, child.directory, {L"--child-fail"}, stop);
    CHECK(!result.succeeded); CHECK(!result.cancelled);
    CHECK(result.output.find("intentional child failure") != std::string::npos);
    return true;
}

bool RealProcessTimeoutIsBounded() {
    ChildFixture child; std::atomic_bool stop{false};
    const auto started = std::chrono::steady_clock::now();
    const auto result = adb::Execute(child.executable, child.directory, {L"--child-sleep"}, stop, 80);
    CHECK(!result.succeeded); CHECK(!result.cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(result.output.find("timed out") != std::string::npos);
    return true;
}

bool RealProcessCancellationIsBounded() {
    ChildFixture child; std::atomic_bool stop{false};
    std::thread cancel([&stop] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); stop = true; });
    struct Join { std::thread& worker; ~Join() { if (worker.joinable()) worker.join(); } } join{cancel};
    const auto started = std::chrono::steady_clock::now();
    const auto result = adb::Execute(child.executable, child.directory, {L"--child-sleep"}, stop, 10000);
    CHECK(!result.succeeded); CHECK(result.cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    return true;
}

bool RealProcessOutputCapIsBounded() {
    ChildFixture child; std::atomic_bool stop{false};
    const auto started = std::chrono::steady_clock::now();
    const auto result = adb::Execute(child.executable, child.directory, {L"--child-noise"}, stop);
    CHECK(!result.succeeded); CHECK(!result.cancelled);
    CHECK(result.output.size() <= 65536);
    CHECK(result.output.find("exceeded") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    return true;
}

bool RealProcessCannotInheritUnrelatedHandle() {
    ChildFixture child; std::atomic_bool stop{false};
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    adb::Handle event; event.value = CreateEventW(&security, TRUE, FALSE, nullptr);
    CHECK(event.value != nullptr);
    const auto result = adb::Execute(child.executable, child.directory,
        {L"--child-handle", std::to_wstring(reinterpret_cast<uintptr_t>(event.value))}, stop);
    CHECK(result.succeeded); CHECK(result.output.find("unrelated handle not inherited") != std::string::npos);
    return true;
}

bool RealProcessMissingExecutableIsFailure() {
    ChildFixture child; std::atomic_bool stop{false};
    const auto result = adb::Execute(child.directory + L"\\missing helper.exe", child.directory, {}, stop);
    CHECK(!result.succeeded); CHECK(!result.cancelled);
    CHECK(result.output.find("Cannot launch") != std::string::npos);
    return true;
}

struct ScopedEnvironment {
    std::wstring name, previous;
    bool existed = false;
    ScopedEnvironment(const wchar_t* key, const wchar_t* value) : name(key) {
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
        existed = count != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (count) {
            std::vector<wchar_t> text(count);
            GetEnvironmentVariableW(key, text.data(), count);
            previous.assign(text.data());
        }
        if (!SetEnvironmentVariableW(key, value)) throw std::runtime_error("test environment update failed");
    }
    ~ScopedEnvironment() { SetEnvironmentVariableW(name.c_str(), existed ? previous.c_str() : nullptr); }
};

bool RunPinsLocalUsbSelectorDespiteLauncherEnvironment() {
    ChildFixture child;
    const auto base = adb::dir();
    const auto helperDir = base + L"\\adb";
    std::filesystem::create_directory(helperDir);
    const auto bundledHelper = helperDir + L"\\adb.exe";
    // Only this test binary's own ignored build/usb-bridge/adb fixture is created.
    if (!CopyFileW(child.executable.c_str(), bundledHelper.c_str(), FALSE))
        throw std::runtime_error("bundled fake helper copy failed");
    ScopedEnvironment serial(L"ANDROID_SERIAL", L"synthetic-wifi-device");
    ScopedEnvironment server(L"ADB_SERVER_SOCKET", L"tcp:synthetic-remote:9999");
    std::atomic_bool stop{false};
    const auto result = adb::Run({L"-d", L"get-state"}, stop);
    CHECK(result.succeeded); CHECK(!result.cancelled);
    CHECK(result.output.find("HOST=127.0.0.1") != std::string::npos);
    CHECK(result.output.find("PORT=5037") != std::string::npos);
    CHECK(result.output.find("SELECTOR=-d") != std::string::npos);
    CHECK(result.output.find("INHERITED_SERIAL=synthetic-wifi-device") != std::string::npos);
    return true;
}
bool BootstrapCase(const wchar_t* mode, bool allow, bool expected, DWORD bootstrapBudget=200) {
    ChildFixture child; std::atomic_bool stop{false};
    const auto marker=std::filesystem::path(child.directory)/L"bootstrap.marker";
    std::filesystem::remove(marker);
    ScopedEnvironment environment(L"PRISMFORGE_TEST_BOOTSTRAP",mode);
    ScopedEnvironment remote(L"ADB_SERVER_SOCKET",L"tcp:synthetic-remote:9999");
    const auto started=std::chrono::steady_clock::now();
    const auto result=adb::RunLocal(child.executable,child.directory,{L"-d",L"get-state"},stop,200,allow,bootstrapBudget);
    CHECK(result.succeeded==expected);
    CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds(2));
    CHECK(std::filesystem::exists(marker)==expected);
    if(expected) CHECK(result.output.find("device")!=std::string::npos);
    else CHECK(result.output.find("cannot connect to daemon")!=std::string::npos);
    std::filesystem::remove(marker);
    return true;
}
bool ColdDaemonBootstrapsLocalOnly(){return BootstrapCase(L"success",true,true);}
bool FailedBootstrapNeverClaimsReady(){return BootstrapCase(L"fail",true,false);}
bool TimedOutBootstrapIsBounded(){return BootstrapCase(L"timeout",true,false);}
bool CleanupNeverStartsDaemon(){return BootstrapCase(L"success",false,false);}
bool BootstrapHasIndependentStartupBudget(){return BootstrapCase(L"slow",true,true,10000);}
bool BootstrapCancellationIsBounded(){
    ChildFixture child; std::atomic_bool stop{false};
    const auto marker=std::filesystem::path(child.directory)/L"bootstrap.marker";
    std::filesystem::remove(marker);ScopedEnvironment environment(L"PRISMFORGE_TEST_BOOTSTRAP",L"timeout");
    std::thread cancel([&stop]{Sleep(100);stop=true;});struct Join{std::thread& t;~Join(){t.join();}} join{cancel};
    const auto start=std::chrono::steady_clock::now();
    auto result=adb::RunLocal(child.executable,child.directory,{L"-d",L"get-state"},stop,200,true,10000);
    CHECK(!result.succeeded&&result.cancelled);CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
    CHECK(!std::filesystem::exists(marker));return true;
}

int RunChildMode(int argc, wchar_t** argv) {
    const std::wstring mode = argv[1];
    wchar_t bootstrapMode[32]{};
    if(GetEnvironmentVariableW(L"PRISMFORGE_TEST_BOOTSTRAP",bootstrapMode,32)) {
        const auto marker=std::filesystem::current_path()/L"bootstrap.marker";
        if(mode==L"-H") {
            if(std::filesystem::exists(marker)){std::cout<<"device\n";return 0;}
            std::cerr<<"adb.exe: cannot connect to daemon at tcp:127.0.0.1:5037\n";return 1;
        }
        if(argc!=4 || mode!=L"-L" || std::wstring(argv[2])!=L"tcp:localhost:5037" || std::wstring(argv[3])!=L"start-server") return 22;
        if(std::wstring(bootstrapMode)==L"fail"){std::cerr<<"bootstrap failed\n";return 23;}
        if(std::wstring(bootstrapMode)==L"timeout"){Sleep(5000);return 24;}
        if(std::wstring(bootstrapMode)==L"slow"){Sleep(350);}
        std::ofstream(marker)<<"local endpoint started";return 0;
    }
    if (mode == L"-H") {
        if (argc != 7 || std::wstring(argv[2]) != L"127.0.0.1" || std::wstring(argv[3]) != L"-P" ||
            std::wstring(argv[4]) != L"5037" || std::wstring(argv[5]) != L"-d" || std::wstring(argv[6]) != L"get-state") return 11;
        std::vector<wchar_t> serial(1024);
        const DWORD count = GetEnvironmentVariableW(L"ANDROID_SERIAL", serial.data(), static_cast<DWORD>(serial.size()));
        if (!count || count >= serial.size()) return 12;
        std::cout << "HOST=127.0.0.1\nPORT=5037\nSELECTOR=-d\nINHERITED_SERIAL=" << Utf8(std::wstring(serial.data(), count)) << "\n";
        return 0;
    }
    if (mode == L"--echo") {
        std::vector<wchar_t> cwd(32768);
        const DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
        if (!count || count >= cwd.size()) return 8;
        std::cout << "CWD=" << Utf8(std::wstring(cwd.data(), count)) << "\n";
        for (int i = 2; i != argc; ++i) std::cout << "ARG" << (i - 2) << "=" << Utf8(argv[i]) << "\n";
        std::cerr << "STDERR=visible\n";
        return 0;
    }
    if (mode == L"--child-fail") { std::cerr << "intentional child failure\n"; return 7; }
    if (mode == L"--child-sleep") { Sleep(60000); return 0; }
    if (mode == L"--child-noise") {
        const std::string noise(4096, 'n');
        for (int i = 0; i != 128; ++i) std::cout << noise << std::flush;
        return 0;
    }
    if (mode == L"--child-handle" && argc == 3) {
        const auto value = static_cast<uintptr_t>(std::stoull(argv[2]));
        DWORD flags = 0;
        if (GetHandleInformation(reinterpret_cast<HANDLE>(value), &flags)) return 9;
        std::cout << "unrelated handle not inherited\n"; return 0;
    }
    return -1;
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--force-failure") return 19;
    if (argc > 1) { const int child = RunChildMode(argc, argv); if (child != -1) return child; }
    const std::vector<std::pair<const char*, std::function<bool()>>> tests{
        {"late USB attachment establishes both tunnels", LateAttachment},
        {"partial creation failure recovers and remains owned", PartialFailureRecovers},
        {"unplug and reconnect restores vanished tunnels", ReconnectRepairsDisappearedMaps},
        {"matching preexisting maps remain unowned", AdoptMatchingMapsWithoutOwning},
        {"foreign reverse destination is preserved", ReverseConflictPreserved},
        {"foreign forward device and destination are preserved", ForwardConflictPreserved},
        {"owned cleanup preserves unrelated ports", OwnedCleanupPreservesOtherPorts},
        {"cleanup preserves mappings replaced by another owner", CleanupDoesNotRemoveReplacedMapping},
        {"ambiguous USB selection causes no mapping writes", AmbiguousDeviceHasNoSideEffects},
        {"invalid serial identity cannot create tunnels", InvalidIdentityRejected},
        {"cancelled tick never launches a helper", CancelBeforeTickDoesNothing},
        {"cancellation between helpers cannot publish ready", CancelBetweenCommandsDoesNotPublishReady},
        {"failed map listing does not claim readiness or delete", FailedListingNeverClaimsReadyOrDeletes},
        {"helper exception is contained and recovery succeeds", HelperExceptionContainedAndRecoverable},
        {"new device cannot use another device's forward", SerialChangeCannotUseOldForward},
        {"malformed listing cannot overwrite conflicting tunnels", MalformedListingCannotMaskConflict},
        {"real Unicode helper path quotes arguments and pins child cwd", RealProcessQuotedArgumentsAndWorkingDirectory},
        {"application directory ignores caller cwd", ApplicationDirectoryIgnoresParentWorkingDirectory},
        {"real nonzero child exit propagates as failure", RealProcessNonzeroExitIsFailure},
        {"real child timeout terminates within bound", RealProcessTimeoutIsBounded},
        {"real child cancellation terminates within bound", RealProcessCancellationIsBounded},
        {"real noisy child output cannot exceed limit", RealProcessOutputCapIsBounded},
        {"real child cannot inherit unrelated native handle", RealProcessCannotInheritUnrelatedHandle},
        {"real missing executable reports launch failure", RealProcessMissingExecutableIsFailure},
        {"Run pins local server and USB selector despite launcher environment", RunPinsLocalUsbSelectorDespiteLauncherEnvironment},
        {"absent daemon bootstraps only explicit local socket", ColdDaemonBootstrapsLocalOnly},
        {"failed daemon bootstrap never claims readiness", FailedBootstrapNeverClaimsReady},
        {"daemon bootstrap timeout is bounded", TimedOutBootstrapIsBounded},
        {"shutdown cleanup never starts absent daemon", CleanupNeverStartsDaemon},
        {"daemon bootstrap has independent startup budget", BootstrapHasIndependentStartupBudget},
        {"long bootstrap is promptly cancellable", BootstrapCancellationIsBounded},
    };
    int passed = 0;
    for (const auto& test : tests) {
        bool ok = false;
        try { ok = test.second(); }
        catch (const std::exception& error) { std::cerr << "EXCEPTION " << test.first << ": " << error.what() << "\n"; }
        catch (...) { std::cerr << "EXCEPTION " << test.first << "\n"; }
        std::cout << (ok ? "PASS " : "FAIL ") << test.first << "\n";
        if (ok) ++passed;
    }
    std::cout << passed << "/" << tests.size() << " passed\n";
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}
