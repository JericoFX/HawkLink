#pragma once
#include "hawken/auth_service.hpp"
#include "hawken/session_wire.hpp"
#include "hawken/posthello_control.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace hawken::assignment {
struct Destination {
    std::string server_guid;
    std::string ip;
    std::int32_t port;
    std::string region;
    std::string game_version;
};
// Explicit new server policy, not recovered matchmaking ratings or TTL.
struct AssignmentPolicy {
    std::int64_t lifetime_seconds;
    std::int32_t skill_rating_average;
    std::int32_t skill_rating_search_min;
};
struct SessionProvision {
    store::AccountRef account;
    std::string user_guid;
    session::SessionId id{};
    session::SessionKey key{};
    Destination destination;
    std::int64_t expires_at_monotonic = 0;
};

// The UDP owner installs ONE server Context using precisely this provision.
// A returned active lease confirms local installation, not network atomicity,
// gameplay readiness or admission. It must own retirement of that context.
// Destruction is noexcept and synchronizes retirement with all in-flight UDP
// users; it cannot destroy a context still in use. active() is thread-safe.
// Neither method nor destruction may re-enter SessionAssignmentService.
class UdpSessionLease {
public:
    virtual ~UdpSessionLease() = default;
    virtual bool active() const noexcept = 0;
};

// First local slice: exactly one account/grant selection, session and optional
// advertisement in memory. No SQL, directory, entitlement or GameServer claim.
// Destination GUID: canonical lower-case nonzero UUID; IP: IPv4 loopback;
// port: 1..65535. Region/version: ASCII [A-Za-z0-9_.-], 1..64 bytes.
// TTL: 1..900 seconds. Invalid configuration throws invalid_argument with the
// fixed message "Invalid session assignment configuration." after RED.
// Clocks are separate: nonnegative monotonic seconds govern expiry (exclusive
// boundary, regression fails closed); UTC Unix seconds format TimeCreated only
// (ISO8601, range 0..253402300799). UTC changes cannot extend session lifetime.
// RandomFill must provide production cryptographic randomness; fixtures inject
// public bytes. No credentials are generated from a Steam ID or client input.
// AuthService/callback owners must outlive this service. Stop all callers before
// teardown. Complete operations are serialized under a private service mutex:
// service -> AuthService -> store/verifier, and service -> owner callbacks.
// Owner callbacks must not re-enter. Copy the datagram ID before registry access.
// Call authorize_udp BEFORE obtaining/pinning any lease, Context or in-flight user.
// Then access registry and recheck active. While pinned/in-flight, no service/Auth
// calls/callbacks; retirement waits only on users that finished authorization.
// Owner must call authorize_udp before use and on idle lifecycle checks; this
// service has no background thread and does not promise instant revocation.
class SessionAssignmentService {
public:
    using Clock = std::function<std::int64_t()>;
    using RandomFill = std::function<bool(std::span<std::uint8_t>)>;
    using DestinationBound = std::function<bool(const Destination&)>;
    // Null, inactive or throwing handoff publishes no session/ad DTO. On throw
    // or null the owner must roll back any partial install; active lease teardown
    // must retire it safely. These are internal owner duties, not client flags.
    using Handoff = std::function<std::unique_ptr<UdpSessionLease>(const SessionProvision&)>;
    static constexpr std::size_t max_assignments = 1;
    static constexpr std::size_t max_advertisements = 1;
    static constexpr std::size_t max_request_bytes = 4096;

    SessionAssignmentService(auth::AuthService& auth, Destination destination,
        AssignmentPolicy policy, Clock monotonic_clock, Clock utc_clock,
        RandomFill random, DestinationBound bound, Handoff handoff);
    ~SessionAssignmentService();
    SessionAssignmentService(const SessionAssignmentService&) = delete;
    SessionAssignmentService& operator=(const SessionAssignmentService&) = delete;

    bool supports_target(std::string_view raw_target) const;
    // Trusted direct-GUID selection is separate from legacy POST; GET /session
    // never chooses via query. Identical selection preserves Context/counters.
    auth::AuthReply select(std::string_view authorization, std::string_view server_guid);
    // Exact GET /session and POST /hawkenClientMatchmakingAdvertisements;
    // GET/DELETE /hawkenClientMatchmakingAdvertisements/{canonical lower UUID}.
    // GET/DELETE bodies must be empty. DELETE retires only the matching account/
    // grant advertisement and session before its void ACK; unknown/repeated 404
    // and this lifecycle are local policy. POST is one authenticated participant:
    // exactly OwnerGuid, Region, GameVersion, Users=[OwnerGuid], plus optional
    // RequestedServerGuid. Metadata must match destination. No other filters.
    // Omitted RequestedServerGuid requires prior same-grant selection. Equivalent
    // POSTs are idempotent. Unknown target 404 precedes auth; wrong method 405.
    // Direct selection has ClientAdvertisementGuid="" as new absence policy.
    // Ratings/TimeCreated are our explicit policy, not historic values.
    auth::AuthReply handle(std::string_view method, std::string_view raw_target,
        std::string_view authorization, std::string_view body = {});
    // Owner-only check; revalidates the retained grant, ban, monotonic expiry,
    // bound destination and lease. Retires invalid retained state; an unknown
    // ID alone never retires a different, valid active session.
    bool authorize_udp(const session::SessionId& id) noexcept;
    // Owner-only identity consistency check, not admission or ID16 validation.
    // Rechecks the same retained authorization/lifecycle before comparing Response
    // to the server-owned user GUID. Narrow/wide ASCII GUID36, case-insensitive
    // hex, optionally exactly one final NUL; no URL or Unicode normalization.
    // Unknown ID or malformed/mismatched claim cannot evict a valid session.
    // Call BEFORE any registry/lease/Context/in-flight pin, with no owner locks.
    // A true result is this check only; owner must revalidate before applying it.
    // Concurrent revocation may follow; this is not an atomic admission grant.
    bool validate_login_response(const session::SessionId& id,
        const control::posthello::RawString& response) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
