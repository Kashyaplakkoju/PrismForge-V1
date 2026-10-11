#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include <filesystem>
#include "apppaths.h"

namespace adb {
struct Result {
    bool succeeded = false;
    std::string output;
    bool cancelled = false;
};
class Handle {
public:
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline std::wstring dir() { return PrismForge::ApplicationDirectory(); }
// Quote a single Windows argv item, including embedded quotes and trailing slashes.
inline std::wstring Quote(const std::wstring& argument) {
    std::wstring result=L"\"";
    size_t slashes=0;
    for (wchar_t character : argument) {
        if (character==L'\\') { ++slashes; continue; }
        if (character==L'\"') { result.append(slashes*2+1,L'\\'); result+=character; }
        else { result.append(slashes,L'\\'); result+=character; }
        slashes=0;
    }
    result.append(slashes*2,L'\\'); result+=L'\"';
    return result;
}
// Shell-free, cancellable capture. Only the explicit child stdio handles inherit.
inline Result Execute(const std::wstring& executable, const std::wstring& workingDirectory,
                      const std::vector<std::wstring>& arguments,
                      const std::atomic_bool& stop, DWORD timeoutMs=3000) {
    Result result;
    if (stop.load()) { result.cancelled=true; return result; }
    Handle readPipe,writePipe,input,process,thread;
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    if (!CreatePipe(&readPipe.value,&writePipe.value,&security,0) ||
        !SetHandleInformation(readPipe.value,HANDLE_FLAG_INHERIT,0)) {
        result.output="Cannot create ADB output pipe"; return result;
    }
    input.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                           &security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (input.value==INVALID_HANDLE_VALUE) { result.output="Cannot create ADB input"; return result; }
    SIZE_T attributeSize=0;
    InitializeProcThreadAttributeList(nullptr,1,0,&attributeSize);
    std::vector<unsigned char> attributes(attributeSize);
    auto* attributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attributeList,1,0,&attributeSize)) {
        result.output="Cannot initialize ADB handle list"; return result;
    }
    HANDLE inherited[]={writePipe.value,input.value};
    if (!UpdateProcThreadAttribute(attributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited,sizeof(inherited),nullptr,nullptr)) {
        DeleteProcThreadAttributeList(attributeList);
        result.output="Cannot restrict ADB handle inheritance"; return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput=writePipe.value;
    startup.StartupInfo.hStdError=writePipe.value;
    startup.StartupInfo.hStdInput=input.value;
    startup.lpAttributeList=attributeList;
    PROCESS_INFORMATION info{};
    std::wstring command=Quote(executable);
    for (const auto& argument : arguments) command+=L" "+Quote(argument);
    const BOOL launched=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,workingDirectory.c_str(),
        &startup.StartupInfo,&info);
    const DWORD launchError=GetLastError();
    DeleteProcThreadAttributeList(attributeList);
    if (!launched) {
        result.output="Cannot launch bundled ADB, Windows error "+std::to_string(launchError);
        return result;
    }
    process.value=info.hProcess;thread.value=info.hThread;
    CloseHandle(writePipe.value);writePipe.value=nullptr;
    const ULONGLONG began=GetTickCount64();
    auto drain=[&] {
        DWORD available=0;
        char buffer[4096];
        // A noisy helper cannot hold cancellation or grow memory without bound.
        for (unsigned batch=0;batch<16;++batch) {
            if (!PeekNamedPipe(readPipe.value,nullptr,0,nullptr,&available,nullptr) || !available) break;
            DWORD count=0;
            if (!ReadFile(readPipe.value,buffer,(std::min)(available,DWORD(sizeof(buffer))),&count,nullptr)) break;
            if (result.output.size()+count>65536) return false;
            result.output.append(buffer,count);
        }
        return true;
    };
    for (;;) {
        if (!drain()) { result.output="ADB output exceeded 64 KiB"; break; }
        const DWORD wait=WaitForSingleObject(process.value,20);
        if (wait==WAIT_OBJECT_0) {
            if (!drain()) { result.output="ADB output exceeded 64 KiB"; break; }
            DWORD code=1;
            result.succeeded=GetExitCodeProcess(process.value,&code) && code==0;
            return result;
        }
        if (stop.load()) { result.cancelled=true; break; }
        if (wait==WAIT_FAILED || GetTickCount64()-began>=timeoutMs) {
            result.output+="\nADB helper timed out or wait failed"; break;
        }
    }
    // Terminate only this invocation, never the shared ADB daemon.
    TerminateProcess(process.value,1);WaitForSingleObject(process.value,1000);
    return result;
}
inline Result RunLocal(const std::wstring& executable, const std::wstring& directory,
                       const std::vector<std::wstring>& arguments, const std::atomic_bool& stop,
                       DWORD timeoutMs=3000, bool allowBootstrap=true, DWORD bootstrapTimeoutMs=10000) {
    std::vector<std::wstring> local={L"-H",L"127.0.0.1",L"-P",L"5037"};
    local.insert(local.end(),arguments.begin(),arguments.end());
    auto result=Execute(executable,directory,local,stop,timeoutMs);
    // ADB regards explicit -H as a remote server, even for loopback, and cannot
    // auto-start that endpoint. Bootstrap an explicitly LOCAL listening socket.
    // Starting a daemon may exceed a normal query timeout; bound it separately,
    // still cancellable every 20ms and never performed during shutdown cleanup.
    if (!result.succeeded && !result.cancelled && !stop.load() && allowBootstrap &&
        result.output.find("cannot connect to daemon")!=std::string::npos) {
        const auto bootstrap=Execute(executable,directory,
            {L"-L",L"tcp:localhost:5037",L"start-server"},stop,bootstrapTimeoutMs);
        if (!bootstrap.succeeded) {
            result.cancelled=bootstrap.cancelled;
            result.output+="\nLocal ADB bootstrap failed:\n"+bootstrap.output;
            if(result.output.size()>65536) result.output.resize(65536);
            return result;
        }
        return Execute(executable,directory,local,stop,timeoutMs);
    }
    return result;
}
inline Result Run(const std::vector<std::wstring>& arguments, const std::atomic_bool& stop,
                  DWORD timeoutMs=3000, bool allowBootstrap=true) {
    const auto directory=dir();
    if (directory.empty()) return {false,"Cannot resolve the application directory",false};
    return RunLocal((std::filesystem::path(directory)/L"adb"/L"adb.exe").wstring(),directory,arguments,stop,timeoutMs,allowBootstrap);
}
}
