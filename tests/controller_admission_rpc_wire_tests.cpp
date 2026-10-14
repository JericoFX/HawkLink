#include "hawken/controller_admission_rpc_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using hawken::wire::rpc::read_selection_stage_arguments;
using hawken::control::Endian;
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

TEST_CASE("Selection stage decoder reads independent little and big-endian scalar literals") {
    struct Fixture {
        std::array<std::uint8_t, 5> bytes;
        Endian order;
        std::int32_t stage;
    };
    const Fixture fixtures[]{
        {{0x07, 0x00, 0x00, 0x00, 0x00}, Endian::little, 3},
        {{0x01, 0x00, 0x00, 0x06, 0x00}, Endian::big, 3},
        {{0xff, 0xff, 0xff, 0xff, 0x01}, Endian::little, -1},
        {{0xff, 0xff, 0xff, 0xff, 0x01}, Endian::big, -1},
        {{0x01, 0x00, 0x00, 0x00, 0x01}, Endian::little, std::numeric_limits<std::int32_t>::min()},
        {{0xff, 0xff, 0xff, 0xff, 0x00}, Endian::little, std::numeric_limits<std::int32_t>::max()},
        {{0x01, 0x01, 0x00, 0x00, 0x00}, Endian::big, std::numeric_limits<std::int32_t>::min()},
        {{0xff, 0xfe, 0xff, 0xff, 0x01}, Endian::big, std::numeric_limits<std::int32_t>::max()},
        {{0x07, 0x02, 0x00, 0x00, 0x00}, Endian::little, 259}
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.stage);
        BitReader reader(fixture.bytes, 33);
        const auto decoded = read_selection_stage_arguments(reader, fixture.order, 33);
        CHECK(decoded.stage == fixture.stage);
        CHECK(decoded.consumed_bits == 33);
        CHECK(reader.position() == 33);
    }
}

TEST_CASE("Selection stage decoder defaults absent storage and accepts present zero") {
    const Bytes absent_with_suffix{0x16}; // absent0, then opaque four-bit b.
    BitReader absent(absent_with_suffix, 5);
    const auto zero = read_selection_stage_arguments(absent, Endian::little, 1);
    CHECK(zero.stage == 0);
    CHECK(zero.consumed_bits == 1);
    CHECK(absent.read_bits(4) == Bytes{0x0b});
    const Bytes present_zero{0x01, 0, 0, 0, 0};
    BitReader present(present_zero, 33);
    const auto decoded = read_selection_stage_arguments(present, Endian::big, 33);
    CHECK(decoded.stage == 0);
    CHECK(decoded.consumed_bits == 33);
}

TEST_CASE("Selection stage decoder starts after the conditional tag rather than reading another field") {
    const Bytes little{0x19, 0x0f, 0, 0, 0, 0};
    BitReader le(little, 42);
    REQUIRE(le.read_bounded(294) == 281);
    REQUIRE(le.position() == 9);
    CHECK(read_selection_stage_arguments(le, Endian::little, 33).stage == 3);
    CHECK(le.position() == 42);
    const Bytes big{0x19, 0x03, 0, 0, 0x0c, 0};
    BitReader be(big, 42);
    REQUIRE(be.read_bounded(294) == 281);
    CHECK(read_selection_stage_arguments(be, Endian::big, 33).stage == 3);
}

TEST_CASE("Selection stage decoder retains every initial bit offset and suffix") {
    for (const auto order : std::array<Endian, 2>{Endian::little, Endian::big}) {
        const std::uint64_t literal = order == Endian::little ? 0x07ULL : 0x06000001ULL;
        for (std::size_t offset = 0; offset < 8; ++offset) {
            CAPTURE(offset);
            const auto word = (0x55ULL & ((1ULL << offset) - 1ULL)) |
                (literal << offset) | (0x0aULL << (offset + 33));
            std::array<std::uint8_t, 6> bytes{};
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(word >> (8 * i));
            BitReader reader(bytes, offset + 37);
            for (std::size_t i = 0; i < offset; ++i) reader.read_bit();
            const auto decoded = read_selection_stage_arguments(reader, order, 33);
            CHECK(decoded.stage == 3);
            CHECK(decoded.consumed_bits == 33);
            CHECK(reader.position() == offset + 33);
            CHECK(reader.read_bits(4) == Bytes{0x0a});
        }
    }
}

TEST_CASE("Selection stage decoder rolls back all truncated present scalar prefixes") {
    const Bytes literal{0x19, 0x0f, 0, 0, 0, 0};
    for (std::size_t cut = 0; cut < 33; ++cut) {
        CAPTURE(cut);
        BitReader reader(literal, 9 + cut);
        REQUIRE(reader.read_bounded(294) == 281);
        expect_code("truncated", [&] { read_selection_stage_arguments(reader, Endian::little, 33); });
        CHECK(reader.position() == 9);
        if (cut > 0) CHECK(reader.read_bit());
    }
}

TEST_CASE("Selection stage decoder rolls back budget and integer-order rejections") {
    const Bytes literal{0x19, 0x0f, 0, 0, 0, 0};
    for (std::size_t budget = 0; budget < 33; ++budget) {
        CAPTURE(budget);
        BitReader reader(literal, 42);
        REQUIRE(reader.read_bounded(294) == 281);
        expect_code(budget == 0 ? "invalid_limit" : "field_too_large",
            [&] { read_selection_stage_arguments(reader, Endian::little, budget); });
        CHECK(reader.position() == 9);
        CHECK(reader.read_bit());
    }
    BitReader invalid_order(literal, 42);
    REQUIRE(invalid_order.read_bounded(294) == 281);
    expect_code("invalid_value", [&] {
        read_selection_stage_arguments(invalid_order, static_cast<Endian>(9), 33);
    });
    CHECK(invalid_order.position() == 9);
}
