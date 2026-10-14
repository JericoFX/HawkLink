#pragma once
#include <array>
#include <cstdint>

namespace hawken::game {
enum class SpawnValidationStatus : std::uint8_t {
    in_process = 1,
    failed = 2,
    succeeded = 3
};

// Original dce050 consumes five raw bytes in physical offset order:
// +10fb ItemInstanceCollection, +10fc PresetLoaded, +10fd MechInstance,
// +10fe SelectedPrimaryWeapon, +10ff AchievementsLoaded.
// Provenance: lifecycle/completion-aggregate-predicate-supplement.json under
// evidence/pri-mech-setup/server-set-start-mech-20261014 (EXE 4a861ecb...6ea8a2).
using SpawnValidationChecks = std::array<std::uint8_t, 5>;

// Any 2 wins over every other byte. Otherwise all 3 yields succeeded;
// every remaining combination (including 0 and 4..255) stays in_process.
// Pure aggregation only: no validator execution, RPC, transport or spawn.
SpawnValidationStatus aggregate_spawn_validation(const SpawnValidationChecks& checks) noexcept;
}
