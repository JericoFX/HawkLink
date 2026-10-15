#include "hawken/bool_property_wire.hpp"

namespace hawken::wire::property {
BoolPropertyPayload encode_bool_properties(std::span<const BoolProperty> values,
    std::uint32_t exclusive_bound, std::size_t max_payload_bits) {
    if (exclusive_bound == 0 || max_payload_bits == 0)
        throw WireError("invalid_limit");

    BitWriter writer;
    for (const auto& value : values) {
        // At most 32 field bits and one value bit in this bounded temporary.
        BitWriter record;
        record.write_bounded(value.field_id, exclusive_bound);
        record.write_bit(value.value);
        if (writer.bit_count() > max_payload_bits ||
            record.bit_count() > max_payload_bits - writer.bit_count())
            throw WireError("field_too_large");
        writer.write_bits(record.payload_bytes(), record.bit_count());
    }
    return {writer.payload_bytes(), writer.bit_count()};
}
}
