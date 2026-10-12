#ifndef _WIN64
#error This diagnostic is only for Windows x64.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bit>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace {
constexpr wchar_t original_dll[] =
    L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\steam_api64.dll";
constexpr std::wstring_view evidence_root = L"F:\\HawkenServer\\implementation\\evidence\\";
// The parent must check the original DLL SHA256 and UDP port availability before launch.
// Contract: m4-steam-provider-abi/probe-contract.json,
// SHA256 500582e6b57a28476ed1219aff6cfaf82e0b1b1edb8596fb2e72b256d88d1dec.
// Opt-in ABI: m4-steam-provider-abi/logon-contract/contract.json,
// SHA256 22e795d66fc38a884fdcad662050519d31b221305898327ec656aad70924394a.
static_assert(sizeof(void*) == 8);

enum class Stage { Start, Environment, Load, Exports, Ownership, Init, Handles,
    Client, Server, Utilities, AppContext, Pump, Shutdown, Completed, Exception,
    Metadata, LogOn, LogOff, LocalHint, ClientInit, ClientHandles, ClientUser,
    ClientAppContext, ClientIdentity, ClientShutdown };
enum class Error : unsigned { None, Environment, Load, MissingExport, ForeignContext,
    InitFailed, MissingHandles, MissingClient, MissingServer, MissingUtilities,
    AppContextMismatch, ShutdownFailed, PipeStillPresent, OutputWrite, Exception,
    LogOnTimedOut, LogOffFailed, ClientForeignContext, ClientInitFailed,
    ClientMissingHandles, ClientMissingUser, ClientMissingUtilities,
    ClientAppContextMismatch, ClientIdentityInvalid, ClientShutdownFailed,
    ClientPipeStillPresent };

const char* stage_name(Stage stage) {
    switch (stage) {
    case Stage::Start: return "start";
    case Stage::Environment: return "environment";
    case Stage::Load: return "load";
    case Stage::Exports: return "exports";
    case Stage::Ownership: return "ownership";
    case Stage::Init: return "init";
    case Stage::Handles: return "handles";
    case Stage::Client: return "client";
    case Stage::Server: return "server";
    case Stage::Utilities: return "utilities";
    case Stage::AppContext: return "app_context";
    case Stage::Pump: return "pump";
    case Stage::Shutdown: return "shutdown";
    case Stage::Completed: return "completed";
    case Stage::Exception: return "exception";
    case Stage::Metadata: return "metadata";
    case Stage::LogOn: return "logon";
    case Stage::LogOff: return "logoff";
    case Stage::LocalHint: return "local_hint";
    case Stage::ClientInit: return "client_init";
    case Stage::ClientHandles: return "client_handles";
    case Stage::ClientUser: return "client_user";
    case Stage::ClientAppContext: return "client_app_context";
    case Stage::ClientIdentity: return "client_identity";
    case Stage::ClientShutdown: return "client_shutdown";
    }
    return "exception";
}

struct Report {
    Stage stage = Stage::Start;
    Error error = Error::None;
    DWORD win32_error = 0;
    bool environment_set = false;
    bool dll_loaded = false;
    bool exports[9]{};
    bool fresh_context = false;
    bool init_invoked = false;
    bool init_returned = false;
    bool init_return = false;
    bool handles_available = false;
    bool client_available = false;
    bool server_available = false;
    bool utilities_available = false;
    bool appid_matches = false;
    unsigned pump_iterations = 0;
    bool pump_completed = false;
    bool shutdown_invoked = false;
    bool shutdown_returned = false;
    bool pipe_zero_after_shutdown = false;
    bool logon_requested = false;
    bool logon_exports[8]{};
    bool metadata_calls_returned = false;
    bool advertising_disable_before_returned = false;
    bool logon_invoked = false;
    bool logon_returned = false;
    bool advertising_disable_after_returned = false;
    bool logged_on = false;
    bool logon_timed_out = false;
    bool logoff_invoked = false;
    bool logoff_returned = false;
    bool local_hint_attempted = false;
    bool local_hint_exports[6]{};
    bool client_fresh_context = false;
    bool client_init_invoked = false;
    bool client_init_returned = false;
    bool client_init_return = false;
    bool client_context_owned = false;
    bool client_handles_available = false;
    bool client_user_available = false;
    bool client_utilities_available = false;
    bool client_appid_matches = false;
    bool client_identity_nonzero = false;
    bool client_identity_structure_valid = false;
    bool client_identity_individual = false;
    bool client_shutdown_invoked = false;
    bool client_shutdown_returned = false;
    bool client_pipe_zero_after_shutdown = false;
};

