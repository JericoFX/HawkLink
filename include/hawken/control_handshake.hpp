#pragma once
#include "hawken/control_wire.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace hawken::control {
enum class HandshakeState {
    awaiting_initial, awaiting_response, awaiting_hello, hello_validated, closed
};
enum class HandshakeErrorCode {
    invalid_configuration, unexpected_message, terminal_state, not_implemented
};
class HandshakeError : public std::runtime_error {
public:
    explicit HandshakeError(HandshakeErrorCode code);
    HandshakeErrorCode code() const noexcept;
private:
    HandshakeErrorCode code_;
};

struct NegotiatedHello {
    std::int32_t version;
    std::uint32_t caps;
    bool operator==(const NegotiatedHello&) const = default;
};
struct HandshakeStep {
    // A single logical reply, or empty. The caller owns reliable channel0,
    // packet/ACK/sequence/marker framing, session wrapping and transmission.
    std::vector<std::uint8_t> reply;
    std::optional<NegotiatedHello> hello;
    bool close = false;
    bool operator==(const HandshakeStep&) const = default;
};

// Per-connection logical server state only. All configuration is explicit;
// no challenge generation, endian detection, native CDO default or reset.
// Requires server_min<=server_max and a valid Endian (our configuration policy).
class ServerHandshake {
public:
    ServerHandshake(Endian endian, std::uint32_t challenge,
                    std::int32_t server_min, std::int32_t server_max);
    HandshakeState state() const noexcept;

    // Consumes ONE exact logical archive, already extracted/authenticated by
    // the caller. This module does not authenticate its input or a session.
    // ControlError from decode remains fixed and leaves state unchanged.
    // Initial1a's u32 is consumed without a value restriction. A valid CRC1c
    // produces Ready1d; a mismatch closes without a reply. Hello uses only
    // recovered signed interval comparisons, even if its min>max; caps retain
    // all32bits. Disjoint intervals produce Upgrade and close. Otherwise,
    // hello_validated transfers to the next stage with min(clientMax,serverMax),
    // NOT admission, Challenge3, Login, Welcome, spawn or gameplay.
    // Out-of-order/terminal input throws HandshakeError without mutation:
    // this strict API policy does not claim native ignore/retry behaviour.
    HandshakeStep receive(std::span<const std::uint8_t> archive);
private:
    Endian endian_;
    std::uint32_t challenge_;
    std::int32_t server_min_;
    std::int32_t server_max_;
    HandshakeState state_ = HandshakeState::awaiting_initial;
};
}
