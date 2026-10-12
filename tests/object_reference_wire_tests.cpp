#include "hawken/object_reference_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::object;
template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("reference accepted rejected input");
}
void literal(const Reference& value, const std::vector<std::uint8_t>& bytes,
             std::size_t bits) {
    BitWriter writer;
    CHECK(write_reference(writer,value,bits) == bits);
    CHECK(writer.bit_count() == bits);
    CHECK(writer.payload_bytes() == bytes);
    BitReader reader(bytes,bits);
    const auto result=read_reference(reader,bits);
    CHECK(result.value == value);
    CHECK(result.consumed_bits == bits);
    CHECK(reader.position() == bits);
    CHECK(reader.at_end());
}
struct OffsetFixture { std::size_t offset; std::vector<std::uint8_t> bytes; };
void offsets(const Reference& value, std::size_t bits,
             const std::vector<OffsetFixture>& fixtures) {
    const std::array<bool,7> prefix{true,false,true,true,false,true,false};
    const std::array<std::uint8_t,1> suffix{5};
    for (const auto& fixture:fixtures) {
        CAPTURE(fixture.offset);
        BitReader reader(fixture.bytes,fixture.offset+bits+3);
        for (std::size_t i=0;i<fixture.offset;++i) CHECK(reader.read_bit() == prefix[i]);
        const auto result=read_reference(reader,bits);
        CHECK(result.value == value);
        CHECK(result.consumed_bits == bits);
        CHECK(reader.position() == fixture.offset+bits);
        CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
        CHECK(reader.at_end());
        BitWriter writer;
        for (std::size_t i=0;i<fixture.offset;++i) writer.write_bit(prefix[i]);
        CHECK(write_reference(writer,value,bits) == bits);
        writer.write_bits(suffix,3);
        CHECK(writer.bit_count() == fixture.offset+bits+3);
        CHECK(writer.payload_bytes() == fixture.bytes);
    }
}
void rejected_write(const Reference& value, std::size_t cap, const char* code) {
    BitWriter writer;
    const std::array<std::uint8_t,1> prefix{5};
    writer.write_bits(prefix,3);
    const auto before=writer.payload_bytes();
    expect_code([&] { (void)write_reference(writer,value,cap); },code);
    CHECK(writer.bit_count() == 3);
    CHECK(writer.payload_bytes() == before);
}
}

