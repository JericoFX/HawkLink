#include "hawken/steam_local_auth.hpp"
#include <array>
#include <utility>

#if defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "hawken/sha256.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>
#endif

namespace hawken::auth {
namespace detail {
bool callback143_is_ok(std::span<const std::uint8_t> payload, std::uint64_t hint) {
    if (payload.size() != 20 || hint == 0) return false;
    std::uint64_t identity = 0;
    for (unsigned index = 0; index < 8; ++index)
        identity |= static_cast<std::uint64_t>(payload[index]) << (index * 8U);
    return identity == hint && payload[8] == 0 && payload[9] == 0
        && payload[10] == 0 && payload[11] == 0;
}
}

#if defined(_WIN64)
namespace {
constexpr wchar_t api_path[] =
    L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\steam_api64.dll";
constexpr wchar_t client_path[] = L"C:\\Program Files (x86)\\Steam\\steamclient64.dll";
constexpr char api_hash[] = "38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29";
constexpr char client_hash[] = "86112382982fa855086f566b2fb8343290e798849029337dcfeecba0d5051b5e";
constexpr std::uint32_t app_id = 271290;
constexpr auto bootstrap_budget = std::chrono::seconds(30);
constexpr auto auth_budget = std::chrono::seconds(15);
constexpr auto poll_interval = std::chrono::milliseconds(20);
std::atomic<bool> process_owner{false};
std::atomic<bool> process_attempted{false};

struct CallbackMessage {
    std::int32_t user;
    std::int32_t callback;
    const std::uint8_t* payload;
    std::int32_t bytes;
};
static_assert(sizeof(CallbackMessage) == 24);
static_assert(offsetof(CallbackMessage, user) == 0);
static_assert(offsetof(CallbackMessage, callback) == 4);
static_assert(offsetof(CallbackMessage, payload) == 8);
static_assert(offsetof(CallbackMessage, bytes) == 16);

struct Api {
    using Init = bool (__cdecl*)(std::uint32_t, std::uint16_t, std::uint16_t,
                                std::uint16_t, std::int32_t, const char*);
    using Handle = std::int32_t (__cdecl*)();
    using Factory = void* (__cdecl*)(const char*);
    using Interface = void* (__cdecl*)(void*, std::int32_t, std::int32_t, const char*);
    using Utilities = void* (__cdecl*)(void*, std::int32_t, const char*);
    using AppId = std::uint32_t (__cdecl*)(void*);
    using Void = void (__cdecl*)();
    using Metadata = void (__cdecl*)(void*, const char*);
    using ServerBool = void (__cdecl*)(void*, bool);
    using ServerVoid = void (__cdecl*)(void*);
    using LoggedOn = bool (__cdecl*)(void*);
    using Begin = std::int32_t (__cdecl*)(void*, const void*, std::int32_t, std::uint64_t);
    using End = void (__cdecl*)(void*, std::uint64_t);
    using License = std::int32_t (__cdecl*)(void*, std::uint64_t, std::uint32_t);
    using GetCallback = bool (__cdecl*)(std::int32_t, CallbackMessage*);
    using FreeCallback = void (__cdecl*)(std::int32_t);
    Init init{};
    Handle pipe{}, user{};
    Factory factory{};
    Interface server{};
    Utilities utilities{};
    AppId appid{};
    Void pump{}, shutdown{};
    Metadata product{}, description{}, mod_dir{};
    ServerBool dedicated{}, heartbeats{};
    ServerVoid logon{}, logoff{};
    LoggedOn logged_on{};
    Begin begin{};
    End end{};
    License license{};
    GetCallback get_callback{};
    FreeCallback free_callback{};
};

template<class Function>
bool bind(HMODULE module, const char* name, Function& function) {
    const auto address = GetProcAddress(module, name);
    static_assert(sizeof(Function) == sizeof(address));
    function = std::bit_cast<Function>(address);
    return address != nullptr;
}

bool module_matches(HMODULE module, const wchar_t* expected) {
    std::array<wchar_t, 1024> name{};
    const DWORD count = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    return count != 0 && count < name.size()
        && CompareStringOrdinal(name.data(), static_cast<int>(count), expected, -1, TRUE) == CSTR_EQUAL;
}

bool local_individual(std::uint64_t identity) {
    const auto universe = identity >> 56;
    const auto type = (identity >> 52) & 0xfU;
    const auto instance = (identity >> 32) & 0xfffffU;
    return universe > 0 && universe < 5 && type == 1 && instance <= 4
        && static_cast<std::uint32_t>(identity) != 0;
}

class CallbackRelease {
public:
    CallbackRelease(const Api& api, std::int32_t pipe) : api_(api), pipe_(pipe) {}
    ~CallbackRelease() { api_.free_callback(pipe_); }
    CallbackRelease(const CallbackRelease&) = delete;
    CallbackRelease& operator=(const CallbackRelease&) = delete;
private:
    const Api& api_;
    std::int32_t pipe_;
};

class NativeState {
public:
    ~NativeState() { close(); }
    explicit NativeState(const LocalSteamObserver& observer) : observer_(observer) {}
    NativeState(const NativeState&) = delete;
    NativeState& operator=(const NativeState&) = delete;

