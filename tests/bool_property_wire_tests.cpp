#include "hawken/bool_property_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::property;
using Bytes = std::vector<std::uint8_t>;

template<class Operation>
void expect_code(const char* code, Operation operation) {
    bool caught=false;
    try { operation(); }
    catch (const WireError& error) {
        caught=true;
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
    }
    CHECK(caught);
}
void literal(std::span<const BoolProperty> values, std::uint32_t bound,
             const Bytes& bytes, std::size_t bits, std::size_t budget) {
    const auto encoded=encode_bool_properties(values,bound,budget);
    CHECK(encoded.payload == bytes);
    CHECK(encoded.logical_bits == bits);
}
}

TEST_CASE("BoolProperty PRI readiness encodes both values with no presence bit") {
    // bound87/tag30 has6 IDbits: bit64 is omitted as30+64>=87.
    const std::array<BoolProperty,1> yes{{{30,true}}};
    const std::array<BoolProperty,1> no{{{30,false}}};
    literal(yes,87,{0x5e},7,7);
    literal(no,87,{0x1e},7,7);
}

TEST_CASE("BoolProperty GRI flags retain independently derived original field literals") {
    struct Fixture { std::uint32_t field; bool value; std::uint8_t byte; };
    const Fixture fixtures[]{
        {29,true,0x5d},{29,false,0x1d}, // bMatchHasBegun
        {30,true,0x5e},{30,false,0x1e}  // bStopCountDown
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.field);
        CAPTURE(fixture.value);
        const std::array<BoolProperty,1> value{{{fixture.field,fixture.value}}};
        literal(value,73,{fixture.byte},7,7);
    }
}

// SYNTHETIC field IDs48/49 below test only generic bounded-tag arithmetic.
// Original GRI fields48/49 are IntProperty counters, never bool records.
TEST_CASE("BoolProperty synthetic fields retain generic bounded tag arithmetic") {
    struct Fixture { std::uint32_t field; bool value; std::uint8_t byte; };
    const Fixture fixtures[]{
        {48,true,0x70},{48,false,0x30},
        {49,true,0x71},{49,false,0x31}
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.field);
        CAPTURE(fixture.value);
        const std::array<BoolProperty,1> value{{{fixture.field,fixture.value}}};
        literal(value,73,{fixture.byte},7,7);
    }
}

TEST_CASE("BoolProperty synthetic unaligned records have no alignment or terminator") {
    // SYNTHETIC records: 0x70 | (0x31<<7) | (0x5d<<14) =0x1758f0,21bits.
    const std::array<BoolProperty,3> values{{{48,true},{49,false},{29,true}}};
    literal(values,73,{0xf0,0x58,0x17},21,21);
    expect_code("field_too_large",[&] { encode_bool_properties(values,73,20); });
}

TEST_CASE("BoolProperty ordered repeated fields are records rather than deduplicated state") {
    const std::array<BoolProperty,2> values{{{3,true},{3,false}}};
    literal(values,8,{0x3b},8,8);
}

TEST_CASE("BoolProperty field zero and false are real data including bound one") {
    const std::array<BoolProperty,1> no{{{0,false}}};
    const std::array<BoolProperty,1> yes{{{0,true}}};
    literal(no,1,{0},1,1);
    literal(yes,1,{1},1,1);
    literal(no,8,{0},4,4);
    literal(yes,8,{8},4,4);
}

TEST_CASE("BoolProperty nonpower bound uses value dependent widths and exact budgets") {
    const std::array<BoolProperty,1> zero{{{0,true}}};
    const std::array<BoolProperty,1> one{{{1,true}}};
    const std::array<BoolProperty,1> two{{{2,false}}};
    literal(zero,3,{4},3,3);
    literal(one,3,{3},2,2);
    literal(two,3,{2},3,3);
    expect_code("field_too_large",[&] { encode_bool_properties(zero,3,2); });
    expect_code("field_too_large",[&] { encode_bool_properties(one,3,1); });
    expect_code("field_too_large",[&] { encode_bool_properties(two,3,2); });
    const std::array<BoolProperty,1> low{{{0,true}}};
    literal(low,73,{0x80},8,8); // Unlike tag48, tag0 uses7 IDbits.
}

TEST_CASE("BoolProperty maximum uint32 bound does not overflow the last bool bit") {
    const std::array<BoolProperty,1> values{{{0xfffffffeU,true}}};
    literal(values,0xffffffffU,{0xfe,0xff,0xff,0xff,1},33,33);
    expect_code("field_too_large",[&] { encode_bool_properties(values,0xffffffffU,32); });
    literal(values,0xffffffffU,{0xfe,0xff,0xff,0xff,1},33,
        std::numeric_limits<std::size_t>::max());
}

TEST_CASE("BoolProperty empty list produces zero bits but limits remain mandatory") {
    literal({},1,{},0,1);
    expect_code("invalid_limit",[] { encode_bool_properties({},0,1); });
    expect_code("invalid_limit",[] { encode_bool_properties({},1,0); });
}

TEST_CASE("BoolProperty invalid fields and limits are rejected before returning output") {
    const std::array<BoolProperty,1> values{{{0,false}}};
    const std::array<BoolProperty,1> equal{{{8,true}}};
    const std::array<BoolProperty,1> beyond{{{0xffffffffU,false}}};
    expect_code("invalid_limit",[&] { encode_bool_properties(values,0,8); });
    expect_code("invalid_limit",[&] { encode_bool_properties(values,1,0); });
    expect_code("invalid_value",[&] { encode_bool_properties(equal,8,8); });
    expect_code("invalid_value",[&] { encode_bool_properties(beyond,8,64); });
}

TEST_CASE("BoolProperty later invalid record and global budget never return partial output") {
    const std::array<BoolProperty,2> invalid{{{3,true},{8,false}}};
    expect_code("invalid_value",[&] { encode_bool_properties(invalid,8,64); });
    const std::array<BoolProperty,2> valid{{{3,true},{3,false}}};
    expect_code("field_too_large",[&] { encode_bool_properties(valid,8,7); });
    literal(valid,8,{0x3b},8,8);
}
