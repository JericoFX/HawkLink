#pragma once
#include <cstdint>

namespace hawken::wire::movement {

enum class ScopedSavedMoveDirection : std::uint8_t {
    none = 0,
    left = 1,
    right = 2,
    forward = 3,
    back = 4
};

struct HawkenSavedMoveInput {
    bool run = false;
    bool heal = false;
    bool pressed_jump = false;
    bool taunt = false;
    bool double_jump = false;
    bool precise_destination = false;
    ScopedSavedMoveDirection direction = ScopedSavedMoveDirection::none;
};

constexpr HawkenSavedMoveInput decode_hawken_saved_move_flags(std::uint8_t flags) noexcept {
    const auto direction = static_cast<std::uint8_t>(flags & 7U);
    return {(flags & 8U) != 0, (flags & 16U) != 0, (flags & 32U) != 0,
            (flags & 64U) != 0, false, (flags & 128U) != 0,
            direction <= 4 ? static_cast<ScopedSavedMoveDirection>(direction) : ScopedSavedMoveDirection::none};
}

} // namespace hawken::wire::movement
