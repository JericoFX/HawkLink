#pragma once
#include "hawken/object_reference_wire.hpp"
#include <optional>

namespace hawken::wire::rpc {
struct PawnRpc {
    std::uint32_t field_id;
    std::optional<object::Reference> value;
};
struct RpcPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};
struct RpcResult {
    std::uint32_t field_id;
    std::optional<object::Reference> value;
    std::size_t consumed_bits;
};

// One logical RPC containing one non-Bool Pawn ObjectProperty parameter.
// Native687c10/9ec370: bounded fieldID, then (only if map eligibility is true)
// one presence bit, then the primary object reference only if present.
// Sender Pawn flags80/ArrayDim1: null pointer -> absent. A present channel0 is
// a distinct physical representation; this codec does not resolve objects or
// claim absent means initialized null on the receiver. The caller supplies
// the EFFECTIVE fieldID, exclusive cache bound and eligibility; UPK export
// ordinals/localNetIndex are not substituted for fieldID. No RPC execution,
// class check, global-map selection, packets, admission or possession.
// See implementation/evidence/controller-rpc-sender/contract.json.
//
// Own policy: bound>0, fieldID<bound, positive per-RPC max_rpc_bits including
// the fieldID. Eligibility false omits presence/value; supplying a value in
// that case is rejected instead of silently discarded. Bound1/id0 writes
// zero field bits, so an ineligible RPC legitimately consumes zero bits.
// Encoder owns all output; payload is raw bits, no packet terminator.
// Decode consumes ONE prefix at the current cursor, preserving suffix.
// FieldID is read on a copied cursor; truncation or budget failure there
// leaves the original cursor unchanged. Budget uses actual bounded width,
// including shorter non-power-of-two values. Once fieldID commits, presence
// and reference failures retain their consumed cursor as in read_reference;
// no rollback. No operation consumes beyond the caller budget.
// Fixed WireError codes: invalid_limit, invalid_value, field_too_large,
// truncated (not_implemented is only the RED scaffold).
RpcPayload encode_rpc(const PawnRpc& value, std::uint32_t exclusive_bound,
                      bool eligible, std::size_t max_rpc_bits);
RpcResult decode_rpc(BitReader& reader, std::uint32_t exclusive_bound,
                     bool eligible, std::size_t max_rpc_bits);
}
