#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"

namespace hawken::wire::movement {
struct MovementReplyPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// One original ClientAckGoodMove(float TimeStamp) RPC prefix, not a transport
// ACK or an acceptance decision. Caller establishes the effective Controller
// field/cache, the sole ArrayDim1 FloatProperty parameter's map eligibility,
// and the actual archive byte order. Original field66/bound294 is conditional
// on the pinned Link0/PRIMARY class cache, not hardcoded by this codec.
// Native f3400 omits either signed zero; other finite IEEE32 values carry one
// presence bit then four bytes via f8fb0/3a30 in explicit scalar order. Negative
// finite timestamps are wire values; temporal validation belongs to the server.
// NaN/Inf rejection is this codec's finite-value policy, not a general native
// wire-invalid claim. No alignment, function terminator, bunch or packet suffix.
// Positive budget includes tag and arguments; output is owned with zero unused
// high bits. WireError: invalid_limit, invalid_value, field_too_large.
MovementReplyPayload encode_client_ack_good_move(float timestamp,
    std::uint32_t field_id,std::uint32_t exclusive_bound,
    control::Endian integer_order,std::size_t max_rpc_bits);
}
