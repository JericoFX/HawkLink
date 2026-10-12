#pragma once
#include "hawken/object_reference_wire.hpp"
#include <span>

namespace hawken::wire::property {
struct ObjectProperty {
    std::uint32_t field_id;
    object::Reference value;
};
struct PropertyPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// Normal scalar ObjectProperty records on the explicit PRIMARY PackageMap:
// bounded fieldID followed immediately by object::Reference. No RPC presence
// bit, ArrayDim element byte, alignment or terminator. Logical bunch bitcount
// delimits the list; fieldID0 is not a sentinel. Native sender9efa60/9f0a05..
// 9f0a6a and ObjectProperty f8fd0 -> primary9f4080 are retained in
// implementation/evidence/replicated-property-framing/contract.json.
// Caller supplies eligible ArrayDim1 fields, EFFECTIVE IDs/cache bound and
// resolving references. No eligibility/class check, lookup, alternative-map
// selection, arrays, decoder, actor creation or possession is performed here.
//
// Own policy: positive bound and global max_payload_bits, even for an empty
// list (valid zero-bit output). IDs must be below bound, reference ranges must
// be valid, and the exact aggregate bitcount must fit the budget. Non-power
// bounds retain their value-dependent native width; bound1/id0 uses zero ID
// bits. Encoder owns output and never returns truncated or partial payloads.
// Returned bytes have zero unused high bits and no packet marker.
// Fixed WireError codes: invalid_limit, invalid_value, field_too_large,
// invalid_length; not_implemented exists only in the RED scaffold.
PropertyPayload encode_object_properties(std::span<const ObjectProperty> values,
    std::uint32_t exclusive_bound, std::size_t max_payload_bits);
}
