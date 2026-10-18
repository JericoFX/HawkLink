#include "hawken/weapon_refire.hpp"

namespace hawken::game::weapon_fire {
std::optional<float> refire_interval(std::span<const float> intervals,
                                    std::uint8_t mode, std::int32_t hand,
                                    std::optional<OwnerRefireScales> owner) noexcept {
    if (mode >= intervals.size()) return std::nullopt;
    const float interval = intervals[mode];
    if (!owner) return interval;
    const float scale = hand == 0 ? owner->hand_zero : owner->other_hand;
    const float product = interval * scale;
    constexpr float minimum = 0.01F; // Original float32 bits 0x3c23d70a.
    return !(product >= minimum) ? minimum : product;
}
}
