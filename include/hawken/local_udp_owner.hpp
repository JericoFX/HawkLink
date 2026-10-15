#pragma once
#include "hawken/game_connection.hpp"
#include "hawken/initial_controller.hpp"
#include "hawken/session_assignment.hpp"
#include "hawken/mech_setup_wire.hpp"
#include "hawken/start_mech_rpc_wire.hpp"
#include "hawken/spawn_validation.hpp"
#include "hawken/match_readiness.hpp"
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
    // Sticky only after explicit Pawn datagrams were sent; not possession/gameplay.
    bool pawn_sent = false;
    // Sticky only after explicit PRI datagrams were sent; not client links/setup.
    bool pri_sent = false;
    // Sticky scalar observations only: recognized configured request and actual
    // nonempty validated/failed wire sends. Never actor/gameplay acceptance.
    bool spawn_validation_requested = false;
    bool spawn_validation_sent = false;
    bool spawn_validation_failed = false;
    bool stage3_requested = false;
    bool client_restart_sent = false;
    // Genuine outgoing-opening/queued delivery observations only. These never
    // prove client property application, actor creation or admission.
    bool controller_open_acked = false;
    bool pri_open_acked = false;
    bool pawn_open_acked = false;
    bool actor_links_sent = false;
    bool pri_identity_sent = false;
    bool pri_identity_acked = false;
    bool gri_sent = false;
    bool gri_open_acked = false;
    bool game_class_sent = false;
    bool game_class_acked = false;
    bool hud_sent = false;
    bool hud_acked = false;
    bool ready_rpc_received = false;
    bool pri_ready_sent = false;
    bool pri_ready_acked = false;
    bool match_start_requested = false;
    bool gri_match_begun_sent = false;
    bool gri_match_begun_acked = false;

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

struct InitialPawnPlan {
    wire::reliable::ActorChannelConfig channel;
    std::array<std::uint32_t,4> archetype_package_guid;
    std::int32_t archetype_local_net_index;
    wire::actor::QuantizedPosition position;
    std::optional<wire::actor::Rotation> rotation;
};
// Common dynamic-actor prefix, conditional on the native branch returning no
// PlayerController. The caller supplies the concrete PRI archetype and rotation.
using InitialPriPlan = InitialPawnPlan;

// Explicit effective scalar ObjectProperty IDs on the PRIMARY PackageMap.
// Caller establishes eligibility, ArrayDim1 and expected object classes; this
// plan does not discover fields, select native defaults or prove possession.
struct InitialActorLinksPlan {
    std::uint32_t controller_pri_field_id;
    std::uint32_t controller_pawn_field_id;
    std::uint32_t controller_exclusive_bound;
    std::uint32_t pawn_controller_field_id;
    std::uint32_t pawn_pri_field_id;
    std::uint32_t pawn_exclusive_bound;
};


// Caller evidence for the PRIMARY all-members branch and these exact class caches.
// These typed premises are explicit activation gates, not runtime discoveries.
struct StartMechEligibility {
    bool primary_package_map;
    bool controller_cache_correct;
    bool rpc_eligible;
    std::array<bool,3> parameters_eligible;
    bool pri_cache_correct;
    bool pri_property_eligible;
    bool all_members_eligible;
};
// Optional known-prefix compatibility schema for the no-consumables trial.
// Inherits StartMech's PRIMARY map, Controller cache and exclusive bound.
// Consumes the eligible IntProperty argument only; no active-item/PRI gameplay
// state or consumable execution is implemented by this compatibility path.
struct ActiveItemIndexRpcPlan {
    std::uint32_t field_id;
    bool rpc_eligible;
    bool argument_eligible;
};
struct StartMechDispatchPlan {
    std::uint32_t controller_rpc_field_id;
    std::uint32_t controller_exclusive_bound;
    std::uint32_t pri_mech_setup_field_id;
    std::uint32_t pri_exclusive_bound;
    StartMechEligibility eligibility;
    std::uint8_t initial_setup_id;
    std::optional<ActiveItemIndexRpcPlan> active_item_index = std::nullopt;
};

