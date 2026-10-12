#include "hawken/pawn_rpc_wire.hpp"

namespace hawken::wire::rpc {
RpcPayload encode_rpc(const PawnRpc& value, std::uint32_t exclusive_bound,
                      bool eligible, std::size_t max_rpc_bits) {
    if (exclusive_bound == 0 || max_rpc_bits == 0)
        throw WireError("invalid_limit");
    if (value.field_id >= exclusive_bound || (!eligible && value.value))
        throw WireError("invalid_value");

    BitWriter writer;
    writer.write_bounded(value.field_id,exclusive_bound);
    if (writer.bit_count() > max_rpc_bits) throw WireError("field_too_large");
    if (eligible) {
        if (writer.bit_count() == max_rpc_bits) throw WireError("field_too_large");
        writer.write_bit(value.value.has_value());
        if (value.value) {
            const auto remaining=max_rpc_bits-writer.bit_count();
            if (remaining == 0) throw WireError("field_too_large");
            object::write_reference(writer,*value.value,remaining);
        }
    }
    return {writer.payload_bytes(),writer.bit_count()};
}
RpcResult decode_rpc(BitReader& reader, std::uint32_t exclusive_bound,
                     bool eligible, std::size_t max_rpc_bits) {
    if (exclusive_bound == 0 || max_rpc_bits == 0)
        throw WireError("invalid_limit");

    const auto start=reader.position();
    auto field_reader=reader;
    const auto field_id=field_reader.read_bounded(exclusive_bound);
    // Non-power-of-two bounds may use fewer bits for some IDs. Commit only
    // after measuring the actual prefix, not a conservative maximum width.
    if (field_reader.position()-start > max_rpc_bits)
        throw WireError("field_too_large");
    reader=field_reader;

    std::optional<object::Reference> value;
    if (eligible) {
        if (reader.position()-start == max_rpc_bits)
            throw WireError("field_too_large");
        if (reader.read_bit()) {
            const auto remaining=max_rpc_bits-(reader.position()-start);
            if (remaining == 0) throw WireError("field_too_large");
            value=object::read_reference(reader,remaining).value;
        }
    }
    return {field_id,value,reader.position()-start};
}
}
