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
