#include "hawken/mech_setup_wire.hpp"
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <ostream>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::mech;
using hawken::control::Endian;
using object::PackageExportReference;

SMechSetupStruct patterned_setup() {
    // Caller-chosen synthetic data, not an original/persisted preset.
    SMechSetupStruct value{};
    value.allocated_optimizations={0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                                 0x18,0x19,0x1a,0x1b,0x1c,0x1d};
    value.mech_appearance={0x21,0x22,0x23,
        {0x31,0x32,0x33,true},{0x41,0x42,0x43,false},
        {0x51,0x52,0x53,true},{0x61,0x62,0x63,false},
        0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
        {0x81,0x82,0x83},0x91,0x92,0x93,0x94,0x95,true};
    value.mech_internals={0xa1,0xa2,0xa3,0xa4,0xa5,0xa6};
    value.mech_weapons={0xb1,0xb2,0xb3,0xb4};
    value.ability_index=0x2d; // 45 is wire-representable; no invented <=35 guard.
    value.ability_upgrade_flags=0xc1;
    value.mech_type=0x12345678;
    value.mech_sub_type=-1985229329; // signed 0x89abcdef.
    value.b_ready_for_setup=true;
    value.setup_id=0xd1;
    return value;
}

template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("Mech setup encoder accepted rejected input");
}
void literal(const SMechSetupStruct& value, Endian endian,
             const std::vector<std::uint8_t>& expected, std::size_t bits) {
    const auto encoded=encode_mech_setup(value,endian,bits);
    CHECK(encoded.logical_bits == bits);
    CHECK(encoded.payload == expected);
}
}

