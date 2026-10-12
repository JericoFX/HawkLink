#include "hawken/game_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>

namespace {
using hawken::wire::BitReader;
using hawken::wire::BitWriter;
using hawken::wire::decode_packet;

template<class Action>
void expect_code(Action action, const char* code) {
    try {
        action();
    } catch (const hawken::wire::WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()).find("never-echo-this") == std::string::npos);
        return;
    } catch (const std::exception&) {
        FAIL_CHECK("wire failure must use sanitized WireError");
        return;
    }
    FAIL_CHECK("wire operation accepted rejected input");
}

void append_ack(BitWriter& writer, std::uint32_t id) {
    writer.write_bit(true);
    writer.write_bounded(id, 0x4000);
}

// Only a fixture helper: production packet emission and channel dispatch are out of scope.
void append_bunch(BitWriter& writer, bool control, bool open, bool close, bool reliable,
                  std::uint32_t channel, std::uint32_t sequence, std::uint32_t type,
                  std::span<const std::uint8_t> payload, std::uint32_t bits, std::uint32_t bytes_limit) {
    writer.write_bit(false);
    writer.write_bit(control);
    if (control) { writer.write_bit(open); writer.write_bit(close); }
    writer.write_bit(reliable);
    writer.write_bounded(channel, 0x800);
    if (reliable) writer.write_bounded(sequence, 0x400);
    if (reliable || open) writer.write_bounded(type, 8);
    writer.write_bounded(bits, bytes_limit * 8);
    writer.write_bits(payload, bits);
}
}

TEST_CASE("Game bit reader reads literal 95 least significant bit first") {
    const std::array<std::uint8_t, 1> bytes{0x95};
    BitReader reader(bytes, 8);
    const bool expected[] = {true, false, true, false, true, false, false, true};
    CHECK(reader.position() == 0);
    for (const auto bit : expected) CHECK(reader.read_bit() == bit);
    CHECK(reader.position() == 8);
    CHECK(reader.at_end());
    expect_code([&] { reader.read_bit(); }, "truncated");
    CHECK(reader.position() == 8);
}

TEST_CASE("Game bounded literal limit10 uses value-dependent widths") {
    SUBCASE("value2 consumes three bits 010") {
        const std::array<std::uint8_t, 1> bytes{0x02};
        BitReader reader(bytes, 3);
        CHECK(reader.read_bounded(10) == 2);
        CHECK(reader.position() == 3);
        CHECK(reader.at_end());
    }
    SUBCASE("value9 consumes four bits 1001 in LSB order") {
        const std::array<std::uint8_t, 1> bytes{0x09};
        BitReader reader(bytes, 4);
        CHECK(reader.read_bounded(10) == 9);
        CHECK(reader.position() == 4);
        CHECK(reader.at_end());
    }
}

TEST_CASE("Game bounded reader covers zero one and full uint32 limits") {
    BitReader empty({}, 0);
    CHECK(empty.read_bounded(1) == 0);
    CHECK(empty.position() == 0);
    expect_code([&] { empty.read_bounded(0); }, "invalid_limit");
    const std::array<std::uint8_t, 4> all{0xff, 0xff, 0xff, 0xff};
    BitReader power(all, 31);
    CHECK(power.read_bounded(0x80000000U) == 0x7fffffffU);
    CHECK(power.position() == 31);
    BitReader max_short(all, 31);
    CHECK(max_short.read_bounded(std::numeric_limits<std::uint32_t>::max()) == 0x7fffffffU);
    CHECK(max_short.position() == 31);
    const std::array<std::uint8_t, 4> max_minus_one{0xfe, 0xff, 0xff, 0xff};
    BitReader maximum(max_minus_one, 32);
    CHECK(maximum.read_bounded(std::numeric_limits<std::uint32_t>::max()) == 0xfffffffeU);
    CHECK(maximum.position() == 32);
}

TEST_CASE("Game bit reader rejects invalid lengths and truncated bounded data") {
    const std::array<std::uint8_t, 1> bytes{0x01};
    expect_code([&] { BitReader reader(bytes, 9); }, "invalid_length");
    expect_code([&] { BitReader reader(bytes, std::numeric_limits<std::size_t>::max()); }, "invalid_length");
    expect_code([] { BitReader reader({}, 1); }, "invalid_length");
    BitReader reader(bytes, 2);
    expect_code([&] { reader.read_bounded(8); }, "truncated");
    CHECK(reader.position() == 2);
    CHECK(reader.at_end());
}

TEST_CASE("Game bit reader copies unaligned literal bits with zero tail padding") {
    const std::array<std::uint8_t, 2> bytes{0x95, 0xf2};
    BitReader reader(bytes, 16);
    CHECK(reader.read_bit());
    CHECK_FALSE(reader.read_bit());
    CHECK(reader.read_bit());
    // Literal 0xf295 >> 3, low 10 bits = 0x252, with zero output padding.
    CHECK((reader.read_bits(10) == std::vector<std::uint8_t>{0x52, 0x02}));
    CHECK(reader.position() == 13);
    CHECK((reader.read_bits(3) == std::vector<std::uint8_t>{0x07}));
    CHECK(reader.at_end());
    CHECK(reader.read_bits(0).empty());
}

