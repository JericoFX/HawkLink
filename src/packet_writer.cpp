#include "hawken/packet_writer.hpp"

namespace hawken::wire {
namespace {
[[noreturn]] void fail(const char* code) { throw WireError(code); }

bool empty_bunch(const Bunch& bunch) {
    return !bunch.control && !bunch.open && !bunch.close && !bunch.reliable &&
        bunch.channel == 0 && bunch.sequence == 0 && bunch.channel_type == 0 &&
        bunch.payload_bits == 0 && bunch.payload.empty();
}

// Count the same value-dependent prefix bits that write_bounded emits.
std::size_t bounded_width(std::uint32_t value, std::uint32_t limit) {
    std::size_t count = 0;
    std::uint64_t prefix = 0;
    for (std::uint64_t mask = 1; prefix + mask < limit; mask <<= 1U) {
        ++count;
        if ((value & mask) != 0) prefix |= mask;
    }
    return count;
}

void add_bits(std::size_t& total, std::size_t count, std::size_t capacity) {
    if (count > capacity - total) fail("packet_too_large");
    total += count;
}

void preflight(const Packet& packet, std::uint32_t max_packet_bytes) {
    if (max_packet_bytes == 0 || max_packet_bytes > 65507U) fail("invalid_limit");
    if (packet.packet_id >= 0x4000) fail("invalid_value");

    // This is our interior-buffer policy; the session wrapper is not included.
    const auto capacity = static_cast<std::size_t>(max_packet_bytes) * 8 - 1;
    if (capacity < 14) fail("packet_too_large");
    // Every record occupies at least the ACK's selector plus 14-bit ID.
    // Reject impossible record counts before walking caller-owned storage.
    if (packet.records.size() > (capacity - 14) / 15) fail("packet_too_large");
    const auto payload_bound = max_packet_bytes * 8U;
    std::size_t total = 14;
    for (const auto& record : packet.records) {
        const auto& bunch = record.bunch;
        if (record.is_ack) {
            if (record.ack_packet_id >= 0x4000) fail("invalid_value");
            if (!empty_bunch(bunch)) fail("inconsistent_record");
            add_bits(total, 15, capacity);
            continue;
        }

        if (record.ack_packet_id != 0) fail("inconsistent_record");
        if (bunch.channel >= 0x800 ||
            (bunch.reliable && bunch.sequence >= 0x400) ||
            ((bunch.reliable || bunch.open) && bunch.channel_type >= 8))
            fail("invalid_value");
        if (bunch.control != (bunch.open || bunch.close) ||
            (!bunch.reliable && bunch.sequence != 0) ||
            (!(bunch.reliable || bunch.open) && bunch.channel_type != 0))
            fail("inconsistent_record");
        if (bunch.payload_bits >= payload_bound) fail("invalid_value");

        const auto payload_bytes = bunch.payload_bits / 8 +
            (bunch.payload_bits % 8 != 0 ? 1 : 0);
        if (bunch.payload.size() != payload_bytes) fail("invalid_length");
        const auto tail_bits = bunch.payload_bits % 8;
        if (tail_bits != 0 &&
            (bunch.payload.back() & (0xffU << tail_bits)) != 0)
            fail("inconsistent_record");

        // Selector, flags-present, reliable and channel are always present.
        std::size_t metadata_bits = 1 + 1 + 1 + 11;
        if (bunch.control) metadata_bits += 2;
        if (bunch.reliable) metadata_bits += 10;
        if (bunch.reliable || bunch.open) metadata_bits += 3;
        metadata_bits += bounded_width(static_cast<std::uint32_t>(bunch.payload_bits),
                                       payload_bound);
        add_bits(total, metadata_bits, capacity);
        add_bits(total, bunch.payload_bits, capacity);
    }
}
}

std::vector<std::uint8_t> encode_packet(const Packet& packet, std::uint32_t max_packet_bytes) {
    preflight(packet, max_packet_bytes);

    BitWriter writer;
    writer.write_bounded(packet.packet_id, 0x4000);
    for (const auto& record : packet.records) {
        writer.write_bit(record.is_ack);
        if (record.is_ack) {
            writer.write_bounded(record.ack_packet_id, 0x4000);
            continue;
        }
        const auto& bunch = record.bunch;
        writer.write_bit(bunch.control);
        if (bunch.control) {
            writer.write_bit(bunch.open);
            writer.write_bit(bunch.close);
        }
        writer.write_bit(bunch.reliable);
        writer.write_bounded(bunch.channel, 0x800);
        if (bunch.reliable) writer.write_bounded(bunch.sequence, 0x400);
        if (bunch.reliable || bunch.open) writer.write_bounded(bunch.channel_type, 8);
        writer.write_bounded(static_cast<std::uint32_t>(bunch.payload_bits),
                             max_packet_bytes * 8U);
        writer.write_bits(bunch.payload, bunch.payload_bits);
    }
    auto bytes = writer.packet_bytes();
    if (bytes.size() > max_packet_bytes) fail("packet_too_large");
    return bytes;
}
}
