#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"
#include <span>

namespace hawken::wire::property {
struct IntProperty {
    std::uint32_t field_id;
    std::int32_t value;
};
struct IntPropertyPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// Normal eligible scalar IntProperty records: bounded effective field tag then
// raw32 storage in explicit integer byte order. Original f8fb0 ->3a30 count4;
// no RPC presence, array element byte, alignment, terminator or packet marker.
// Caller establishes IntProperty type, ArrayDim1, eligibility, full class-cache
// bound/IDs and the active integer archive order. No metadata lookup, state
// transitions, authorization, duplicate elimination or match-start policy.
// Preserve ordered records (including duplicates) and value-dependent tag width.
// Positive bound and aggregate budget are required even for an empty list;
// empty output has zero bits. ID<bound and all records must fit the budget.
// Output owns bytes, with zero unused high bits; never return a partial payload.
// Fixed WireError: invalid_limit, invalid_value, field_too_large. Unknown Endian
// is invalid_value, matching the accepted UniqueId property's explicit-order API.
IntPropertyPayload encode_int_properties(std::span<const IntProperty> values,
    std::uint32_t exclusive_bound, control::Endian integer_order,
    std::size_t max_payload_bits);
}
