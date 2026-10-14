#include "hawken/spawn_validation_rpc_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::rpc;
using Bytes = std::vector<std::uint8_t>;
template<class Operation>
void expect_code(const char* code, Operation operation) {
    bool caught = false;
    try { operation(); }
    catch (const WireError& error) {
        caught = true;
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
    }
    CHECK(caught);
}
}

TEST_CASE("Spawn validation zero-argument RPCs contain only the bounded field tag") {
    const auto request = encode_spawn_validated(249, 294, 8);
    CHECK(request.payload == Bytes{0xf9});
    CHECK(request.logical_bits == 8);
    const auto success = encode_spawn_validated(250, 294, 8);
    CHECK(success.payload == Bytes{0xfa});
    CHECK(success.logical_bits == 8);
    const auto empty_tag = encode_spawn_validated(0, 1, 1);
    CHECK(empty_tag.payload.empty());
    CHECK(empty_tag.logical_bits == 0);
    // Two independently packed zero-arg records need no separator/alignment.
    const Bytes coalesced{0xf9, 0xfa};
    BitReader reader(coalesced, 16);
    CHECK(reader.read_bounded(294) == 249);
    CHECK(reader.position() == 8);
    CHECK(reader.read_bounded(294) == 250);
}

TEST_CASE("Spawn validation failure reproduces all sixteen independent nibble literals") {
    constexpr std::array<std::uint8_t, 16> second{
        0x00, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f,
        0x11, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f};
    for (std::size_t reason = 0; reason < second.size(); ++reason) {
        CAPTURE(reason);
        const std::size_t bits = reason == 0 ? 9 : 13;
        const auto encoded = encode_spawn_failed(static_cast<std::uint8_t>(reason), 251, 294, bits);
        CHECK(encoded.payload == Bytes{0xfb, second[reason]});
        CHECK(encoded.logical_bits == bits);
        const Bytes literal{0xfb, second[reason]};
        BitReader reader(literal, bits);
        REQUIRE(reader.read_bounded(294) == 251);
        const auto decoded = read_spawn_failed_arguments(reader, bits - 8);
        CHECK(decoded.reason == static_cast<std::uint8_t>(reason));
        CHECK(decoded.consumed_bits == bits - 8);
        CHECK(reader.position() == bits);
    }
}

TEST_CASE("Spawn failure arguments accept native absent and present zero without an enum range invention") {
    const Bytes absent{0x00};
    BitReader zero(absent, 1);
    CHECK(read_spawn_failed_arguments(zero, 1).reason == 0);
    CHECK(zero.position() == 1);
    const Bytes present_zero{0x01};
    BitReader noncanonical(present_zero, 5);
    const auto decoded = read_spawn_failed_arguments(noncanonical, 5);
    CHECK(decoded.reason == 0);
    CHECK(decoded.consumed_bits == 5);
    const Bytes high_nibble{0x1f};
    BitReader high(high_nibble, 5);
    CHECK(read_spawn_failed_arguments(high, 5).reason == 15);
}

TEST_CASE("Spawn failure arguments preserve all initial bit offsets and opaque suffixes") {
    for (std::size_t offset = 0; offset < 8; ++offset) {
        CAPTURE(offset);
        // Known five-bit literal0f = presence1/reason7, then four-bit suffixa.
        const auto word = (0x55U & ((1U << offset) - 1U)) |
            (0x0fU << offset) | (0x0aU << (offset + 5));
        const Bytes bytes{static_cast<std::uint8_t>(word),
            static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word >> 16)};
        BitReader reader(bytes, offset + 9);
        for (std::size_t bit = 0; bit < offset; ++bit) reader.read_bit();
        const auto decoded = read_spawn_failed_arguments(reader, 5);
        CHECK(decoded.reason == 7);
        CHECK(decoded.consumed_bits == 5);
        CHECK(reader.position() == offset + 5);
        CHECK(reader.read_bits(4) == Bytes{0x0a});
    }
}

TEST_CASE("Spawn validation RPC encoders validate fields limits and actual non-power-of-two tag width") {
    expect_code("invalid_limit", [] { encode_spawn_validated(0, 0, 1); });
    expect_code("invalid_limit", [] { encode_spawn_validated(0, 1, 0); });
    expect_code("invalid_value", [] { encode_spawn_validated(294, 294, 20); });
    expect_code("invalid_limit", [] { encode_spawn_failed(0, 0, 0, 1); });
    expect_code("invalid_limit", [] { encode_spawn_failed(0, 0, 1, 0); });
    expect_code("invalid_value", [] { encode_spawn_failed(1, 294, 294, 20); });
    for (unsigned reason = 16; reason <= 255; ++reason)
        expect_code("invalid_value", [reason] { encode_spawn_failed(static_cast<std::uint8_t>(reason), 251, 294, 13); });
    const auto short_tag = encode_spawn_validated(2, 10, 3);
    CHECK(short_tag.payload == Bytes{0x02});
    CHECK(short_tag.logical_bits == 3);
    const auto long_tag = encode_spawn_validated(9, 10, 4);
    CHECK(long_tag.payload == Bytes{0x09});
    CHECK(long_tag.logical_bits == 4);
    expect_code("field_too_large", [] { encode_spawn_validated(9, 10, 3); });
}

TEST_CASE("Spawn validation RPC encoders enforce exact budgets without padding or truncation") {
    CHECK(encode_spawn_validated(250, 294, 8).logical_bits == 8);
    expect_code("field_too_large", [] { encode_spawn_validated(250, 294, 7); });
    CHECK(encode_spawn_failed(0, 251, 294, 9).logical_bits == 9);
    expect_code("field_too_large", [] { encode_spawn_failed(0, 251, 294, 8); });
    CHECK(encode_spawn_failed(7, 251, 294, 13).logical_bits == 13);
    expect_code("field_too_large", [] { encode_spawn_failed(7, 251, 294, 12); });
    const auto zero_field = encode_spawn_failed(15, 0, 1, 5);
    CHECK(zero_field.payload == Bytes{0x1f});
    CHECK(zero_field.logical_bits == 5);
    expect_code("field_too_large", [] { encode_spawn_failed(15, 0, 1, 4); });
}

TEST_CASE("Spawn failure argument rejection rolls back every truncated or over-budget cursor") {
    const Bytes literal{0xfb, 0x1f};
    for (std::size_t cut = 0; cut < 5; ++cut) {
        CAPTURE(cut);
        BitReader reader(literal, 8 + cut);
        REQUIRE(reader.read_bounded(294) == 251);
        expect_code("truncated", [&] { read_spawn_failed_arguments(reader, 5); });
        CHECK(reader.position() == 8);
        if (cut > 0) CHECK(reader.read_bit());
    }
    for (std::size_t budget = 0; budget < 5; ++budget) {
        CAPTURE(budget);
        BitReader reader(literal, 13);
        REQUIRE(reader.read_bounded(294) == 251);
        expect_code(budget == 0 ? "invalid_limit" : "field_too_large",
            [&] { read_spawn_failed_arguments(reader, budget); });
        CHECK(reader.position() == 8);
        CHECK(reader.read_bit());
    }
}