// Independent literal concatenation of original Children/Next schema. The
// fixture arithmetic is retained in evidence/pri-mech-setup/codec-red; it uses
// integer low-bit concatenation, not BitWriter or the encoder under test.
// 57 raw bytes, six bool bits, two int32, AbilityIndex6 =>532 bits before
// Template. Null adds12, mapped package adds32. No IDs/presence/alignment.
TEST_CASE("Mech setup full nonzero null golden uses little endian integers") {
    literal(patterned_setup(),Endian::little,{
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x21,0x22,0x23,0x31,0x32,0x33,0x83,0x84,0x86,0x44,0x49,0x4d,0x0d,
        0x13,0x1b,0x13,0x27,0x37,0x47,0x57,0x67,0x77,0x87,0x97,0x17,0x28,
        0x38,0x18,0x29,0x39,0x49,0x59,0x39,0x54,0x74,0x94,0xb4,0xd4,0x34,
        0x56,0x76,0x96,0xb6,0x0d,0xc6,0xb3,0xa2,0x91,0x78,0x6f,0x5e,0x4d,
        0x1c,0x1d,0x00},544);
}
TEST_CASE("Mech setup full nonzero null golden uses big endian integers") {
    literal(patterned_setup(),Endian::big,{
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x21,0x22,0x23,0x31,0x32,0x33,0x83,0x84,0x86,0x44,0x49,0x4d,0x0d,
        0x13,0x1b,0x13,0x27,0x37,0x47,0x57,0x67,0x77,0x87,0x97,0x17,0x28,
        0x38,0x18,0x29,0x39,0x49,0x59,0x39,0x54,0x74,0x94,0xb4,0xd4,0x34,
        0x56,0x76,0x96,0xb6,0x0d,0x96,0xa0,0xb1,0xc2,0x4b,0x5c,0x6d,0x7e,
        0x1f,0x1d,0x00},544);
}
TEST_CASE("Mech setup full nonzero mapped Template golden little endian") {
    auto value=patterned_setup(); value.template_reference=PackageExportReference{0x12345678};
    literal(value,Endian::little,{
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x21,0x22,0x23,0x31,0x32,0x33,0x83,0x84,0x86,0x44,0x49,0x4d,0x0d,
        0x13,0x1b,0x13,0x27,0x37,0x47,0x57,0x67,0x77,0x87,0x97,0x17,0x28,
        0x38,0x18,0x29,0x39,0x49,0x59,0x39,0x54,0x74,0x94,0xb4,0xd4,0x34,
        0x56,0x76,0x96,0xb6,0x0d,0xc6,0xb3,0xa2,0x91,0x78,0x6f,0x5e,0x4d,
        0x1c,0x0d,0xcf,0x8a,0x46,0x02},564);
}
TEST_CASE("Mech setup full nonzero mapped Template golden big endian") {
    auto value=patterned_setup(); value.template_reference=PackageExportReference{0x12345678};
    literal(value,Endian::big,{
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x21,0x22,0x23,0x31,0x32,0x33,0x83,0x84,0x86,0x44,0x49,0x4d,0x0d,
        0x13,0x1b,0x13,0x27,0x37,0x47,0x57,0x67,0x77,0x87,0x97,0x17,0x28,
        0x38,0x18,0x29,0x39,0x49,0x59,0x39,0x54,0x74,0x94,0xb4,0xd4,0x34,
        0x56,0x76,0x96,0xb6,0x0d,0x96,0xa0,0xb1,0xc2,0x4b,0x5c,0x6d,0x7e,
        0x1f,0x0d,0xcf,0x8a,0x46,0x02},564);
}
TEST_CASE("Mech setup bools contribute exactly six unaligned bits") {
    SMechSetupStruct value{};
    auto& appearance=value.mech_appearance;
    appearance.upper.b_accent=true; appearance.middle.b_accent=true;
    appearance.lower.b_accent=true; appearance.arm.b_accent=true;
    appearance.b_tint_reticle=true; value.b_ready_for_setup=true;
    // Bits152,177,202,227,364,523; null selector at532.
    std::vector<std::uint8_t> expected(68,0);
    expected[19]=1; expected[22]=2; expected[25]=4; expected[28]=8;
    expected[45]=0x10; expected[65]=8; expected[66]=0x10;
    literal(value,Endian::little,expected,544);
}
TEST_CASE("Mech setup cockpit array emits three bytes without index or alignment") {
    SMechSetupStruct value{}; value.mech_appearance.cockpit_decoration={0x12,0x34,0x56};
    // Three byte values begin at bits300,308,316 (current cursor mod8=4).
    std::vector<std::uint8_t> expected(68,0);
    expected[37]=0x20; expected[38]=0x41; expected[39]=0x63; expected[40]=5;
    expected[66]=0x10;
    literal(value,Endian::little,expected,544);
}
TEST_CASE("Mech setup ability63 is valid six bit wire input") {
    SMechSetupStruct value{}; value.ability_index=63;
    std::vector<std::uint8_t> expected(68,0);
    expected[55]=0xe0; expected[56]=7; expected[66]=0x10;
    literal(value,Endian::little,expected,544);
}
TEST_CASE("Mech setup rejects ability64 rather than truncating") {
    SMechSetupStruct value{}; value.ability_index=64;
    expect_code([&] { (void)encode_mech_setup(value,Endian::little,544); },"invalid_value");
}
TEST_CASE("Mech setup mapped coordinate zero is distinct from null") {
    SMechSetupStruct value{}; value.template_reference=PackageExportReference{0};
    literal(value,Endian::little,std::vector<std::uint8_t>(71,0),564);
    value.template_reference.reset();
    std::vector<std::uint8_t> null(68,0); null[66]=0x10;
    literal(value,Endian::little,null,544);
}
TEST_CASE("Mech setup accepts31bit coordinate and rejects bit31") {
    SMechSetupStruct value{}; value.template_reference=PackageExportReference{0x7fffffff};
    std::vector<std::uint8_t> expected(71,0);
    expected[66]=0xe0; expected[67]=0xff; expected[68]=0xff;
    expected[69]=0xff; expected[70]=0x0f;
    literal(value,Endian::big,expected,564);
    value.template_reference=PackageExportReference{0x80000000};
    expect_code([&] { (void)encode_mech_setup(value,Endian::little,564); },"invalid_value");
}
TEST_CASE("Mech setup int32 signed boundaries keep all raw bits") {
    SMechSetupStruct value{}; value.mech_type=std::numeric_limits<std::int32_t>::min();
    value.mech_sub_type=std::numeric_limits<std::int32_t>::max();
    std::vector<std::uint8_t> expected(68,0);
    expected[61]=0xfc; expected[62]=0xff; expected[63]=0xff;
    expected[64]=0xff; expected[65]=3; expected[66]=0x10;
    literal(value,Endian::little,expected,544);
}
TEST_CASE("Mech setup invalid endian and zero budget have fixed errors") {
    const SMechSetupStruct value{};
    expect_code([&] { (void)encode_mech_setup(value,static_cast<Endian>(7),544); },"invalid_value");
    expect_code([&] { (void)encode_mech_setup(value,Endian::little,0); },"invalid_limit");
}
TEST_CASE("Mech setup exact and larger budgets produce same complete payload") {
    const auto value=patterned_setup();
    const auto exact=encode_mech_setup(value,Endian::little,544);
    const auto larger=encode_mech_setup(value,Endian::little,std::numeric_limits<std::size_t>::max());
    CHECK(exact.logical_bits == 544);
    CHECK(larger.logical_bits == 544);
    CHECK(larger.payload == exact.payload);
    expect_code([&] { (void)encode_mech_setup(value,Endian::little,543); },"field_too_large");
    auto mapped=value; mapped.template_reference=PackageExportReference{9};
    expect_code([&] { (void)encode_mech_setup(mapped,Endian::big,563); },"field_too_large");
}