TEST_CASE("Game bit reader failed bulk copy leaves cursor and sets end error") {
    const std::array<std::uint8_t, 1> bytes{0xff};
    BitReader reader(bytes, 3);
    CHECK(reader.read_bit());
    expect_code([&] { reader.read_bits(3); }, "truncated");
    CHECK(reader.position() == 1);
    CHECK(reader.at_end());
}

TEST_CASE("Game bit writer anchors literal bits and a separate marker byte") {
    BitWriter writer;
    CHECK(writer.bit_count() == 0);
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x01}));
    const bool bits[] = {true, false, true, false, true, false, false, true};
    for (const auto bit : bits) writer.write_bit(bit);
    CHECK(writer.bit_count() == 8);
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x95, 0x01}));
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x95, 0x01}));
    CHECK(writer.bit_count() == 8);
}

TEST_CASE("Game bit writer partial marker does not mutate data on repeated reads") {
    BitWriter writer;
    writer.write_bit(true); writer.write_bit(false); writer.write_bit(true);
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x0d}));
    CHECK(writer.bit_count() == 3);
    writer.write_bit(false);
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x15}));
    CHECK(writer.bit_count() == 4);
}

TEST_CASE("Game bounded writer anchors variable width literal packets") {
    BitWriter two;
    two.write_bounded(2, 10);
    CHECK(two.bit_count() == 3);
    CHECK((two.packet_bytes() == std::vector<std::uint8_t>{0x0a}));
    BitWriter nine;
    nine.write_bounded(9, 10);
    CHECK(nine.bit_count() == 4);
    CHECK((nine.packet_bytes() == std::vector<std::uint8_t>{0x19}));
    BitWriter maximum;
    maximum.write_bounded(0xfffffffeU, 0xffffffffU);
    CHECK(maximum.bit_count() == 32);
    CHECK((maximum.packet_bytes() == std::vector<std::uint8_t>{0xfe, 0xff, 0xff, 0xff, 0x01}));
}

TEST_CASE("Game bit writer rejects invalid limits values and source lengths") {
    BitWriter writer;
    writer.write_bounded(0, 1);
    CHECK(writer.bit_count() == 0);
    expect_code([&] { writer.write_bounded(0, 0); }, "invalid_limit");
    expect_code([&] { writer.write_bounded(1, 1); }, "invalid_value");
    expect_code([&] { writer.write_bounded(10, 10); }, "invalid_value");
    expect_code([&] { writer.write_bounded(0xffffffffU, 0xffffffffU); }, "invalid_value");
    const std::array<std::uint8_t, 1> bytes{0xff};
    expect_code([&] { writer.write_bits(bytes, 9); }, "invalid_length");
    expect_code([&] { writer.write_bits(bytes, std::numeric_limits<std::size_t>::max()); }, "invalid_length");
    CHECK(writer.bit_count() == 0);
}

TEST_CASE("Game bit writer copies from bit zero into an unaligned zeroed destination") {
    const std::array<std::uint8_t, 1> bytes{0x95};
    BitWriter writer;
    writer.write_bit(true);
    writer.write_bits(bytes, 7);
    // 1 | ((0x95 & 0x7f) << 1) = 0x2b; marker is a separate byte.
    CHECK(writer.bit_count() == 8);
    CHECK((writer.packet_bytes() == std::vector<std::uint8_t>{0x2b, 0x01}));
    BitWriter padded;
    const std::array<std::uint8_t, 2> all{0xff, 0xff};
    padded.write_bits(all, 9);
    CHECK((padded.packet_bytes() == std::vector<std::uint8_t>{0xff, 0x03}));
}

TEST_CASE("Game packet decoder anchors literal ACK without a writer") {
    // Positions: packet 0x123 [0..13], ACK=1 [14], ack 0x234 [15..28], marker [29].
    // Combined bits: 0x123 | (1<<14) | (0x234<<15) | (1<<29) = 0x211a4123.
    const std::array<std::uint8_t, 4> bytes{0x23, 0x41, 0x1a, 0x21};
    const auto packet = decode_packet(bytes, 16);
    CHECK(packet.packet_id == 0x123);
    REQUIRE(packet.records.size() == 1);
    CHECK(packet.records[0].is_ack);
    CHECK(packet.records[0].ack_packet_id == 0x234);
}

TEST_CASE("Game packet decoder anchors literal control open reliable bunch without a writer") {
    // packet=5 [0..13]; selector=0 [14]; control=1 [15]; open=1 [16]; close=0 [17];
    // reliable=1 [18]; channel=3 [19..29]; sequence=7 [30..39]; type=2 [40..42];
    // length=5, bound128 [43..49]; payload=0x15 [50..54]; marker=1 [55].
    const std::array<std::uint8_t, 7> bytes{0x05, 0x80, 0x1d, 0xc0, 0x01, 0x2a, 0xd4};
    const auto packet = decode_packet(bytes, 16);
    CHECK(packet.packet_id == 5);
    REQUIRE(packet.records.size() == 1);
    const auto& record = packet.records[0];
    CHECK_FALSE(record.is_ack);
    const auto& bunch = record.bunch;
    CHECK(bunch.control); CHECK(bunch.open); CHECK_FALSE(bunch.close); CHECK(bunch.reliable);
    CHECK(bunch.channel == 3); CHECK(bunch.sequence == 7); CHECK(bunch.channel_type == 2);
    CHECK(bunch.payload_bits == 5);
    CHECK((bunch.payload == std::vector<std::uint8_t>{0x15}));
}

