#include "hawken/pawn_combat_state.hpp"
#include <cmath>

namespace hawken::game {
PawnCombatPermission evaluate_pawn_combat(const PawnCombatState& state,
    std::uint64_t current_spawn_revision,bool client_shutdown) noexcept {
    if (!state.spawn_revision || state.spawn_revision!=current_spawn_revision ||
        !state.alive || !state.spawn_active || state.move_state>=23 ||
        !std::isfinite(state.forward_boost_fire_cooldown) ||
        !std::isfinite(state.emp_time_remaining) ||
        !std::isfinite(state.heavy_mode_time) ||
        !std::isfinite(state.heavy_mode_start_time) ||
        !std::isfinite(state.heavy_mode_transform_time_multiplier)) return {};

    // d50750: the checked enum range also makes this shift defined.
    if ((0x5a4200U & (1U << state.move_state))!=0) return {};
    if (!state.boost_fire_active && (state.move_state==4 || state.move_state==5 ||
        state.forward_boost_fire_cooldown>0.0F)) return {};

    // d964e0: preserve MULSS rounding before the ordered comparison.
    if (state.move_state==13) return {};
    if (state.move_state==11) {
        const float heavy_threshold=state.heavy_mode_start_time *
            state.heavy_mode_transform_time_multiplier;
        if (!std::isfinite(heavy_threshold) || heavy_threshold>state.heavy_mode_time) return {};
    }
    if (state.spectator || state.overheated || state.weapons_down ||
        state.emp_time_remaining>0.0F || state.weapon_disabled) return {};

    // d9a180 copies Pawn permission to the server bit before client shutdown.
    return {true,true,!client_shutdown};
}
}
