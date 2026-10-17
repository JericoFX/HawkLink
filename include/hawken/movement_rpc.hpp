#pragma once
#include "hawken/actor_transform_wire.hpp"
#include "hawken/control_wire.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hawken::wire::movement {
enum class Kind { dual_server_move, old_server_move, server_move };
struct Eligibility {
    bool primary_package_map;
    bool controller_cache_correct;
    bool parameter_order_correct;
    bool all_parameters_eligible;
};
struct Move {
    float time_stamp = 0;
    actor::QuantizedPosition packed_acceleration{};
    actor::QuantizedPosition client_location{};
    std::uint8_t move_flags = 0;
    std::uint8_t client_roll = 0;
    std::int32_t view = 0;
    bool jump = false;
};
struct Result {
    Kind kind;
    Move current;
    // Dual's first move carries only timestamp, acceleration, flags and view.
    // Its client_location/client_roll remain zero; no synthetic sentinel is added.
    std::optional<Move> pending;
    // Old's acceleration is three compressed bytes, not the packed Vector type.
    std::array<std::uint8_t,3> old_acceleration{};
    std::size_t consumed_bits = 0;
};

// ONE field59/60/61 under the explicit conditional Controller cache bound294.
// Original Engine exports4236/4243/4270, Children/Next parameter order and native
// property serializers: implementation/evidence/movement-upk-contract-20261012
// and movement-wire-native-20261012. All four eligibility premises must hold.
// NonBool parameters have presence+optional value, absent values zero. Scalars
// use explicit archive byte order; Vector uses the existing packed position
// reader, with no float quantization or acceleration/physics interpretation.
// Local safety policy rejects nonfinite float timestamps, unsupported fields,
// false premises, invalid endian and zero budget. Zero timestamp is valid wire.
// Transactional cursor, including the field tag; unaligned suffix stays untouched.
// No state mutation, ACK, correction, movement simulation or jump acceptance.
// Fixed WireError codes: invalid_limit, invalid_value, field_too_large, truncated.
Result read_rpc(BitReader& reader,control::Endian integer_order,
    const Eligibility& eligibility,std::size_t max_rpc_bits);
}
