#include "hawken/movement_reply.hpp"
#include <array>
#include <bit>

namespace hawken::wire::movement {
MovementReplyPayload encode_client_ack_good_move(float timestamp,
    std::uint32_t field_id,std::uint32_t exclusive_bound,
    control::Endian integer_order,std::size_t max_rpc_bits) {
    if (exclusive_bound==0 || max_rpc_bits==0) throw WireError("invalid_limit");
    if (field_id>=exclusive_bound ||
        (integer_order!=control::Endian::little && integer_order!=control::Endian::big))
        throw WireError("invalid_value");

    const auto bits=std::bit_cast<std::uint32_t>(timestamp);
    if ((bits&0x7f800000U)==0x7f800000U) throw WireError("invalid_value");
    // Inspect storage to preserve finite subnormal values independently of the
    // host's floating-point comparison mode. Both signed zeros are omitted.
    const bool present=(bits&0x7fffffffU)!=0;
    BitWriter writer;
    writer.write_bounded(field_id,exclusive_bound);
    const std::size_t argument_bits=present?33:1;
    if (writer.bit_count()>max_rpc_bits || argument_bits>max_rpc_bits-writer.bit_count())
        throw WireError("field_too_large");
    writer.write_bit(present);
    if (present) {
        std::array<std::uint8_t,4> scalar{};
        for (std::size_t byte=0;byte<scalar.size();++byte) {
            const auto shift=(integer_order==control::Endian::little?byte:3-byte)*8;
            scalar[byte]=static_cast<std::uint8_t>(bits>>shift);
        }
        writer.write_bits(scalar,32);
    }
    return {writer.payload_bytes(),writer.bit_count()};
}
}
