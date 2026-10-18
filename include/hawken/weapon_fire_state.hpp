#pragma once
#include "hawken/pawn_combat_state.hpp"
#include "hawken/session_wire.hpp"
#include "hawken/weapon_fire_rpc.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hawken::game::weapon_fire {
struct Scope {
    session::SessionId session_id;
    std::uint64_t spawn_revision;
    std::uint16_t actor_channel;
    bool operator==(const Scope&) const = default;
};
struct Config {
    Scope scope;
    // Four original Dual slots. The finite Recruit consumer supplies only0/1;
    // this model does not infer that policy or manufacture missing weapons.
    std::array<bool,4> supported_hands;
};
struct Authority {
    Scope scope;
    bool authenticated;
    bool actor_owned;
    bool instigator_valid;
    PawnCombatState pawn;
    // Server-handled shutdown request, never an unchecked peer snapshot bool.
    bool client_shutdown;
};
enum class ActionKind { begin_fire, end_fire };
struct Action {
    ActionKind kind = ActionKind::begin_fire;
    std::uint8_t hand = 0;
    bool operator==(const Action&) const = default;
};
struct Snapshot {
    std::array<bool,4> pending_fire{}, firing{};
    bool enabled = false;
    std::array<std::optional<double>,2> stop_deadlines{};
    std::optional<double> last_time;
    bool operator==(const Snapshot&) const = default;
};
struct Step {
    bool accepted = false;
    std::array<Action,8> actions{};
    std::size_t action_count = 0;
};

// One copied session/spawn/weapon binding. No socket, SQL, client-state write,
// shot simulation, damage or projectile is implied by a semantic action.
// Original retained UC: Engine.Weapon BeginFire032173 sets PendingFire and
// EndFire032179 clears it; Manager017572/575 write1/0, without action dedupe.
// Dual.Active ShutdownWeapons047405 ends each present hand but keeps intent;
// RestartWeapons047407 resumes each pending hand. Valid repeated requests
// retain their Begin/End invocations; reliable packet duplicate filtering is
// the transport's responsibility, not a new deduplication rule here.
//
// Local authoritative admission policy: exact nonzero session/revision,
// non-control channel1..2047, configured supported hand, fresh authenticated
// owned actor and valid instigator. Pawn revision must match. Start also needs
// alive+spawn_active, but records pending intention even if combat is vetoed.
// Actual firing uses evaluate_pawn_combat(...).local_can_fire; it does not turn
// the client's bShutdown off or infer permission from ACK/GivenTo.
// Stop remains valid when alive/active/permission is lost; wrong scope never
// clears another weapon. Invalid inputs are rejected transactionally.
//
// Caller supplies finite, nonnegative, monotonic double time (equality valid).
// Permission transitions run first: shutdown emits End for all supported
// hands; restart emits Begin for each pending hand, in ascending hand order.
// apply then executes ONE RPC, with no hidden timer expiry. tick then expires
// due timers in0/1 order; caller explicitly schedules tick in its worker loop.
// Extended stop047349 recent=true for0/1 arms now+double(0.2F); other stops end
// immediately. New start047346 cancels its own0/1 timer. Missing/inconsistent
// recent_fire or unknown Kind/unsupported modes are invalid typed requests.
// No unproven timestamp/bTempRestart latency bypass is implemented.
// Eight actions structurally cover four permission actions plus two timers
// (tick), or one RPC (apply); overflow must never silently discard an action.
class FireState {
public:
    explicit FireState(Config config);
    Step apply(const Result& request,const Authority& authority,double now);
    Step tick(const Authority& authority,double now);
    const Snapshot& snapshot() const noexcept;
private:
    const Config config_;
    Snapshot state_;
};
}
