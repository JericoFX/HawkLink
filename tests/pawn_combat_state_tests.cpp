#include "hawken/pawn_combat_state.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <ostream>

namespace {
using hawken::game::PawnCombatState;
using hawken::game::evaluate_pawn_combat;

// Explicit local-trial policy fixture, not asserted original CDO defaults:
// revision7, active living walking Pawn, no server effects or accumulated heat.
PawnCombatState active_spawn() {
    return {7,true,true,0,false,0.0F,false,false,false,0.0F,0.0F,1.5F,1.0F,false};
}
void permission(const PawnCombatState& state,std::uint64_t revision,bool client_shutdown,
                bool want_pawn,bool want_local) {
    const auto value=evaluate_pawn_combat(state,revision,client_shutdown);
    CHECK(value.pawn_can_fire==want_pawn);
    CHECK(value.server_can_fire==want_pawn);
    CHECK(value.local_can_fire==want_local);
}
}

TEST_CASE("Pawn combat permits an explicit active neutral spawn") {
    permission(active_spawn(),7,false,true,true);
}

TEST_CASE("Pawn combat client shutdown does not change replicated Pawn permission") {
    const auto state=active_spawn();
    permission(state,7,true,true,false);
    permission(state,7,false,true,true);
}

TEST_CASE("Pawn combat rejects inactive dead unset and mismatched spawn revisions") {
    auto state=active_spawn();
    SUBCASE("inactive") {state.spawn_active=false;}
    SUBCASE("dead") {state.alive=false;}
    SUBCASE("unset spawn revision") {state.spawn_revision=0;}
    SUBCASE("older spawn revision") {state.spawn_revision=6;}
    SUBCASE("future spawn revision") {state.spawn_revision=8;}
    permission(state,7,false,false,false);
}

TEST_CASE("Pawn combat current revision must be selected and match the active spawn") {
    permission(active_spawn(),0,false,false,false);
    auto state=active_spawn();state.spawn_revision=0;
    permission(state,0,false,false,false);
    state.spawn_revision=std::numeric_limits<std::uint64_t>::max();
    permission(state,std::numeric_limits<std::uint64_t>::max(),false,true,true);
}

TEST_CASE("Pawn combat selection replacement cannot reuse the previous active spawn") {
    auto state=active_spawn();
    permission(state,7,false,true,true);
    permission(state,8,false,false,false);
    state.spawn_revision=8;state.spawn_active=false;
    permission(state,8,false,false,false);
    state.spawn_active=true;
    permission(state,8,false,true,true);
    state.alive=false;
    permission(state,8,false,false,false);
    state.spawn_revision=9;state.alive=true;state.spawn_active=false;
    permission(state,9,false,false,false);
    state.spawn_active=true;
    permission(state,9,false,true,true);
}

TEST_CASE("Pawn combat preserves the original movement state veto table") {
    // Literal decisions from d50750 mask5a4200, forward-boost branch and d964e0.
    // Heavy11 is tested after transformation; MatchOver18 is not an invented
    // native veto. Caller lifecycle must deactivate a Pawn when appropriate.
    struct Case {std::uint8_t state;bool allowed;};
    constexpr std::array<Case,23> cases{{
        {0,true},{1,true},{2,true},{3,true},{4,false},{5,false},
        {6,true},{7,true},{8,true},{9,false},{10,true},{11,true},
        {12,true},{13,false},{14,false},{15,true},{16,true},{17,false},
        {18,true},{19,false},{20,false},{21,true},{22,false}}};
    for(const auto& row:cases) {
        CAPTURE(static_cast<unsigned>(row.state));
        auto state=active_spawn();state.move_state=row.state;state.heavy_mode_time=2.0F;
        permission(state,7,false,row.allowed,row.allowed);
    }
}

TEST_CASE("Pawn combat rejects movement states outside the defined original enum") {
    for(const auto raw:std::array<std::uint8_t,2>{23,255}) {
        CAPTURE(static_cast<unsigned>(raw));auto state=active_spawn();state.move_state=raw;
        permission(state,7,false,false,false);
    }
}