    void observe_startup(bool available) const noexcept {
        LocalSteamObservation observation;
        observation.phase = LocalSteamPhase::startup;
        observation.available = available;
        observe(observation);
    }

    bool initialize(const std::atomic<bool>& stopping) {
        if (sha256_file(api_path) != api_hash || sha256_file(client_path) != client_hash)
            return false;
        if (!SetEnvironmentVariableW(L"SteamAppId", L"271290")
            || !SetEnvironmentVariableW(L"SteamGameId", L"271290")) return false;
        constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        // References are intentionally retained until process exit, also after failure.
        const HMODULE native_client = LoadLibraryExW(client_path, nullptr, flags);
        if (native_client == nullptr || !module_matches(native_client, client_path)) return false;
        const HMODULE native_api = LoadLibraryExW(api_path, nullptr, flags);
        if (native_api == nullptr || !module_matches(native_api, api_path)) return false;
        if (!bind(native_api, "SteamInternal_GameServer_Init", api_.init)
            || !bind(native_api, "SteamGameServer_GetHSteamPipe", api_.pipe)
            || !bind(native_api, "SteamGameServer_GetHSteamUser", api_.user)
            || !bind(native_api, "SteamInternal_CreateInterface", api_.factory)
            || !bind(native_api, "SteamAPI_ISteamClient_GetISteamGameServer", api_.server)
            || !bind(native_api, "SteamAPI_ISteamClient_GetISteamUtils", api_.utilities)
            || !bind(native_api, "SteamAPI_ISteamUtils_GetAppID", api_.appid)
            || !bind(native_api, "SteamGameServer_RunCallbacks", api_.pump)
            || !bind(native_api, "SteamGameServer_Shutdown", api_.shutdown)
            || !bind(native_api, "SteamAPI_ISteamGameServer_SetProduct", api_.product)
            || !bind(native_api, "SteamAPI_ISteamGameServer_SetGameDescription", api_.description)
            || !bind(native_api, "SteamAPI_ISteamGameServer_SetModDir", api_.mod_dir)
            || !bind(native_api, "SteamAPI_ISteamGameServer_SetDedicatedServer", api_.dedicated)
            || !bind(native_api, "SteamAPI_ISteamGameServer_EnableHeartbeats", api_.heartbeats)
            || !bind(native_api, "SteamAPI_ISteamGameServer_LogOnAnonymous", api_.logon)
            || !bind(native_api, "SteamAPI_ISteamGameServer_BLoggedOn", api_.logged_on)
            || !bind(native_api, "SteamAPI_ISteamGameServer_LogOff", api_.logoff)
            || !bind(native_api, "SteamAPI_ISteamGameServer_BeginAuthSession", api_.begin)
            || !bind(native_api, "SteamAPI_ISteamGameServer_EndAuthSession", api_.end)
            || !bind(native_api, "SteamAPI_ISteamGameServer_UserHasLicenseForApp", api_.license)
            || !bind(native_client, "Steam_BGetCallback", api_.get_callback)
            || !bind(native_client, "Steam_FreeLastCallback", api_.free_callback)) return false;
        if (api_.pipe() != 0 || api_.user() != 0) return false;
        const auto beginning = std::chrono::steady_clock::now();
        const bool initialized = api_.init(0x7f000001U, 26020, 26021, 26022, 2, "1.6.0.154767");
        init_returned_ = true; // One GS Shutdown after every normal Init return, including false.
        if (!initialized) return false;
        pipe_ = api_.pipe();
        user_ = api_.user();
        if (pipe_ == 0 || user_ == 0) return false;
        void* client = api_.factory("SteamClient017");
        if (client == nullptr) return false;
        server_ = api_.server(client, user_, pipe_, "SteamGameServer012");
        void* utilities = api_.utilities(client, pipe_, "SteamUtils008");
        if (server_ == nullptr || utilities == nullptr || api_.appid(utilities) != app_id)
            return false;
        api_.heartbeats(server_, false);
        api_.product(server_, "271290");
        api_.description(server_, "HAWKEN");
        api_.mod_dir(server_, "Hawken");
        api_.dedicated(server_, true);
        api_.logon(server_);
        logon_returned_ = true;
        api_.heartbeats(server_, false);
        while (!stopping.load() && std::chrono::steady_clock::now() - beginning < bootstrap_budget) {
            // Bootstrap only: no BeginAuthSession has occurred, so no auth143 is consumed.
            api_.pump();
            if (api_.logged_on(server_)) break;
            std::this_thread::sleep_for(poll_interval);
        }
        if (stopping.load() || !api_.logged_on(server_)
            || std::chrono::steady_clock::now() - beginning >= bootstrap_budget) return false;
        ready_ = true;
        return true;
    }