// Explicit PRIMARY Controller-cache/RPC premises supplied by the caller.
// These are activation gates, not runtime observations or inferred field IDs.
struct SpawnValidationEligibility {
    bool primary_package_map;
    bool controller_cache_correct;
    bool validate_rpc_eligible;
    bool validated_rpc_eligible;
    bool failed_rpc_eligible;
    bool failure_reason_eligible;
};
struct SpawnValidationDispatchPlan {
    std::uint32_t validate_rpc_field_id;
    std::uint32_t validated_rpc_field_id;
    std::uint32_t failed_rpc_field_id;
    std::uint32_t controller_exclusive_bound;
    SpawnValidationEligibility eligibility;
};
struct SpawnSelectionValidationResult {
    game::SpawnValidationChecks checks;
    std::uint8_t failure_reason = 7;
};

// Explicit original stage/int-argument and ClientRestart/Pawn-reference schema.
// Only validated data for the exact accepted revision may release stage3.
struct SpawnAdmissionEligibility {
    bool primary_package_map;
    bool controller_cache_correct;
    bool stage_rpc_eligible;
    bool stage_argument_eligible;
    bool client_restart_rpc_eligible;
    bool pawn_reference_eligible;
};
struct SpawnAdmissionDispatchPlan {
    std::uint32_t stage_rpc_field_id;
    std::uint32_t client_restart_rpc_field_id;
    std::uint32_t controller_exclusive_bound;
    SpawnAdmissionEligibility eligibility;
};

struct PriIdentityEligibility {
    bool primary_package_map;
    bool pri_cache_correct;
    bool unique_id_property_eligible;
    bool unique_net_id_serializer_correct;
    bool integer_archive_order_correct;
};
struct PriIdentityDispatchPlan {
    std::uint32_t field_id;
    std::uint32_t exclusive_bound;
    control::Endian integer_order;
    PriIdentityEligibility eligibility;
};

struct SelectionBootstrapEligibility {
    bool primary_package_map;
    bool common_gri_prefix_correct;
    bool gri_cdo_correct;
    bool gri_cache_correct;
    bool game_class_property_eligible;
    bool game_class_property_class_correct;
    bool game_class_meta_class_correct;
    bool controller_cache_correct;
    bool client_set_hud_rpc_eligible;
    bool hud_parameter_eligible;
    bool hud_parameter_class_correct;
    bool hud_meta_class_correct;
};
struct SelectionBootstrapPlan {
    InitialPawnPlan gri;
    std::array<std::uint32_t,4> game_class_package_guid;
    std::int32_t game_class_local_net_index;
    std::array<std::uint32_t,4> hud_package_guid;
    std::int32_t hud_local_net_index;
    std::uint32_t game_class_field_id;
    std::uint32_t gri_exclusive_bound;
    std::uint32_t client_set_hud_field_id;
    std::uint32_t controller_exclusive_bound;
    SelectionBootstrapEligibility eligibility;
};

