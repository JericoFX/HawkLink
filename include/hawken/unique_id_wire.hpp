#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"
#include <array>

namespace hawken::wire::identity {
struct UniqueId {
    std::array<std::uint32_t, 4> words{};
};
struct UniqueIdPropertyPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};
struct UniqueIdValueResult {
    UniqueId value;
    std::size_t consumed_bits;
};

// One eligible ArrayDim1 UniqueNetId StructProperty: bounded field tag followed
// by four raw u32 components in explicit integer byte order, without RPC
// presence, length, alignment or terminator. Caller establishes the effective
// field cache/bound, primary-map eligibility and UniqueNetId special branch.
// Original f9020 -> 12ca0 -> 3a30 establishes the four component calls; concrete
// Archive selection and native application remain caller preconditions.
// Zero words are syntactically valid, never proof of authenticated identity.
// No GUID-string parsing, SteamID conversion, actor send or authorization.
// Positive budget includes the tag and all128 value bits. Fixed WireError codes:
// invalid_limit, invalid_value, field_too_large; no partial payload returned.
UniqueIdPropertyPayload encode_unique_id_property(const UniqueId& value,
    std::uint32_t field_id, std::uint32_t exclusive_bound,
    control::Endian integer_order, std::size_t max_payload_bits);

// Value-only reader, after the caller consumes the known property field tag.
// Reads exactly128 logical bits at any cursor offset and preserves any suffix.
// Positive budget is relative to the current cursor. Copy/commit only on success;
// errors leave position and error state unchanged. Additional error: truncated.
UniqueIdValueResult read_unique_id_value(BitReader& reader,
    control::Endian integer_order, std::size_t max_value_bits);
}