    SteamAuthResult verify(std::span<const std::uint8_t> ticket, std::uint64_t claimed_identity,
                           const std::atomic<bool>& stopping) {
        // No retry on this pipe, including after success/logout. A late callback143
        // lacks an attempt nonce and must never approve a later ticket for this hint.
        if (attempted_) return {};
        if (!local_individual(claimed_identity)) return {SteamAuthStatus::rejected, 0};
        if (ticket.empty() || ticket.size() > 1024) return {SteamAuthStatus::invalid_ticket, 0};
        if (!alive()) return {};
        if (poll() == Poll::failed) return {SteamAuthStatus::rejected, 0};
        // Private, untrusted claim; only callback143 plus license can validate it.
        hint_ = claimed_identity;
        attempted_ = true;
        process_attempted.store(true);
        const auto result = api_.begin(server_, ticket.data(), static_cast<std::int32_t>(ticket.size()), hint_);
        LocalSteamObservation observation;
        observation.phase = LocalSteamPhase::begin;
        observation.begin_ok = result == 0;
        observe(observation);
        if (result != 0) return {SteamAuthStatus::rejected, 0};
        session_started_ = true;
        const auto beginning = std::chrono::steady_clock::now();
        while (!stopping.load() && std::chrono::steady_clock::now() - beginning < auth_budget) {
            if (!alive()) return {};
            const auto verdict = poll();
            if (verdict == Poll::failed) return {SteamAuthStatus::rejected, 0};
            if (verdict == Poll::approved) {
                if (!has_license(hint_)) {
                    invalidate();
                    return {SteamAuthStatus::license_missing, 0};
                }
                validated_ = hint_;
                return {SteamAuthStatus::authenticated, validated_};
            }
            std::this_thread::sleep_for(poll_interval);
        }
        invalidate();
        return {SteamAuthStatus::timed_out, 0};
    }

    SteamAuthResult reject_invalid_ticket() const {
        return attempted_ ? SteamAuthResult{} : SteamAuthResult{SteamAuthStatus::invalid_ticket, 0};
    }

    bool is_authenticated(std::uint64_t identity) {
        idle();
        if (identity == 0 || identity != validated_) return false;
        if (!has_license(identity)) { invalidate(); return false; }
        return true;
    }

    void end_session(std::uint64_t identity) {
        if (identity != 0 && identity == hint_) invalidate();
    }

    void idle() {
        if (alive()) (void)poll();
    }

    void invalidate() {
        validated_ = 0;
        if (session_started_) {
            session_started_ = false;
            api_.end(server_, hint_);
            LocalSteamObservation observation;
            observation.phase = LocalSteamPhase::session_end;
            observe(observation);
        }
    }

private:
    enum class Poll { empty, approved, failed };

    void observe(const LocalSteamObservation& observation) const noexcept {
        try { if (observer_) observer_(observation); } catch (...) {}
    }

    bool has_license(std::uint64_t identity) const {
        const bool licensed = api_.license(server_, identity, app_id) == 0;
        LocalSteamObservation observation;
        observation.phase = LocalSteamPhase::license;
        observation.license_ok = licensed;
        observe(observation);
        return licensed;
    }

    bool alive() {
        if (!ready_) return false;
        if (api_.pipe() != pipe_ || api_.user() != user_ || !api_.logged_on(server_)) {
            ready_ = false;
            invalidate();
            return false;
        }
        return true;
    }

