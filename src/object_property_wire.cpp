#include "hawken/object_property_wire.hpp"

namespace hawken::wire::property {
PropertyPayload encode_object_properties(std::span<const ObjectProperty> values,
    std::uint32_t exclusive_bound, std::size_t max_payload_bits) {
    if (exclusive_bound == 0 || max_payload_bits == 0)
        throw WireError("invalid_limit");

    BitWriter writer;
    for (const auto& value : values) {
        // Bounded temporary: at most 32 ID bits plus 32 primary reference bits.
        // Reuse the actual bounded algorithm to measure value-dependent widths
        // before appending to the aggregate output under its exact budget.
        BitWriter record;
        record.write_bounded(value.field_id,exclusive_bound);
        object::write_reference(record,value.value,32);
        if (record.bit_count() > max_payload_bits-writer.bit_count())
            throw WireError("field_too_large");
        writer.write_bits(record.payload_bytes(),record.bit_count());
    }
    return {writer.payload_bytes(),writer.bit_count()};
}
}
