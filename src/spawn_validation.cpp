#include "hawken/spawn_validation.hpp"

namespace hawken::game {
SpawnValidationStatus aggregate_spawn_validation(const SpawnValidationChecks& checks) noexcept {
    constexpr auto failed_byte = static_cast<std::uint8_t>(SpawnValidationStatus::failed);
    constexpr auto succeeded_byte = static_cast<std::uint8_t>(SpawnValidationStatus::succeeded);
    bool all_succeeded = true;
    for (const auto state : checks) {
        if (state == failed_byte) return SpawnValidationStatus::failed;
        if (state != succeeded_byte) all_succeeded = false;
    }
    return all_succeeded ? SpawnValidationStatus::succeeded : SpawnValidationStatus::in_process;
}
}
