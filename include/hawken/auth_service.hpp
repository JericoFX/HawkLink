#pragma once
#include "hawken/local_store.hpp"
#include "hawken/steam_auth.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace hawken::auth {
struct AuthReply {
    int status = 500;
    std::string message;
    nlohmann::json result = nullptr;
};

struct AuthenticatedAccount {
    store::AccountRef account;
    std::string user_guid;
};
struct AccessReply {
    int status = 500;
    std::optional<AuthenticatedAccount> account;
};

// Per-server account facade. Only SteamVerifier success can create an account
// or an access grant. It does not provide menu catalogues or game admission.
// First local diagnostic: one concurrent Steam session. Remote multi-player
// identity resolution is not provided by the local Steam verifier.
// The store and verifier must outlive this instance. Stop callers before teardown.
// Grant lifetime and the stable account GUID are local policies, not recovered
// publisher values. Preserve the database and server_id to preserve these GUIDs.
class AuthService {
public:
    using Clock = std::function<std::int64_t()>;
    static constexpr std::int64_t grant_lifetime_seconds = 900;
    AuthService(store::LocalStore& store, SteamVerifier& verifier, Clock clock = {});
    ~AuthService();
    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;

    AuthReply login(std::string_view json_body);
    // Local bridge extension: the claim is untrusted until native ticket
    // validation returns the same identity and confirms the app license.
    AuthReply login(std::string_view json_body, std::string_view claimed_identity);
    AuthReply profile(std::string_view user_guid, std::string_view authorization);
    // Trusted in-process context for protected catalogue/account handlers.
    // Each call checks grant expiry, Steam revocation and the local ban.
    // A supplied path GUID must match the authenticated account. Without it,
    // the raw Basic grant selects its own account. No ticket/grant/SteamID is
    // returned. This snapshot is not a credential; recheck every request.
    AccessReply access(std::string_view authorization,
                       std::optional<std::string_view> user_guid = std::nullopt);
    AuthReply logout(std::string_view user_guid, std::string_view authorization,
                     std::string_view json_body);
private:
    AuthReply login_impl(std::string_view json_body, std::optional<std::string_view> claimed_identity);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
