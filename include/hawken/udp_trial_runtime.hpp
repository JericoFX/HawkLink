#pragma once
#include "hawken/local_udp_owner.hpp"
#include "hawken/starter_mech_setup.hpp"
#include <functional>
#include <string_view>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hawken::udp {
// Original default matchmaking build ID: CRC32 of the Engine package GUID,
// formatted signed-decimal by 998a10/bb9cf0. Effective override branch and
// loaded GUID remain conditional until original-client query verification.
inline constexpr char trial_game_version[] = "547429238";
struct StartMechSource {
    inventory::StarterCatalogueIds expected_catalogue;
    // Reads existing account-owned effective HTTP baseline + overrides only.
    // Callback target outlives runtime.stop/join; no grant or account credential.
    std::function<store::StarterMechSnapshot(const store::AccountRef&,std::string_view)> read_effective;
};
struct SpawnValidationSource {
    // Read account-scoped achievement IDs after a successful SQL scope/query.
    // Empty is a completed own-server no-awards load, not an error fallback.
    // No active achievement catalogue is configured here: nonempty is unsupported
    // and fails validation, never discarded or converted to an empty success.
    std::function<std::vector<std::string>(const store::AccountRef&)> read_achievement_ids;
    // Explicit own-server permanence policy for configured starter instances;
    // false cannot activate this dispatcher. No historical expiry assertion.
    bool permanent_starter_instances = false;
};
struct UdpTrialConfig {
    // Port0 is fixture-only. Main requires an explicit port1..65535.
    std::uint16_t port;
    std::string server_guid;
    std::string region;
    // Optional caller-owned Package/Welcome metadata, copied by the runtime.
    // No automatic map/package selection, asset I/O or hash verification here.
    // Nullopt preserves identity-only observation. A supplied plan is offered
    // only after Response validation; invalid publication retires the session
    // through the existing Owner policy. No prebind plan validation promised.
    std::optional<game::WorldPublication> publication = std::nullopt;
    // Optional copied first-controller policy; requires publication. Owner
    // validates it before starting the worker, then emits only after Join.
    // No inferred identity/transform defaults or gameplay/spawn guarantee.
    std::optional<InitialControllerPlan> controller = std::nullopt;
    // Optional copied first-Pawn policy requires Controller and publication.
    // No controller byte or inferred identity/transform defaults.
    std::optional<InitialPawnPlan> pawn = std::nullopt;
    // Optional copied PRI policy requires Controller and publication, with a
    // distinct channel. Owner sends it after Controller and before any Pawn.
    // No inferred identity/rotation defaults or player-link replication.
    std::optional<InitialPriPlan> pri = std::nullopt;
    // Optional immutable link policy requires publication and all three actor
    // plans. Runtime checks these prerequisites before binding; Owner validates
    // IDs/bounds/budgets and sends after all three actual outgoing-opening ACKs.
    // Explicit caller schema only; no native IDs, inferred links or possession.
    std::optional<InitialActorLinksPlan> links = std::nullopt;
    // Explicit copied dispatcher/source pair. No schema/default preset is inferred.
    // Runtime obtains fresh assignment scope outside Use, reads the exact root,
    // validates/materializes nine roles and revalidates the same account identity.
    // Both RPC int arguments undergo original BYTE conversion before selection.
    std::optional<StartMechDispatchPlan> start_mech = std::nullopt;
    std::optional<StartMechSource> start_mech_source = std::nullopt;
    // Optional copied pair requires all actor/link/StartMech policies and source.
    // Fresh exact account scope is checked before and after owned/achievement SQL
    // loads outside Use. Re-materialize the accepted finite owned selection with
    // original BYTE arguments; type/root/parent/permanence and completed loads
    // provide the five checks, never HTTP status or ACK. No grants/SQL writes.
    std::optional<SpawnValidationDispatchPlan> spawn_validation = std::nullopt;
    std::optional<SpawnValidationSource> spawn_validation_source = std::nullopt;
    // Explicit stage3 admission policy requires the real selected-data validation.
    // No automatic stage request or additional recovery ServerRestart RPC.
    std::optional<SpawnAdmissionDispatchPlan> spawn_admission = std::nullopt;


};
// Pure preflight; no binding, randomness, SQL or Steam. Canonical lower-case
// nonzero GUID36 and ASCII region[A-Za-z0-9_.-]1..64; port0 is API-valid.
// Publication/controller/Pawn/PRI are not validated by this identity preflight.
bool validate_trial_config(const UdpTrialConfig& config) noexcept;

// One local diagnostic session, not a world, public listing or game admission.
// Auth/store/verifier must outlive this object and all HTTP callers. The caller
// stops/drains HTTP BEFORE stop/destruction. No callback may stop this runtime.
// Owns a real shared steady-clock epoch (double seconds for UDP; checked floor
// int64 seconds for expiry), separate UTC and production CSPRNG. No test RNG or
// credential API. Challenges1b/3 use separate random draws; material stays private.
// Native hypotheses: fresh reliable0/swapLE, versions9188..154767 and interior512.
// These remain conditional on the original loaded class/CDO, not runtime proof.
// Explicit new policies: TTL900, ratings0/0, timeout120s, poll20ms; queues64/64/64,
// payload65536, records64/packets64; output16messages/64datagrams/35840bytes;
// login archive512/string256/speed30000. Replyflags false/false respond to a
// client-opened channel; open_confirmed starts false and receive(open) updates it.
// Constructor validates identity before randomness/bind, then binds exclusively
// on127.0.0.1, constructs Assignment using the actual bound port and starts last.
// Failure rolls back before publication. Destructor stop/joins BEFORE destroying
// Assignment/callback targets. No second Context, key getter or secret observer.
// First trial retains the existing one-successful-install owner limitation.
class UdpTrialRuntime {
public:
    UdpTrialRuntime(auth::AuthService& auth, const UdpTrialConfig& config);
    ~UdpTrialRuntime();
    UdpTrialRuntime(const UdpTrialRuntime&) = delete;
    UdpTrialRuntime& operator=(const UdpTrialRuntime&) = delete;
    UdpTrialRuntime(UdpTrialRuntime&&) = delete;
    UdpTrialRuntime& operator=(UdpTrialRuntime&&) = delete;

    bool running() const noexcept;
    std::uint16_t port() const noexcept;
    assignment::SessionAssignmentService& assignment();
    LocalUdpDiagnostics diagnostics() const noexcept;
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
