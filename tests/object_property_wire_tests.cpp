#include "hawken/object_property_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <string>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::property;
using object::ActorChannelReference;
using object::PackageExportReference;

template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("Object property encoder accepted rejected input");
}
void literal(std::span<const ObjectProperty> values, std::uint32_t bound,
             const std::vector<std::uint8_t>& bytes, std::size_t bits,
             std::size_t budget) {
    const auto encoded=encode_object_properties(values,bound,budget);
    CHECK(encoded.payload == bytes);
    CHECK(encoded.logical_bits == bits);
}
}

// Independent literal LSB arithmetic, no BitWriter used to create expected:
// id3/bound8 = 110 (3bits). Actor ref = selector1 then 11bits channel;
// package ref = selector0 then 31bits index. Thus total15/35bits, not RPC16/36.
TEST_CASE("Normal ObjectProperty channel null literal has no RPC presence bit") {
    const std::array<ObjectProperty,1> values{{{3,ActorChannelReference{0}}}};
    literal(values,8,{0x0b,0},15,15);
}
TEST_CASE("Normal ObjectProperty channel five literal has no RPC presence bit") {
    const std::array<ObjectProperty,1> values{{{3,ActorChannelReference{5}}}};
    literal(values,8,{0x5b,0},15,15);
}
TEST_CASE("Normal ObjectProperty package nine literal has no RPC presence bit") {
    const std::array<ObjectProperty,1> values{{{3,PackageExportReference{9}}}};
    literal(values,8,{0x93,0,0,0,0},35,35);
}
TEST_CASE("Normal ObjectProperty list concatenates without alignment or terminator") {
    // 0x0b (15bits) | (0x5b << 15) = 0x002d800b (30bits).
    const std::array<ObjectProperty,2> values{{
        {3,ActorChannelReference{0}}, {3,ActorChannelReference{5}}}};
    literal(values,8,{0x0b,0x80,0x2d,0},30,30);
    expect_code([&] { (void)encode_object_properties(values,8,29); },"field_too_large");
}
TEST_CASE("Normal ObjectProperty field zero is a real record and package zero is distinct") {
    const std::array<ObjectProperty,1> channel{{{0,ActorChannelReference{0}}}};
    const std::array<ObjectProperty,1> package{{{0,PackageExportReference{0}}}};
    literal(channel,8,{8,0},15,15);
    literal(package,8,{0,0,0,0,0},35,35);
}
TEST_CASE("Normal ObjectProperty bound one omits only the field ID bits") {
    const std::array<ObjectProperty,1> values{{{0,ActorChannelReference{0}}}};
    literal(values,1,{1,0},12,12);
    expect_code([&] { (void)encode_object_properties(values,1,11); },"field_too_large");
}
TEST_CASE("Normal ObjectProperty non-power bounds use exact value dependent widths") {
    // Bound3: id0=00, id1=1, id2=01. After each ID append selector1,
    // channel5 LSB. These integer bit layouts are 0x2c/0x17/0x2e.
    const std::array<ObjectProperty,1> zero{{{0,ActorChannelReference{5}}}};
    const std::array<ObjectProperty,1> one{{{1,ActorChannelReference{5}}}};
    const std::array<ObjectProperty,1> two{{{2,ActorChannelReference{5}}}};
    literal(zero,3,{0x2c,0},14,14);
    literal(one,3,{0x17,0},13,13);
    literal(two,3,{0x2e,0},14,14);
    expect_code([&] { (void)encode_object_properties(zero,3,13); },"field_too_large");
    expect_code([&] { (void)encode_object_properties(two,3,13); },"field_too_large");
}
TEST_CASE("Normal ObjectProperty uint32 field bound and reference maxima retain every bit") {
    const std::array<ObjectProperty,1> field{{{0xfffffffeU,ActorChannelReference{2047}}}};
    literal(field,0xffffffffU,{0xfe,0xff,0xff,0xff,0xff,0x0f},44,44);
    const std::array<ObjectProperty,1> package{{{3,PackageExportReference{0x7fffffffU}}}};
    literal(package,8,{0xf3,0xff,0xff,0xff,7},35,35);
}
TEST_CASE("Normal ObjectProperty exact global budgets include both ID and reference") {
    const std::array<ObjectProperty,1> channel{{{3,ActorChannelReference{5}}}};
    const std::array<ObjectProperty,1> package{{{3,PackageExportReference{9}}}};
    expect_code([&] { (void)encode_object_properties(channel,8,14); },"field_too_large");
    expect_code([&] { (void)encode_object_properties(channel,8,3); },"field_too_large");
    expect_code([&] { (void)encode_object_properties(channel,8,2); },"field_too_large");
    expect_code([&] { (void)encode_object_properties(package,8,34); },"field_too_large");
    literal(channel,8,{0x5b,0},15,std::numeric_limits<std::size_t>::max());
}
TEST_CASE("Normal ObjectProperty empty list is valid but limits remain mandatory") {
    literal({},1,{},0,1);
    expect_code([] { (void)encode_object_properties({},0,1); },"invalid_limit");
    expect_code([] { (void)encode_object_properties({},1,0); },"invalid_limit");
}
TEST_CASE("Normal ObjectProperty populated list rejects zero bound and budget") {
    const std::array<ObjectProperty,1> values{{{0,ActorChannelReference{0}}}};
    expect_code([&] { (void)encode_object_properties(values,0,12); },"invalid_limit");
    expect_code([&] { (void)encode_object_properties(values,1,0); },"invalid_limit");
}
TEST_CASE("Normal ObjectProperty rejects ID equal to or beyond its bound") {
    const std::array<ObjectProperty,1> equal{{{8,ActorChannelReference{0}}}};
    const std::array<ObjectProperty,1> beyond{{{0xffffffffU,ActorChannelReference{0}}}};
    expect_code([&] { (void)encode_object_properties(equal,8,64); },"invalid_value");
    expect_code([&] { (void)encode_object_properties(beyond,8,64); },"invalid_value");
}
TEST_CASE("Normal ObjectProperty rejects invalid primary reference ranges") {
    const std::array<ObjectProperty,1> channel{{{0,ActorChannelReference{2048}}}};
    const std::array<ObjectProperty,1> package{{{0,PackageExportReference{0x80000000U}}}};
    expect_code([&] { (void)encode_object_properties(channel,8,64); },"invalid_value");
    expect_code([&] { (void)encode_object_properties(package,8,64); },"invalid_value");
}
TEST_CASE("Normal ObjectProperty invalid later record produces no partial returned list") {
    const std::array<ObjectProperty,2> values{{
        {3,ActorChannelReference{5}}, {8,ActorChannelReference{0}}}};
    expect_code([&] { (void)encode_object_properties(values,8,64); },"invalid_value");
}


