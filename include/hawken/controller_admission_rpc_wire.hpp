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

struct ReadyArgumentsResult {
    bool ready = false;
    std::size_t consumed_bits = 0;
};

// Arguments only, AFTER the caller resolves the effective ServerPlayerReady
// field and the eligible scalar BoolProperty NET argument (ArrayDim1).
// Native sender mask0x20000 skips RPC presence for this bool: consume exactly
// one data bit, even when false. No alignment, endian conversion or terminator.
// Read one prefix at the existing cursor; leave any suffix opaque and commit
// the reader only after complete success. No RPC execution or match decision.
// Budget must be positive. Fixed WireError: invalid_limit, truncated.
ReadyArgumentsResult read_ready_arguments(BitReader& reader,
    std::size_t max_argument_bits);
}
