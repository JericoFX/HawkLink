#pragma once
#if !defined(_WIN64)
#error Client identity transport supports Windows x64 only.
#endif
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

namespace hawken::client::identity {
struct WinHttpApi {
    decltype(&WinHttpConnect) connect;
    decltype(&WinHttpOpenRequest) open_request;
    decltype(&WinHttpCloseHandle) close_handle;
    decltype(&WinHttpAddRequestHeaders) add_headers;
    decltype(&WinHttpSetOption) set_option;
};
struct IdentityProvider {
    // Trusted caller-owned adapter. Production reads ONLY the already
    // initialized original Hawken SteamUser interface; no Init, new pipe/user,
    // ticket, callbacks or Steam desktop context in the backend. The fixture
    // uses a separate synthetic provider. No ID is authenticated by this API.
    bool (*read)(void* context,std::uint64_t* value) noexcept;
    void* context;
};
struct Config {
    std::string meteor_uri; // Validated routing root supplied by bridge.
    WinHttpApi api;         // Saved REAL functions, never hooked dispatchers.
    IdentityProvider identity;
};
class ConfigurationError : public std::runtime_error {
public:
    ConfigurationError(); // Fixed message, no URI/identity or native text.
};

// Three ABI-compatible adapter methods; no SendRequest hook/body access.
// Future bridge owns SDK SHA/import/callsite verification and SDK-local IAT
// installation outside DllMain. This object neither installs hooks nor loads
// any DLL, starts Steam, creates sockets or logs request/identity information.
// Saved function pointers/provider context must outlive this nonmovable object.
// Only exact meteor_uri==http://127.0.0.1 enables identity transport. Connect
// host literal127.0.0.1 and port0/80 qualify; OpenRequest must be exact POST,
// /steam/login, no WINHTTP_FLAG_SECURE. This connection class is distinct from
// the optional local trial PlayFab API failure class below.
// Selected requests force no proxy and no redirects before adding exactly one
// canonical X-Hawken-SteamID decimal uint64 header. Original body is untouched.
// ID0/unavailable provider/options/header failure: close actual new request,
// returnNULL, SetLastError(ERROR_ACCESS_DENIED). No fallback or partial claim.
// Only the same exact local Meteor root enables a diagnostic rejection for
// host literal3F09.playfabapi.com, port0/443: exact POST /Client/GetTitleData or
// /Client/ExecuteCloudScript with WINHTTP_FLAG_SECURE returnsNULL and
// ERROR_WINHTTP_CANNOT_CONNECT before real OpenRequest, provider, headers or
// options. These are GENERIC APIs, not calendar-only routes. All other requests
// pass through intact. No original body, PlayFab auth or rewards are inspected.
// TODO: [shortcut] Local trial rejects these optional APIs to observe native
// failure callbacks; return them to a compatible service when its calendar
// contract is implemented. This is not a reward implementation or final routing.
// Own resource policy:128 synchronized live classified connection handles
// (identity-local and PlayFab API combined). On
// overflow close ONLY just-created qualifying connection and returnNULL with
// same fixed error; foreign connections never consume quota. Handle generations
// protect classification reuse; close/recovery observable through real APIs.
// No identity/header/body retained in the table, only handle/generation/class. No
// accessor of internal state, no test-mode option in production API.
class Transport {
public:
    explicit Transport(const Config& config);
    Transport(const Transport&)=delete;
    Transport& operator=(const Transport&)=delete;
    Transport(Transport&&)=delete;
    Transport& operator=(Transport&&)=delete;
    HINTERNET connect(HINTERNET session,LPCWSTR host,INTERNET_PORT port,DWORD reserved) noexcept;
    HINTERNET open_request(HINTERNET connection,LPCWSTR verb,LPCWSTR object,
        LPCWSTR version,LPCWSTR referrer,LPCWSTR* accept_types,DWORD flags) noexcept;
    BOOL close_handle(HINTERNET handle) noexcept;
private:
    enum class ConnectionKind { identity_local,playfab_api };
    struct Connection {
        HINTERNET handle=nullptr;
        std::uint64_t generation=0;
        ConnectionKind kind=ConnectionKind::identity_local;
    };
    Config config_;
    std::mutex mutex_;
    std::array<Connection,128> connections_{};
    std::uint64_t generation_=0;
};
}
