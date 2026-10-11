#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "adb.h"
#include "brand.h"

namespace PrismForge::Usb {
using Command = std::vector<std::wstring>;
using CommandResult = adb::Result;
using Runner = std::function<CommandResult(const Command&,const std::atomic_bool&)>;

// Single USB phone bridge. Wi-Fi/emulators never win the ADB transport selection.
// Matching pre-existing maps are usable but not owned; foreign maps are untouched.
class BridgeReconciler {
public:
    BridgeReconciler(int hostControlPort, Runner runner,
        int deviceControlPort=PrismForge::DEFAULT_CONTROL_PORT,
        int mediaPort=PrismForge::RTSP_PORT)
        : m_hostPort(hostControlPort),m_devicePort(deviceControlPort),m_mediaPort(mediaPort),m_runner(std::move(runner)) {}
    bool Ready() const { return m_ready; }
    const std::string& GetStatus() const { return m_status; }
    void Tick(const std::atomic_bool& stop) {
        m_ready=false;
        if (stop.load()) { m_status="USB recovery stopped"; return; }
        try {
            const auto state=m_runner({L"-d",L"get-state"},stop);
            if (!state.succeeded || Trim(state.output)!="device") {
                m_status="Waiting for one authorized USB phone";
                if (state.output.find("more than one")!=std::string::npos)
                    m_status="USB needs one connected phone; multiple USB devices are available";
                return;
            }
            const auto serial=m_runner({L"-d",L"get-serialno"},stop);
            const auto selected=Trim(serial.output);
            if (!serial.succeeded || !ValidSerial(selected)) { m_status="USB phone identity unavailable"; return; }
            if (selected!=m_serial) { Cleanup(stop); m_serial=selected; }
            const auto reverse=m_runner(DeviceCommand({L"reverse",L"--list"}),stop);
            const auto forward=m_runner({L"forward",L"--list"},stop);
            if (!reverse.succeeded || !forward.succeeded) { m_status="USB tunnel inspection failed; retrying"; return; }
            const auto reverseMatch=FindMapping(reverse.output,Endpoint(m_devicePort),Endpoint(m_hostPort),{},false);
            const auto forwardMatch=FindMapping(forward.output,Endpoint(m_mediaPort),Endpoint(m_mediaPort),m_serial,true);
            if (reverseMatch==Mapping::Conflict || forwardMatch==Mapping::Conflict) {
                m_status="USB port conflicts with an existing tunnel; existing mapping preserved"; return;
            }
            if (reverseMatch==Mapping::Missing) {
                if (!m_runner(DeviceCommand({L"reverse",L"--no-rebind",WideEndpoint(m_devicePort),WideEndpoint(m_hostPort)}),stop).succeeded) {
                    m_status="USB control tunnel unavailable; retrying"; return;
                }
                m_ownedReverse=true;
            }
            if (forwardMatch==Mapping::Missing) {
                if (!m_runner(DeviceCommand({L"forward",L"--no-rebind",WideEndpoint(m_mediaPort),WideEndpoint(m_mediaPort)}),stop).succeeded) {
                    m_status="USB video tunnel unavailable; retrying"; return;
                }
                m_ownedForward=true;
            }
            if (stop.load()) { m_status="USB recovery stopped"; return; }
            m_ready=true;m_status="USB control and video tunnels ready";
        } catch (...) { m_ready=false;m_status="USB recovery failed; retrying"; }
    }
    void Cleanup(const std::atomic_bool& stop) { Cleanup(stop,m_runner); }
    void Cleanup(const std::atomic_bool& stop,const Runner& runner) {
        m_ready=false;
        if (m_serial.empty()) return;
        try {
            if (m_ownedReverse) {
                const auto list=runner(DeviceCommand({L"reverse",L"--list"}),stop);
                if (list.succeeded && FindMapping(list.output,Endpoint(m_devicePort),Endpoint(m_hostPort),{},false)==Mapping::Match)
                    runner(DeviceCommand({L"reverse",L"--remove",WideEndpoint(m_devicePort)}),stop);
            }
            if (m_ownedForward) {
                const auto list=runner({L"forward",L"--list"},stop);
                if (list.succeeded && FindMapping(list.output,Endpoint(m_mediaPort),Endpoint(m_mediaPort),m_serial,true)==Mapping::Match)
                    runner(DeviceCommand({L"forward",L"--remove",WideEndpoint(m_mediaPort)}),stop);
            }
        } catch (...) { /* Cleanup never interrupts app shutdown. */ }
        m_ownedReverse=false;m_ownedForward=false;
    }
private:
    enum class Mapping { Missing,Match,Conflict };
    int m_hostPort,m_devicePort,m_mediaPort;
    Runner m_runner;
    std::string m_serial,m_status="USB discovery pending";
    bool m_ownedReverse=false,m_ownedForward=false,m_ready=false;
    static std::string Trim(const std::string& value) {
        const auto first=value.find_first_not_of(" \t\r\n");
        if (first==std::string::npos) return {};
        return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
    }
    static bool ValidSerial(const std::string& value) {
        if (value.empty() || value.size()>256 || value=="unknown" || value=="device") return false;
        for (unsigned char c:value) if (c<=32 || c>=127 || c=='"' || c=='\\') return false;
        return true;
    }
    static std::string Endpoint(int port) { return "tcp:"+std::to_string(port); }
    static std::wstring WideEndpoint(int port) { return L"tcp:"+std::to_wstring(port); }
    Command DeviceCommand(Command tail) const {
        Command result={L"-s",std::wstring(m_serial.begin(),m_serial.end())};
        result.insert(result.end(),tail.begin(),tail.end());return result;
    }
    static Mapping FindMapping(const std::string& text,const std::string& local,
        const std::string& remote,const std::string& serial,bool checkSerial) {
        std::istringstream lines(text);
        std::string line;
        Mapping match=Mapping::Missing;
        while (std::getline(lines,line)) {
            std::istringstream fields(line);
            std::string owner,source,target,extra;
            if (!(fields>>owner>>source>>target) || (fields>>extra)) continue;
            if (source!=local) continue;
            if (target!=remote || (checkSerial && owner!=serial)) return Mapping::Conflict;
            match=Mapping::Match;
        }
        return match;
    }
};

class Bridge {
public:
    explicit Bridge(int controlPort,Runner runner={})
        : m_runner(runner ? runner : Runner([](const Command& command,const std::atomic_bool& stop) { return adb::Run(command,stop); })),
          m_customRunner(static_cast<bool>(runner)), m_reconciler(controlPort,m_runner) {}
    ~Bridge() { Stop(); }
    Bridge(const Bridge&)=delete;
    Bridge& operator=(const Bridge&)=delete;
    void Start(std::function<void(const std::string&)> onStatus) {
        if (m_thread.joinable()) return;
        m_stop=false;
        m_thread=std::thread([this,onStatus=std::move(onStatus)] {
            std::string previous;
            while (!m_stop.load()) {
                m_reconciler.Tick(m_stop);
                if (m_reconciler.GetStatus()!=previous) {
                    previous=m_reconciler.GetStatus();
                    try { if (onStatus) onStatus(previous); } catch (...) {}
                }
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock,std::chrono::milliseconds(m_reconciler.Ready()?3000:750),
                    [this] { return m_stop.load() || m_retry; });
                m_retry=false;
            }
            std::atomic_bool cleanupStop{false};
            m_reconciler.Cleanup(cleanupStop,m_customRunner ? m_runner : Runner([](const Command& command,const std::atomic_bool& stop) {
                return adb::Run(command,stop,300,false);
            }));
        });
    }
    void RequestRetry() { { std::lock_guard<std::mutex> lock(m_mutex);m_retry=true; } m_wake.notify_all(); }
    void Stop() {
        m_stop=true;m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
private:
    Runner m_runner;
    bool m_customRunner;
    BridgeReconciler m_reconciler;
    std::atomic_bool m_stop{false};
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_retry=false;
    std::thread m_thread;
};
}
