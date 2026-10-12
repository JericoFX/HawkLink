#pragma once
#include "hawken/control_wire.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace hawken::control::posthello {
enum class StringForm { zero, narrow, wide };

// Raw wire units, including every transmitted NUL if present. This is not a
// semantic FString: no +/-1 collapse, terminator check, Unicode normalization,
// UTF8 decoding or surrogate validation. Narrow units must each be <=0xff.
// zero requires no units; narrow/wide require at least one. Prefix is signed32
// 0/+units/-units in scalar endian; wide body units remain UTF16LE byte pairs.
struct RawString {
    StringForm form;
    std::u16string units;
    bool operator==(const RawString&) const = default;
};
struct Welcome {
    RawString level;
    RawString game;
    bool operator==(const Welcome&) const = default;
};
struct Challenge {
    std::uint32_t version;
    RawString challenge;
    bool operator==(const Challenge&) const = default;
};
struct NetSpeed {
    std::uint32_t value;
    bool operator==(const NetSpeed&) const = default;
};
struct Login {
    RawString response;
    RawString url;
    // Modern archive branch ONLY: four individually endian-ordered u32 words,
    // exactly16bytes. Caller accepts this scope; no legacy8byte compatibility,
    // UUID text interpretation, Steam verification or authorization.
    std::array<std::uint32_t,4> unique_id;
    bool operator==(const Login&) const = default;
};
struct Failure {
    RawString error;
    bool operator==(const Failure&) const = default;
};
struct Package {
    std::array<std::uint32_t,4> identity;
    RawString name0;
    RawString name1;
    RawString text2;
    std::uint32_t value0;
    std::uint32_t value1;
    RawString name3;
    std::uint8_t flags;
    bool operator==(const Package&) const = default;
};
struct DownloadMetadata {
    // Opcode0b: two physical FString archives and one scalar. Neutral value,
    // not interpreted as flags. Does not resolve classes, activate downloads,
    // transmit package content or certify a world/Welcome/admission.
    RawString class_path;
    RawString parameters;
    std::uint32_t value;
    bool operator==(const DownloadMetadata&) const = default;
};
// Opcode8: four raw identity words followed by generation, each scalar u32.
// No UUID text conversion, package authorization or world/admission implied.
struct PackageAck {
    std::array<std::uint32_t,4> identity;
    std::uint32_t generation;
    bool operator==(const PackageAck&) const = default;
};
// Opcode9 has no body in the observed server World dispatcher782660. This
// logical archive does not authorize entry, resolve a world or spawn a player.
struct Join {
    bool operator==(const Join&) const = default;
};
using Message = std::variant<Welcome, Challenge, NetSpeed, Login, Failure, Package,
    DownloadMetadata, PackageAck, Join>;

// Caller-owned safety policy, not native defaults or a UDP size guarantee.
// Both limits are positive. max_string_units must fit positive signed32.
// max_archive_bytes caps ONE consumed/produced message, not a coalesced span.
// No relation between the two limits is required: actual total bytes are checked.
struct Limits {
    std::size_t max_archive_bytes;
    std::size_t max_string_units;
};
enum class ErrorCode {
    invalid_endian, invalid_limits, invalid_message, unsupported_opcode,
    truncated_archive, extra_bytes, resource_limit, not_implemented
};
class Error : public std::runtime_error {
public:
    explicit Error(ErrorCode code);
    ErrorCode code() const noexcept;
private:
    ErrorCode code_;
};
struct Decoded {
    Message message;
    std::size_t consumed;
};

// Exactly the nine logical opcodes1/3/4/5/6/7/0b/8/9, with explicit scalar endian.
// Invalid forms, INT32_MIN, arithmetic overflow, exceeded limits, and valueless
// variants are rejected with fixed typed errors. Complete encode preflight
// precedes reserve/copy; decode checks body budgets before allocating strings.
// No default world/list, URL parsing, state machine, PreLogin, admission, bunch
// headers, reliable sequencing, packet markers, session crypto or sockets.
std::vector<std::uint8_t> encode(const Message& message, Endian endian, Limits limits);
// Consumes only the first logical message; later coalesced messages/suffix bytes
// remain unread. Never read or allocate outside that message's byte budget.
Decoded decode_prefix(std::span<const std::uint8_t> archive, Endian endian, Limits limits);
// Same parse, then requires consumed==archive.size(); rejects any suffix.
Message decode_exact(std::span<const std::uint8_t> archive, Endian endian, Limits limits);
}
