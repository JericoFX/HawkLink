#pragma once
#include <cstdint>

namespace hawken::game {
// One explicit caller-owned authoritative snapshot for a particular spawn.
// No implicit walking/zero-heat/empty-effects initialization: the local trial
// must supply that policy deliberately. These are not client-supplied facts,
// native memory offsets, or claims that a native Pawn was initialized.
struct PawnCombatState {
    std::uint64_t spawn_revision;
    bool alive;
    bool spawn_active;
    std::uint8_t move_state;
    bool boost_fire_active;
    float forward_boost_fire_cooldown;
    bool spectator;
    bool overheated;
    bool weapons_down;
    float emp_time_remaining;
    float heavy_mode_time;
    float heavy_mode_start_time;
    float heavy_mode_transform_time_multiplier;
    bool weapon_disabled;
};
struct PawnCombatPermission {
    bool pawn_can_fire = false;
    bool server_can_fire = false;
    bool local_can_fire = false;
};

// Pure initial-spawn permission, not movement/physics, damage, heat or effect
// simulation. The caller owns spawn/revision changes and all state updates.
// Original EXE 4a861ecb...6ea8a2: CanFireWeapon wrapper d30ae0 -> virtualaa8
// -> concrete PawnContent d50750. Block states9/14/17/19/20/22; without boost
// fire, also block4/5 and positive forward-boost cooldown. Spectator, overheat,
// weapons-down, positive EMP time and weapon-disabled each veto. Callbacka90
// d964e0 vetoes state13, or state11 while the single-precision product of
// heavy start time and transform multiplier is greater than heavy time.
// Proven raw instruction/branch records: fire-gates-20261017/pawn-predicate/.
// Pawn property names/layout are a reconstruction's semantic inputs here;
// their native reflected Offset/Mask binding is a separate observation.
// LocalCanFire d9a180 copies PawnCanFire to the replicated server bool BEFORE
// applying client shutdown to its returned local activation permission:
// server_can_fire == pawn_can_fire; local_can_fire adds !client_shutdown.
// The caller supplies its handled shutdown-request state, never an unchecked
// bool from a client snapshot. No field IDs, encoding, ACKs or sends here.
//
// Explicit fail-closed local policies: revision0 is unset; state revision must
// equal the caller's current revision; Pawn must be alive and spawn-active;
// defined MoveState range is0..22. All five float inputs must be finite, and
// the heavy product must be finite when state11 reaches that comparison.
// Finite negative cooldown/EMP values remain expired, matching <=0 tests.
// Invalid inputs return all false. No snapshot, revision or latch is mutated.
PawnCombatPermission evaluate_pawn_combat(const PawnCombatState& state,
    std::uint64_t current_spawn_revision,bool client_shutdown) noexcept;
}
