#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace hawken::game {
struct PendingMatchRules {
    bool players_must_be_ready;
    std::int32_t min_players;
    float min_percent_ready;
    std::int32_t min_delay_seconds;
    std::int32_t max_delay_seconds;
    std::int32_t countdown_seconds;
    float max_wait_for_travelling_seconds;
};
struct PendingMatchState {
    float pending_start_time;
    float countdown_begin_time;
    bool timer_active;
    std::int32_t waiting_for;
    std::int32_t waiting_to_ready;
    std::int32_t countdown_remaining;
    bool operator==(const PendingMatchState&) const = default;
};
struct MatchPlayerState {
    bool is_player;
    bool waiting_player;
    bool ready;
    bool online_synced;
    bool purchasing;
    bool saving;
};
struct PendingMatchInputs {
    float now;
    std::int32_t num_players;
    std::int32_t num_travelling_players;
    std::span<const MatchPlayerState> controllers;
    bool tracking_created;
    bool pending_quit;
    bool waiting_to_start_match;
    bool match_has_begun;
};
struct PendingMatchEvaluation {
    bool start_requested = false;
};

// One caller-owned shared PendingMatch model per world, with explicit rules,
// initial state, world clock and a roster of concrete R_PlayerControllers.
// online_synced/purchase/save/tracking/quit and historical match_has_begun are
// caller-supplied facts; this model never derives them from auth/HTTP/ACK.
// Original R_MPGameBase013424/013421/013430/013452 and R_Deathmatch013987:
// population shortage returns before readiness, preserving waiting_to_ready;
// zero total preserves both counts. Ratio uses connected num_players only.
// Synchronization/tracking/quit early returns preserve timers and counters.
// Travelling blocks until elapsed strictly exceeds max_wait, and cancels the
// active countdown. Eligible preminimum publishes int(countdown+min-elapsed);
// later countdown measures elapsed since its own activation, not last ready.
// Maximum delay uses int(elapsed)>=max if max>0; zero disables that override.
// start_requested is emitted once by this model. It neither writes the supplied
// historical match_has_begun nor proves that a native match has started.
// Caller owns StartMatch effects, roster lifetime during evaluate and serial
// access. No actor/RPC/transport, account lookup or selected-mech state here.
//
// Explicit local validation: finite nonnegative rules/time values, percent in
// [0,1], nonnegative population/counters, no summed-count or float-to-int32
// overflow. Initial countdown_remaining may be any int32 (including -1).
// Clock is nonnegative, not before pending_start_time and monotonic relative to
// the last valid evaluate, including valid early returns. Invalid evaluation
// throws std::invalid_argument("Invalid match readiness.") without changing
// visible state, action latch or last valid clock. Do not require min<=max.
class PendingMatch {
public:
    PendingMatch(PendingMatchRules rules, PendingMatchState initial_state);
    PendingMatchEvaluation evaluate(const PendingMatchInputs& inputs);
    PendingMatchState snapshot() const noexcept { return state_; }
private:
    const PendingMatchRules rules_;
    PendingMatchState state_;
    std::optional<float> last_valid_evaluation_;
    bool start_request_emitted_ = false;
};
}
