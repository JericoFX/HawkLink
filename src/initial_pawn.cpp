#include "hawken/initial_pawn.hpp"

namespace hawken::wire::actor {
InitialPawnPayload encode_initial_pawn(const InitialPawn& value,
                                      std::size_t max_payload_bits) {
    if (max_payload_bits == 0) throw WireError("invalid_limit");
    BitWriter writer;
    const auto remaining=[&] {
        if (writer.bit_count() >= max_payload_bits) throw WireError("field_too_large");
        return max_payload_bits-writer.bit_count();
    };
    object::write_reference(writer,object::Reference{value.archetype},remaining());
    write_position(writer,value.position,remaining());
    if (value.rotation) write_rotation(writer,*value.rotation,remaining());
    return {writer.payload_bytes(),writer.bit_count()};
}
}
