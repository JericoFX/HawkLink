#include <windows.h>
#include <tlhelp32.h>
#include "hawken/sha256.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
constexpr wchar_t original_path[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\HawkenGame-Win64-Shipping.exe";
constexpr std::string_view original_sha="4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2";
constexpr std::uint64_t class_slot_rva=0x1d3fa68;
constexpr std::uint64_t controller_vtable_rva=0x17b9c50;
using Json=nlohmann::json;

class Handle {
public:
    explicit Handle(HANDLE value):value_(value) {}
    ~Handle() { if (value_ && value_!=INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_!=INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

bool parse_pid(std::string_view text,DWORD& pid) noexcept {
    if (text.empty() || text.front()=='0') return false;
    std::uint64_t value=0;
    for (const char c:text) {
        if (c<'0' || c>'9') return false;
        value=value*10+static_cast<unsigned>(c-'0');
        if (value>std::numeric_limits<DWORD>::max()) return false;
    }
    pid=static_cast<DWORD>(value);
    return pid!=0;
}
bool same_original_path(std::wstring_view path) noexcept {
    constexpr std::wstring_view expected=original_path;
    // Windows case folding only; no alternate location, alias or basename match.
    return path.size()==expected.size() && CompareStringOrdinal(path.data(),
        static_cast<int>(path.size()),expected.data(),static_cast<int>(expected.size()),TRUE)==CSTR_EQUAL;
}
bool process_path_verified(HANDLE process) {
    std::array<wchar_t,32768> buffer{};
    DWORD length=static_cast<DWORD>(buffer.size());
    return QueryFullProcessImageNameW(process,0,buffer.data(),&length)!=0 &&
        same_original_path({buffer.data(),length});
}
bool address(std::uint64_t base,std::uint64_t offset,std::uint64_t& result) noexcept {
    if (base==0 || base>std::numeric_limits<std::uint64_t>::max()-offset) return false;
    result=base+offset;
    return result<=std::numeric_limits<std::uintptr_t>::max();
}
template<class T>
bool read_field(HANDLE process,std::uint64_t base,std::uint64_t offset,T& result) noexcept {
    std::uint64_t location=0;
    if (!address(base,offset,location) || location>std::numeric_limits<std::uintptr_t>::max()-sizeof(T)) return false;
    SIZE_T received=0;
    return ReadProcessMemory(process,reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(location)),
        &result,sizeof(T),&received)!=0 && received==sizeof(T);
}
struct Sample {
    std::uint64_t klass=0,cdo=0,vtable=0,flags=0,class_backref=0;
    std::int32_t net_index=0;
    std::array<std::uint32_t,2> fname{};
    std::uint32_t ec=0;
    bool operator==(const Sample&) const = default;
};
bool sample(HANDLE process,std::uint64_t module,std::uint64_t expected_vtable,Sample& out) noexcept {
    // Positive class-storage binding only. Never create a CDO, resolve FNames,
    // inspect adjacent objects or traverse a general object/name registry.
    if (!read_field(process,module,class_slot_rva,out.klass) || out.klass==0 ||
        !read_field(process,out.klass,0x1d4,out.cdo) || out.cdo==0 ||
        !read_field(process,out.cdo,0,out.vtable) || out.vtable!=expected_vtable ||
        !read_field(process,out.cdo,0x10,out.flags) || (out.flags&0x600)!=0x200 ||
        !read_field(process,out.cdo,0x50,out.class_backref) || out.class_backref!=out.klass)
        return false;
    return read_field(process,out.cdo,0x3c,out.net_index) &&
        read_field(process,out.cdo,0x48,out.fname) && read_field(process,out.cdo,0xec,out.ec);
}
int observe(DWORD pid) {
    Json report={{"status","inconclusive"},{"error","process_unavailable"},
        {"identity_verified",false},{"original_path_verified",false},{"original_hash_verified",false},
        {"module_verified",false},{"samples_equal",false},{"cdo_vtable_verified",false},
        {"class_backref_verified",false},{"cdo_flags_verified",false},
        {"net_index",nullptr},{"fname_raw",nullptr},{"cdo_flags",nullptr},
        {"observed_ec",nullptr},{"bit0",nullptr}};
    const auto finish=[&](const char* error) {
        report["error"]=error;
        std::cout<<report.dump()<<'\n';
        return 1;
    };
    const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,pid));
    if (!process) return finish("process_unavailable");
    if (!process_path_verified(process.get())) return finish("original_identity_mismatch");
    report["original_path_verified"]=true;
    if (hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)
        return finish("original_identity_mismatch");
    report["original_hash_verified"]=true;
    const Handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid));
    if (!modules) return finish("module_unavailable");
    MODULEENTRY32W module{}; module.dwSize=static_cast<DWORD>(sizeof(module));
    if (!Module32FirstW(modules.get(),&module) || !same_original_path(module.szExePath) ||
        module.modBaseSize<class_slot_rva+sizeof(std::uint64_t) ||
        module.modBaseSize<=controller_vtable_rva)
        return finish("module_unavailable");
    const auto base=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module.modBaseAddr));
    std::uint64_t expected_vtable=0;
    if (!address(base,controller_vtable_rva,expected_vtable)) return finish("module_unavailable");
    report["module_verified"]=true;
    Sample first,second;
    if (!sample(process.get(),base,expected_vtable,first) ||
        !sample(process.get(),base,expected_vtable,second)) return finish("cdo_unavailable_or_mismatch");
    if (first!=second) return finish("unstable_sample");
    // Repeat process identity after sampling; disk identity is not loaded-memory
    // attestation and agreement between two reads is not an atomic snapshot.
    if (!process_path_verified(process.get())) return finish("original_identity_mismatch");
    report["status"]="observed"; report["error"]=nullptr;
    report["identity_verified"]=true; report["samples_equal"]=true;
    report["cdo_vtable_verified"]=true; report["class_backref_verified"]=true;
    report["cdo_flags_verified"]=true;
    report["net_index"]=second.net_index; report["fname_raw"]=second.fname;
    report["cdo_flags"]=second.flags; report["observed_ec"]=second.ec;
    report["bit0"]=(second.ec&1)!=0;
    std::cout<<report.dump()<<'\n';
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Usage: hawken-controller-cdo-probe PID\n"
            "Read-only bounded original-controller CDO observation; no process creation.\n";
        return 0;
    }
    DWORD pid=0;
    if (argc!=2 || !parse_pid(argv[1],pid)) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"invalid_arguments\",\"identity_verified\":false}\n";
        return 2;
    }
    try { return observe(pid); }
    catch (...) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"observation_unavailable\",\"identity_verified\":false}\n";
        return 1;
    }
}