// Literal oracle: selector+2*value, no alignment. Expected bytes are public
// integer fixtures from evidence/object-reference-codec/literal-oracle.py,
// never from the product encoder or BitWriter.
TEST_CASE("Reference package zero is a 32-bit export coordinate") {
    literal(PackageExportReference{0},{0,0,0,0},32);
}
TEST_CASE("Reference package one literal") {
    literal(PackageExportReference{1},{2,0,0,0},32);
}
TEST_CASE("Reference package mixed bits literal") {
    literal(PackageExportReference{0x12345678},{0xf0,0xac,0x68,0x24},32);
}
TEST_CASE("Reference package high bit remains inside 31-bit range") {
    literal(PackageExportReference{0x40000000},{0,0,0,0x80},32);
}
TEST_CASE("Reference package maximum literal") {
    literal(PackageExportReference{0x7fffffff},{0xfe,0xff,0xff,0xff},32);
}
TEST_CASE("Reference channel zero is a distinct 12-bit null representation") {
    literal(ActorChannelReference{0},{1,0},12);
}
TEST_CASE("Reference channel one literal") {
    literal(ActorChannelReference{1},{3,0},12);
}
TEST_CASE("Reference channel mixed bits literal") {
    literal(ActorChannelReference{0x456},{0xad,0x08},12);
}
TEST_CASE("Reference channel maximum literal") {
    literal(ActorChannelReference{2047},{0xff,0x0f},12);
}
TEST_CASE("Reference package zero never decodes as channel null") {
    const std::array<std::uint8_t,4> bytes{0,0,0,0};
    BitReader reader(bytes,32);
    const auto result=read_reference(reader,32);
    REQUIRE(std::holds_alternative<PackageExportReference>(result.value));
    CHECK(std::get<PackageExportReference>(result.value).index == 0);
}
TEST_CASE("Reference channel null never decodes as package zero") {
    const std::array<std::uint8_t,2> bytes{1,0};
    BitReader reader(bytes,12);
    const auto result=read_reference(reader,12);
    REQUIRE(std::holds_alternative<ActorChannelReference>(result.value));
    CHECK(std::get<ActorChannelReference>(result.value).channel == 0);
}
TEST_CASE("Reference package composes at every unaligned cursor") {
    offsets(PackageExportReference{0x12345678},32,{
        {0,{0xf0,0xac,0x68,0x24,0x05}}, {1,{0xe1,0x59,0xd1,0x48,0x0a}},
        {2,{0xc1,0xb3,0xa2,0x91,0x14}}, {3,{0x85,0x67,0x45,0x23,0x29}},
        {4,{0x0d,0xcf,0x8a,0x46,0x52}}, {5,{0x0d,0x9e,0x15,0x8d,0xa4}},
        {6,{0x2d,0x3c,0x2b,0x1a,0x49,0x01}}, {7,{0x2d,0x78,0x56,0x34,0x92,0x02}}});
}
TEST_CASE("Reference channel composes at every unaligned cursor") {
    offsets(ActorChannelReference{0x456},12,{
        {0,{0xad,0x58}}, {1,{0x5b,0xb1}}, {2,{0xb5,0x62,0x01}}, {3,{0x6d,0xc5,0x02}},
        {4,{0xdd,0x8a,0x05}}, {5,{0xad,0x15,0x0b}}, {6,{0x6d,0x2b,0x16}}, {7,{0xad,0x56,0x2c}}});
}
TEST_CASE("Reference prefixes consume package then channel without suffix") {
    const std::array<std::uint8_t,6> bytes{0xfe,0xff,0xff,0xff,1,0x50};
    BitReader reader(bytes,47);
    CHECK(read_reference(reader,32).value == Reference{PackageExportReference{0x7fffffff}});
    CHECK(reader.position() == 32);
    CHECK(read_reference(reader,12).value == Reference{ActorChannelReference{0}});
    CHECK(reader.position() == 44);
    CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
    CHECK(reader.at_end());
}
TEST_CASE("Reference prefixes consume channel then unaligned package") {
    const std::array<std::uint8_t,6> bytes{0xad,0x08,0xcf,0x8a,0x46,0x02};
    BitReader reader(bytes,44);
    CHECK(read_reference(reader,12).value == Reference{ActorChannelReference{0x456}});
    CHECK(reader.position() == 12);
    CHECK(read_reference(reader,32).value == Reference{PackageExportReference{0x12345678}});
    CHECK(reader.position() == 44);
    CHECK(reader.at_end());
}
TEST_CASE("Reference package truncation retains each available prefix bit") {
    const std::array<std::uint8_t,4> bytes{0xfe,0xff,0xff,0xff};
    for (std::size_t available=0;available<32;++available) {
        CAPTURE(available);
        BitReader reader(bytes,available);
        expect_code([&] { (void)read_reference(reader,32); },"truncated");
        CHECK(reader.position() == available);
        CHECK(reader.at_end());
    }
}
TEST_CASE("Reference channel truncation retains each available prefix bit") {
    const std::array<std::uint8_t,2> bytes{0xff,0x0f};
    for (std::size_t available=0;available<12;++available) {
        CAPTURE(available);
        BitReader reader(bytes,available);
        expect_code([&] { (void)read_reference(reader,12); },"truncated");
        CHECK(reader.position() == available);
        CHECK(reader.at_end());
    }
}
TEST_CASE("Reference truncation is partial after an unaligned existing cursor") {
    const std::array<std::uint8_t,2> bytes{0xfd,0x7f};
    BitReader reader(bytes,10);
    CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
    expect_code([&] { (void)read_reference(reader,12); },"truncated");
    CHECK(reader.position() == 10);
}
TEST_CASE("Reference read zero budget rejects before selector") {
    const std::array<std::uint8_t,4> bytes{0,0,0,0};
    BitReader reader(bytes,32);
    expect_code([&] { (void)read_reference(reader,0); },"invalid_limit");
    CHECK(reader.position() == 0);
    CHECK_FALSE(reader.at_end());
}
TEST_CASE("Reference package insufficient budgets stop after selector") {
    const std::array<std::uint8_t,4> bytes{0xfe,0xff,0xff,0xff};
    for (std::size_t cap=1;cap<32;++cap) {
        CAPTURE(cap);
        BitReader reader(bytes,32);
        expect_code([&] { (void)read_reference(reader,cap); },"field_too_large");
        CHECK(reader.position() == 1);
        CHECK_FALSE(reader.at_end());
    }
}
TEST_CASE("Reference channel insufficient budgets stop after selector") {
    const std::array<std::uint8_t,2> bytes{0xff,0x0f};
    for (std::size_t cap=1;cap<12;++cap) {
        CAPTURE(cap);
        BitReader reader(bytes,12);
        expect_code([&] { (void)read_reference(reader,cap); },"field_too_large");
        CHECK(reader.position() == 1);
        CHECK_FALSE(reader.at_end());
    }
}
TEST_CASE("Reference budget violation wins before a physically short bounded field") {
    const std::array<std::uint8_t,1> bytes{1};
    BitReader reader(bytes,1);
    expect_code([&] { (void)read_reference(reader,11); },"field_too_large");
    CHECK(reader.position() == 1);
}
TEST_CASE("Reference writer zero budget preserves preexisting bits") {
    rejected_write(PackageExportReference{0},0,"invalid_limit");
    rejected_write(ActorChannelReference{0},0,"invalid_limit");
}
TEST_CASE("Reference writer all insufficient budgets preserve preexisting bits") {
    for (std::size_t cap=1;cap<32;++cap)
        rejected_write(PackageExportReference{0x7fffffff},cap,"field_too_large");
    for (std::size_t cap=1;cap<12;++cap)
        rejected_write(ActorChannelReference{2047},cap,"field_too_large");
}
TEST_CASE("Reference writer rejects package range overflow without mutation") {
    rejected_write(PackageExportReference{0x80000000},32,"invalid_value");
    rejected_write(PackageExportReference{0xffffffff},32,"invalid_value");
}
TEST_CASE("Reference writer rejects channel range overflow without mutation") {
    rejected_write(ActorChannelReference{2048},12,"invalid_value");
    rejected_write(ActorChannelReference{65535},12,"invalid_value");
}
TEST_CASE("Reference large positive budgets do not impose an artificial ceiling") {
    for (const std::size_t cap: {std::size_t{512},std::numeric_limits<std::size_t>::max()}) {
        const std::array<std::uint8_t,2> bytes{0xad,8};
        BitReader reader(bytes,12);
        const auto result=read_reference(reader,cap);
        CHECK(result.value == Reference{ActorChannelReference{0x456}});
        CHECK(result.consumed_bits == 12);
        BitWriter writer;
        CHECK(write_reference(writer,ActorChannelReference{0x456},cap) == 12);
        CHECK(writer.payload_bytes() == std::vector<std::uint8_t>{0xad,8});
    }
}
TEST_CASE("Reference reader ignores unused high storage bits outside explicit length") {
    const std::array<std::uint8_t,2> bytes{1,0xf0};
    BitReader reader(bytes,12);
    CHECK(read_reference(reader,12).value == Reference{ActorChannelReference{0}});
    CHECK(reader.position() == 12);
    CHECK(reader.at_end());
}