// FIRST local diagnostic pipe, not a multiplayer GameServer or admission API.
struct ReadyMatchEligibility {
    bool primary_package_map;
    bool controller_cache_correct;
    bool pri_cache_correct;
    bool gri_cache_correct;
    bool ready_rpc_eligible;
    bool ready_argument_bool_net_correct;
    bool pri_ready_property_eligible;
    bool pri_waiting_property_eligible;
    bool gri_waiting_to_ready_property_eligible;
    bool gri_waiting_for_property_eligible;
    bool gri_countdown_property_eligible;
    bool gri_match_begun_property_eligible;
    bool integer_archive_order_correct;
};
struct ReadyMatchPlan {
    std::uint32_t ready_rpc_field_id;
    std::uint32_t controller_exclusive_bound;
    std::uint32_t pri_ready_field_id;
    std::uint32_t pri_waiting_field_id;
    std::uint32_t pri_exclusive_bound;
    std::uint32_t gri_waiting_to_ready_field_id;
    std::uint32_t gri_waiting_for_field_id;
    std::uint32_t gri_countdown_field_id;
    std::uint32_t gri_match_begun_field_id;
    std::uint32_t gri_exclusive_bound;
    control::Endian integer_order;
    game::PendingMatchRules rules;
    game::PendingMatchState initial_state;
    bool role_is_player;
    bool role_waiting_player;
    bool world_tracking_created;
    bool world_pending_quit;
    ReadyMatchEligibility eligibility;
};
struct ReadyAccountFacts {
    bool owned_collection_loaded;
    bool unranked_stats_available;
    bool item_stats_loaded;
    bool achievements_loaded;
    bool purchasing;
    bool saving;
};

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
    using StartMechProvider = std::function<std::optional<wire::mech::SMechSetupStruct>(
        const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t)>;
    using SpawnSelectionValidator = std::function<SpawnSelectionValidationResult(
        const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t)>;
    using ReadyFactsProvider = std::function<std::optional<ReadyAccountFacts>(
        const session::SessionId&)>;
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
    // Optional copied Pawn plan requires Controller, validator and publication;
    // its channel must differ from Controller and be1..2047, local index>=0,
    // transforms encodable before worker/start commit. After successful Controller
    // send (and PRI send if configured), resolve/compose/register/send Pawn on the SAME connection and Use,
    // reliable/type2/open with no controller byte. Failure retires after Use;
    // controller_sent may be true while pawn_sent remains false. No possession.
    // Optional copied PRI plan has the same prerequisites and prefix as Pawn;
    // its channel must differ from Controller AND any Pawn. Validate before
    // start commit, then send Controller -> PRI -> Pawn on the SAME Use/Context.
    // PRI failure retires after Use, preserving controller_sent and skipping Pawn.
    // pri_sent requires nonempty successful datagrams and no stopping after send;
    // it does not prove client actor creation or PlayerReplicationInfo bindings.
    // Optional copied links require all three actor plans and both callbacks.
    // Positive per-class bounds, distinct IDs below each bound and encodable
    // batches are validated before start commit. References use those plans'
    // channels. Own conservative sequencing waits for all three real opening
    // PacketIDs plus outgoing_open_acked, nonclosed snapshots, then sends exactly
    // one Controller batch and one Pawn batch on the SAME Use/Context. Both
    // payloads are composed before either send; partial failure retires after
    // Use. ACK/transmission is not native creation, linkage or possession proof.
    // Optional explicit dispatch requires all actors, publication, links, a provider
    // and every schema premise. Only the first field of a Controller bunch is
    // considered; known schemas consume exact argument PREFIX boundaries. With
    // admission configured, consecutive StartMech/Validate/Stage fields are read;
    // unknown/truncated tags terminate parsing without scanning opaque suffixes.
    // Without admission, the prior StartMech -> Validate prefix policy remains.
    // One pending request per exact Entry waits for real opening ACKs and links;
    // equal pending requests are idempotent, conflicting pending requests retire.
    // Provider runs outside all locks/Use. Reauthorize, sample fresh time and pin
    // the exact Entry before reliable/type2 PRI property send on the same Context.
    // Revision advances only after nonempty send success without stopping. Failure
    // retires after Use; no automatic preset, native admission or possession proof.
    // Optional copied spawn-validation plan/callback requires explicit StartMech
    // dispatch/source, all actors, links and every PRIMARY/RPC schema premise.
    // Only an explicit first-field Controller request starts validation; suffix
    // remains opaque. Retain the exact accepted request and setup revision only
    // after successful selected PRI property send. Wait for all real opening ACKs,
    // links and nonclosed PRI outgoing_bunches==0 before invoking the callback.
    // That delivery gate never supplies any of the five data-validation results.
    // Callback runs outside all Use/locks. Reauthorize and recheck exact Entry,
    // selection revision, delivery and lifecycle before response send. All five3
    // produce one reliable Controller validated RPC; any2 produces failed RPC
    // with reason0..8; all other combinations stay pending. Duplicate requests
    // for the same accepted revision are idempotent; transport owns retries.
    // Without an accepted selection, explicit request fails GeneralFailure0.
    // Callback exceptions/invalid failed reason retire after Use; stopped/stale/
    // revoked decisions emit no response. Callback lifetime ends after stop/join.
    // Neither data validation nor RPC transmission proves client admission/spawn.
    // Optional admission requires all validation prerequisites and six explicit
    // schema premises. Exact int32 stage3 after successful current-revision data
    // validation requests one reliable ClientRestart with the existing Pawn ref.
    // Fresh auth/Entry/lifecycle and the same actor/PRI delivery gates apply.
    // New StartMech invalidates permission; duplicates use transport retries.
    // No additional recovery ServerRestart RPC or native possession guarantee.
    // Optional copied PRI identity requires validator/publication/PRI/links and
    // all five explicit schema/order premises, validated before start commits.
    // Only configured handoff retains a strictly canonical lower/nonzero server
    // provision.user_guid as four native eight-hex words. Invalid input installs
    // no Entry and consumes no installation budget. Login.UniqueId/Steam64 are
    // never identity sources. After the PRI's genuine outgoing-opening ACK, send
    // one reliable UniqueId property on that same channel/Context. Links then
    // require that send and the actual PRI outgoing queue to be empty. Fresh
    // auth/Entry/lifecycle and existing retirement rules remain in force.
    // Sent/ACK diagnostics do not prove native identity application. No plan
    // preserves the prior wire behavior and retains no provision identity.
    // Selection bootstrap is explicit: a fourth GRI opening uses the common
    // no-controller prefix. Only its real opening ACK permits GameClass; only
    // GameClass and both previously sent actor-link queues being acknowledged
    // permits ClientSetHUD on the existing Controller. All twelve caller schema
    // premises, distinct channels and shared Controller cache must be coherent.
    // References are package GUID + LOCAL NetIndex, never global coordinates.
    // HUD is created locally by the RPC, not a fifth server actor. Sent/ACK
    // diagnostics do not establish native application, UI, match or possession.
    // Optional ReadyMatchPlan is a finite ONE-entry local-world policy, separate
    // from selected-mech revision and admission. Caller supplies all schema IDs,
    // bounds/premises, explicit role/waiting facts and world tracking/quit policy;
    // no NET isPlayer inference or original Meteor tracking/quit implementation.
    // initial_state.pending_start_time must be zero relative to this world's
    // creation epoch. Subtract the double owner clock base before float conversion.
    // After actual opening/UID/links/GameClass/HUD ACKs, echo Ready+Waiting as a
    // PRI bool tuple; model synchronization requires its actual reliable ACK.
    // Four distinct loaded/available account domains and current purchase/save
    // facts come from the optional provider outside every Use/registry lock,
    // then fresh auth/time/exact Entry and ready revision are checked again.
    // Empty provider/result never grants synchronization/start; Ready echo remains
    // available. Provider without a plan is invalid. No travel API: joined finite
    // roster has zero travelling only after these explicit lifecycle gates.
    // One PendingMatch per world, not per Entry; its StartMatch action emits the
    // reliable GRI begun signal before reevaluating pending validated Stage3.
    // While pending, Stage3 marks this entry ready but cannot send ClientRestart.
    // Own action alone is insufficient without current existing admission gates;
    // no extra GRI begun ACK requirement, RPC25, StartHumans/physics implementation
    // or native gameplay acceptance is implied. No-plan keeps prior behavior.
    void start(Authorizer authorize, LoginResponseValidator validate_response = {},
               PublicationProvider publication = {},
               std::optional<InitialControllerPlan> controller = std::nullopt,
               std::optional<InitialPawnPlan> pawn = std::nullopt,
               std::optional<InitialPriPlan> pri = std::nullopt,
               std::optional<InitialActorLinksPlan> links = std::nullopt,
               std::optional<StartMechDispatchPlan> start_mech = std::nullopt,
               StartMechProvider mech_provider = {},
               std::optional<SpawnValidationDispatchPlan> spawn_validation = std::nullopt,
               SpawnSelectionValidator selection_validator = {},
               std::optional<SpawnAdmissionDispatchPlan> spawn_admission = std::nullopt,
               std::optional<PriIdentityDispatchPlan> pri_identity = std::nullopt,
               std::optional<SelectionBootstrapPlan> selection_bootstrap = std::nullopt,
               std::optional<ReadyMatchPlan> ready_match = std::nullopt,
               ReadyFactsProvider ready_facts = {});
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
