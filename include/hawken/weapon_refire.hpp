#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace hawken::game::weapon_fire {
struct OwnerRefireScales {
    float hand_zero;
    float other_hand;
};

// Original d29b40 forwards the byte mode to virtual +788; the observed Machine
// and MissileLauncher CDO tables select d6b690. No owner preserves the selected
// interval bits. An owner selects hand_zero only for hand == 0, multiplies in
// float32, then clamps below/unordered products to original 0x3c23d70a (0.01F).
// +infinity passes; there is no finite-value or rate policy here. nullopt only
// bounds the mode to this span, avoiding the original out-of-range access.
// Provenance: gameplay-bootstrap/20261018/fire-executor-inputs retained bodies,
// EXE SHA256 4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2.
std::optional<float> refire_interval(std::span<const float> intervals,
                                    std::uint8_t mode, std::int32_t hand,
                                    std::optional<OwnerRefireScales> owner) noexcept;
}
