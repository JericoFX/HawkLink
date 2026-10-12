#pragma once
#include "hawken/game_connection.hpp"
#include "hawken/initial_controller.hpp"
#include "hawken/session_assignment.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace hawken::udp {
enum class LocalUdpErrorCode { invalid_configuration, bind_failed, invalid_lifecycle };
class LocalUdpError : public std::runtime_error {
public:
    explicit LocalUdpError(LocalUdpErrorCode code);
    LocalUdpErrorCode code() const noexcept;
private:
    LocalUdpErrorCode code_;
};
// Thread-safe scalar observations only; no ID/key/account/peer or raw payload.
// Successful authenticated receive is NOT game admission. Fields are monotonic
// aggregate observations, not a transaction spanning all counters/flags.
struct LocalUdpDiagnostics {
    std::uint64_t authenticated_datagrams = 0;
    bool hello_validated = false;
    bool closed = false;
    // Sticky only after GameConnection returns a candidate successfully. No
    // raw Login fields retained/logged, no identity verification or admission.
    bool login_received = false;
    // Sticky only after a configured validator accepted AND fresh authorization
    // and exact active-entry checks succeeded. Identity consistency, NOT admission.
    bool login_response_validated = false;
    // Sticky only after supplied metadata was successfully published or Join9
    // was successfully returned. Neither observation proves world spawn/admission.
    bool world_published = false;
    bool join_requested = false;
    // Sticky only after the explicit initial-controller datagrams were sent.
    // Payload transmission, NOT client actor creation, possession or gameplay.
    bool controller_sent = false;
    bool operator==(const LocalUdpDiagnostics&) const = default;
};

struct InitialControllerPlan {
    wire::reliable::ActorChannelConfig channel;
    std::array<std::uint32_t,4> archetype_package_guid;
    std::int32_t archetype_local_net_index;
    wire::actor::QuantizedPosition position;
    std::optional<wire::actor::Rotation> rotation;
    std::uint8_t net_player_index;
};

// FIRST local diagnostic pipe, not a multiplayer GameServer or admission API.
// Exclusive IPv4 127.0.0.1 bind. Production callers supply a nonzero port;
// port0 is reserved for isolated fixtures and port() publishes getsockname.
// Skeleton ID/key must be zero: only handoff's authorized SessionProvision
// supplies real material. All transport seeds/limits, endian, challenge, version
// interval, reply flags and post-Hello login config are explicit caller
// inputs, not native defaults.
// Clock is finite nonnegative monotonic seconds, shared by receive and tick;
// use the SAME epoch as SessionProvision/Assignment's monotonic expiry clock.
// poll_interval must be1..1000ms; it bounds idle authorization/socket waits,
// not scheduling latency or arbitrary caller callback duration.
// Constructor validation/bind never constructs a published Context.
// Errors are fixed: "Local UDP operation could not be completed." No logging.
class LocalUdpOwner {
public:
    using Clock = std::function<double()>;
    using Authorizer = std::function<bool(const session::SessionId&)>;
    using LoginResponseValidator = std::function<bool(const session::SessionId&,
        const control::posthello::RawString&)>;
    using PublicationProvider = std::function<std::optional<game::WorldPublication>(
        const session::SessionId&,const control::posthello::Login&)>;
    LocalUdpOwner(std::uint16_t port, const game::ConnectionConfig& skeleton,
                  Clock clock, std::chrono::milliseconds poll_interval);
    ~LocalUdpOwner();
    LocalUdpOwner(const LocalUdpOwner&) = delete;
    LocalUdpOwner& operator=(const LocalUdpOwner&) = delete;
    LocalUdpOwner(LocalUdpOwner&&) = delete;
    LocalUdpOwner& operator=(LocalUdpOwner&&) = delete;

