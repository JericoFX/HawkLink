#include "hawken/control_handshake.hpp"

namespace {
const char* error_message(hawken::control::HandshakeErrorCode code) noexcept {
    using hawken::control::HandshakeErrorCode;
    switch (code) {
    case HandshakeErrorCode::invalid_configuration: return "invalid control handshake configuration";
    case HandshakeErrorCode::unexpected_message: return "unexpected control handshake message";
    case HandshakeErrorCode::terminal_state: return "control handshake already terminal";
    case HandshakeErrorCode::not_implemented: return "control handshake unavailable";
    }
    return "control handshake failed";
}
}

namespace hawken::control {
HandshakeError::HandshakeError(HandshakeErrorCode code)
    : std::runtime_error(error_message(code)), code_(code) {}
HandshakeErrorCode HandshakeError::code() const noexcept { return code_; }

ServerHandshake::ServerHandshake(Endian endian, std::uint32_t challenge,
                                 std::int32_t server_min, std::int32_t server_max)
    : endian_(endian), challenge_(challenge), server_min_(server_min), server_max_(server_max) {
    if ((endian != Endian::little && endian != Endian::big) || server_min > server_max)
        throw HandshakeError(HandshakeErrorCode::invalid_configuration);
}
HandshakeState ServerHandshake::state() const noexcept { return state_; }
HandshakeStep ServerHandshake::receive(std::span<const std::uint8_t> archive) {
    if (state_ == HandshakeState::hello_validated || state_ == HandshakeState::closed)
        throw HandshakeError(HandshakeErrorCode::terminal_state);

    // Decode the whole logical archive before any state mutation. The caller
    // has already handled the packet/channel/session layers independently.
    const auto message=decode(archive,endian_);
    switch (state_) {
    case HandshakeState::awaiting_initial: {
        if (!std::holds_alternative<Initial1a>(message))
            throw HandshakeError(HandshakeErrorCode::unexpected_message);
        // Native9ed310 does not restrict the Initial1a scalar. The challenge
        // is supplied by the caller; this module does not reproduce rand().
        HandshakeStep result;
        result.reply=encode(Challenge1b{challenge_},endian_);
        state_=HandshakeState::awaiting_response;
        return result;
    }
    case HandshakeState::awaiting_response: {
        const auto* response=std::get_if<Response1c>(&message);
        if (response == nullptr)
            throw HandshakeError(HandshakeErrorCode::unexpected_message);
        HandshakeStep result;
        if (response->crc != response_crc(challenge_)) {
            result.close=true;
            state_=HandshakeState::closed;
            return result;
        }
        result.reply=encode(Ready1d{},endian_);
        state_=HandshakeState::awaiting_hello;
        return result;
    }
    case HandshakeState::awaiting_hello: {
        const auto* hello=std::get_if<Hello>(&message);
        if (hello == nullptr)
            throw HandshakeError(HandshakeErrorCode::unexpected_message);
        HandshakeStep result;
        // Exact signed32 comparisons from782660, including an inverted client
        // interval. Caps are data here; no capability/admission policy added.
        if (hello->max_version < server_min_ || server_max_ < hello->min_version) {
            result.reply=encode(Upgrade{server_min_,server_max_},endian_);
            result.close=true;
            state_=HandshakeState::closed;
            return result;
        }
        const auto version=hello->max_version <= server_max_ ? hello->max_version : server_max_;
        result.hello=NegotiatedHello{version,hello->caps};
        state_=HandshakeState::hello_validated;
        return result;
    }
    case HandshakeState::hello_validated:
    case HandshakeState::closed:
        throw HandshakeError(HandshakeErrorCode::terminal_state);
    }
    throw HandshakeError(HandshakeErrorCode::invalid_configuration);
}
}