    Poll poll() {
        Poll result = Poll::empty;
        // No RunCallbacks here: the legacy poll must retain ownership of auth143.
        // Bound work per tick so irrelevant callback traffic cannot starve commands.
        for (unsigned index = 0; index < 64; ++index) {
            CallbackMessage message{};
            if (!api_.get_callback(pipe_, &message)) return result;
            CallbackRelease release(api_, pipe_);
            if (message.callback != 143) continue;
            if (message.user == user_) {
                LocalSteamObservation observation;
                observation.phase = LocalSteamPhase::callback143;
                observation.packed20 = message.bytes == 20;
                if (observation.packed20 && message.payload != nullptr) {
                    std::uint64_t identity = 0;
                    for (unsigned byte = 0; byte < 8; ++byte)
                        identity |= static_cast<std::uint64_t>(message.payload[byte]) << (byte * 8U);
                    observation.identity_match = hint_ != 0 && identity == hint_;
                    observation.response_ok = message.payload[8] == 0 && message.payload[9] == 0
                        && message.payload[10] == 0 && message.payload[11] == 0;
                }
                observe(observation);
            }
            if (message.user != user_ || message.bytes != 20 || message.payload == nullptr
                || !detail::callback143_is_ok({message.payload, 20}, hint_)) {
                invalidate();
                result = Poll::failed;
            } else if (session_started_ && result != Poll::failed) {
                result = Poll::approved;
            }
        }
        // Never approve while unexamined queued callbacks could contain revocation.
        return result == Poll::failed ? Poll::failed : Poll::empty;
    }

    void close() noexcept {
        ready_ = false;
        LocalSteamObservation observation;
        observation.phase = LocalSteamPhase::shutdown;
        const bool owned_context = init_returned_;
        try { invalidate(); } catch (...) {}
        if (logon_returned_) {
            logon_returned_ = false;
            try { api_.logoff(server_); } catch (...) {}
        }
        if (init_returned_) {
            init_returned_ = false;
            try {
                api_.shutdown();
                observation.gs_shutdown_returned = true;
                observation.gs_pipe_zero = api_.pipe() == 0;
            } catch (...) {}
        }
        if (owned_context) observe(observation);
        SecureZeroMemory(&hint_, sizeof(hint_));
        SecureZeroMemory(&validated_, sizeof(validated_));
    }

    const LocalSteamObserver& observer_;
    Api api_;
    void* server_{};
    std::int32_t pipe_{}, user_{};
    std::uint64_t hint_{}, validated_{};
    bool init_returned_{}, logon_returned_{}, session_started_{}, ready_{}, attempted_{};
};

enum class Command { none, verify, check, end };
struct Request {
    Command command = Command::none;
    std::array<std::uint8_t, 1024> ticket{};
    std::size_t bytes = 0;
    std::uint64_t identity = 0;
};

class TicketErase {
public:
    explicit TicketErase(Request& request) : request_(request) {}
    ~TicketErase() {
        SecureZeroMemory(request_.ticket.data(), request_.ticket.size());
        SecureZeroMemory(&request_.identity, sizeof(request_.identity));
    }
private:
    Request& request_;
};

class LocalSteamVerifier final : public SteamVerifier {
public:
    explicit LocalSteamVerifier(LocalSteamObserver observer) : observer_(std::move(observer)) {}

    ~LocalSteamVerifier() override {
        stopping_.store(true);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        SecureZeroMemory(request_.ticket.data(), request_.ticket.size());
        SecureZeroMemory(&request_.identity, sizeof(request_.identity));
        process_owner.store(false);
    }

    bool start() {
        worker_ = std::thread([this] { run(); });
        std::unique_lock lock(state_mutex_);
        finished_.wait(lock, [this] { return startup_finished_; });
        return startup_ready_ && !worker_finished_;
    }

    SteamAuthResult verify(std::span<const std::uint8_t>) override {
        // No desktop-user identity lookup or ticket-only fallback.
        return {};
    }

    SteamAuthResult verify(std::span<const std::uint8_t> ticket,
                           std::uint64_t claimed_identity) override {
        if (!local_individual(claimed_identity)) return {SteamAuthStatus::rejected, 0};
        // Serialize complete public operations, not merely individual native calls.
        std::lock_guard operation(operation_mutex_);
        Request request;
        TicketErase erase(request);
        request.command = Command::verify;
        request.identity = claimed_identity;
        if (ticket.size() > request.ticket.size()) request.bytes = request.ticket.size() + 1;
        else {
            request.bytes = ticket.size();
            std::copy(ticket.begin(), ticket.end(), request.ticket.begin());
        }
        return submit(request).auth;
    }

