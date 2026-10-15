#include "hawken/int_property_wire.hpp"
#include <array>

namespace hawken::wire::property {
IntPropertyPayload encode_int_properties(std::span<const IntProperty> values,
    std::uint32_t exclusive_bound, control::Endian integer_order,
    std::size_t max_payload_bits) {
    if (exclusive_bound == 0 || max_payload_bits == 0)
        throw WireError("invalid_limit");
    if (integer_order != control::Endian::little && integer_order != control::Endian::big)
        throw WireError("invalid_value");

    BitWriter writer;
    for (const auto& property : values) {
        if (property.field_id >= exclusive_bound) throw WireError("invalid_value");
        // Reserve the scalar first; a bounded uint32 tag adds at most32 bits.
        if (max_payload_bits < 32 || writer.bit_count() > max_payload_bits - 32)
            throw WireError("field_too_large");
        writer.write_bounded(property.field_id, exclusive_bound);
        if (writer.bit_count() > max_payload_bits - 32)
            throw WireError("field_too_large");

        const auto word = static_cast<std::uint32_t>(property.value);
        std::array<std::uint8_t, 4> bytes{};
        for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
            const auto shift = (integer_order == control::Endian::little ? byte : 3 - byte) * 8;
            bytes[byte] = static_cast<std::uint8_t>(word >> shift);
        }
        writer.write_bits(bytes, 32);
    }
    return {writer.payload_bytes(), writer.bit_count()};
}
}
