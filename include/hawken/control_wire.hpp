#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace hawken::control {
enum class Endian { little, big };
enum class ControlErrorCode {
    invalid_endian, invalid_size, unsupported_opcode, invalid_message, not_implemented
};

class ControlError : public std::runtime_error {
public:
    explicit ControlError(ControlErrorCode code);
    ControlErrorCode code() const noexcept;
private:
    ControlErrorCode code_;
};

struct Initial1a {
    std::uint32_t value;
    bool operator==(const Initial1a&) const = default;
};
struct Challenge1b {
    std::uint32_t challenge;
    bool operator==(const Challenge1b&) const = default;
};
struct Response1c {
    std::uint32_t crc;
    bool operator==(const Response1c&) const = default;
};
struct Ready1d {
    bool operator==(const Ready1d&) const = default;
};
struct Hello {
    std::int32_t min_version;
    std::int32_t max_version;
    std::uint32_t caps;
    bool operator==(const Hello&) const = default;
};
struct Upgrade {
    std::int32_t min_version;
    std::int32_t max_version;
    bool operator==(const Upgrade&) const = default;
};
using Message = std::variant<Initial1a, Challenge1b, Response1c, Ready1d, Hello, Upgrade>;

// Interprets challenge bits as int32 via bit_cast, formats signed decimal,
// then CRCs UTF16LE of the recovered literal+decimal without terminal NUL.
// Does not authenticate a session, generate challenge entropy or prove login.
std::uint32_t response_crc(std::uint32_t challenge_bits);

// One logical archive only: opcode plus scalar fields. Exact lengths1/5/9/13.
// Explicit endian has no native default; version fields preserve all32bits,
// with no interval admission policy. A valueless Message is invalid_message.
// No packetId, bunch headers, marker, session wrapper, sequencing or sockets.
std::vector<std::uint8_t> encode(const Message& message, Endian endian);
// Rejects empty/truncated/extra bytes and out-of-scope opcodes. The caller
// extracts a single logical message from LSB bunch bits; no stream-prefix parse.
Message decode(std::span<const std::uint8_t> archive, Endian endian);
}
