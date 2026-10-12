#pragma once
#include "hawken/control_handshake.hpp"
#include "hawken/posthello_control.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace hawken::control::login {
struct Config {
    Endian endian;
    posthello::Limits limits;
    std::uint32_t challenge_token; // Supplied by caller; no RNG or session derivation.
    std::int32_t driver_max_speed;
};
enum class State { awaiting_hello, awaiting_login, login_received, closed };
enum class ErrorCode {
    invalid_configuration, invalid_archive, unexpected_message, resource_limit,
    terminal_state, not_implemented
};
class Error : public std::runtime_error {
public:
    explicit Error(ErrorCode code);
    ErrorCode code() const noexcept;
private:
    ErrorCode code_;
};
struct Snapshot {
    State state;
    // Empty until begin()/NetSpeed respectively: own observable policy, not a
    // recovered native default. All caps bits are retained without admission.
    std::optional<std::uint32_t> caps;
    std::optional<std::int32_t> net_speed;
    bool operator==(const Snapshot&) const = default;
};
struct Step {
    // ONE logical archive, or empty. Caller owns bunch/packet/session framing.
    std::vector<std::uint8_t> reply;
    // Caller-owned raw Login5 fields, never retained by this machine. No Guid,
    // URL, Steam, session or entitlement interpretation/authorization occurs.
    std::optional<posthello::Login> candidate;
    bool closed = false;
    bool operator==(const Step&) const = default;
};

// Pure next logical stage after a validated Hello. No socket, SQL, credentials,
// authority callback, reset, Welcome, packages, world, spawn or gameplay.
// Own configuration policy: valid Endian, positive codec Limits with string
// bound<=INT32_MAX, and driver_max_speed>=1800. Smaller valid budgets may fail
// begin() while encoding its 18-byte reply/9-unit string; state remains intact.
class ServerLoginHandshake {
public:
    explicit ServerLoginHandshake(const Config& config);
    Snapshot snapshot() const noexcept;

    // Exactly once: Challenge3 carries negotiated signed version bits as u32
    // plus narrow FString of eight uppercase padded hex digits and one NUL.
    // Encode/allocation failure leaves state/caps/speed unchanged. This token
    // is distinct from pre-Hello challenge1b, Steam ticket and session key.
    Step begin(NegotiatedHello hello);

    // ONE exact bounded archive, already authenticated/extracted by caller;
    // coalesced suffixes are rejected, not partially consumed. NetSpeed4 may
    // repeat while awaiting_login: reinterpret u32 bits as signed32, clamp
    // <=1799 to 1800, otherwise min(value,driver_max_speed). Login5 does NOT
    // require prior NetSpeed: returns two RawStrings and modern four-u32 ID,
    // then transfers to login_received, NOT admission. Valid peer Failure6
    // while awaiting_login closes with no reply/candidate/error-string echo.
    // Invalid/truncated/unsupported archives -> invalid_archive, exceeded
    // budgets/allocation -> resource_limit. Codec rejection preserves state,
    // caps and speed. Other decoded opcodes/order/duplicates are strict own
    // unexpected_message policy; closed operations -> terminal_state. Every
    // operation after login_received is rejected without mutation, including
    // Failure6. These restrictions do not claim native ignore/retry behavior.
    Step receive(std::span<const std::uint8_t> archive);
private:
    Config config_;
    Snapshot view_{State::awaiting_hello,std::nullopt,std::nullopt};
};
}
