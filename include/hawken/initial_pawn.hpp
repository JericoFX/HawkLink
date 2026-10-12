#pragma once
#include "hawken/actor_transform_wire.hpp"
#include "hawken/object_reference_wire.hpp"
#include <optional>

namespace hawken::wire::actor {
struct InitialPawn {
    object::PackageExportReference archetype;
    QuantizedPosition position;
    std::optional<Rotation> rotation;
};
struct InitialPawnPayload {
    std::vector<std::uint8_t> payload;
    std::size_t payload_bits;
};

// Precondition: caller selects the native dynamic-actor PRIMARY reference
// branch whose GetPlayerController returns null. No controller byte is emitted.
// Rotation presence is the actor+ec bit0 decision supplied by the caller;
// this function neither selects a CDO nor infers its effective rotation flag.
// Compose reference, position and optional rotation at one logical bit cursor.
// Empty field tail: no sentinel, alignment, Bunch flags or packet terminator.
// A positive caller budget caps the complete payload without truncation.
// Existing codecs validate fields; fixed WireError codes are invalid_limit,
// invalid_value and field_too_large. Local construction returns no partial DTO.
// No object lookup, actor creation, transport, admission or gameplay claim.
InitialPawnPayload encode_initial_pawn(const InitialPawn& value,
                                      std::size_t max_payload_bits);
}