TEST_CASE("Pawn combat boost fire bypasses only the forward boost and cooldown vetoes") {
    for(const auto raw:std::array<std::uint8_t,2>{4,5}) {
        CAPTURE(static_cast<unsigned>(raw));auto state=active_spawn();state.move_state=raw;
        state.forward_boost_fire_cooldown=2.0F;
        permission(state,7,false,false,false);
        state.boost_fire_active=true;
        permission(state,7,false,true,true);
        state.overheated=true;
        permission(state,7,false,false,false);
    }
    auto state=active_spawn();state.boost_fire_active=true;state.move_state=9;
    permission(state,7,false,false,false);
}

TEST_CASE("Pawn combat forward boost cooldown uses the exact positive boundary") {
    auto state=active_spawn();state.forward_boost_fire_cooldown=0.001F;
    permission(state,7,false,false,false);
    state.forward_boost_fire_cooldown=0.0F;
    permission(state,7,false,true,true);
    state.forward_boost_fire_cooldown=-0.25F;
    permission(state,7,false,true,true);
}

TEST_CASE("Pawn combat rejects every independent authoritative disabling flag") {
    constexpr std::array<bool PawnCombatState::*,4> flags{
        &PawnCombatState::spectator,&PawnCombatState::overheated,
        &PawnCombatState::weapons_down,&PawnCombatState::weapon_disabled};
    for(std::size_t i=0;i<flags.size();++i) {
        CAPTURE(i);auto state=active_spawn();state.*flags[i]=true;
        permission(state,7,false,false,false);
    }
}

TEST_CASE("Pawn combat EMP time permits zero or expired values and vetoes positive time") {
    auto state=active_spawn();state.emp_time_remaining=0.001F;
    permission(state,7,false,false,false);
    state.emp_time_remaining=0.0F;
    permission(state,7,false,true,true);
    state.emp_time_remaining=-1.0F;
    permission(state,7,false,true,true);
}

TEST_CASE("Pawn combat heavy transformation respects threshold equality and exit state") {
    auto state=active_spawn();state.move_state=11;state.heavy_mode_time=1.499F;
    permission(state,7,false,false,false);
    state.heavy_mode_time=1.5F;
    permission(state,7,false,true,true);
    state.heavy_mode_transform_time_multiplier=2.0F;state.heavy_mode_time=2.999F;
    permission(state,7,false,false,false);
    state.heavy_mode_time=3.0F;
    permission(state,7,false,true,true);
    state.move_state=13;
    permission(state,7,false,false,false);
    state.move_state=11;state.heavy_mode_transform_time_multiplier=0.0F;state.heavy_mode_time=0.0F;
    permission(state,7,false,true,true);
}

TEST_CASE("Pawn combat heavy threshold uses single precision multiplication before comparison") {
    auto state=active_spawn();state.move_state=11;
    // MULSS: 0x3f800001 squared rounds to 0x3f800002. A double product is
    // slightly larger and would incorrectly veto at this exact float boundary.
    state.heavy_mode_start_time=0x1.000002p+0F;
    state.heavy_mode_transform_time_multiplier=0x1.000002p+0F;
    state.heavy_mode_time=0x1.000004p+0F;
    permission(state,7,false,true,true);
}

TEST_CASE("Pawn combat rejects nonfinite authoritative inputs as an explicit local policy") {
    constexpr std::array<float PawnCombatState::*,5> fields{
        &PawnCombatState::forward_boost_fire_cooldown,&PawnCombatState::emp_time_remaining,
        &PawnCombatState::heavy_mode_time,&PawnCombatState::heavy_mode_start_time,
        &PawnCombatState::heavy_mode_transform_time_multiplier};
    const std::array<float,3> values{std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
    for(std::size_t i=0;i<fields.size();++i) for(std::size_t j=0;j<values.size();++j) {
        CAPTURE(i);CAPTURE(j);auto state=active_spawn();state.*fields[i]=values[j];
        permission(state,7,false,false,false);
    }
}

TEST_CASE("Pawn combat rejects a nonfinite heavy product without granting on negative overflow") {
    auto state=active_spawn();state.move_state=11;
    state.heavy_mode_start_time=std::numeric_limits<float>::max();
    state.heavy_mode_transform_time_multiplier=2.0F;
    permission(state,7,false,false,false);
    state.heavy_mode_start_time=-std::numeric_limits<float>::max();
    permission(state,7,false,false,false);
}
