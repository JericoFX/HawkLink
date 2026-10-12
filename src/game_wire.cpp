#include "hawken/game_wire.hpp"
#include <limits>
#include <utility>

namespace hawken::wire {
namespace {
[[noreturn]] void fail(const char* code) { throw WireError(code); }

bool fits_bits(std::size_t count, std::size_t bytes) {
    return count / 8 < bytes || (count / 8 == bytes && count % 8 == 0);
}
}

WireError::WireError(std::string error_code)
    : std::runtime_error("Game wire data could not be read or validated."),
      code(std::move(error_code)) {}

BitReader::BitReader(std::span<const std::uint8_t> bytes, std::size_t bit_count)
    : bytes_(bytes), bit_count_(bit_count) {
    if (!fits_bits(bit_count, bytes.size())) fail("invalid_length");
}
bool BitReader::read_bit() {
    if (position_ == bit_count_) {
        error_ = true;
        fail("truncated");
    }
    const auto bit = (bytes_[position_ / 8] & (1U << (position_ % 8))) != 0;
    ++position_;
    return bit;
}
std::uint32_t BitReader::read_bounded(std::uint32_t limit) {
    if (limit == 0) fail("invalid_limit");
    std::uint64_t value = 0;
    for (std::uint64_t mask = 1; value + mask < limit; mask <<= 1U)
        if (read_bit()) value |= mask;
    return static_cast<std::uint32_t>(value);
}
std::vector<std::uint8_t> BitReader::read_bits(std::size_t count) {
    if (count > bit_count_ - position_) {
        error_ = true;
        fail("truncated");
    }
    std::vector<std::uint8_t> result(count / 8 + (count % 8 != 0 ? 1 : 0), 0);
    for (std::size_t index = 0; index < count; ++index)
        if (read_bit()) result[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
    return result;
}
std::size_t BitReader::position() const { return position_; }
bool BitReader::at_end() const { return error_ || position_ == bit_count_; }

void BitWriter::write_bit(bool value) {
    if (bit_count_ == std::numeric_limits<std::size_t>::max()) fail("invalid_length");
    if (bit_count_ % 8 == 0) bytes_.push_back(0);
    if (value) bytes_[bit_count_ / 8] |= static_cast<std::uint8_t>(1U << (bit_count_ % 8));
    ++bit_count_;
}
void BitWriter::write_bounded(std::uint32_t value, std::uint32_t limit) {
    if (limit == 0) fail("invalid_limit");
    if (value >= limit) fail("invalid_value");
    std::uint64_t prefix = 0;
    for (std::uint64_t mask = 1; prefix + mask < limit; mask <<= 1U) {
        const auto bit = (value & mask) != 0;
        write_bit(bit);
        if (bit) prefix |= mask;
    }
}
void BitWriter::write_bits(std::span<const std::uint8_t> bytes, std::size_t count) {
    if (!fits_bits(count, bytes.size()) || count > std::numeric_limits<std::size_t>::max() - bit_count_)
        fail("invalid_length");
    for (std::size_t index = 0; index < count; ++index)
        write_bit((bytes[index / 8] & (1U << (index % 8))) != 0);
}
std::size_t BitWriter::bit_count() const { return bit_count_; }
std::vector<std::uint8_t> BitWriter::payload_bytes() const {
    return bytes_;
}
std::vector<std::uint8_t> BitWriter::packet_bytes() const {
    auto result = bytes_;
    if (bit_count_ % 8 == 0) result.push_back(0);
    result[bit_count_ / 8] |= static_cast<std::uint8_t>(1U << (bit_count_ % 8));
    return result;
}

Packet decode_packet(std::span<const std::uint8_t> datagram, std::uint32_t max_packet_bytes) {
    if (max_packet_bytes == 0 || max_packet_bytes > std::numeric_limits<std::uint32_t>::max() / 8U)
        fail("invalid_limit");
    if (datagram.empty() || datagram.back() == 0) fail("invalid_marker");
    std::size_t marker = 0;
    for (unsigned int last = datagram.back(); last > 1; last >>= 1U) ++marker;
    if (datagram.size() - 1 > (std::numeric_limits<std::size_t>::max() - marker) / 8)
        fail("invalid_length");
    const auto bit_count = (datagram.size() - 1) * 8 + marker;
    BitReader reader(datagram, bit_count);
    Packet packet;
    packet.packet_id = static_cast<std::uint16_t>(reader.read_bounded(0x4000));
    while (!reader.at_end()) {
        PacketRecord record;
        record.is_ack = reader.read_bit();
        if (record.is_ack) record.ack_packet_id = static_cast<std::uint16_t>(reader.read_bounded(0x4000));
        else {
            auto& bunch = record.bunch;
            bunch.control = reader.read_bit();
            if (bunch.control) {
                bunch.open = reader.read_bit();
                bunch.close = reader.read_bit();
            }
            bunch.reliable = reader.read_bit();
            bunch.channel = static_cast<std::uint16_t>(reader.read_bounded(0x800));
            if (bunch.reliable) bunch.sequence = static_cast<std::uint16_t>(reader.read_bounded(0x400));
            if (bunch.reliable || bunch.open)
                bunch.channel_type = static_cast<std::uint8_t>(reader.read_bounded(8));
            bunch.payload_bits = reader.read_bounded(max_packet_bytes * 8U);
            bunch.payload = reader.read_bits(bunch.payload_bits);
        }
        packet.records.push_back(std::move(record));
    }
    return packet;
}
}
