#include "hawken/spawn_validation_rpc_wire.hpp"

namespace hawken::wire::rpc {
SpawnValidationRpcPayload encode_spawn_validated(std::uint32_t field_id,
    std::uint32_t exclusive_bound, std::size_t max_rpc_bits) {
    if (exclusive_bound == 0 || max_rpc_bits == 0) throw WireError("invalid_limit");
    if (field_id >= exclusive_bound) throw WireError("invalid_value");
    BitWriter writer;
    writer.write_bounded(field_id, exclusive_bound);
    if (writer.bit_count() > max_rpc_bits) throw WireError("field_too_large");
    return {writer.payload_bytes(), writer.bit_count()};
}
SpawnValidationRpcPayload encode_spawn_failed(std::uint8_t reason,
    std::uint32_t field_id, std::uint32_t exclusive_bound, std::size_t max_rpc_bits) {
    if (exclusive_bound == 0 || max_rpc_bits == 0) throw WireError("invalid_limit");
    if (field_id >= exclusive_bound || reason > 15) throw WireError("invalid_value");
    BitWriter writer;
    writer.write_bounded(field_id, exclusive_bound);
    const std::size_t argument_bits = reason == 0 ? 1 : 5;
    if (writer.bit_count() > max_rpc_bits || argument_bits > max_rpc_bits - writer.bit_count())
        throw WireError("field_too_large");
    writer.write_bit(reason != 0);
    if (reason != 0) {
        for (unsigned bit = 0; bit < 4; ++bit)
            writer.write_bit((reason & (1U << bit)) != 0);
    }
    return {writer.payload_bytes(), writer.bit_count()};
}
SpawnFailedArgumentsResult read_spawn_failed_arguments(BitReader& reader,
    std::size_t max_argument_bits) {
    if (max_argument_bits == 0) throw WireError("invalid_limit");
    const auto start = reader.position();
    auto candidate = reader;
    std::uint8_t reason = 0;
    if (candidate.read_bit()) {
        if (max_argument_bits < 5) throw WireError("field_too_large");
        for (unsigned bit = 0; bit < 4; ++bit) {
            if (candidate.read_bit()) reason |= static_cast<std::uint8_t>(1U << bit);
        }
    }
    const auto consumed = candidate.position() - start;
    reader = candidate;
    return {reason, consumed};
}
}
