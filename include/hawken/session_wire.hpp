#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace hawken::session {
using SessionId = std::array<std::uint8_t, 8>;
using SessionKey = std::array<std::uint8_t, 32>;
inline constexpr std::size_t wrapper_bytes = 48;
inline constexpr std::size_t max_packet_bytes = 65507;
inline constexpr std::size_t max_payload_bytes = max_packet_bytes - wrapper_bytes;
inline constexpr std::uint64_t direction_bit = std::uint64_t{1} << 63;

enum class Role { client, server };
enum class SessionErrorCode {
    invalid_role, invalid_counter, invalid_payload, invalid_packet_size,
    session_mismatch, wrong_direction, replay, authentication_failed,
    counter_exhausted, crypto_failure, not_implemented
};

class SessionError : public std::runtime_error {
public:
    explicit SessionError(SessionErrorCode code);
    SessionErrorCode code() const noexcept;
private:
    SessionErrorCode code_;
};

// Pure transition, with no context mutation/reset. Rejects exhaustion and a
// current counter outside the sender's half; it never wraps to the other half.
std::uint64_t next_send_counter(Role role, std::uint64_t current);

// Thread-confined, noncopyable session owner. One live context per ID/key/role;
// constructing another with the same credentials restarts nonces and is unsafe.
// Credentials must come from the legitimate backend, not from a Steam ticket.
// Client receive-high-bit is recovered; server receive-low-bit is our reciprocal
// policy, not a recovered native server implementation. No sockets/admission.
class Context {
public:
    Context(Role role, const SessionId& id, const SessionKey& key);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    // ID8 | counterLE64 | original-ChaCha20(payload) | HMAC-SHA512 first32.
    // Stream block counter starts at zero per packet; nonce is header counter.
    // The same key is used for stream and MAC to reproduce the observed format.
    // Input validation precedes reservation of the next send counter. Once
    // reserved, any allocation/provider failure consumes it (no nonce reuse).
    std::vector<std::uint8_t> encode(std::span<const std::uint8_t> payload);
    // ID/direction/strict unsigned ordering and MAC checked before decryption.
    // Receive counter advances only after successful verification/decryption.
    std::vector<std::uint8_t> decode(std::span<const std::uint8_t> packet);
    std::uint64_t last_sent() const noexcept;
    std::uint64_t last_received() const noexcept;
private:
    Role role_;
    SessionId id_{};
    SessionKey key_{};
    std::uint64_t last_sent_ = 0;
    std::uint64_t last_received_ = 0;
};
}
