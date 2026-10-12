// Standalone Windows fixture. Link dinput8.lib and dxguid.lib; place the
// HAWKEN_CLIENT_BRIDGE_HARNESS DLL as dinput8.dll beside this executable.
// This does not test Steam authentication or the original Hawken executable.
#define DIRECTINPUT_VERSION 0x0800
#include <cpp-httplib/httplib.h>
#include <windows.h>
#include <dinput.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
LPWSTR volatile last_command_line = nullptr;
bool identity_available = false;
unsigned identity_reads = 0;
HINTERNET volatile last_http_handle = nullptr;
}

// Synthetic provider exported only by this fixture EXE, never by Hawken or
// the production bridge. The integration must not query it during startup.
extern "C" __declspec(dllexport) bool HarnessReadIdentity(void*, std::uint64_t* value) noexcept {
    ++identity_reads;
    *value = identity_available ? 76561197960265729ULL : 0;
    return identity_available;
}

extern "C" __declspec(dllexport) __declspec(noinline) HINTERNET WINAPI HarnessIdentityConnect(
    HINTERNET session, LPCWSTR host, INTERNET_PORT port, DWORD reserved) {
    HINTERNET value = WinHttpConnect(session, host, port, reserved);
    last_http_handle = value; // Keep a real, bounded call/return site.
    return value;
}
extern "C" __declspec(dllexport) __declspec(noinline) HINTERNET WINAPI HarnessIdentityRequest(
    HINTERNET connection, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
    LPCWSTR referrer, LPCWSTR* types, DWORD flags) {
    HINTERNET value = WinHttpOpenRequest(connection, verb, object, version, referrer, types, flags);
    last_http_handle = value;
    return value;
}

extern "C" __declspec(noinline) LPWSTR WINAPI HarnessCommandLine() {
    LPWSTR result = GetCommandLineW();
    // A volatile store prevents a tail call: the fixture has a real return site.
    last_command_line = result;
    return result;
}

struct HarnessMarker {
    std::uint64_t magic;
    LPWSTR (WINAPI* command_line)();
};

// Plain constant-initialized data, available before executable CRT startup.
extern "C" {
__declspec(dllexport) constinit HarnessMarker HawkenBridgeHarnessMarker{
    0x4841574b454e5445ULL, &HarnessCommandLine};
}

