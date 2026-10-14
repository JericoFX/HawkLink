#include "hawken/unique_id_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::identity;
using hawken::control::Endian;
using Bytes = std::vector<std::uint8_t>;
const UniqueId distinguishing{{0x10213243U, 0x54657687U, 0x98a9bacbU, 0xdcedfe0fU}};
const Bytes little_value{0x43,0x32,0x21,0x10,0x87,0x76,0x65,0x54,
    0xcb,0xba,0xa9,0x98,0x0f,0xfe,0xed,0xdc};
const Bytes big_value{0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87,
    0x98,0xa9,0xba,0xcb,0xdc,0xed,0xfe,0x0f};

template<class Operation>
void expect_code(const char* expected, Operation operation) {
    bool caught = false;
    try { operation(); }
    catch (const WireError& error) {
        caught = true;
        CHECK(error.code == expected);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
    }
    CHECK(caught);
}

// Test input placement only; expected words/tagged bytes are independent literals.
Bytes shifted_with_suffix(const Bytes& value, std::size_t offset) {
    Bytes result((offset + 128 + 4 + 7) / 8, 0);
    for (std::size_t i = 0; i < offset; ++i)
        result[i / 8] |= static_cast<std::uint8_t>(1U << (i % 8));
    for (std::size_t i = 0; i < 128; ++i)
        if ((value[i / 8] & (1U << (i % 8))) != 0)
            result[(offset+i) / 8] |= static_cast<std::uint8_t>(1U << ((offset+i) % 8));
    // The opaque suffix is literal four-bit0xd, LSB first.
    for (std::size_t i = 0; i < 4; ++i)
        if ((0xdU & (1U << i)) != 0)
            result[(offset+128+i) / 8] |= static_cast<std::uint8_t>(1U << ((offset+128+i) % 8));
    return result;
}
}

TEST_CASE("UniqueId property matches independent six-bit tag and four-component goldens") {
    // Native12ca0's distinguishing words and3a30's two byte-traversal branches.
    // Field41/bound87 has six bits; the synthetic pattern is not a live identity.
    const Bytes little{0xe9,0x90,0x4c,0x08,0xc4,0xa1,0x5d,0x19,0xd5,
        0xb2,0x6e,0x2a,0xe6,0x83,0x7f,0x3b,0x37};
    const Bytes big{0x29,0x44,0x88,0xcc,0x10,0x55,0x99,0xdd,0x21,
        0x66,0xaa,0xee,0x32,0x77,0xbb,0xff,0x03};
    for (const auto order : {Endian::little, Endian::big}) {
        const auto encoded = encode_unique_id_property(distinguishing, 41, 87, order, 134);
        CHECK(encoded.logical_bits == 134);
        CHECK(encoded.payload == (order == Endian::little ? little : big));
        REQUIRE(encoded.payload.size() == 17);
        CHECK((encoded.payload.back() & 0xc0U) == 0);
    }
}

TEST_CASE("UniqueId property preserves value-dependent bounded tag width") {
    const Bytes last_field{0xd6,0x21,0x99,0x10,0x88,0x43,0xbb,0x32,0xaa,
        0x65,0xdd,0x54,0xcc,0x07,0xff,0x76,0x6e};
    const auto last = encode_unique_id_property(distinguishing, 86, 87, Endian::little, 135);
    CHECK(last.logical_bits == 135);
    CHECK(last.payload == last_field);
    const auto no_tag = encode_unique_id_property(distinguishing, 0, 1, Endian::little, 128);
    CHECK(no_tag.logical_bits == 128);
    CHECK(no_tag.payload == little_value);
    const auto big = encode_unique_id_property(distinguishing, 0, 1, Endian::big,
        std::numeric_limits<std::size_t>::max());
    CHECK(big.logical_bits == 128);
    CHECK(big.payload == big_value);
}

TEST_CASE("UniqueId value reads unaligned raw literals and leaves opaque suffix intact") {
    for (const auto order : {Endian::little, Endian::big}) {
        for (std::size_t offset = 0; offset < 8; ++offset) {
            CAPTURE(offset);
            const auto input = shifted_with_suffix(order == Endian::little ? little_value : big_value, offset);
            BitReader reader(input, offset+132);
            for (std::size_t i = 0; i < offset; ++i) REQUIRE(reader.read_bit());
            const auto decoded = read_unique_id_value(reader, order, 128);
            CHECK(decoded.value.words == distinguishing.words);
            CHECK(decoded.consumed_bits == 128);
            CHECK(reader.position() == offset+128);
            CHECK(reader.read_bit());
            CHECK_FALSE(reader.read_bit());
            CHECK(reader.read_bit());
            CHECK(reader.read_bit());
            CHECK(reader.at_end());
        }
    }
}