TEST_CASE("Mech property tag follows original variable-width field cache literals") {
    const SMechSetupStruct value{};
    for (const auto id : {0u,53u,86u}) {
        const auto encoded=encode_mech_setup_property(value,id,87,Endian::little,551);
        const bool six_bits=id==53;
        std::vector<std::uint8_t> expected(69,0);
        expected[0]=static_cast<std::uint8_t>(id);
        expected[67]=six_bits ? 4 : 8; // Native null: selector1 at 532 + tag width.
        CAPTURE(id);
        CHECK(encoded.logical_bits == (six_bits ? 550 : 551));
        CHECK(encoded.payload == expected); // No alignment, marker or terminator.
    }
}
TEST_CASE("Mech property integer order survives the unaligned field tag") {
    SMechSetupStruct value{}; value.mech_type=0x01020304;
    for (const auto order : {Endian::little,Endian::big}) {
        std::vector<std::uint8_t> expected(69,0); expected[0]=53; expected[67]=4;
        // Original MechType begins at bit459; six-bit tag shifts it to465.
        if (order==Endian::little) { expected[58]=8; expected[59]=6; expected[60]=4; expected[61]=2; }
        else { expected[58]=2; expected[59]=4; expected[60]=6; expected[61]=8; }
        const auto result=encode_mech_setup_property(value,53,87,order,550);
        CHECK(result.logical_bits == 550); CHECK(result.payload == expected);
    }
}
TEST_CASE("Mech property mapped Template literal retains the thirty-two-bit reference") {
    SMechSetupStruct value{}; value.template_reference=PackageExportReference{1};
    std::vector<std::uint8_t> expected(72,0); expected[0]=53; expected[67]=8;
    const auto result=encode_mech_setup_property(value,53,87,Endian::little,570);
    CHECK(result.logical_bits == 570); CHECK(result.payload == expected);
}
TEST_CASE("Mech property budget covers both tag and entire struct") {
    const SMechSetupStruct value{};
    expect_code([&]{(void)encode_mech_setup_property(value,53,87,Endian::little,0);},"invalid_limit");
    expect_code([&]{(void)encode_mech_setup_property(value,53,87,Endian::little,549);},"field_too_large");
    const auto exact=encode_mech_setup_property(value,53,87,Endian::little,550);
    const auto larger=encode_mech_setup_property(value,53,87,Endian::little,std::numeric_limits<std::size_t>::max());
    CHECK(exact.payload == larger.payload); CHECK(larger.logical_bits == 550);
    auto mapped=value; mapped.template_reference=PackageExportReference{1};
    expect_code([&]{(void)encode_mech_setup_property(mapped,53,87,Endian::little,569);},"field_too_large");
}
TEST_CASE("Mech property rejects invalid cache bounds and fields") {
    const SMechSetupStruct value{};
    expect_code([&]{(void)encode_mech_setup_property(value,0,0,Endian::little,550);},"invalid_value");
    expect_code([&]{(void)encode_mech_setup_property(value,87,87,Endian::little,550);},"invalid_value");
}
TEST_CASE("Mech property preserves the scalar codec validation") {
    SMechSetupStruct value{};
    expect_code([&]{(void)encode_mech_setup_property(value,53,87,static_cast<Endian>(7),550);},"invalid_value");
    value.ability_index=64;
    expect_code([&]{(void)encode_mech_setup_property(value,53,87,Endian::little,550);},"invalid_value");
}
