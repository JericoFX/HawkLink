#include "hawken/control_wire.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <string_view>
#include <type_traits>

namespace {
using namespace hawken::control;
const char* error_message(ControlErrorCode code) noexcept {
    switch (code) {
    case ControlErrorCode::invalid_endian: return "invalid control endian";
    case ControlErrorCode::invalid_size: return "invalid control archive size";
    case ControlErrorCode::unsupported_opcode: return "unsupported control opcode";
    case ControlErrorCode::invalid_message: return "invalid control message";
    case ControlErrorCode::not_implemented: return "control codec unavailable";
    }
    return "control codec failed";
}

void check_endian(Endian endian) {
    if (endian != Endian::little && endian != Endian::big)
        throw ControlError(ControlErrorCode::invalid_endian);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value, Endian endian) {
    for (unsigned i=0; i<4; ++i) {
        const unsigned shift=(endian == Endian::little ? i : 3-i)*8;
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// Only used after decode validates the full message size and explicit endian.
std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset, Endian endian) {
    std::uint32_t value=0;
    for (unsigned i=0; i<4; ++i) {
        if (endian == Endian::big)
            value=(value << 8) | bytes[offset+i];
        else
            value |= static_cast<std::uint32_t>(bytes[offset+i]) << (i*8);
    }
    return value;
}

std::uint32_t crc_byte(std::uint32_t crc, std::uint8_t byte) noexcept {
    crc ^= static_cast<std::uint32_t>(byte) << 24;
    for (unsigned bit=0; bit<8; ++bit)
        crc=(crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    return crc;
}
}

namespace hawken::control {
ControlError::ControlError(ControlErrorCode code)
    : std::runtime_error(error_message(code)), code_(code) {}
ControlErrorCode ControlError::code() const noexcept { return code_; }

std::uint32_t response_crc(std::uint32_t challenge_bits) {
    // Twelve chars accommodate signed int32 decimal, including -2147483648;
    // to_chars is locale independent and does not append a NUL.
    std::array<char,12> decimal{};
    const auto formatted=std::to_chars(decimal.data(), decimal.data()+decimal.size(),
                                       std::bit_cast<std::int32_t>(challenge_bits));
    if (formatted.ec != std::errc{})
        throw ControlError(ControlErrorCode::invalid_message);

    std::uint32_t crc=0xffffffffu;
    // RVA0c37f0 consumes each UTF16 unit low byte, then high byte. Prefix and
    // decimal are deliberately serialized as UTF16LE, independently of the
    // endian selected for logical-message scalar fields. No terminal NUL.
    constexpr std::u16string_view prefix=u"895fcf626f55798667e4e94cb7a636af ";
    for (const char16_t unit: prefix) {
        crc=crc_byte(crc, static_cast<std::uint8_t>(unit));
        crc=crc_byte(crc, static_cast<std::uint8_t>(unit >> 8));
    }
    for (const char* at=decimal.data(); at!=formatted.ptr; ++at) {
        crc=crc_byte(crc, static_cast<std::uint8_t>(*at));
        crc=crc_byte(crc, 0);
    }
    return ~crc;
}

std::vector<std::uint8_t> encode(const Message& message, Endian endian) {
    check_endian(endian);
    if (message.valueless_by_exception())
        throw ControlError(ControlErrorCode::invalid_message);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(13); // Largest archive in this deliberately narrow scope.
    std::visit([&](const auto& value) {
        using Type=std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type,Initial1a>) {
            bytes.push_back(0x1a);
            write_u32(bytes, value.value, endian);
        } else if constexpr (std::is_same_v<Type,Challenge1b>) {
            bytes.push_back(0x1b);
            write_u32(bytes, value.challenge, endian);
        } else if constexpr (std::is_same_v<Type,Response1c>) {
            bytes.push_back(0x1c);
            write_u32(bytes, value.crc, endian);
        } else if constexpr (std::is_same_v<Type,Ready1d>) {
            bytes.push_back(0x1d);
        } else if constexpr (std::is_same_v<Type,Hello>) {
            bytes.push_back(0);
            write_u32(bytes, std::bit_cast<std::uint32_t>(value.min_version), endian);
            write_u32(bytes, std::bit_cast<std::uint32_t>(value.max_version), endian);
            write_u32(bytes, value.caps, endian);
        } else {
            static_assert(std::is_same_v<Type,Upgrade>);
            bytes.push_back(2);
            write_u32(bytes, std::bit_cast<std::uint32_t>(value.min_version), endian);
            write_u32(bytes, std::bit_cast<std::uint32_t>(value.max_version), endian);
        }
    }, message);
    return bytes;
}

Message decode(std::span<const std::uint8_t> archive, Endian endian) {
    check_endian(endian);
    if (archive.empty()) throw ControlError(ControlErrorCode::invalid_size);
    std::size_t expected=0;
    switch (archive[0]) {
    case 0x1a:
    case 0x1b:
    case 0x1c: expected=5; break;
    case 0x1d: expected=1; break;
    case 0: expected=13; break;
    case 2: expected=9; break;
    default: throw ControlError(ControlErrorCode::unsupported_opcode);
    }
    if (archive.size() != expected) throw ControlError(ControlErrorCode::invalid_size);
    switch (archive[0]) {
    case 0x1a: return Initial1a{read_u32(archive,1,endian)};
    case 0x1b: return Challenge1b{read_u32(archive,1,endian)};
    case 0x1c: return Response1c{read_u32(archive,1,endian)};
    case 0x1d: return Ready1d{};
    case 0: return Hello{std::bit_cast<std::int32_t>(read_u32(archive,1,endian)),
                         std::bit_cast<std::int32_t>(read_u32(archive,5,endian)),
                         read_u32(archive,9,endian)};
    case 2: return Upgrade{std::bit_cast<std::int32_t>(read_u32(archive,1,endian)),
                           std::bit_cast<std::int32_t>(read_u32(archive,5,endian))};
    default: throw ControlError(ControlErrorCode::unsupported_opcode);
    }
}
}
