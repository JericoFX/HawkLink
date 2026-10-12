#pragma once
#include "hawken/local_udp_owner.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace hawken::udp {
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
};
// Pure preflight; no binding, randomness, SQL or Steam. Canonical lower-case
// nonzero GUID36 and ASCII region[A-Za-z0-9_.-]1..64; port0 is API-valid.
// Publication/controller/Pawn are not validated by this identity preflight.
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