namespace {
LPWSTR actual_command_line() {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return nullptr;
    const auto function = reinterpret_cast<LPWSTR (WINAPI*)()>(
        GetProcAddress(kernel, "GetCommandLineW"));
    return function == nullptr ? nullptr : function();
}

BOOL CALLBACK count_devices(const DIDEVICEINSTANCEW*, void* context) {
    ++*static_cast<unsigned*>(context);
    return DIENUM_CONTINUE;
}

int failure() {
    std::cerr << "client-bridge harness failed.\n";
    return 1;
}

int production_mismatch(const wchar_t* path) {
    if (!std::filesystem::path(path).is_absolute()) return failure();
    SetLastError(ERROR_SUCCESS);
    HMODULE module = LoadLibraryW(path);
    const DWORD error = GetLastError();
    if (module != nullptr) {
        FreeLibrary(module);
        return failure();
    }
    if (error != ERROR_DLL_INIT_FAILED) return failure();
    std::cout << "production-host-mismatch rejected.\n";
    return 0;
}

int early_export() {
    void* result = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    const HRESULT status = DirectInput8Create(GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION, IID_IDirectInput8W, &result, nullptr);
    if (status != DIERR_NOTINITIALIZED || result != nullptr) return failure();
    std::cout << "early-export rejected without initialization.\n";
    return 0;
}

struct InternetHandle {
    HINTERNET value;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

int identity_http(bool unavailable) {
    // Catches missing SDK IAT installation, premature identity reads, changed
    // body, and missing CloseHandle hook through repeated connection retirement.
    constexpr char body[] = "{\"SteamAuthTicket\":\"public-synthetic-fixture\"}";
    httplib::Server server;
    std::atomic<unsigned> requests{0};
    server.Post("/steam/login", [&](const httplib::Request& request, httplib::Response& response) {
        ++requests;
        response.status = request.get_header_value_count("X-Hawken-SteamID") == 1
            && request.get_header_value("X-Hawken-SteamID") == "76561197960265729"
            && request.body == body ? 200 : 412;
        response.set_content("fixture", "text/plain");
    });
    if (!server.bind_to_port("127.0.0.1", 80)) return failure();
    std::jthread worker([&] { server.listen_after_bind(); });
    struct Stop { httplib::Server& server; ~Stop() { server.stop(); } } stop{server};
    server.wait_until_ready();
    if (!server.is_running()) return failure();
    (void)HarnessCommandLine();
    if (identity_reads != 0) return failure();
    identity_available = !unavailable;
    InternetHandle session(WinHttpOpen(L"HawkenBridgeFixture", WINHTTP_ACCESS_TYPE_NO_PROXY,
        nullptr, nullptr, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) return failure();
    {
        // Same destination from a different callsite must not read identity.
        InternetHandle connection(HarnessIdentityConnect(session.value, L"127.0.0.1", 80, 0));
        InternetHandle request(WinHttpOpenRequest(connection.value, L"POST", L"/steam/login",
            nullptr, nullptr, nullptr, 0));
        if (!request.value || identity_reads != 0) return failure();
    }
    const unsigned iterations = unavailable ? 1 : 130;
    for (unsigned i = 0; i < iterations; ++i) {
        InternetHandle connection(HarnessIdentityConnect(session.value, L"127.0.0.1", 80, 0));
        if (!connection.value) return failure();
        SetLastError(0);
        InternetHandle request(HarnessIdentityRequest(connection.value, L"POST", L"/steam/login",
            nullptr, nullptr, nullptr, 0));
        if (unavailable) {
            if (request.value || GetLastError() != ERROR_ACCESS_DENIED
                || identity_reads != 1 || requests != 0) return failure();
            std::cout << "bridge unavailable identity rejected before send.\n";
            return 0;
        }
        if (!request.value || !WinHttpSendRequest(request.value, nullptr, 0,
            const_cast<char*>(body), sizeof(body)-1, sizeof(body)-1, 0)
            || !WinHttpReceiveResponse(request.value, nullptr)) return failure();
        DWORD status = 0, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &bytes, nullptr) || status != 200) {
            std::cerr << "bridge identity/body HTTP check failed.\n";
            return 1;
        }
    }
    if (identity_reads != iterations || requests != iterations) return failure();
    std::cout << "bridge identity transport and retired connections passed.\n";
    return 0;
}

int success() {
    const LPWSTR actual = actual_command_line();
    if (actual == nullptr || GetCommandLineW() != actual) return failure();
    const std::wstring original(actual); // Independent system command line.
    LPWSTR first = HarnessCommandLine();
    if (first == nullptr) return failure();
    // Independent literal for the one approved fixture configuration.
    const std::wstring expected = original + L" MeteorURI=http://127.0.0.1";
    if (std::wstring_view(first) != expected) return failure();
    if (HarnessCommandLine() != first || std::wstring_view(first) != expected)
        return failure();
    if (std::wstring_view(GetCommandLineW()) != original) return failure();

    IDirectInput8W* input = nullptr;
    const HRESULT status = DirectInput8Create(GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(&input), nullptr);
    if (FAILED(status) || input == nullptr) return failure();
    unsigned devices = 0;
    const HRESULT enumerated = input->EnumDevices(DI8DEVCLASS_ALL,
        count_devices, &devices, DIEDFL_ALLDEVICES);
    input->Release();
    if (FAILED(enumerated)) return failure();
    // No device names/IDs or command line are printed. Enumeration only;
    // keyboard/mouse input and original-game rendering are not verified here.
    std::cout << "fixture-command-line and real DirectInput enumeration passed.\n";
    return 0;
}
}

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--identity-http")
            return identity_http(false);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--identity-unavailable")
            return identity_http(true);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--early-export")
            return early_export();
        if (argc == 2 && std::wstring_view(argv[1]) == L"--passthrough") {
            const LPWSTR actual = actual_command_line();
            if (actual == nullptr) return failure();
            for (unsigned i = 0; i < 3; ++i)
                if (GetCommandLineW() != actual) return failure();
            std::cout << "non-allowlisted caller passed through.\n";
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--production-mismatch")
            return production_mismatch(argv[2]);
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--expected-failure") {
            // A separate process must exit with the bridge's fixed code before
            // returning here. Luna supplies the invalid/missing fixture file.
            (void)HarnessCommandLine();
            return failure();
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--success")
            return success();
        return 2;
    } catch (...) {
        return failure();
    }
}
