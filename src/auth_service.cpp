#include "hawken/auth_service.hpp"
#include "hawken/sha256.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/crypto.h>
#include <openssl/rand.h>
#endif

namespace {
using namespace hawken::auth;
void erase(void* bytes, std::size_t size) noexcept {
#ifdef _WIN32
    SecureZeroMemory(bytes, size);
#else
    OPENSSL_cleanse(bytes, size);
#endif
}
AuthReply failure(int status) {
    switch (status) {
    case 400: return {status, "Invalid authentication request.", nullptr};
    case 401: return {status, "Authentication required.", nullptr};
    case 403: return {status, "Steam authentication or access was rejected.", nullptr};
    case 503: return {status, "Steam authentication is not available.", nullptr};
    default: return {500, "Request could not be processed.", nullptr};
    }
}
std::optional<std::string> single_string(std::string_view body, std::string_view key) {
    if (body.empty() || body.size() > 8192) return std::nullopt;
    // nlohmann normally keeps the final value of a duplicate key. Reject it
    // explicitly before interpreting this one-field wire contract.
    bool duplicate = false;
    std::size_t keys = 0;
    auto parsed = nlohmann::json::parse(body.begin(), body.end(),
        [&](int, nlohmann::json::parse_event_t event, nlohmann::json&) {
            if (event == nlohmann::json::parse_event_t::key && ++keys > 1) duplicate = true;
            return true;
        }, false);
    if (parsed.is_discarded() || duplicate || !parsed.is_object() || parsed.size() != 1)
        return std::nullopt;
    const auto found = parsed.find(std::string(key));
    if (found == parsed.end() || !found->is_string()) return std::nullopt;
    return found->get<std::string>();
}
struct PrivateString {
    std::string value;
    ~PrivateString() { if (!value.empty()) erase(value.data(), value.size()); }
};
struct PrivateTicket {
    std::array<std::uint8_t, 1024> bytes{};
    std::size_t size = 0;
    ~PrivateTicket() { erase(bytes.data(), bytes.size()); }
};
int upper_hex(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
bool decode_ticket(std::string_view body, PrivateTicket& ticket) {
    auto text = single_string(body, "SteamAuthTicket");
    if (!text) return false;
    PrivateString private_text{std::move(*text)};
    const auto& encoded = private_text.value;
    if (encoded.empty() || encoded.size() > 2048 || encoded.size() % 2 != 0) return false;
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = upper_hex(encoded[index]), low = upper_hex(encoded[index + 1]);
        if (high < 0 || low < 0) return false;
        ticket.bytes[index / 2] = static_cast<std::uint8_t>((high << 4) | low);
    }
    ticket.size = encoded.size() / 2;
    return true;
}
std::optional<std::uint64_t> parse_claim(std::string_view text) {
    if (text.empty() || text.size() > 20 || text.front() == '0') return std::nullopt;
    for (const char value : text)
        if (value < '0' || value > '9') return std::nullopt;
    std::uint64_t identity = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), identity);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    const auto universe = identity >> 56;
    const auto type = (identity >> 52) & 0xfU;
    const auto instance = (identity >> 32) & 0xfffffU;
    if (universe == 0 || universe >= 5 || type != 1 || instance > 4
        || static_cast<std::uint32_t>(identity) == 0) return std::nullopt;
    return identity;
}
std::int64_t system_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string account_guid(const hawken::store::AccountRef& account) {
    const auto scope = account.server_id + "/" + std::to_string(account.local_id);
    const auto digest = hawken::sha256(std::span(
        reinterpret_cast<const std::uint8_t*>(scope.data()), scope.size()));
    // The client uses a 128-bit FGuid, not a requirement for UUIDv4 version bits.
    return digest.substr(0, 8) + "-" + digest.substr(8, 4) + "-" + digest.substr(12, 4)
        + "-" + digest.substr(16, 4) + "-" + digest.substr(20, 12);
}
std::string random_grant() {
    PrivateTicket random;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, random.bytes.data(), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Access grant generation failed.");
#else
    if (RAND_bytes(random.bytes.data(), 32) != 1)
        throw std::runtime_error("Access grant generation failed.");
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < 32; ++index) {
        result[2 * index] = hex[random.bytes[index] >> 4];
        result[2 * index + 1] = hex[random.bytes[index] & 15];
    }
    return result;
}
}

