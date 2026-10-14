#pragma once
#include "hawken/game_wire.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hawken::wire::rpc {
struct SpawnValidationRpcPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};
struct SpawnFailedArgumentsResult {
    std::uint8_t reason = 0;
    std::size_t consumed_bits = 0;
};

// Caller supplies the effective field ID/cache bound, correct RPC direction
// and exact original schema. This tag-only form also represents the zero-arg
// ServerValidate request. No presence, discriminator, padding or terminator.
// Failure precondition: Reason is eligible, ByteProperty/ArrayDim1, enum Names
// count10 (including SVR_MAX). Zero emits presence0; nonzero emits presence1
// plus four raw LSB bits. Own input policy rejects reason>15 instead of truncating;
// values9..15 are not rejected merely because they lack a playable enum label.
// See lifecycle/spawn-validation-rpc-contract/contract.json under evidence/
// pri-mech-setup/server-set-start-mech-20261014. No RPC execution or spawn claim.
// Positive budget includes the actual variable-width field tag for encoders.
// The argument reader starts AFTER that tag, consumes only one argument prefix,
// preserves every suffix bit, and commits its copied cursor only on success.
// Fixed WireError codes: invalid_limit, invalid_value, field_too_large, truncated.
SpawnValidationRpcPayload encode_spawn_validated(std::uint32_t field_id,
    std::uint32_t exclusive_bound, std::size_t max_rpc_bits);
SpawnValidationRpcPayload encode_spawn_failed(std::uint8_t reason,
    std::uint32_t field_id, std::uint32_t exclusive_bound, std::size_t max_rpc_bits);
SpawnFailedArgumentsResult read_spawn_failed_arguments(BitReader& reader,
    std::size_t max_argument_bits);
}
