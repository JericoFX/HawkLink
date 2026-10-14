#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"
#include <cstdint>
#include <string>

namespace hawken::wire::rpc {
struct StartMechArguments {
    std::string mech_instance_id;
    std::int32_t primary_weapon = 0;
    std::int32_t holo_taunt_type = 0;
};
struct StartMechArgumentsResult {
    StartMechArguments value;
    std::size_t consumed_bits;
};

// ONLY ServerSetStartMech's three argument records, after the caller has read
// and resolved the effective RPC fieldID. Caller establishes ALL three exact
// original parameters eligible on the primary PackageMap, Children/Next order
// StrProperty, IntProperty, IntProperty, ArrayDim1 and active scalar endian.
// No fieldID/cache bound, class lookup, packet header, alignment or terminator.
// Evidence: pri-mech-setup/server-set-start-mech-20261014/contract-v3.json.
//
// Each non-Bool parameter has a presence bit. Absence leaves string empty or
// int32 zero, matching the original zero-initialized parameter buffer. Present
// FString supports count0, +1/NUL, -1/UTF16LE NUL, or a textual GUID36 plus NUL
// (count+37 narrow / -37 UTF16LE). Count and ints use explicit scalar endian;
// wide units remain UTF16LE. Raw int32 values are preserved before the original
// handler's later byte conversion. No implicit selected mech or completed setup.
//
// Own bounded selection policy rejects every other nonempty string/count,
// malformed NUL/unit/UUID shape. ASCII hex case normalizes to lowercase. Zero
// UUID is syntactically valid; the operation caller must reject any unowned or
// invalid selector through authenticated account/snapshot/type validation.
// Nothing here grants items, accepts a session, reads SQL or proves readiness.
//
// Positive budget counts argument bits only. Decode starts at any bit offset,
// preserves suffix, and commits the caller cursor ONLY after complete success.
// No partial value or cursor change on failure. Fixed WireError codes:
// invalid_limit, invalid_value, field_too_large, truncated, resource_limit.
StartMechArgumentsResult read_start_mech_arguments(BitReader& reader,
    control::Endian integer_order, std::size_t max_argument_bits);
}