    bool is_authenticated(std::uint64_t identity) const override {
        std::lock_guard operation(operation_mutex_);
        Request request;
        request.command = Command::check;
        request.identity = identity;
        return submit(request).authenticated;
    }

    void end_session(std::uint64_t identity) override {
        std::lock_guard operation(operation_mutex_);
        Request request;
        request.command = Command::end;
        request.identity = identity;
        (void)submit(request);
    }

private:
    struct Response {
        SteamAuthResult auth;
        bool authenticated = false;
    };

    Response submit(const Request& request) const {
        std::unique_lock lock(state_mutex_);
        if (!startup_ready_ || worker_finished_) return {};
        request_ = request;
        response_ = {};
        command_finished_ = false;
        wake_.notify_one();
        finished_.wait(lock, [this] { return command_finished_ || worker_finished_; });
        return response_;
    }

    void run() noexcept {
        NativeState state(observer_); // All API calls and observer notifications on this worker.
        try {
            const bool initialized = state.initialize(stopping_);
            state.observe_startup(initialized);
            {
                std::lock_guard lock(state_mutex_);
                startup_ready_ = initialized;
                startup_finished_ = true;
                finished_.notify_all();
            }
            if (initialized) {
                while (!stopping_.load()) {
                    Request request;
                    TicketErase erase(request);
                    {
                        std::unique_lock lock(state_mutex_);
                        wake_.wait_for(lock, poll_interval, [this] {
                            return stopping_.load() || request_.command != Command::none;
                        });
                        if (stopping_.load()) break;
                        request = request_;
                        SecureZeroMemory(request_.ticket.data(), request_.ticket.size());
                        SecureZeroMemory(&request_.identity, sizeof(request_.identity));
                        request_.command = Command::none;
                    }
                    if (request.command == Command::none) { state.idle(); continue; }
                    Response response;
                    if (request.command == Command::verify) {
                        if (request.bytes > request.ticket.size()) {
                            response.auth = state.reject_invalid_ticket();
                        } else response.auth = state.verify({request.ticket.data(), request.bytes},
                                                            request.identity, stopping_);
                    } else if (request.command == Command::check) {
                        response.authenticated = state.is_authenticated(request.identity);
                    } else if (request.command == Command::end) state.end_session(request.identity);
                    {
                        std::lock_guard lock(state_mutex_);
                        response_ = response;
                        command_finished_ = true;
                        finished_.notify_all();
                    }
                }
            }
        } catch (...) {
            // Fixed status only; no native errors, payloads, IDs, paths or ticket diagnostics.
            if (!startup_finished_) state.observe_startup(false);
        }
        {
            std::lock_guard lock(state_mutex_);
            SecureZeroMemory(request_.ticket.data(), request_.ticket.size());
            SecureZeroMemory(&request_.identity, sizeof(request_.identity));
            response_ = {};
            startup_finished_ = true;
            worker_finished_ = true;
            finished_.notify_all();
        }
    }

    LocalSteamObserver observer_;
    mutable std::mutex operation_mutex_, state_mutex_;
    mutable std::condition_variable wake_, finished_;
    mutable Request request_;
    mutable Response response_;
    mutable bool command_finished_ = false;
    bool startup_finished_ = false, startup_ready_ = false, worker_finished_ = false;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};
}
#endif

std::unique_ptr<SteamVerifier> make_local_steam_verifier(LocalSteamObserver observer) {
#if defined(_WIN64)
    if (process_attempted.load()) return nullptr;
    bool expected = false;
    if (!process_owner.compare_exchange_strong(expected, true)) return nullptr;
    if (process_attempted.load()) {
        process_owner.store(false);
        return nullptr;
    }
    std::unique_ptr<LocalSteamVerifier> verifier;
    try {
        verifier = std::make_unique<LocalSteamVerifier>(std::move(observer));
        if (!verifier->start()) return nullptr;
        return verifier;
    } catch (...) {
        // Once constructed, the verifier releases the claim exactly once itself.
        if (!verifier) process_owner.store(false);
        return nullptr;
    }
#else
    (void)observer;
    // No implementation of the original Windows DLL ABI on other platforms.
    return nullptr;
#endif
}
}