namespace hawken::auth {
struct AuthService::Impl {
    struct Grant {
        std::string token;
        std::string guid;
        store::AccountRef account;
        std::uint64_t steam_id = 0;
        std::int64_t issued_at = 0;
        std::int64_t expires_at = 0;
    };
    store::LocalStore& database;
    SteamVerifier& verifier;
    Clock clock;
    std::mutex mutex;
    std::optional<Grant> current;
    Impl(store::LocalStore& store, SteamVerifier& native, Clock time)
        : database(store), verifier(native), clock(time ? std::move(time) : Clock{system_seconds}) {}
    ~Impl() { clear(); }
    void clear() noexcept {
        if (!current) return;
        const auto id = current->steam_id;
        erase(current->token.data(), current->token.size());
        current.reset();
        try { verifier.end_session(id); } catch (...) {}
    }
    int access(std::string_view guid, std::string_view authorization) {
        if (!current || guid != current->guid || !authorization.starts_with("Basic ")
            || authorization.substr(6) != current->token) return 401;
        const auto now = clock();
        if (now < current->issued_at || now >= current->expires_at
            || !verifier.is_authenticated(current->steam_id)) {
            clear();
            return 401;
        }
        if (database.is_banned(current->account, now)) {
            clear();
            return 403;
        }
        return 200;
    }
};
AuthService::AuthService(store::LocalStore& store, SteamVerifier& verifier, Clock clock)
    : impl_(std::make_unique<Impl>(store, verifier, std::move(clock))) {}
AuthService::~AuthService() = default;
AuthReply AuthService::login(std::string_view body, std::string_view claimed_identity) {
    return login_impl(body, claimed_identity);
}
AuthReply AuthService::login(std::string_view body) {
    return login_impl(body, std::nullopt);
}
AuthReply AuthService::login_impl(std::string_view body, std::optional<std::string_view> claimed_identity) {
    try {
        PrivateTicket ticket;
        if (!decode_ticket(body, ticket)) return failure(400);
        std::optional<std::uint64_t> claim;
        if (claimed_identity) {
            claim = parse_claim(*claimed_identity);
            if (!claim) return failure(400);
        }
        std::lock_guard lock(impl_->mutex);
        impl_->clear();
        const auto now = impl_->clock();
        if (now < 0 || now > std::numeric_limits<std::int64_t>::max() - grant_lifetime_seconds)
            return failure(500);
        SteamAuthResult native;
        try {
            const auto bytes = std::span(ticket.bytes).first(ticket.size);
            native = claim ? impl_->verifier.verify(bytes, *claim) : impl_->verifier.verify(bytes);
        }
        catch (...) { return failure(503); }
        if (native.status != SteamAuthStatus::authenticated) {
            if (native.status == SteamAuthStatus::unavailable || native.status == SteamAuthStatus::timed_out)
                return failure(503);
            return failure(403);
        }
        if (native.steam_id == 0) return failure(503);
        // Keep ownership of a verified session through SQL/crypto failures.
        struct SessionOwner {
            SteamVerifier& verifier;
            std::uint64_t id;
            bool retained = false;
            ~SessionOwner() {
                if (!retained) { try { verifier.end_session(id); } catch (...) {} }
            }
        } owner{impl_->verifier, native.steam_id};
        if (claim && native.steam_id != *claim) return failure(403);
        if (!impl_->verifier.is_authenticated(native.steam_id)) return failure(503);
        const auto account = impl_->database.get_or_create_account(std::to_string(native.steam_id), now);
        if (impl_->database.is_banned(account.ref, now)) return failure(403);
        Impl::Grant grant{random_grant(), account_guid(account.ref), account.ref,
            native.steam_id, now, now + grant_lifetime_seconds};
        // Profile is already provisioned, so the separate create-account path
        // is unnecessary. This is a local policy, not a claim about Meteor users.
        AuthReply reply{200, "", {{"AccessGrant", grant.token}, {"UserId", grant.guid}, {"NewUser", false}}};
        impl_->current.emplace(std::move(grant));
        owner.retained = true;
        return reply;
    } catch (...) { return failure(500); }
}
AuthReply AuthService::profile(std::string_view guid, std::string_view authorization) {
    try {
        std::lock_guard lock(impl_->mutex);
        const auto status = impl_->access(guid, authorization);
        if (status != 200) return failure(status);
        // No invented nickname, email, roles, PlayFab identity or inventory.
        return {200, "", {{"Guid", impl_->current->guid}, {"Roles", nlohmann::json::array()}}};
    } catch (...) { return failure(500); }
}
AuthReply AuthService::logout(std::string_view guid, std::string_view authorization, std::string_view body) {
    try {
        std::lock_guard lock(impl_->mutex);
        const auto status = impl_->access(guid, authorization);
        if (status != 200) return failure(status);
        auto token = single_string(body, "AccessGrant");
        if (!token) return failure(400);
        PrivateString private_token{std::move(*token)};
        if (private_token.value != impl_->current->token) return failure(400);
        impl_->clear();
        return {200, "", nullptr};
    } catch (...) { return failure(500); }
}
AccessReply AuthService::access(std::string_view authorization, std::optional<std::string_view> guid) {
    try {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->current) return {401, std::nullopt};
        const auto expected_guid = guid.value_or(impl_->current->guid);
        const auto status = impl_->access(expected_guid, authorization);
        if (status != 200) return {status, std::nullopt};
        return {200, AuthenticatedAccount{impl_->current->account, impl_->current->guid}};
    } catch (...) { return {500, std::nullopt}; }
}
}
