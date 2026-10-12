#pragma once
#include "hawken/game_wire.hpp"
#include <array>

namespace hawken::wire::actor {
using QuantizedPosition = std::array<std::int32_t,3>;
// Native loading produces byte<<8. This API preserves those units without
// accepting arbitrary unquantized angles or applying a float quantizer.
using Rotation = std::array<std::uint16_t,3>;
struct PositionResult {
    QuantizedPosition value;
    std::size_t consumed_bits;
};
struct RotationResult {
    Rotation value;
    std::size_t consumed_bits;
};

// Fields at the caller's current LSB cursor, with no alignment or terminator.
// Position09d6f0: n=clamp(bit_length(max(abs(xyz))),1,20)-1, bounded20 header,
// then three biased integers bounded by1<<(n+2). No float/MXCSR assumptions.
// Rotation09d5d0: presence bit then optional raw8bits for each component.
// Native reader/writer bindings: Bunch1588d00/1588c20 slots8/10/18, see
// implementation/evidence/actor-transform-archive-bindings/contract.json.
//
// Our safety policy: max_field_bits>0 (no artificial upper bound), positions
// in[-1048576,1048575], rotation units multiples256. Write preflight checks
// value, field budget and absolute bit_count overflow before any mutation.
// Allocation failure during emission may leave a partial writer; no rollback.
// Read consumes ONE field and reports its length; following bits are untouched.
// A budget/truncation error may leave a partial reader: bounded vector reads
// consume available bits; rotation's read_bits(8) prechecks the complete byte.
// No native actor IDs, world, admission, sockets or packet/session wrapping.
// Fixed WireError.code: invalid_limit, invalid_value, field_too_large,
// invalid_length, truncated; not_implemented belongs only to the RED stub.
std::size_t write_position(BitWriter& writer, const QuantizedPosition& value,
                           std::size_t max_field_bits);
PositionResult read_position(BitReader& reader, std::size_t max_field_bits);
std::size_t write_rotation(BitWriter& writer, const Rotation& value,
                           std::size_t max_field_bits);
RotationResult read_rotation(BitReader& reader, std::size_t max_field_bits);
}
