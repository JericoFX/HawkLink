#pragma once
#include "hawken/game_wire.hpp"
#include <span>

namespace hawken::wire::property {
struct BoolProperty {
    std::uint32_t field_id;
    bool value;
};
struct BoolPropertyPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// Normal eligible scalar BoolProperty records: bounded effective field ID
// immediately followed by one value bit, including false. Native serializer
// f8ec0. No RPC presence, element index, alignment, terminator or packet marker.
// Caller supplies verified field IDs, exclusive class-cache bound, ArrayDim1
// eligibility and intended ordered records. No property/class lookup, member
// filtering, duplicate elimination, RPC execution or match-start policy.
// Positive bound and aggregate budget are mandatory even for an empty list,
// which returns zero bits. IDs must be below bound; exact concatenated length
// must fit the budget. Preserve native value-dependent bounded ID width.
// Output owns bytes with zero unused high bits; no partial payload is returned.
// Fixed WireError: invalid_limit, invalid_value, field_too_large.
BoolPropertyPayload encode_bool_properties(std::span<const BoolProperty> values,
    std::uint32_t exclusive_bound, std::size_t max_payload_bits);
}
