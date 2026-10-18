#pragma once
#include "hawken/game_wire.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hawken::game::weapon_fire {
enum class Kind { start_fire, stop_fire, stop_fire_extended };
struct Schema {
    std::uint32_t exclusive_bound;
    std::uint32_t start_field_id;
    std::uint32_t stop_field_id;
    std::optional<std::uint32_t> extended_stop_field_id;
    bool primary_package_map;
    bool weapon_cache_correct;
    bool parameter_order_correct;
    bool all_parameters_eligible;
};
struct Result {
    Kind kind;
    std::uint8_t fire_mode;
    std::optional<bool> recent_fire;
    std::size_t consumed_bits;
};

// Decode ONE known field tag and its arguments under the caller's proven cache.
// Byte parameters (Enum == null) carry presence plus eight LSB-first value bits;
// absence means zero. Extended stop adds one raw Bool bit without presence.
// No fire-mode restriction, authority decision, handler or suffix interpretation.
// Budget and consumed_bits include the actual bounded tag width. The cursor is
// committed only on success; an unaligned opaque suffix remains untouched.
// WireError: invalid_limit (zero bound/budget), invalid_value (schema, premises
// or unknown field), field_too_large (budget), truncated (physical input).
Result read_rpc(wire::BitReader& reader,const Schema& schema,std::size_t max_rpc_bits);
}
