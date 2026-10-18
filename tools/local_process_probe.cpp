#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "hawken/sha256.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
// Enforce one child in the anonymous job. Native diagnostics require the
// parent's GREEN self-test; --dx11-server execution remains unauthorized.
constexpr bool child_limit_enabled=true;
constexpr DWORD kTimeoutExit=0xe0000001U;
constexpr wchar_t kGameDirectory[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64";
constexpr wchar_t kGameExe[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\HawkenGame-Win64-Shipping.exe";
constexpr wchar_t kSteamApi[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\steam_api64.dll";
void require(bool condition) { if (!condition) throw std::runtime_error("local_process_probe_failed"); }
class Handle {
public:
    explicit Handle(HANDLE value):value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};
struct Process {
    Handle process,thread;
    DWORD pid;
    FILETIME creation{};
    bool waited=false;
    explicit Process(const PROCESS_INFORMATION& info)
        :process(info.hProcess),thread(info.hThread),pid(info.dwProcessId) {}
    ~Process() {
        // Covers every failure before ResumeThread, and exceptional wait paths.
        if (!waited) {
            TerminateProcess(process.get(),kTimeoutExit);
            WaitForSingleObject(process.get(),INFINITE);
        }
    }
    Process(const Process&)=delete;
    Process& operator=(const Process&)=delete;
};
std::wstring own_exe() {
    std::array<wchar_t,32768> buffer{};
    const auto size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    require(size!=0 && size<buffer.size());
    return {buffer.data(),size};
}
void configure_job(HANDLE job) {
    require(job!=nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if constexpr (child_limit_enabled) {
        limits.BasicLimitInformation.LimitFlags|=JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit=1;
    }
    require(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0);
}
void record(const Process& child,DWORD exit) {
    const auto creation=(static_cast<std::uint64_t>(child.creation.dwHighDateTime)<<32)|
        child.creation.dwLowDateTime;
    std::cout<<"child_pid="<<child.pid<<" creation_filetime="<<creation<<" exit="<<exit<<'\n';
}
int spawn(const std::wstring& application,std::wstring command,const wchar_t* directory,
          HANDLE job,DWORD timeout,bool denial_expected=false) {
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,directory,&startup,&info)) {
        const auto error=GetLastError();
        if (denial_expected && (error==ERROR_ACCESS_DENIED || error==ERROR_NOT_ENOUGH_QUOTA)) return 0;
        throw std::runtime_error("local_process_probe_create_failed");
    }
    Process child(info);
    FILETIME exit_time{},kernel{},user{};
    require(GetProcessTimes(child.process.get(),&child.creation,&exit_time,&kernel,&user)!=0);
    if (job) require(AssignProcessToJobObject(job,child.process.get())!=0);
    else {
        BOOL inherited_job=FALSE;
        require(IsProcessInJob(child.process.get(),nullptr,&inherited_job)!=0 && inherited_job);
    }
    require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1));
    const auto wait=WaitForSingleObject(child.process.get(),timeout);
    if (wait!=WAIT_OBJECT_0) {
        // Only this anonymous job / these owned handles are terminated.
        if (job) require(TerminateJobObject(job,kTimeoutExit)!=0);
        else require(TerminateProcess(child.process.get(),kTimeoutExit)!=0);
        require(WaitForSingleObject(child.process.get(),INFINITE)==WAIT_OBJECT_0);
    }
    child.waited=true;
    DWORD exit=0;require(GetExitCodeProcess(child.process.get(),&exit)!=0);
    record(child,exit);
    if (wait!=WAIT_OBJECT_0) return 2;
    return denial_expected ? 1 : static_cast<int>(exit);
}
int self_test() {
    const auto executable=own_exe();
    const auto directory=std::filesystem::path(executable).parent_path().wstring();
    Handle job(CreateJobObjectW(nullptr,nullptr));configure_job(job.get());
    // The helper parent is never assigned to the job.
    return spawn(executable,L"\""+executable+L"\" --spawn-child",directory.c_str(),job.get(),5000);
}
int spawn_leaf() {
    BOOL in_job=FALSE;
    require(IsProcessInJob(GetCurrentProcess(),nullptr,&in_job)!=0 && in_job);
    const auto executable=own_exe();
    const auto directory=std::filesystem::path(executable).parent_path().wstring();
    // The leaf automatically inherits the current job; no handles are inherited.
    return spawn(executable,L"\""+executable+L"\" --leaf",directory.c_str(),nullptr,5000,true);
}
int native_server() {
    if constexpr (!child_limit_enabled) {
        return 2;
    } else {
        require(hawken::sha256_file(std::filesystem::path(kGameExe))==
            "4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2");
        require(hawken::sha256_file(std::filesystem::path(kSteamApi))==
            "38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29");
        Handle job(CreateJobObjectW(nullptr,nullptr));configure_job(job.get());
        std::wstring command=L"\"";command+=kGameExe;
        command+=L"\" server VS-Wreckage?Game=HawkenGameContent.HawkenDM?Port=7787"
            L" -MULTIHOME=127.0.0.1 -dx11 -windowed -ResX=800 -ResY=600 -unattended"
            L" -nosound -nopause MeteorURI=http://127.0.0.1"
            L" -ABSLOG=\"F:\\HawkenServer\\implementation\\evidence\\gameplay-bootstrap\\20261018"
            L"\\server-commandlet-dx11\\server-private.log\"";
        // Job limits block direct/inherited CreateProcess children. They do not
        // claim to contain a broker's ShellExecute process outside this job.
        return spawn(kGameExe,std::move(command),kGameDirectory,job.get(),45000);
    }
}
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    try {
        const std::wstring_view mode(argv[1]);
        if (mode==L"--leaf") return 0;
        if (mode==L"--spawn-child") return spawn_leaf();
        if (mode==L"--self-test") return self_test();
        if (mode==L"--dx11-server") return native_server();
        return 2;
    } catch (...) { return 2; }
}
