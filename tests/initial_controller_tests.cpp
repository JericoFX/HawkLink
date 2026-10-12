#include "hawken/initial_controller.hpp"
#include <doctest/doctest.h>
#include <limits>
#include <string>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::actor;

template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("initial controller accepted rejected input");
}
void literal(const InitialController& value,
             const std::vector<std::uint8_t>& bytes, std::size_t bits) {
    const auto result=encode_initial_controller(value,bits);
    CHECK(result.payload == bytes);
    CHECK(result.payload_bits == bits);
    CHECK(result.payload.size() == (bits+7)/8);
    // Decode each field independently at the original logical cursor.
    BitReader reader(result.payload,result.payload_bits);
    const auto reference=object::read_reference(reader,32);
    CHECK(std::get<object::PackageExportReference>(reference.value) == value.archetype);
    CHECK(read_position(reader,bits).value == value.position);
    if (value.rotation) CHECK(read_rotation(reader,bits).value == *value.rotation);
    CHECK(reader.read_bits(8) == std::vector<std::uint8_t>{value.net_player_index});
    CHECK(reader.at_end());
}
}

// Independent literal bit concatenation: selector0/index31, known position
// field, optional rotation component bits, raw netbyte. Never product encoders.
TEST_CASE("Initial controller zero prefix ends at 51 logical bits") {
    literal({{0},{0,0,0},std::nullopt,0},
            {0x00,0x00,0x00,0x00,0x40,0x05,0x00},51);
}
TEST_CASE("Initial controller optional zero rotation contributes three bits") {
    literal({{0},{0,0,0},Rotation{0,0,0},127},
            {0x00,0x00,0x00,0x00,0x40,0xc5,0x1f},54);
}
TEST_CASE("Initial controller candidate reference preserves signed position and raw byte") {
    // 56389 is an explicit fixture coordinate, not a proved runtime CDO mapping.
    literal({{56389},{1,-1,0},std::nullopt,35},
            {0x8a,0xb8,0x01,0x00,0xe0,0x1c,0x01},51);
}
TEST_CASE("Initial controller nonzero rotation concatenates without alignment") {
    // Position20 + rotation19 + netbyte8 follow the reference32 exactly.
    literal({{56389},{8,-8,0},Rotation{256,0,65280},2},
            {0x8a,0xb8,0x01,0x00,0x03,0x23,0x38,0xc0,0x7f,0x01},79);
}
TEST_CASE("Initial controller accepts full primary index and netbyte ranges") {
    literal({{0x7fffffffU},{0,0,0},std::nullopt,255},
            {0xfe,0xff,0xff,0xff,0x40,0xfd,0x07},51);
}
TEST_CASE("Initial controller rejects invalid reference position and rotation fields") {
    const InitialController base{{0},{0,0,0},std::nullopt,0};
    auto invalid=base;
    invalid.archetype.index=0x80000000U;
    expect_code([&]{ (void)encode_initial_controller(invalid,512); },"invalid_value");
    for (std::size_t component=0;component<3;++component) {
        for (const auto coordinate:{1048576,-1048577}) {
            invalid=base; invalid.position[component]=coordinate;
            expect_code([&]{ (void)encode_initial_controller(invalid,512); },"invalid_value");
        }
        invalid=base; invalid.rotation=Rotation{0,0,0};
        (*invalid.rotation)[component]=1;
        expect_code([&]{ (void)encode_initial_controller(invalid,512); },"invalid_value");
    }
}
TEST_CASE("Initial controller requires positive complete payload budget") {
    const InitialController zero{{0},{0,0,0},std::nullopt,0};
    expect_code([&]{ (void)encode_initial_controller(zero,0); },"invalid_limit");
    // Reference boundary, position boundary and final-byte boundary: no partial DTO.
    for (const auto cap:{std::size_t{1},std::size_t{31},std::size_t{32},
                        std::size_t{42},std::size_t{43},std::size_t{50}})
        expect_code([&]{ (void)encode_initial_controller(zero,cap); },"field_too_large");
    CHECK(encode_initial_controller(zero,51).payload_bits == 51);
    const InitialController rotated{{56389},{8,-8,0},Rotation{256,0,65280},2};
    expect_code([&]{ (void)encode_initial_controller(rotated,78); },"field_too_large");
    CHECK(encode_initial_controller(rotated,79).payload_bits == 79);
}
TEST_CASE("Initial controller larger budget never pads adds sentinel or mutates input") {
    const InitialController value{{56389},{8,-8,0},Rotation{256,0,65280},2};
    const auto result=encode_initial_controller(value,std::numeric_limits<std::size_t>::max());
    CHECK(result.payload == std::vector<std::uint8_t>{
        0x8a,0xb8,0x01,0x00,0x03,0x23,0x38,0xc0,0x7f,0x01});
    CHECK(result.payload_bits == 79);
    CHECK((result.payload.back() & 0x80U) == 0);
    CHECK(value.archetype.index == 56389);
    CHECK(value.position == QuantizedPosition{8,-8,0});
    CHECK(value.rotation == std::optional<Rotation>{Rotation{256,0,65280}});
    CHECK(value.net_player_index == 2);
}
