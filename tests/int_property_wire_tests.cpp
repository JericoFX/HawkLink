#include "hawken/int_property_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::property;
using Endian = hawken::control::Endian;
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

void literal(std::span<const IntProperty> values, std::uint32_t bound,
             Endian order, const Bytes& bytes, std::size_t bits,
             std::size_t budget) {
    const auto encoded = encode_int_properties(values, bound, order, budget);
    CHECK(encoded.payload == bytes);
    CHECK(encoded.logical_bits == bits);
}
}

TEST_CASE("IntProperty original GRI counters use raw32 values in explicit byte order") {
    // Conditional original full GRI bound73: fields48/49 are waiting counters,
    // field51 countdown. Each tag has6 bits: field+64>=73 omits bit64.
    // These are metadata-backed codec fixtures, not observed live field IDs.
    struct Fixture { std::uint32_t field; std::int32_t value; Bytes little; Bytes big; };
    const Fixture fixtures[]{
        {48,0,{0x30,0,0,0,0},{0x30,0,0,0,0}},
        {48,1,{0x70,0,0,0,0},{0x30,0,0,0x40,0}},
        {49,0,{0x31,0,0,0,0},{0x31,0,0,0,0}},
        {49,1,{0x71,0,0,0,0},{0x31,0,0,0x40,0}},
        {51,10,{0xb3,0x02,0,0,0},{0x33,0,0,0x80,0x02}},
        {51,-1,{0xf3,0xff,0xff,0xff,0x3f},{0xf3,0xff,0xff,0xff,0x3f}},
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.field);
        CAPTURE(fixture.value);
        const std::array<IntProperty,1> value{{{fixture.field,fixture.value}}};
        literal(value,73,Endian::little,fixture.little,38,38);
        literal(value,73,Endian::big,fixture.big,38,38);
    }
}

TEST_CASE("IntProperty zero field and zero value are a full scalar rather than absence") {
    const std::array<IntProperty,1> zero{{{0,0}}};
    const std::array<IntProperty,1> one{{{0,1}}};
    literal(zero,1,Endian::little,{0,0,0,0},32,32);
    literal(zero,1,Endian::big,{0,0,0,0},32,32);
    literal(one,1,Endian::little,{1,0,0,0},32,32);
    literal(one,1,Endian::big,{0,0,0,1},32,32);
    expect_code("field_too_large",[&] { encode_int_properties(zero,1,Endian::little,31); });
}

TEST_CASE("IntProperty consecutive GRI fields have no alignment presence or terminator") {
    const std::array<IntProperty,3> values{{{48,0},{49,1},{51,10}}};
    // LE: 0x30 | (0x71<<38) | (0x2b3<<76), 3*(6+32)=114bits.
    literal(values,73,Endian::little,
        {0x30,0,0,0,0x40,0x1c,0,0,0,0x30,0x2b,0,0,0,0},114,114);
    // BE: 0x30 | (0x40000031<<38) | (0x280000033<<76).
    literal(values,73,Endian::big,
        {0x30,0,0,0,0x40,0x0c,0,0,0x10,0x30,0x03,0,0,0x28,0},114,114);
}

TEST_CASE("IntProperty repeated fields preserve record order instead of merging state") {
    const std::array<IntProperty,2> first{{{3,1},{3,0}}};
    const std::array<IntProperty,2> reversed{{{3,0},{3,1}}};
    // Synthetic bound8 gives3tagbits: 0xb | (3<<35), and3 | (0xb<<35).
    literal(first,8,Endian::little,{0x0b,0,0,0,0x18,0,0,0,0},70,70);
    literal(reversed,8,Endian::little,{0x03,0,0,0,0x58,0,0,0,0},70,70);
}

