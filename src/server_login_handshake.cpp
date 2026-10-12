#include "hawken/server_login_handshake.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <utility>

namespace hawken::control::login {
Error::Error(ErrorCode code)
    : std::runtime_error("Server login handshake operation could not be completed."), code_(code) {}
ErrorCode Error::code() const noexcept { return code_; }

ServerLoginHandshake::ServerLoginHandshake(const Config& config) : config_(config) {
    if ((config.endian != Endian::little && config.endian != Endian::big) ||
        config.limits.max_archive_bytes == 0 || config.limits.max_string_units == 0 ||
        config.limits.max_string_units > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        config.driver_max_speed < 1800) {
        throw Error(ErrorCode::invalid_configuration);
    }
}
Snapshot ServerLoginHandshake::snapshot() const noexcept { return view_; }

Step ServerLoginHandshake::begin(NegotiatedHello hello) {
    if (view_.state == State::closed) throw Error(ErrorCode::terminal_state);
    if (view_.state != State::awaiting_hello) throw Error(ErrorCode::unexpected_message);
    try {
        // The observed %08X challenge uses eight digits plus a transmitted NUL.
        // The caller supplies the token; it is not a Steam credential or key.
        std::u16string digits(9, u'\0');
        constexpr char16_t hex[] = u"0123456789ABCDEF";
        for (unsigned index = 0; index < 8; ++index) {
            digits[index] = hex[(config_.challenge_token >> ((7u - index) * 4u)) & 15u];
        }
        Step result;
        result.reply = posthello::encode(posthello::Challenge{
            std::bit_cast<std::uint32_t>(hello.version),
            {posthello::StringForm::narrow, std::move(digits)}}, config_.endian, config_.limits);
        // Publish state only after allocation/encoding has succeeded.
        view_.caps = hello.caps;
        view_.state = State::awaiting_login;
        return result;
    } catch (const posthello::Error& error) {
        throw Error(error.code() == posthello::ErrorCode::resource_limit
            ? ErrorCode::resource_limit : ErrorCode::invalid_archive);
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}

Step ServerLoginHandshake::receive(std::span<const std::uint8_t> archive) {
    if (view_.state == State::closed) throw Error(ErrorCode::terminal_state);
    if (view_.state != State::awaiting_login) throw Error(ErrorCode::unexpected_message);
    try {
        auto message = posthello::decode_exact(archive, config_.endian, config_.limits);
        if (const auto* speed = std::get_if<posthello::NetSpeed>(&message)) {
            view_.net_speed = std::clamp(std::bit_cast<std::int32_t>(speed->value),
                                        std::int32_t{1800}, config_.driver_max_speed);
            return {};
        }
        if (auto* login = std::get_if<posthello::Login>(&message)) {
            Step result;
            result.candidate = std::move(*login);
            view_.state = State::login_received;
            return result;
        }
        if (std::holds_alternative<posthello::Failure>(message)) {
            view_.state = State::closed;
            return {{}, std::nullopt, true};
        }
        throw Error(ErrorCode::unexpected_message);
    } catch (const posthello::Error& error) {
        throw Error(error.code() == posthello::ErrorCode::resource_limit
            ? ErrorCode::resource_limit : ErrorCode::invalid_archive);
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}
}
