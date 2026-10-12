#pragma once
#include "hawken/actor_transform_wire.hpp"
#include "hawken/object_reference_wire.hpp"
#include <optional>

namespace hawken::wire::actor {
struct InitialController {
    object::PackageExportReference archetype;
    QuantizedPosition position;
    std::optional<Rotation> rotation;
    std::uint8_t net_player_index;
};
struct InitialControllerPayload {
    std::vector<std::uint8_t> payload;
    std::size_t payload_bits;
};

// Precondition: caller selects the native dynamic-actor/controller PRIMARY
// reference branch. This composes archetype, position, optional rotation and
// the raw controller byte; it does not resolve objects or validate a CDO.
// Rotation presence reflects the actor+ec bit0 branch selected by the caller;
// no extra presence bit is emitted for the optional field itself.
// Empty field tail: no sentinel, alignment, packet terminator or Bunch flags.
// A positive caller budget caps the complete logical payload without truncation.
// Existing codecs validate index, position and quantized rotation. Errors use
// fixed WireError codes invalid_limit, invalid_value and field_too_large.
// Local construction prevents returning partial payload on any failure.
// No actor creation, transport, admission or gameplay compatibility claim.
InitialControllerPayload encode_initial_controller(const InitialController& value,
                                                    std::size_t max_payload_bits);
}
