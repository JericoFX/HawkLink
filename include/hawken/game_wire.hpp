#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace hawken::wire {
class WireError : public std::runtime_error {
public:
    explicit WireError(std::string error_code);
    std::string code;
};

// Borrows the input span; its storage must outlive the reader.
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> bytes, std::size_t bit_count);
    bool read_bit();
    std::uint32_t read_bounded(std::uint32_t limit);
    std::vector<std::uint8_t> read_bits(std::size_t count);
    std::size_t position() const;
    bool at_end() const;
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t bit_count_ = 0;
    std::size_t position_ = 0;
    bool error_ = false;
};

class BitWriter {
public:
    void write_bit(bool value);
    void write_bounded(std::uint32_t value, std::uint32_t limit);
    void write_bits(std::span<const std::uint8_t> bytes, std::size_t count);
    std::size_t bit_count() const;
    // Raw ceil(bit_count/8) payload with zero unused high bits; no packet
    // terminator. Empty writer gives empty bytes. Does not change the cursor.
    std::vector<std::uint8_t> payload_bytes() const;
    std::vector<std::uint8_t> packet_bytes() const;
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bit_count_ = 0;
};

// Physical positions are confirmed; Open/Close/Reliable are probable semantic names.
struct Bunch {
    bool control = false;
    bool open = false;
    bool close = false;
    bool reliable = false;
    std::uint16_t channel = 0;
    std::uint16_t sequence = 0;
    std::uint8_t channel_type = 0;
    std::size_t payload_bits = 0;
    std::vector<std::uint8_t> payload;
};

struct PacketRecord {
    bool is_ack = false;
    std::uint16_t ack_packet_id = 0;
    Bunch bunch;
};

struct Packet {
    std::uint16_t packet_id = 0;
    std::vector<PacketRecord> records;
};

Packet decode_packet(std::span<const std::uint8_t> datagram, std::uint32_t max_packet_bytes);
}