TEST_CASE("Game packet decoder omits absent conditional fields in a literal bunch") {
    // packet3 [0..13], selector/control/reliable=0 [14..16], channel1 [17..27],
    // length0 bound128 [28..34], marker [35]. No open/close/sequence/type bits.
    const std::array<std::uint8_t, 5> bytes{0x03, 0x00, 0x02, 0x00, 0x08};
    const auto packet = decode_packet(bytes, 16);
    REQUIRE(packet.records.size() == 1);
    const auto& bunch = packet.records[0].bunch;
    CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close); CHECK_FALSE(bunch.reliable);
    CHECK(bunch.channel == 1); CHECK(bunch.sequence == 0); CHECK(bunch.channel_type == 0);
    CHECK(bunch.payload_bits == 0); CHECK(bunch.payload.empty());
}

TEST_CASE("Game packet decoder rejects invalid marker and impossible dynamic limits") {
    const std::array<std::uint8_t, 1> zero{0};
    expect_code([] { decode_packet({}, 16); }, "invalid_marker");
    expect_code([&] { decode_packet(zero, 16); }, "invalid_marker");
    const std::array<std::uint8_t, 2> header{0, 0x40};
    expect_code([&] { decode_packet(header, 0); }, "invalid_limit");
    expect_code([&] { decode_packet(header, 0x20000000U); }, "invalid_limit");
    expect_code([&] { decode_packet(header, 0xffffffffU); }, "invalid_limit");
    CHECK(decode_packet(header, 0x1fffffffU).records.empty());
}

TEST_CASE("Game packet decoder rejects truncated physical records") {
    const std::vector<std::uint8_t> packets[] = {
        {0x01}, // Marker only: packet ID absent.
        {0x00, 0xc0}, // packet ID, ACK selector, marker: ACK ID absent.
        {0x00, 0x80, 0x01}, // control=1, marker before Open.
        {0x00, 0x80, 0x02}, // control=1, Open=0, marker before Close.
        {0x00, 0x00, 0x01}, // selector/control read, marker before Reliable.
        {0x05, 0x80, 0x1d, 0xc0, 0x01, 0x2a, 0x14} // Original bunch, only 2/5 payload bits before marker.
    };
    for (const auto& packet : packets)
        expect_code([&] { decode_packet(packet, 16); }, "truncated");
}

TEST_CASE("Game packet decoder parses consecutive records without alignment or type policy") {
    BitWriter writer;
    writer.write_bounded(0x3fff, 0x4000);
    append_ack(writer, 7);
    const std::array<std::uint8_t, 2> payload{0x95, 0x01};
    append_bunch(writer, true, true, true, false, 2047, 0, 7, payload, 9, 16);
    append_ack(writer, 0x3fff);
    append_bunch(writer, false, false, false, true, 2, 1023, 0, {}, 0, 16);
    const auto packet = decode_packet(writer.packet_bytes(), 16);
    CHECK(packet.packet_id == 0x3fff);
    REQUIRE(packet.records.size() == 4);
    CHECK(packet.records[0].is_ack); CHECK(packet.records[0].ack_packet_id == 7);
    CHECK_FALSE(packet.records[1].is_ack);
    const auto& open = packet.records[1].bunch;
    CHECK(open.open); CHECK(open.close); CHECK_FALSE(open.reliable);
    CHECK(open.channel == 2047); CHECK(open.channel_type == 7);
    CHECK(open.payload_bits == 9); CHECK((open.payload == std::vector<std::uint8_t>{0x95, 0x01}));
    CHECK(packet.records[2].is_ack); CHECK(packet.records[2].ack_packet_id == 0x3fff);
    CHECK(packet.records[3].bunch.reliable); CHECK(packet.records[3].bunch.sequence == 1023);
    CHECK(packet.records[3].bunch.channel_type == 0);
}

TEST_CASE("Game packet decoder uses value-dependent payload bound for non-power limit") {
    BitWriter writer;
    writer.write_bounded(0, 0x4000);
    const std::array<std::uint8_t, 1> payload{0x95};
    // For bound24, length8 consumes only four bits: value8 + mask16 == bound24.
    append_bunch(writer, false, false, false, false, 0, 0, 0, payload, 8, 3);
    append_ack(writer, 1);
    const auto packet = decode_packet(writer.packet_bytes(), 3);
    REQUIRE(packet.records.size() == 2);
    CHECK(packet.records[0].bunch.payload_bits == 8);
    CHECK((packet.records[0].bunch.payload == std::vector<std::uint8_t>{0x95}));
    CHECK(packet.records[1].is_ack);
    CHECK(packet.records[1].ack_packet_id == 1);
}