    std::uint16_t port() const noexcept;
    bool running() const noexcept;
    // Count ONLY after receive returns successfully, never on handoff/tick or
    // MAC/replay/inner-parser failure; saturates at uint64 max. Hello is sticky
    // ONLY when Step.hello is present. Closed is sticky on stop or session retire.
    // Diagnostics do not pin/read Context or call Authorizer; stop retains them.
    LocalUdpDiagnostics diagnostics() const noexcept;
    // True only while running, for exact127.0.0.1 and actual bound port.
    // Other destination metadata is validated by SessionAssignmentService.
    bool destination_bound(const assignment::Destination& destination) const noexcept;
    // start installs a nonempty callback BEFORE starting the single worker.
    // One-shot; repeated start or start after stop is invalid_lifecycle.
    // Callback owners must outlive stop/join and must complete promptly.
    // Optional identity validator runs once per successful Login candidate, after
    // releasing all registry/socket locks and in-flight pins. Only Response is
    // transiently passed to the validator. An optional publication provider then
    // receives the complete transient Login ONLY after validator acceptance;
    // provider requires a nonempty validator. Neither callback runs under a pin
    // or registry/socket lock; no Login fields retained/logged by this owner.
    // No provider preserves the prior observation-only continuation. Provider
    // nullopt/throw denies with fixed Failure6; accepted metadata is published on
    // the SAME persistent GameConnection after fresh authorization/time/exact
    // active-entry checks. Publication/send failure retires after releasing Use.
    // Reauthorize after callback, then recheck the EXACT active shared entry before
    // applying any decision. A stale decision is discarded. Acceptance remains
    // pending admission; metadata publication and Join observation do not spawn
    // or authorize a player. Rejection sends fixed Failure6 then retires.
    // Callback may synchronously retire its lease, but may not start/stop/destruct
    // this owner. Owners must outlive stop/join and callbacks must finish promptly.
    // Optional copied immutable plan requires validator AND publication provider.
    // Before worker/start commit: validate channel1..2047, local index>=0 and
    // transform encodability; fixed invalid_configuration leaves start retryable.
    // Actual package/index resolution occurs only after successful Join/ACK send.
    // Within that same Use: resolve reference, compose exact bits, register and
    // send actor through existing connection. Own first-controller policy is
    // reliable/type2/open=true/close=false, with explicit supplied channel seeds.
    // Composer budget is packet bits; transport separately validates overhead.
    // Any resolution/composition/register/send failure retires AFTER releasing
    // Use. No extra callbacks/Context or inferred native actor/rotation defaults.
    // controller_sent requires nonempty output, send success and no stopping
    // observed AFTER send; stopped send's no-op success must not set it.
    // Absence preserves observation-only Join behavior; no spawn claim.
    void start(Authorizer authorize, LoginResponseValidator validate_response = {},
               PublicationProvider publication = {},
               std::optional<InitialControllerPlan> controller = std::nullopt);
    void stop() noexcept;

    // Null on wrong destination, not running, expired/nonfinite clock, occupied
    // registry or a prior successful installation. Throw only a fixed error on
    // configuration/construction failure. Failed installs publish nothing and
    // do not consume the successful-installation budget.
    // Exactly ONE persistent GameConnection owns its ONE server Context.
    // An active lease confirms local installation only; no DTO/network atomicity.
    std::unique_ptr<assignment::UdpSessionLease> handoff(
        const assignment::SessionProvision& provision);

    // Temporary first-trial policy: one successful installation per owner life.
    // Before multi-session/multi-account admission, review a bounded identity/
    // entropy strategy for fresh unique ID/key rotation without old Context reset.
    // Socket input is bounded/truncation-checked; peer must be loopback. The first
    // authenticated successful receive pins its exact IP+port. Other peers cannot
    // consume counters. Malformed/replayed/unauthorized input never admits a user.
    // Clock failure/regression stops the worker and invalidates the active lease;
    // throwing authorization retires its session without exposing the exception.
    // Connection-terminal errors retire; nonterminal MAC/replay errors only drop.
    // Login candidate observation optionally validates Response identity, then
    // leaves an accepted connection/lease pending world admission. With a
    // provider, supplied Package/Welcome metadata can be published and Join9
    // observed. Without a provider, ACK/tick retain the previous continuation.
    // Without a validator it remains observation-only; no Welcome is fabricated or
    // raw fields retained by this owner. No callback runs under registry/pin.
    // Worker copies ID8 before registry access and calls Authorizer with no pin/
    // registry lock. Idle may briefly snapshot ID without a pin, releasing the
    // registry BEFORE Authorizer. Afterwards recheck active and acquire in-flight
    // ownership. While pinned: NO Service/Auth/callbacks. Serialize receive/tick.
    // Lease destruction deactivates/unlinks, waits only on already-authorized
    // in-flight users, and destroys Context after they finish. Never joins worker
    // or re-enters Service/Auth. Registry state survives owner as inactive leases.
    // stop joins worker before dropping callbacks/closing socket; it is idempotent.
    // Do not call start/stop/destruct from callbacks. No concurrent destruction or
    // start/stop operations; stop owner BEFORE Service/Auth/callback owner teardown.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
