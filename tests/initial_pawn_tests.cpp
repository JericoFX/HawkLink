#include "hawken/initial_pawn.hpp"
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
    FAIL_CHECK("initial pawn accepted rejected input");
}
void literal(const InitialPawn& value,
             const std::vector<std::uint8_t>& bytes, std::size_t bits) {
    const auto result=encode_initial_pawn(value,bits);
    CHECK(result.payload == bytes);
    CHECK(result.payload_bits == bits);
    CHECK(result.payload.size() == (bits+7)/8);
    BitReader reader(result.payload,result.payload_bits);
    const auto reference=object::read_reference(reader,32);
    CHECK(std::get<object::PackageExportReference>(reference.value) == value.archetype);
    CHECK(read_position(reader,bits).value == value.position);
    if (value.rotation) CHECK(read_rotation(reader,bits).value == *value.rotation);
    CHECK(reader.at_end());
}
}

// Independent LSB literals: reference selector0/index31, position field,
// optional rotation components. There is no controller byte or tail sentinel.
TEST_CASE("Initial pawn zero prefix ends at 43 logical bits") {
    literal({{0},{0,0,0},std::nullopt},
            {0x00,0x00,0x00,0x00,0x40,0x05},43);
}
TEST_CASE("Initial pawn optional zero rotation contributes three bits") {
    literal({{0},{0,0,0},Rotation{0,0,0}},
            {0x00,0x00,0x00,0x00,0x40,0x05},46);
}
TEST_CASE("Initial pawn signed position follows primary reference without alignment") {
    // Index2 is a synthetic coordinate, not an original CDO or runtime mapping.
    literal({{2},{1,-1,0},std::nullopt},
            {0x04,0x00,0x00,0x00,0xe0,0x04},43);
}
TEST_CASE("Initial pawn nonzero rotation preserves the logical cursor") {
    // Reference32 + position20 + rotation19 = 71 logical bits.
    literal({{2},{8,-8,0},Rotation{256,0,65280}},
            {0x04,0x00,0x00,0x00,0x03,0x23,0x38,0xc0,0x7f},71);
}
TEST_CASE("Initial pawn accepts the full primary reference range") {
    literal({{0x7fffffffU},{0,0,0},std::nullopt},
            {0xfe,0xff,0xff,0xff,0x40,0x05},43);
}
TEST_CASE("Initial pawn rejects invalid reference position and rotation") {
    const InitialPawn base{{0},{0,0,0},std::nullopt};
    auto invalid=base;
    invalid.archetype.index=0x80000000U;
    expect_code([&]{ (void)encode_initial_pawn(invalid,512); },"invalid_value");
    for (std::size_t component=0;component<3;++component) {
        for (const auto coordinate:{1048576,-1048577}) {
            invalid=base; invalid.position[component]=coordinate;
            expect_code([&]{ (void)encode_initial_pawn(invalid,512); },"invalid_value");
        }
        invalid=base; invalid.rotation=Rotation{0,0,0};
        (*invalid.rotation)[component]=1;
        expect_code([&]{ (void)encode_initial_pawn(invalid,512); },"invalid_value");
    }
}
TEST_CASE("Initial pawn requires positive complete payload budget") {
    const InitialPawn zero{{0},{0,0,0},std::nullopt};
    expect_code([&]{ (void)encode_initial_pawn(zero,0); },"invalid_limit");
    for (const auto cap:{std::size_t{1},std::size_t{31},std::size_t{32},std::size_t{42}})
        expect_code([&]{ (void)encode_initial_pawn(zero,cap); },"field_too_large");
    CHECK(encode_initial_pawn(zero,43).payload_bits == 43);
    const InitialPawn rotated_zero{{0},{0,0,0},Rotation{0,0,0}};
    expect_code([&]{ (void)encode_initial_pawn(rotated_zero,45); },"field_too_large");
    CHECK(encode_initial_pawn(rotated_zero,46).payload_bits == 46);
    const InitialPawn rotated{{2},{8,-8,0},Rotation{256,0,65280}};
    expect_code([&]{ (void)encode_initial_pawn(rotated,70); },"field_too_large");
    CHECK(encode_initial_pawn(rotated,71).payload_bits == 71);
}
TEST_CASE("Initial pawn larger budget never pads tail or mutates input") {
    const InitialPawn value{{2},{8,-8,0},Rotation{256,0,65280}};
    const auto result=encode_initial_pawn(value,std::numeric_limits<std::size_t>::max());
    CHECK(result.payload == std::vector<std::uint8_t>{
        0x04,0x00,0x00,0x00,0x03,0x23,0x38,0xc0,0x7f});
    CHECK(result.payload_bits == 71);
    CHECK((result.payload.back() & 0x80U) == 0);
    CHECK(value.archetype.index == 2);
    CHECK(value.position == QuantizedPosition{8,-8,0});
    CHECK(value.rotation == std::optional<Rotation>{Rotation{256,0,65280}});
}
