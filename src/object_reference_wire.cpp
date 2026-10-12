#include "hawken/object_reference_wire.hpp"
#include <limits>

namespace hawken::wire::object {
std::size_t write_reference(BitWriter& writer, const Reference& value,
                            std::size_t max_field_bits) {
    if (max_field_bits == 0) throw WireError("invalid_limit");
    bool channel=false;
    std::uint32_t index=0;
    if (const auto* package=std::get_if<PackageExportReference>(&value)) {
        index=package->index;
        if (index > 0x7fffffffU) throw WireError("invalid_value");
    } else if (const auto* actor=std::get_if<ActorChannelReference>(&value)) {
        channel=true;
        index=actor->channel;
        if (index > 2047) throw WireError("invalid_value");
    } else {
        throw WireError("invalid_value");
    }
    const std::size_t count=channel ? 12 : 32;
    if (count > max_field_bits) throw WireError("field_too_large");
    if (count > std::numeric_limits<std::size_t>::max()-writer.bit_count())
        throw WireError("invalid_length");
    writer.write_bit(channel);
    writer.write_bounded(index,channel ? 0x800U : 0x80000000U);
    return count;
}
ReferenceResult read_reference(BitReader& reader, std::size_t max_field_bits) {
    // A positive budget permits the selector, but not necessarily its field.
    if (max_field_bits == 0) throw WireError("invalid_limit");
    const auto start=reader.position();
    const bool channel=reader.read_bit();
    const std::size_t count=channel ? 12 : 32;
    if (count > max_field_bits) throw WireError("field_too_large");
    // Shared bounded reads retain their partial cursor on physical truncation.
    const auto index=reader.read_bounded(channel ? 0x800U : 0x80000000U);
    if (channel)
        return {ActorChannelReference{static_cast<std::uint16_t>(index)},reader.position()-start};
    return {PackageExportReference{index},reader.position()-start};
}
}
