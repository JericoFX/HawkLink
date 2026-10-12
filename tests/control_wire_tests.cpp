#include "hawken/control_wire.hpp"
#include "hawken/game_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <bit>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace hawken::control;

template<class Action>
void expect_error(Action action, ControlErrorCode expected) {
    try { action(); }
    catch (const ControlError& error) {
        CHECK(error.code() == expected);
        CHECK(std::string(error.what()).find("never-echo-private") == std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed ControlError"); return; }
    FAIL_CHECK("control codec accepted rejected input");
}

struct Fixture {
    Message message;
    std::vector<std::uint8_t> little;
    std::vector<std::uint8_t> big;
};

// Literal archives, not packets or session envelopes. No writer supplies the
// expected values. Each u32 is exactly4bytes in the explicitly chosen endian.
const std::array<Fixture, 6> fixtures{{
    {Initial1a{0x12345678u}, {0x1a,0x78,0x56,0x34,0x12}, {0x1a,0x12,0x34,0x56,0x78}},
    {Challenge1b{0x89abcdefu}, {0x1b,0xef,0xcd,0xab,0x89}, {0x1b,0x89,0xab,0xcd,0xef}},
    {Response1c{0xf6823fe2u}, {0x1c,0xe2,0x3f,0x82,0xf6}, {0x1c,0xf6,0x82,0x3f,0xe2}},
    {Ready1d{}, {0x1d}, {0x1d}},
    {Hello{std::bit_cast<std::int32_t>(0x80000000u),0x7fffffff,0xfedcba98u},
        {0x00,0x00,0x00,0x00,0x80,0xff,0xff,0xff,0x7f,0x98,0xba,0xdc,0xfe},
        {0x00,0x80,0x00,0x00,0x00,0x7f,0xff,0xff,0xff,0xfe,0xdc,0xba,0x98}},
    {Upgrade{-1,0},
        {0x02,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00},
        {0x02,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00}}
}};
}

// Public synthetic CRC vectors: independently calculated by arbitrary-precision
// polynomial division, not by the native table or the C++ codec under test.
// Evidence implementation/evidence/control-wire-red/crc-vectors.json contains
// full UTF16LE input bytes; no trailing NUL. Public model/check reference:
// https://pycrc.org/models.html#crc-32-bzip2 ("123456789" -> fc891918).
// CRC input = u"895fcf626f55798667e4e94cb7a636af " + signed32 decimal.
TEST_CASE("Control CRC formats zero as signed decimal UTF16LE") {
    CHECK(response_crc(0) == 0x8d3f36c3u);
}
TEST_CASE("Control CRC formats maximum signed challenge without reflection") {
    CHECK(response_crc(0x7fffffffu) == 0x8b9008b6u);
}
TEST_CASE("Control CRC formats highbit challenge as negative INT32_MIN") {
    CHECK(response_crc(0x80000000u) == 0xffaf2b7bu);
}
TEST_CASE("Control CRC formats allones challenge as minus one") {
    CHECK(response_crc(0xffffffffu) == 0xf6823fe2u);
}

TEST_CASE("Control encode matches little endian scalar and empty ready literals") {
    for (std::size_t i=0; i<4; ++i) {
        CAPTURE(i);
        CHECK(encode(fixtures[i].message, Endian::little) == fixtures[i].little);
    }
}
TEST_CASE("Control encode matches opposite endian scalar literals") {
    for (std::size_t i=0; i<4; ++i) {
        CAPTURE(i);
        CHECK(encode(fixtures[i].message, Endian::big) == fixtures[i].big);
    }
}
TEST_CASE("Control decode reads literal scalar archives in explicit endians") {
    for (std::size_t i=0; i<4; ++i) {
        CAPTURE(i);
        CHECK(decode(fixtures[i].little, Endian::little) == fixtures[i].message);
        CHECK(decode(fixtures[i].big, Endian::big) == fixtures[i].message);
    }
}
TEST_CASE("Control Hello encoding preserves signed version bits and unsigned caps") {
    CHECK(encode(fixtures[4].message, Endian::little) == fixtures[4].little);
    CHECK(encode(fixtures[4].message, Endian::big) == fixtures[4].big);
}
TEST_CASE("Control Hello decoding preserves signed version bits and unsigned caps") {
    CHECK(decode(fixtures[4].little, Endian::little) == fixtures[4].message);
    CHECK(decode(fixtures[4].big, Endian::big) == fixtures[4].message);
}
TEST_CASE("Control Upgrade encodes and decodes allones negative version") {
    CHECK(encode(fixtures[5].message, Endian::little) == fixtures[5].little);
    CHECK(encode(fixtures[5].message, Endian::big) == fixtures[5].big);
    CHECK(decode(fixtures[5].little, Endian::little) == fixtures[5].message);
    CHECK(decode(fixtures[5].big, Endian::big) == fixtures[5].message);
}
TEST_CASE("Control Upgrade endian changes individual version words") {
    const Message value=Upgrade{0x12345678,std::bit_cast<std::int32_t>(0x80000001u)};
    const std::vector<std::uint8_t> little{2,0x78,0x56,0x34,0x12,1,0,0,0x80};
    const std::vector<std::uint8_t> big{2,0x12,0x34,0x56,0x78,0x80,0,0,1};
    CHECK(encode(value, Endian::little) == little);
    CHECK(encode(value, Endian::big) == big);
    CHECK(decode(little, Endian::little) == value);
    CHECK(decode(big, Endian::big) == value);
}
TEST_CASE("Control codec does not invent version admission policy") {
    // min>max and capsallones remain representable; interval validation belongs
    // to the caller, not this physical logical-archive codec.
    const Message hello=Hello{1,-1,0xffffffffu};
    const std::vector<std::uint8_t> bytes{0,1,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    CHECK(encode(hello, Endian::little) == bytes);
    CHECK(decode(bytes, Endian::little) == hello);
}
TEST_CASE("Control zero scalar is data and ready has no hidden field") {
    const Message initial=Initial1a{0};
    const std::vector<std::uint8_t> zero{0x1a,0,0,0,0};
    CHECK(encode(initial, Endian::little) == zero);
    CHECK(decode(zero, Endian::little) == initial);
    CHECK(encode(Ready1d{}, Endian::big) == std::vector<std::uint8_t>{0x1d});
    CHECK(decode(std::array<std::uint8_t,1>{0x1d}, Endian::little) == Message{Ready1d{}});
}
TEST_CASE("Control opposite endian is never selected implicitly") {
    const std::array<std::uint8_t,5> bytes{0x1b,0x12,0x34,0x56,0x78};
    CHECK(decode(bytes, Endian::big) == Message{Challenge1b{0x12345678u}});
    CHECK(decode(bytes, Endian::little) == Message{Challenge1b{0x78563412u}});
}
TEST_CASE("Control rejects empty logical archive") {
    expect_error([] { decode({}, Endian::little); }, ControlErrorCode::invalid_size);
}
TEST_CASE("Control rejects every truncated message instead of parsing a prefix") {
    for (const auto& fixture: fixtures) {
        for (std::size_t size=1; size<fixture.little.size(); ++size) {
            CAPTURE(size);
            expect_error([&] { decode(std::span(fixture.little).first(size), Endian::little); }, ControlErrorCode::invalid_size);
            expect_error([&] { decode(std::span(fixture.big).first(size), Endian::big); }, ControlErrorCode::invalid_size);
        }
    }
}
TEST_CASE("Control rejects trailing bytes and concatenated logical messages") {
    for (const auto& fixture: fixtures) {
        auto bytes=fixture.little;
        bytes.push_back(0);
        expect_error([&] { decode(bytes, Endian::little); }, ControlErrorCode::invalid_size);
        bytes=fixture.big;
        bytes.insert(bytes.end(), fixture.big.begin(), fixture.big.end());
        expect_error([&] { decode(bytes, Endian::big); }, ControlErrorCode::invalid_size);
    }
}
TEST_CASE("Control rejects opcodes outside its recovered narrow scope") {
    for (const auto opcode: {0x01u,0x03u,0x04u,0x05u,0x06u,0x07u,0x19u,0x1eu,0xffu}) {
        const std::array<std::uint8_t,1> bytes{static_cast<std::uint8_t>(opcode)};
        expect_error([&] { decode(bytes, Endian::little); }, ControlErrorCode::unsupported_opcode);
    }
}
TEST_CASE("Control rejects invalid endian in both directions") {
    const auto invalid=static_cast<Endian>(73);
    expect_error([&] { encode(Ready1d{}, invalid); }, ControlErrorCode::invalid_endian);
    expect_error([&] { decode(fixtures[0].little, invalid); }, ControlErrorCode::invalid_endian);
}
TEST_CASE("Control errors never echo logical payload content") {
    const std::string secret="never-echo-private";
    std::vector<std::uint8_t> bytes{0xff};
    bytes.insert(bytes.end(), secret.begin(), secret.end());
    expect_error([&] { decode(bytes, Endian::little); }, ControlErrorCode::unsupported_opcode);
    bytes[0]=0x1d;
    expect_error([&] { decode(bytes, Endian::little); }, ControlErrorCode::invalid_size);
}
TEST_CASE("Control accepts archive extracted from unaligned LSB payload without adding packet framing") {
    // 3prefixbits(101), then literal1b ef cd ab89 (40bits), no packetId/marker.
    // Manual shift across byte boundaries: dd78 6f5e4d04, length43bits.
    // Existing BitReader owns bitorder/extraction; control codec sees5bytes.
    const std::array<std::uint8_t,6> raw{0xdd,0x78,0x6f,0x5e,0x4d,0x04};
    hawken::wire::BitReader reader(raw,43);
    CHECK(reader.read_bit());
    CHECK_FALSE(reader.read_bit());
    CHECK(reader.read_bit());
    const auto extracted=reader.read_bits(40);
    REQUIRE(extracted == fixtures[1].little);
    CHECK(reader.at_end());
    CHECK(decode(extracted, Endian::little) == fixtures[1].message);
    CHECK(encode(fixtures[1].message, Endian::little) == fixtures[1].little);
}
