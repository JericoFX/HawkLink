#include "hawken/inventory_rpc_wire.hpp"

namespace hawken::wire::rpc {
InventoryRpcPayload encode_client_given_to(std::uint32_t field_id,std::uint32_t exclusive_bound,
    std::uint16_t pawn_channel,bool do_not_activate,std::size_t max_rpc_bits) {
    if (exclusive_bound == 0 || max_rpc_bits == 0)
        throw WireError("invalid_limit");
    if (field_id >= exclusive_bound || pawn_channel > 2047)
        throw WireError("invalid_value");

    BitWriter writer;
    writer.write_bounded(field_id,exclusive_bound);
    const std::size_t argument_bits=pawn_channel == 0 ? 2 : 14;
    if (writer.bit_count() > max_rpc_bits ||
        argument_bits > max_rpc_bits-writer.bit_count())
        throw WireError("field_too_large");
    writer.write_bit(pawn_channel != 0);
    if (pawn_channel != 0)
        object::write_reference(writer,object::ActorChannelReference{pawn_channel},12);
    writer.write_bit(do_not_activate);
    return {writer.payload_bytes(),writer.bit_count()};
}
}
