#include "hawken/actor_transform_wire.hpp"
#include <bit>
#include <limits>

namespace hawken::wire::actor {
namespace {
void validate_limit(std::size_t limit) {
    if (limit == 0) throw WireError("invalid_limit");
}
void require_bits(std::size_t used, std::size_t count, std::size_t limit) {
    if (used > limit || count > limit - used) throw WireError("field_too_large");
}
void validate_write(const BitWriter& writer, std::size_t count, std::size_t limit) {
    require_bits(0,count,limit);
    if (count > std::numeric_limits<std::size_t>::max() - writer.bit_count())
        throw WireError("invalid_length");
}
}

std::size_t write_position(BitWriter& writer, const QuantizedPosition& value,
                           std::size_t max_field_bits) {
    validate_limit(max_field_bits);
    std::uint32_t maximum=0;
    for (const auto component:value) {
        if (component < -1048576 || component > 1048575) throw WireError("invalid_value");
        const auto wide=static_cast<std::int64_t>(component);
        const auto magnitude=static_cast<std::uint32_t>(wide < 0 ? -wide : wide);
        if (magnitude > maximum) maximum=magnitude;
    }
    auto width=static_cast<std::uint32_t>(std::bit_width(maximum));
    if (width == 0) width=1;
    if (width > 20) width=20;
    const auto n=width-1;
    // Exact algebraic bound20 partition; actual emission uses the shared engine.
    const std::size_t header_bits=(n & 0x0fU) < 4 ? 5 : 4;
    const auto count=header_bits+3*static_cast<std::size_t>(n+2);
    validate_write(writer,count,max_field_bits);
    const auto bias=std::uint32_t{1} << (n+1);
    const auto bound=std::uint32_t{1} << (n+2);
    writer.write_bounded(n,20);
    for (const auto component:value) {
        const auto encoded=static_cast<std::uint32_t>(static_cast<std::int64_t>(component)+bias);
        writer.write_bounded(encoded,bound);
    }
    return count;
}

PositionResult read_position(BitReader& reader, std::size_t max_field_bits) {
    validate_limit(max_field_bits);
    // No complete vector fits a smaller budget; this bounds the shared header read.
    require_bits(0,5,max_field_bits);
    const auto start=reader.position();
    const auto n=reader.read_bounded(20);
    require_bits(reader.position()-start,3*static_cast<std::size_t>(n+2),max_field_bits);
    const auto bias=std::uint32_t{1} << (n+1);
    const auto bound=std::uint32_t{1} << (n+2);
    QuantizedPosition value{};
    for (auto& component:value) {
        const auto encoded=reader.read_bounded(bound);
        component=static_cast<std::int32_t>(static_cast<std::int64_t>(encoded)-bias);
    }
    return {value,reader.position()-start};
}

std::size_t write_rotation(BitWriter& writer, const Rotation& value,
                           std::size_t max_field_bits) {
    validate_limit(max_field_bits);
    std::size_t count=3;
    for (const auto component:value) {
        if ((component & 0xffU) != 0) throw WireError("invalid_value");
        if (component != 0) count+=8;
    }
    validate_write(writer,count,max_field_bits);
    for (const auto component:value) {
        writer.write_bit(component != 0);
        if (component != 0) {
            const std::array<std::uint8_t,1> byte{static_cast<std::uint8_t>(component >> 8)};
            writer.write_bits(byte,8);
        }
    }
    return count;
}

RotationResult read_rotation(BitReader& reader, std::size_t max_field_bits) {
    validate_limit(max_field_bits);
    const auto start=reader.position();
    Rotation value{};
    for (auto& component:value) {
        require_bits(reader.position()-start,1,max_field_bits);
        if (reader.read_bit()) {
            require_bits(reader.position()-start,8,max_field_bits);
            // Raw-byte reader checks all8bits before advancing, unlike bounded reads.
            const auto byte=reader.read_bits(8);
            component=static_cast<std::uint16_t>(static_cast<std::uint16_t>(byte[0]) << 8);
        }
    }
    return {value,reader.position()-start};
}
}