TEST_CASE("UniqueId wire preserves zero and full-range unsigned words without presence") {
    const UniqueId limits{{0,1,0x80000000U,0xffffffffU}};
    const Bytes expected_little{0,0,0,0, 1,0,0,0, 0,0,0,0x80, 0xff,0xff,0xff,0xff};
    const Bytes expected_big{0,0,0,0, 0,0,0,1, 0x80,0,0,0, 0xff,0xff,0xff,0xff};
    for (const auto order : {Endian::little, Endian::big}) {
        const auto& bytes = order == Endian::little ? expected_little : expected_big;
        const auto encoded = encode_unique_id_property(limits, 0, 1, order, 128);
        CHECK(encoded.payload == bytes);
        CHECK(encoded.logical_bits == 128);
        BitReader reader(bytes, 128);
        const auto decoded = read_unique_id_value(reader, order, 128);
        CHECK(decoded.value.words == limits.words);
        CHECK(decoded.consumed_bits == 128);
        const Bytes zero(16, 0);
        const auto zero_encoded = encode_unique_id_property(UniqueId{}, 0, 1, order, 128);
        CHECK(zero_encoded.payload == zero);
        CHECK(zero_encoded.logical_bits == 128);
        BitReader zero_reader(zero, 128);
        CHECK(read_unique_id_value(zero_reader, order, 128).value.words == UniqueId{}.words);
    }
}

TEST_CASE("UniqueId property rejects invalid tag endian and insufficient total budgets") {
    expect_code("invalid_limit", [] { encode_unique_id_property(distinguishing, 0, 0, Endian::little, 200); });
    expect_code("invalid_value", [] { encode_unique_id_property(distinguishing, 87, 87, Endian::little, 200); });
    expect_code("invalid_value", [] { encode_unique_id_property(distinguishing, 0, 1, static_cast<Endian>(9), 200); });
    expect_code("invalid_limit", [] { encode_unique_id_property(distinguishing, 41, 87, Endian::little, 0); });
    for (const auto budget : {std::size_t{1}, std::size_t{6}, std::size_t{127}, std::size_t{133}})
        expect_code("field_too_large", [budget] { encode_unique_id_property(distinguishing, 41, 87, Endian::little, budget); });
    expect_code("field_too_large", [] { encode_unique_id_property(distinguishing, 86, 87, Endian::big, 134); });
}

TEST_CASE("UniqueId value truncation rolls back cursor and does not poison caller reader") {
    const auto input = shifted_with_suffix(little_value, 3);
    for (const auto available : {std::size_t{0},std::size_t{1},std::size_t{31},std::size_t{32},
        std::size_t{63},std::size_t{64},std::size_t{96},std::size_t{127}}) {
        CAPTURE(available);
        BitReader reader(input, 3+available);
        for (int i = 0; i < 3; ++i) REQUIRE(reader.read_bit());
        expect_code("truncated", [&] { read_unique_id_value(reader, Endian::little, 128); });
        CHECK(reader.position() == 3);
        CHECK(reader.at_end() == (available == 0));
        if (available != 0) CHECK(reader.read_bit()); // first value byte43 has low bit1.
    }
}

TEST_CASE("UniqueId value invalid inputs preserve cursor and exact128-bit budget") {
    const auto input = shifted_with_suffix(big_value, 7);
    for (const auto budget : {std::size_t{0},std::size_t{1},std::size_t{127}}) {
        BitReader reader(input, 139);
        for (int i = 0; i < 7; ++i) REQUIRE(reader.read_bit());
        expect_code(budget == 0 ? "invalid_limit" : "field_too_large",
            [&] { read_unique_id_value(reader, Endian::big, budget); });
        CHECK(reader.position() == 7);
        CHECK_FALSE(reader.at_end());
        CHECK_FALSE(reader.read_bit()); // first big-endian byte10 has low bit0.
    }
    BitReader reader(input, 139);
    for (int i = 0; i < 7; ++i) REQUIRE(reader.read_bit());
    expect_code("invalid_value", [&] { read_unique_id_value(reader, static_cast<Endian>(9), 128); });
    CHECK(reader.position() == 7);
    const auto decoded = read_unique_id_value(reader, Endian::big, std::numeric_limits<std::size_t>::max());
    CHECK(decoded.value.words == distinguishing.words);
    CHECK(decoded.consumed_bits == 128);
    CHECK(reader.position() == 135);
    CHECK(reader.read_bit()); // suffix was not consumed.
}