TEST_CASE("IntProperty nonpower bound preserves value dependent field widths") {
    const std::array<IntProperty,1> zero{{{0,1}}};
    const std::array<IntProperty,1> one{{{1,1}}};
    const std::array<IntProperty,1> two{{{2,-1}}};
    // Synthetic bound3: ID0/2 use2bits; ID1 uses1bit since1+2>=3.
    literal(zero,3,Endian::little,{4,0,0,0,0},34,34);
    literal(zero,3,Endian::big,{0,0,0,4,0},34,34);
    literal(one,3,Endian::little,{3,0,0,0,0},33,33);
    literal(one,3,Endian::big,{1,0,0,2,0},33,33);
    literal(two,3,Endian::little,{0xfe,0xff,0xff,0xff,3},34,34);
    literal(two,3,Endian::big,{0xfe,0xff,0xff,0xff,3},34,34);
    const std::array<IntProperty,2> adjacent{{{1,1},{0,2}}};
    // 3 | (8<<33), 33+34=67bits; second scalar starts unaligned.
    literal(adjacent,3,Endian::little,{3,0,0,0,0x10,0,0,0,0},67,67);
    expect_code("field_too_large",[&] { encode_int_properties(zero,3,Endian::little,33); });
    expect_code("field_too_large",[&] { encode_int_properties(one,3,Endian::little,32); });
}

TEST_CASE("IntProperty signed extrema and maximum bounds preserve raw32 storage") {
    const std::array<IntProperty,1> min{{{0,std::numeric_limits<std::int32_t>::min()}}};
    const std::array<IntProperty,1> max{{{0,std::numeric_limits<std::int32_t>::max()}}};
    literal(min,1,Endian::little,{0,0,0,0x80},32,32);
    literal(min,1,Endian::big,{0x80,0,0,0},32,32);
    literal(max,1,Endian::little,{0xff,0xff,0xff,0x7f},32,32);
    literal(max,1,Endian::big,{0x7f,0xff,0xff,0xff},32,32);
    const auto bound = std::numeric_limits<std::uint32_t>::max();
    const std::array<IntProperty,1> full_tag{{{bound-1,std::numeric_limits<std::int32_t>::min()}}};
    literal(full_tag,bound,Endian::little,{0xfe,0xff,0xff,0xff,0,0,0,0x80},64,64);
    literal(full_tag,bound,Endian::big,{0xfe,0xff,0xff,0xff,0x80,0,0,0},64,64);
    const std::array<IntProperty,1> short_tag{{{0x7fffffff,1}}};
    // Low31 bits all1: +0x80000000 equalsbound, so that tagbit is omitted.
    literal(short_tag,bound,Endian::little,{0xff,0xff,0xff,0xff,0,0,0,0},63,63);
    expect_code("field_too_large",[&] { encode_int_properties(full_tag,bound,Endian::little,63); });
}

TEST_CASE("IntProperty empty input still validates bound budget and byte order") {
    literal({},73,Endian::little,{},0,1);
    literal({},73,Endian::big,{},0,std::numeric_limits<std::size_t>::max());
    expect_code("invalid_limit",[] { encode_int_properties({},0,Endian::little,1); });
    expect_code("invalid_limit",[] { encode_int_properties({},73,Endian::little,0); });
    // Match accepted UniqueId codec: unknown Endian is invalid_value.
    expect_code("invalid_value",[] { encode_int_properties({},73,static_cast<Endian>(255),1); });
}

TEST_CASE("IntProperty out of range fields and invalid byte order fail closed") {
    const std::array<IntProperty,1> equal{{{73,0}}};
    const std::array<IntProperty,1> greater{{{74,0}}};
    const std::array<IntProperty,1> max{{{std::numeric_limits<std::uint32_t>::max(),0}}};
    expect_code("invalid_value",[&] { encode_int_properties(equal,73,Endian::little,64); });
    expect_code("invalid_value",[&] { encode_int_properties(greater,73,Endian::little,64); });
    expect_code("invalid_value",[&] { encode_int_properties(max,std::numeric_limits<std::uint32_t>::max(),Endian::little,64); });
    const std::array<IntProperty,1> valid{{{48,1}}};
    expect_code("invalid_limit",[&] { encode_int_properties(valid,0,Endian::little,64); });
    expect_code("invalid_limit",[&] { encode_int_properties(valid,73,Endian::little,0); });
    expect_code("invalid_value",[&] { encode_int_properties(valid,73,static_cast<Endian>(255),64); });
}

TEST_CASE("IntProperty budget applies to the aggregate stream not each record") {
    const std::array<IntProperty,3> values{{{48,0},{49,1},{51,10}}};
    expect_code("field_too_large",[&] { encode_int_properties(values,73,Endian::little,38); });
    expect_code("field_too_large",[&] { encode_int_properties(values,73,Endian::little,113); });
    literal(values,73,Endian::little,
        {0x30,0,0,0,0x40,0x1c,0,0,0,0x30,0x2b,0,0,0,0},114,114);
}