namespace {
void array_literal(std::span<const ArrayObjectProperty> values,std::uint32_t bound,
                   const std::vector<std::uint8_t>& bytes,std::size_t bits,std::size_t budget) {
    const auto encoded=encode_object_array_properties(values,bound,budget);
    CHECK(encoded.payload==bytes);
    CHECK(encoded.logical_bits==bits);
}
}
// Independent native framing: field31/bound35 takes5 bits, followed by a
// FULL8-bit array index, then primary ref12/32 bits. No RPC presence bit.
// Channel5 ref is11: 31 + (index<<5) + (11<<13). A bounded2-bit index is wrong.
TEST_CASE("Array ObjectProperty emits all four HandWeap indices as full bytes") {
    const std::array<std::vector<std::uint8_t>,4> expected{{
        {0x1f,0x60,1,0},{0x3f,0x60,1,0},{0x5f,0x60,1,0},{0x7f,0x60,1,0}}};
    for (std::uint32_t index=0;index<4;++index) {
        CAPTURE(index);
        const std::array<ArrayObjectProperty,1> values{{{31,4,index,ActorChannelReference{5}}}};
        array_literal(values,35,expected[index],25,25);
    }
}
TEST_CASE("Array ObjectProperty null selector and package reference literals are distinct") {
    // Null:31+(3<<5)+(1<<13). Package9:31+(2<<5)+((9<<1)<<13).
    const std::array<ArrayObjectProperty,1> null_value{{{31,4,3,ActorChannelReference{0}}}};
    const std::array<ArrayObjectProperty,1> package_value{{{31,4,2,PackageExportReference{9}}}};
    array_literal(null_value,35,{0x7f,0x20,0,0},25,25);
    array_literal(package_value,35,{0x5f,0x40,2,0,0,0},45,45);
}
TEST_CASE("Array ObjectProperty ordered duplicate field records concatenate without alignment") {
    // 0x1601f + (0x207f<<25) =0x40fe01601f, with exactly50 logical bits.
    const std::array<ArrayObjectProperty,2> values{{
        {31,4,0,ActorChannelReference{5}},{31,4,3,ActorChannelReference{0}}}};
    array_literal(values,35,{0x1f,0x60,1,0xfe,0x40,0,0},50,50);
    expect_code([&]{(void)encode_object_array_properties(values,35,49);},"field_too_large");
}
TEST_CASE("Array ObjectProperty exact prefix leaves an opaque tail at bit twenty five") {
    const std::array<ArrayObjectProperty,1> values{{{31,4,3,ActorChannelReference{0}}}};
    const auto encoded=encode_object_array_properties(values,35,25);
    BitWriter stream;stream.write_bits(encoded.payload,encoded.logical_bits);
    stream.write_bits(std::array<std::uint8_t,1>{0x2d},6);
    const auto bytes=stream.payload_bytes();BitReader reader(bytes,stream.bit_count());
    CHECK(reader.read_bounded(35)==31);
    CHECK(reader.read_bits(8)==std::vector<std::uint8_t>{3});
    CHECK(object::read_reference(reader,12).value==object::Reference{ActorChannelReference{0}});
    CHECK(reader.position()==25);
    CHECK(reader.read_bits(6)==std::vector<std::uint8_t>{0x2d});
    CHECK(reader.position()==31);
}
TEST_CASE("Array ObjectProperty field zero and bound one preserve index byte and null") {
    const std::array<ArrayObjectProperty,1> zero{{{0,4,0,ActorChannelReference{0}}}};
    const std::array<ArrayObjectProperty,1> no_tag{{{0,256,255,ActorChannelReference{0}}}};
    // field0/b35 uses6bits: ref1 starts14. Bound1 omitsID, not index8.
    array_literal(zero,35,{0,0x40,0,0},26,26);
    array_literal(no_tag,1,{0xff,1,0},20,20);
}
TEST_CASE("Array ObjectProperty reference maxima retain bits and zero unused high tail") {
    const std::array<ArrayObjectProperty,1> channel{{{31,4,3,ActorChannelReference{2047}}}};
    const std::array<ArrayObjectProperty,1> package_value{{{31,4,3,PackageExportReference{0x7fffffffU}}}};
    array_literal(channel,35,{0x7f,0xe0,0xff,1},25,25);
    array_literal(package_value,35,{0x7f,0xc0,0xff,0xff,0xff,0x1f},45,45);
}
TEST_CASE("Array ObjectProperty requires an actual array and an in range representable index") {
    for (const auto dim:{0U,1U}) {
        const std::array<ArrayObjectProperty,1> values{{{31,dim,0,ActorChannelReference{0}}}};
        expect_code([&]{(void)encode_object_array_properties(values,35,64);},"invalid_value");
    }
    const std::array<ArrayObjectProperty,1> equal{{{31,4,4,ActorChannelReference{0}}}};
    const std::array<ArrayObjectProperty,1> cannot_fit_byte{{{31,512,256,ActorChannelReference{0}}}};
    expect_code([&]{(void)encode_object_array_properties(equal,35,64);},"invalid_value");
    expect_code([&]{(void)encode_object_array_properties(cannot_fit_byte,35,64);},"invalid_value");
    // No invented global ArrayDim256 cap: a representable index in a larger
    // caller-proven array remains valid. We do not silently truncate index256.
    const std::array<ArrayObjectProperty,1> representable{{{0,512,255,ActorChannelReference{0}}}};
    array_literal(representable,1,{0xff,1,0},20,20);
}
TEST_CASE("Array ObjectProperty positive limits are mandatory for empty and populated spans") {
    array_literal({},1,{},0,1);
    expect_code([]{(void)encode_object_array_properties({},0,1);},"invalid_limit");
    expect_code([]{(void)encode_object_array_properties({},1,0);},"invalid_limit");
    const std::array<ArrayObjectProperty,1> values{{{0,4,0,ActorChannelReference{0}}}};
    expect_code([&]{(void)encode_object_array_properties(values,0,64);},"invalid_limit");
    expect_code([&]{(void)encode_object_array_properties(values,1,0);},"invalid_limit");
}
TEST_CASE("Array ObjectProperty rejects invalid field and primary reference without partial list") {
    const std::array<ArrayObjectProperty,2> bad_field{{
        {31,4,0,ActorChannelReference{5}},{35,4,1,ActorChannelReference{0}}}};
    const std::array<ArrayObjectProperty,2> bad_index{{
        {31,4,0,ActorChannelReference{5}},{31,4,4,ActorChannelReference{0}}}};
    const std::array<ArrayObjectProperty,1> bad_channel{{{31,4,0,ActorChannelReference{2048}}}};
    const std::array<ArrayObjectProperty,1> bad_package{{{31,4,0,PackageExportReference{0x80000000U}}}};
    expect_code([&]{(void)encode_object_array_properties(bad_field,35,128);},"invalid_value");
    expect_code([&]{(void)encode_object_array_properties(bad_index,35,128);},"invalid_value");
    expect_code([&]{(void)encode_object_array_properties(bad_channel,35,128);},"invalid_value");
    expect_code([&]{(void)encode_object_array_properties(bad_package,35,128);},"invalid_value");
}
TEST_CASE("Array ObjectProperty global budget includes full index byte before actor or package reference") {
    const std::array<ArrayObjectProperty,1> channel{{{31,4,0,ActorChannelReference{5}}}};
    const std::array<ArrayObjectProperty,1> package_value{{{31,4,2,PackageExportReference{9}}}};
    expect_code([&]{(void)encode_object_array_properties(channel,35,24);},"field_too_large");
    expect_code([&]{(void)encode_object_array_properties(channel,35,13);},"field_too_large");
    expect_code([&]{(void)encode_object_array_properties(channel,35,5);},"field_too_large");
    expect_code([&]{(void)encode_object_array_properties(package_value,35,44);},"field_too_large");
    array_literal(channel,35,{0x1f,0x60,1,0},25,std::numeric_limits<std::size_t>::max());
}