bool result_path_allowed(std::wstring_view path) {
    if (path.size() <= evidence_root.size() || path.size() > 240) return false;
    if (CompareStringOrdinal(path.data(), static_cast<int>(evidence_root.size()),
            evidence_root.data(), static_cast<int>(evidence_root.size()), TRUE) != CSTR_EQUAL)
        return false;
    // Only ordinary absolute paths inside the parent-owned evidence area; no aliases/ADS.
    std::size_t start = evidence_root.size();
    while (start < path.size()) {
        const auto end = path.find(L'\\', start);
        const auto part = path.substr(start, end == std::wstring_view::npos
            ? path.size() - start : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.') return false;
        for (const wchar_t ch : part) {
            if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')
                || (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-' || ch == L'.'))
                return false;
        }
        if (end == std::wstring_view::npos) return part == L"result.json";
        start = end + 1;
    }
    return false;
}

class ReportFile {
public:
    explicit ReportFile(const wchar_t* path) : handle_(CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ReportFile() { if (valid()) CloseHandle(handle_); }
    ReportFile(const ReportFile&) = delete;
    ReportFile& operator=(const ReportFile&) = delete;
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool write(const Report& report) const {
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << std::boolalpha << "{\n  \"stage\":\"" << stage_name(report.stage)
            << "\",\n  \"error_code\":" << static_cast<unsigned>(report.error)
            << ",\n  \"win32_error\":" << report.win32_error
            << ",\n  \"environment_set\":" << report.environment_set
            << ",\n  \"dll_loaded\":" << report.dll_loaded << ",\n  \"exports_available\":[";
        for (unsigned i = 0; i < 9; ++i) {
            if (i != 0) json << ',';
            json << report.exports[i];
        }
        json << "],\n  \"fresh_context\":" << report.fresh_context
            << ",\n  \"init_invoked\":" << report.init_invoked
            << ",\n  \"init_returned\":" << report.init_returned
            << ",\n  \"init_return\":" << report.init_return
            << ",\n  \"handles_available\":" << report.handles_available
            << ",\n  \"client_interface_available\":" << report.client_available
            << ",\n  \"server_interface_available\":" << report.server_available
            << ",\n  \"utilities_interface_available\":" << report.utilities_available
            << ",\n  \"appid_matches_271290\":" << report.appid_matches
            << ",\n  \"pump_iterations\":" << report.pump_iterations
            << ",\n  \"pump_completed\":" << report.pump_completed
            << ",\n  \"shutdown_invoked\":" << report.shutdown_invoked
            << ",\n  \"shutdown_returned\":" << report.shutdown_returned
            << ",\n  \"pipe_zero_after_shutdown\":" << report.pipe_zero_after_shutdown;
        if (report.logon_requested) {
            json << ",\n  \"logon_requested\":true,\n  \"logon_exports_available\":[";
            for (unsigned i = 0; i < 8; ++i) {
                if (i != 0) json << ',';
                json << report.logon_exports[i];
            }
            json << "],\n  \"metadata_calls_returned\":" << report.metadata_calls_returned
                << ",\n  \"advertising_disable_before_returned\":" << report.advertising_disable_before_returned
                << ",\n  \"logon_invoked\":" << report.logon_invoked
                << ",\n  \"logon_returned\":" << report.logon_returned
                << ",\n  \"advertising_disable_after_returned\":" << report.advertising_disable_after_returned
                << ",\n  \"logged_on\":" << report.logged_on
                << ",\n  \"logon_timed_out\":" << report.logon_timed_out
                << ",\n  \"logoff_invoked\":" << report.logoff_invoked
                << ",\n  \"logoff_returned\":" << report.logoff_returned;
            json << ",\n  \"local_hint_attempted\":" << report.local_hint_attempted
                << ",\n  \"local_hint_exports_available\":[";
            for (unsigned i = 0; i < 6; ++i) {
                if (i != 0) json << ',';
                json << report.local_hint_exports[i];
            }
            json << "],\n  \"client_fresh_context\":" << report.client_fresh_context
                << ",\n  \"client_init_invoked\":" << report.client_init_invoked
                << ",\n  \"client_init_returned\":" << report.client_init_returned
                << ",\n  \"client_init_return\":" << report.client_init_return
                << ",\n  \"client_context_owned\":" << report.client_context_owned
                << ",\n  \"client_handles_available\":" << report.client_handles_available
                << ",\n  \"client_user_interface_available\":" << report.client_user_available
                << ",\n  \"client_utilities_interface_available\":" << report.client_utilities_available
                << ",\n  \"client_appid_matches_271290\":" << report.client_appid_matches
                << ",\n  \"client_identity_nonzero\":" << report.client_identity_nonzero
                << ",\n  \"client_identity_structure_valid_sdk137\":" << report.client_identity_structure_valid
                << ",\n  \"client_identity_individual\":" << report.client_identity_individual
                << ",\n  \"client_shutdown_invoked\":" << report.client_shutdown_invoked
                << ",\n  \"client_shutdown_returned\":" << report.client_shutdown_returned
                << ",\n  \"client_pipe_zero_after_shutdown\":" << report.client_pipe_zero_after_shutdown;
        }
        json << "\n}\n";
        const auto payload = json.str();
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_))
            return false;
        DWORD written = 0;
        return WriteFile(handle_, payload.data(), static_cast<DWORD>(payload.size()),
            &written, nullptr) && written == payload.size() && FlushFileBuffers(handle_);
    }
private:
    HANDLE handle_;
};

bool checkpoint(Report& report, const ReportFile& file, Stage stage) {
    report.stage = stage;
    if (file.write(report)) return true;
    report.error = Error::OutputWrite;
    report.win32_error = GetLastError();
    return false;
}

struct Api {
    using Init = bool (__cdecl*)(std::uint32_t, std::uint16_t, std::uint16_t,
        std::uint16_t, std::int32_t, const char*);
    using Handle = std::int32_t (__cdecl*)();
    using ClientInit = bool (__cdecl*)();
    using SteamId = std::uint64_t (__cdecl*)(void*);
    using Factory = void* (__cdecl*)(const char*);
    using Server = void* (__cdecl*)(void*, std::int32_t, std::int32_t, const char*);
    using Utilities = void* (__cdecl*)(void*, std::int32_t, const char*);
    using AppId = std::uint32_t (__cdecl*)(void*);
    using Void = void (__cdecl*)();
    using Metadata = void (__cdecl*)(void*, const char*);
    using ServerBool = void (__cdecl*)(void*, bool);
    using ServerVoid = void (__cdecl*)(void*);
    using LoggedOn = bool (__cdecl*)(void*);
    Init init = nullptr;
    Handle pipe = nullptr;
    Handle user = nullptr;
    Factory factory = nullptr;
    Server server = nullptr;
    Utilities utilities = nullptr;
    AppId appid = nullptr;
    Void pump = nullptr;
    Void shutdown = nullptr;
    Metadata product = nullptr;
    Metadata description = nullptr;
    Metadata mod_dir = nullptr;
    ServerBool dedicated = nullptr;
    ServerBool heartbeats = nullptr;
    ServerVoid logon = nullptr;
    LoggedOn logged_on = nullptr;
    ServerVoid logoff = nullptr;
    ClientInit client_init = nullptr;
    Void client_shutdown = nullptr;
    Handle client_user = nullptr;
    Handle client_pipe = nullptr;
    Server steam_user = nullptr; // Same flat arguments: instance, user, pipe, version.
    SteamId steam_id = nullptr;
};

template<class Function>
Function resolve(HMODULE module, const char* name, bool& available) {
    const auto address = GetProcAddress(module, name);
    available = address != nullptr;
    static_assert(sizeof(Function) == sizeof(address));
    return std::bit_cast<Function>(address);
}

class OwnedContext {
public:
    OwnedContext(const Api& api, Report& report, const ReportFile& file)
        : api_(api), report_(report), file_(file) {}
    ~OwnedContext() { close(); }
    OwnedContext(const OwnedContext&) = delete;
    OwnedContext& operator=(const OwnedContext&) = delete;
    void set_server(void* server) noexcept { server_ = server; }
    void close() noexcept {
        if (closed_ || !report_.init_returned) return;
        closed_ = true;
        if (report_.logon_returned) {
            report_.logoff_invoked = true;
            report_.stage = Stage::LogOff;
            try {
                if (!file_.write(report_) && report_.error == Error::None) {
                    report_.error = Error::OutputWrite;
                    report_.win32_error = GetLastError();
                }
            } catch (...) {
                if (report_.error == Error::None) report_.error = Error::OutputWrite;
            }
            try {
                api_.logoff(server_);
                report_.logoff_returned = true;
            } catch (...) {
                if (report_.error == Error::None) report_.error = Error::LogOffFailed;
            }
        }
        report_.shutdown_invoked = true;
        report_.stage = Stage::Shutdown;
        try {
            if (!file_.write(report_) && report_.error == Error::None) {
                report_.error = Error::OutputWrite;
                report_.win32_error = GetLastError();
            }
        } catch (...) {
            if (report_.error == Error::None) report_.error = Error::OutputWrite;
        }
        // Reporting failures must not prevent the one owned cleanup attempt.
        try {
            api_.shutdown();
            report_.shutdown_returned = true;
            report_.pipe_zero_after_shutdown = api_.pipe() == 0;
            if (!report_.pipe_zero_after_shutdown && report_.error == Error::None)
                report_.error = Error::PipeStillPresent;
        } catch (...) {
            if (report_.error == Error::None) report_.error = Error::ShutdownFailed;
        }
        if (report_.client_context_owned) {
            report_.client_shutdown_invoked = true;
            report_.stage = Stage::ClientShutdown;
            try {
                if (!file_.write(report_) && report_.error == Error::None) {
                    report_.error = Error::OutputWrite;
                    report_.win32_error = GetLastError();
                }
            } catch (...) {
                if (report_.error == Error::None) report_.error = Error::OutputWrite;
            }
            try {
                api_.client_shutdown();
                report_.client_shutdown_returned = true;
                report_.client_pipe_zero_after_shutdown = api_.client_pipe() == 0;
                if (!report_.client_pipe_zero_after_shutdown && report_.error == Error::None)
                    report_.error = Error::ClientPipeStillPresent;
            } catch (...) {
                if (report_.error == Error::None) report_.error = Error::ClientShutdownFailed;
            }
        }
    }
private:
    const Api& api_;
    Report& report_;
    const ReportFile& file_;
    bool closed_ = false;
    void* server_ = nullptr;
};

// SDK137 CSteamID::IsValid bit layout, not proof of ticket authentication.
bool steam_id_structure_valid(std::uint64_t value) noexcept {
    const auto universe = value >> 56;
    const auto type = (value >> 52) & 0xfU;
    const auto instance = (value >> 32) & 0xfffffU;
    if (type == 0 || type >= 11 || universe == 0 || universe >= 5) return false;
    if (type == 1 && (static_cast<std::uint32_t>(value) == 0 || instance > 4)) return false;
    if (type == 7 && (static_cast<std::uint32_t>(value) == 0 || instance != 0)) return false;
    if (type == 3 && static_cast<std::uint32_t>(value) == 0) return false;
    return true;
}

void local_hint(HMODULE module, Api& api, Report& report, const ReportFile& file) {
    report.local_hint_attempted = true;
    if (!checkpoint(report, file, Stage::LocalHint)) return;
    api.client_init = resolve<Api::ClientInit>(module, "SteamAPI_Init", report.local_hint_exports[0]);
    api.client_shutdown = resolve<Api::Void>(module, "SteamAPI_Shutdown", report.local_hint_exports[1]);
    api.client_user = resolve<Api::Handle>(module, "SteamAPI_GetHSteamUser", report.local_hint_exports[2]);
    api.client_pipe = resolve<Api::Handle>(module, "SteamAPI_GetHSteamPipe", report.local_hint_exports[3]);
    api.steam_user = resolve<Api::Server>(module, "SteamAPI_ISteamClient_GetISteamUser", report.local_hint_exports[4]);
    api.steam_id = resolve<Api::SteamId>(module, "SteamAPI_ISteamUser_GetSteamID", report.local_hint_exports[5]);
    for (const bool available : report.local_hint_exports) {
        if (!available) { report.error = Error::MissingExport; return; }
    }
    report.client_fresh_context = api.client_pipe() == 0 && api.client_user() == 0;
    if (!report.client_fresh_context) { report.error = Error::ClientForeignContext; return; }
    if (!checkpoint(report, file, Stage::ClientInit)) return;
    report.client_init_invoked = true;
    if (!file.write(report)) {
        report.client_init_invoked = false;
        report.error = Error::OutputWrite;
        report.win32_error = GetLastError();
        return;
    }
    report.client_init_return = api.client_init();
    report.client_init_returned = true;
    report.client_context_owned = report.client_init_return;
    const auto pipe = api.client_pipe();
    const auto user = api.client_user();
    // Original Init invokes Shutdown internally on its failed created-context paths.
    // A failed Init with residual handles still belongs to our fresh isolated process.
    report.client_context_owned = report.client_init_return || pipe != 0 || user != 0;
    if (!report.client_init_return) { report.error = Error::ClientInitFailed; return; }
    if (!checkpoint(report, file, Stage::ClientHandles)) return;
    report.client_handles_available = pipe != 0 && user != 0;
    if (!report.client_handles_available) { report.error = Error::ClientMissingHandles; return; }
    if (!checkpoint(report, file, Stage::ClientUser)) return;
    void* client = api.factory("SteamClient017");
    if (client == nullptr) { report.error = Error::MissingClient; return; }
    void* steam_user = api.steam_user(client, user, pipe, "SteamUser019");
    report.client_user_available = steam_user != nullptr;
    if (!report.client_user_available) { report.error = Error::ClientMissingUser; return; }
    void* utilities = api.utilities(client, pipe, "SteamUtils008");
    report.client_utilities_available = utilities != nullptr;
    if (!report.client_utilities_available) { report.error = Error::ClientMissingUtilities; return; }
    if (!checkpoint(report, file, Stage::ClientAppContext)) return;
    report.client_appid_matches = api.appid(utilities) == 271290U;
    if (!report.client_appid_matches) { report.error = Error::ClientAppContextMismatch; return; }
    if (!checkpoint(report, file, Stage::ClientIdentity)) return;
    auto identity = api.steam_id(steam_user); // Flat thunk returns uint64 directly in RAX.
    report.client_identity_nonzero = identity != 0;
    report.client_identity_structure_valid = steam_id_structure_valid(identity);
    report.client_identity_individual = ((identity >> 52) & 0xfU) == 1;
    SecureZeroMemory(&identity, sizeof(identity));
    if (!report.client_identity_nonzero || !report.client_identity_structure_valid
        || !report.client_identity_individual) report.error = Error::ClientIdentityInvalid;
    // Only boolean predicates enter Report; no identity/account component is serialized.
}

void probe(Report& report, const ReportFile& file) {
    if (!checkpoint(report, file, Stage::Environment)) return;
    if (!SetEnvironmentVariableW(L"SteamAppId", L"271290")
        || !SetEnvironmentVariableW(L"SteamGameId", L"271290")) {
        report.win32_error = GetLastError();
        report.error = Error::Environment;
        return;
    }
    report.environment_set = true;
    if (!checkpoint(report, file, Stage::Load)) return;
    const HMODULE module = LoadLibraryExW(original_dll, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        report.win32_error = GetLastError();
        report.error = Error::Load;
        return;
    }
    // Deliberately retain this module until this isolated process exits. Unloading it
    // after an ABI fault or a foreign-context guard would not be safe cleanup.
    report.dll_loaded = true;
    if (!checkpoint(report, file, Stage::Exports)) return;
    Api api;
    api.init = resolve<Api::Init>(module, "SteamInternal_GameServer_Init", report.exports[0]);
    api.pipe = resolve<Api::Handle>(module, "SteamGameServer_GetHSteamPipe", report.exports[1]);
    api.user = resolve<Api::Handle>(module, "SteamGameServer_GetHSteamUser", report.exports[2]);
    api.factory = resolve<Api::Factory>(module, "SteamInternal_CreateInterface", report.exports[3]);
    api.server = resolve<Api::Server>(module, "SteamAPI_ISteamClient_GetISteamGameServer", report.exports[4]);
    api.utilities = resolve<Api::Utilities>(module, "SteamAPI_ISteamClient_GetISteamUtils", report.exports[5]);
    api.appid = resolve<Api::AppId>(module, "SteamAPI_ISteamUtils_GetAppID", report.exports[6]);
    api.pump = resolve<Api::Void>(module, "SteamGameServer_RunCallbacks", report.exports[7]);
    api.shutdown = resolve<Api::Void>(module, "SteamGameServer_Shutdown", report.exports[8]);
    for (const bool available : report.exports) {
        if (!available) { report.error = Error::MissingExport; return; }
    }
    if (report.logon_requested) {
        api.product = resolve<Api::Metadata>(module, "SteamAPI_ISteamGameServer_SetProduct", report.logon_exports[0]);
        api.description = resolve<Api::Metadata>(module, "SteamAPI_ISteamGameServer_SetGameDescription", report.logon_exports[1]);
        api.mod_dir = resolve<Api::Metadata>(module, "SteamAPI_ISteamGameServer_SetModDir", report.logon_exports[2]);
        api.dedicated = resolve<Api::ServerBool>(module, "SteamAPI_ISteamGameServer_SetDedicatedServer", report.logon_exports[3]);
        api.heartbeats = resolve<Api::ServerBool>(module, "SteamAPI_ISteamGameServer_EnableHeartbeats", report.logon_exports[4]);
        api.logon = resolve<Api::ServerVoid>(module, "SteamAPI_ISteamGameServer_LogOnAnonymous", report.logon_exports[5]);
        api.logged_on = resolve<Api::LoggedOn>(module, "SteamAPI_ISteamGameServer_BLoggedOn", report.logon_exports[6]);
        api.logoff = resolve<Api::ServerVoid>(module, "SteamAPI_ISteamGameServer_LogOff", report.logon_exports[7]);
        for (const bool available : report.logon_exports) {
            if (!available) { report.error = Error::MissingExport; return; }
        }
    }
    if (!checkpoint(report, file, Stage::Ownership)) return;
    report.fresh_context = api.pipe() == 0 && api.user() == 0;
    if (!report.fresh_context) { report.error = Error::ForeignContext; return; }
    OwnedContext context(api, report, file);
    if (!checkpoint(report, file, Stage::Init)) return;
    report.init_invoked = true;
    if (!file.write(report)) {
        report.init_invoked = false;
        report.error = Error::OutputWrite;
        report.win32_error = GetLastError();
        return;
    }
    report.init_return = api.init(0x7f000001U, 26020, 26021, 26022, 2, "1.6.0.154767");
    report.init_returned = true;
    if (!report.init_return) { report.error = Error::InitFailed; return; }
    if (!checkpoint(report, file, Stage::Handles)) return;
    const auto pipe = api.pipe();
    const auto user = api.user();
    report.handles_available = pipe != 0 && user != 0;
    if (!report.handles_available) { report.error = Error::MissingHandles; return; }
    if (!checkpoint(report, file, Stage::Client)) return;
    void* client = api.factory("SteamClient017");
    report.client_available = client != nullptr;
    if (!report.client_available) { report.error = Error::MissingClient; return; }
    if (!checkpoint(report, file, Stage::Server)) return;
    void* server = api.server(client, user, pipe, "SteamGameServer012");
    report.server_available = server != nullptr;
    if (!report.server_available) { report.error = Error::MissingServer; return; }
    context.set_server(server);
    if (!checkpoint(report, file, Stage::Utilities)) return;
    void* utilities = api.utilities(client, pipe, "SteamUtils008");
    report.utilities_available = utilities != nullptr;
    if (!report.utilities_available) { report.error = Error::MissingUtilities; return; }
    if (!checkpoint(report, file, Stage::AppContext)) return;
    report.appid_matches = api.appid(utilities) == 271290U;
    if (!report.appid_matches) { report.error = Error::AppContextMismatch; return; }
    if (report.logon_requested) {
        // These are new diagnostic metadata selections, not recovered entitlement.
        // Server012 uses EnableHeartbeats; no modern advertising symbol is assumed.
        if (!checkpoint(report, file, Stage::Metadata)) return;
        api.heartbeats(server, false);
        report.advertising_disable_before_returned = true;
        api.product(server, "271290");
        api.description(server, "HAWKEN");
        api.mod_dir(server, "Hawken");
        api.dedicated(server, true);
        report.metadata_calls_returned = true;
        if (!checkpoint(report, file, Stage::LogOn)) return;
        report.logon_invoked = true;
        if (!file.write(report)) {
            report.logon_invoked = false;
            report.error = Error::OutputWrite;
            report.win32_error = GetLastError();
            return;
        }
        api.logon(server);
        report.logon_returned = true;
        api.heartbeats(server, false);
        report.advertising_disable_after_returned = true;
        if (!checkpoint(report, file, Stage::Pump)) return;
        constexpr ULONGLONG budget_ms = 30000;
        const auto beginning = GetTickCount64();
        while (GetTickCount64() - beginning < budget_ms) {
            api.pump();
            ++report.pump_iterations;
            report.logged_on = api.logged_on(server);
            if (report.logged_on) break;
            const auto elapsed = GetTickCount64() - beginning;
            if (elapsed >= budget_ms) break;
            const auto remaining = budget_ms - elapsed;
            Sleep(static_cast<DWORD>(remaining < 50 ? remaining : 50));
        }
        report.logon_timed_out = !report.logged_on;
        if (report.logon_timed_out) report.error = Error::LogOnTimedOut;
        if (report.logged_on) local_hint(module, api, report, file);
    } else {
        if (!checkpoint(report, file, Stage::Pump)) return;
        for (unsigned i = 0; i < 20; ++i) {
            api.pump();
            ++report.pump_iterations;
            Sleep(50);
        }
    }
    report.pump_completed = true;
    context.close();
    if (report.error == Error::None) report.stage = Stage::Completed;
}
}

int wmain(int argc, wchar_t** argv) {
    // Paths and argv are never included in errors or output. Parent redirects native
    // DLL stdout/stderr to NUL and supplies a 10s default or 45s --logon watchdog.
    if ((argc != 3 && argc != 4) || std::wstring_view(argv[1]) != L"--result"
        || (argc == 4 && std::wstring_view(argv[3]) != L"--logon")
        || !result_path_allowed(argv[2])) return 2;
    const ReportFile file(argv[2]); // Parent creates the directory; never replace an old result.
    if (!file.valid()) return 2;
    Report report;
    report.logon_requested = argc == 4;
    try {
        probe(report, file);
    } catch (...) {
        report.stage = Stage::Exception;
        report.error = Error::Exception;
    }
    try {
        if (!file.write(report)) return 2;
    } catch (...) {
        return 2;
    }
    return report.stage == Stage::Completed && report.error == Error::None ? 0 : 1;
}
