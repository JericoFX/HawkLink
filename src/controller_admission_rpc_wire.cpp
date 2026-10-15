#include "hawken/controller_admission_rpc_wire.hpp"
#include <bit>

namespace hawken::wire::rpc {
SelectionStageArgumentsResult read_selection_stage_arguments(BitReader& reader,
    control::Endian integer_order, std::size_t max_argument_bits) {
    if (max_argument_bits == 0) throw WireError("invalid_limit");
    if (integer_order != control::Endian::little && integer_order != control::Endian::big)
        throw WireError("invalid_value");

    auto cursor = reader;
    const auto start = cursor.position();
    std::int32_t stage = 0;
    if (cursor.read_bit()) {
        if (max_argument_bits < 33) throw WireError("field_too_large");
        std::uint32_t storage = 0;
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto shift = integer_order == control::Endian::little
                ? bit : (3 - bit / 8) * 8 + bit % 8;
            if (cursor.read_bit()) storage |= std::uint32_t{1} << shift;
        }
        stage = std::bit_cast<std::int32_t>(storage);
    }
    const auto consumed = cursor.position() - start;
    reader = cursor;
    return {stage, consumed};
}

ReadyArgumentsResult read_ready_arguments(BitReader& reader,
    std::size_t max_argument_bits) {
    if (max_argument_bits == 0) throw WireError("invalid_limit");

    auto cursor = reader;
    const bool ready = cursor.read_bit();
    reader = cursor;
    return {ready, 1};
}
}
