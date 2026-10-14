#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"
#include <cstddef>
#include <cstdint>

namespace hawken::wire::rpc {
struct SelectionStageArgumentsResult {
    std::int32_t stage = 0;
    std::size_t consumed_bits = 0;
};

// Arguments only, AFTER the caller has resolved the effective field tag and
// exact ServerUpdateSelectionStage schema: one eligible IntProperty, flags80,
// ArrayDim1. Presence0 defaults to zero; presence1 reads raw signed32 storage
// using the supplied archive integer order, with no alignment or terminator.
// The raw int is preserved; native byte conversion and stage/restart policy
// belong to the authenticated caller, not this decoder. No RPC execution.
// Consume one prefix at the current cursor, retain opaque suffixes and commit
// the copied reader only on complete success. Budget must be positive.
// Fixed WireError: invalid_limit, invalid_value, field_too_large, truncated.
SelectionStageArgumentsResult read_selection_stage_arguments(BitReader& reader,
    control::Endian integer_order, std::size_t max_argument_bits);
}
