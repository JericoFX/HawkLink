#include "hawken/unique_id_wire.hpp"

namespace hawken::wire::identity {
UniqueIdPropertyPayload encode_unique_id_property(const UniqueId& value,
    std::uint32_t field_id, std::uint32_t exclusive_bound,
    control::Endian integer_order, std::size_t max_payload_bits) {
    if (max_payload_bits == 0) throw WireError("invalid_limit");
    if (integer_order != control::Endian::little && integer_order != control::Endian::big)
        throw WireError("invalid_value");

    BitWriter writer;
    writer.write_bounded(field_id, exclusive_bound);
    if (max_payload_bits < 128 || writer.bit_count() > max_payload_bits - 128)
        throw WireError("field_too_large");
    for (const auto word : value.words) {
        std::array<std::uint8_t, 4> bytes{};
        for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
            const auto shift = (integer_order == control::Endian::little ? byte : 3 - byte) * 8;
            bytes[byte] = static_cast<std::uint8_t>(word >> shift);
        }
        writer.write_bits(bytes, 32);
    }
    return {writer.payload_bytes(), writer.bit_count()};
}
UniqueIdValueResult read_unique_id_value(BitReader& reader,
    control::Endian integer_order, std::size_t max_value_bits) {
    if (max_value_bits == 0) throw WireError("invalid_limit");
    if (integer_order != control::Endian::little && integer_order != control::Endian::big)
        throw WireError("invalid_value");
    if (max_value_bits < 128) throw WireError("field_too_large");

    auto cursor = reader;
    UniqueId value;
    for (auto& word : value.words) {
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto shift = integer_order == control::Endian::little
                ? bit : (3 - bit / 8) * 8 + bit % 8;
            if (cursor.read_bit()) word |= std::uint32_t{1} << shift;
        }
    }
    reader = cursor;
    return {value, 128};
}
}
