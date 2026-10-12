#include "hawken/pawn_rpc_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::rpc;
using hawken::wire::object::ActorChannelReference;
using hawken::wire::object::PackageExportReference;

template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("RPC accepted rejected input");
}
void literal(const PawnRpc& value, std::uint32_t bound, bool eligible,
             const std::vector<std::uint8_t>& bytes, std::size_t bits,
             std::size_t budget) {
    const auto encoded=encode_rpc(value,bound,eligible,budget);
    CHECK(encoded.payload == bytes);
    CHECK(encoded.logical_bits == bits);
    BitReader reader(bytes,bits);
    const auto decoded=decode_rpc(reader,bound,eligible,budget);
    CHECK(decoded.field_id == value.field_id);
    CHECK(decoded.value == value.value);
    CHECK(decoded.consumed_bits == bits);
    CHECK(reader.position() == bits);
    CHECK(reader.at_end());
}
}

// Literal oracle from controller-rpc-wire/public-physical-fixtures.json:
// field3/bound8 -> bits110; then presence. For presence1 append selector
// and 31 package/11 channel LSB bits. Expected bytes never use BitWriter.
TEST_CASE("Pawn RPC actor reference literal bb00") {
    literal({3,ActorChannelReference{5}},8,true,{0xbb,0},16,16);
}
TEST_CASE("Pawn RPC package reference literal 2b01000000") {
    literal({3,PackageExportReference{9}},8,true,{0x2b,1,0,0,0},36,36);
}
TEST_CASE("Pawn RPC absent literal 03 omits the reference") {
    literal({3,std::nullopt},8,true,{3},4,4);
}
TEST_CASE("Pawn RPC present channel zero literal 1b00 remains distinct") {
    literal({3,ActorChannelReference{0}},8,true,{0x1b,0},16,16);
}
TEST_CASE("Pawn RPC package zero remains present and is not actor null") {
    literal({3,PackageExportReference{0}},8,true,{0x0b,0,0,0,0},36,36);
}
TEST_CASE("Pawn RPC ineligible parameter emits only the field ID") {
    literal({3,std::nullopt},8,false,{3},3,3);
}
TEST_CASE("Pawn RPC ineligible supplied value is rejected") {
    expect_code([] { (void)encode_rpc({3,ActorChannelReference{0}},8,false,64); },"invalid_value");
}
TEST_CASE("Pawn RPC bound one permits zero bits when ineligible") {
    literal({0,std::nullopt},1,false,{},0,1);
    literal({0,std::nullopt},1,true,{0},1,1);
}
TEST_CASE("Pawn RPC non-power bound accepts the shorter exact budget") {
    // bound3/id0 -> bits00; id1 -> bit1 (one bit); id2 -> bits01.
    literal({0,std::nullopt},3,false,{0},2,2);
    literal({1,std::nullopt},3,false,{1},1,1);
    literal({2,std::nullopt},3,false,{2},2,2);
    literal({3,std::nullopt},5,true,{3},3,3);
    literal({4,std::nullopt},5,true,{4},4,4);
}
TEST_CASE("Pawn RPC full uint32 bound preserves the field ID") {
    literal({0xfffffffeU,std::nullopt},0xffffffffU,true,{0xfe,0xff,0xff,0xff,0},33,33);
}
TEST_CASE("Pawn RPC prefix preserves a concatenated RPC and suffix") {
    // absent03 (4 bits), bb00 (16 bits), then suffix101 (3 bits).
    const std::array<std::uint8_t,3> bytes{0xb3,0x0b,0x50};
    BitReader reader(bytes,23);
    const auto first=decode_rpc(reader,8,true,4);
    CHECK(first.field_id == 3);
    CHECK_FALSE(first.value.has_value());
    CHECK(first.consumed_bits == 4);
    const auto second=decode_rpc(reader,8,true,16);
    CHECK(second.field_id == 3);
    REQUIRE(second.value.has_value());
    CHECK(*second.value == object::Reference{ActorChannelReference{5}});
    CHECK(second.consumed_bits == 16);
    CHECK(reader.position() == 20);
    CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
    CHECK(reader.at_end());
}
TEST_CASE("Pawn RPC reads at a nonaligned caller cursor") {
    // prefix101 (3 bits), bb00 (16 bits), suffix101 (3 bits).
    const std::array<std::uint8_t,3> bytes{0xdd,5,0x28};
    BitReader reader(bytes,22);
    CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
    const auto decoded=decode_rpc(reader,8,true,16);
    CHECK(decoded.field_id == 3);
    REQUIRE(decoded.value.has_value());
    CHECK(*decoded.value == object::Reference{ActorChannelReference{5}});
    CHECK(decoded.consumed_bits == 16);
    CHECK(reader.position() == 19);
    CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
}
TEST_CASE("Pawn RPC budget rejects before committing an oversized field ID") {
    const std::array<std::uint8_t,1> bytes{0};
    BitReader reader(bytes,2);
    expect_code([&] { (void)decode_rpc(reader,3,false,1); },"field_too_large");
    CHECK(reader.position() == 0);
    CHECK_FALSE(reader.read_bit());
    expect_code([] { (void)encode_rpc({0,std::nullopt},3,false,1); },"field_too_large");
}
TEST_CASE("Pawn RPC truncated field ID does not commit its temporary cursor") {
    const std::array<std::uint8_t,2> bytes{0xbb,0};
    for (std::size_t cut=0;cut<3;++cut) {
        CAPTURE(cut);
        BitReader reader(bytes,cut);
        expect_code([&] { (void)decode_rpc(reader,8,true,16); },"truncated");
        CHECK(reader.position() == 0);
    }
}
TEST_CASE("Pawn RPC truncation after field ID retains partial reference cursor") {
    const std::array<std::uint8_t,5> bytes{0x2b,1,0,0,0};
    for (std::size_t cut=3;cut<36;++cut) {
        CAPTURE(cut);
        BitReader reader(bytes,cut);
        expect_code([&] { (void)decode_rpc(reader,8,true,36); },"truncated");
        CHECK(reader.position() == cut);
    }
}
TEST_CASE("Pawn RPC cap includes ID presence and the reference") {
    const std::array<std::uint8_t,2> bytes{0xbb,0};
    for (const auto& [cap,position]:std::array<std::pair<std::size_t,std::size_t>,4>{{{2,0},{3,3},{4,4},{15,5}}}) {
        CAPTURE(cap);
        BitReader reader(bytes,16);
        expect_code([&] { (void)decode_rpc(reader,8,true,cap); },"field_too_large");
        CHECK(reader.position() == position);
    }
    expect_code([] { (void)encode_rpc({3,ActorChannelReference{5}},8,true,15); },"field_too_large");
    expect_code([] { (void)encode_rpc({3,PackageExportReference{9}},8,true,35); },"field_too_large");
}
TEST_CASE("Pawn RPC zero bound or budget rejects without reader mutation") {
    const std::array<std::uint8_t,1> bytes{3};
    SUBCASE("bound") {
        BitReader reader(bytes,4);
        expect_code([&] { (void)decode_rpc(reader,0,true,4); },"invalid_limit");
        CHECK(reader.position() == 0);
        expect_code([] { (void)encode_rpc({0,std::nullopt},0,true,4); },"invalid_limit");
    }
    SUBCASE("budget") {
        BitReader reader(bytes,4);
        expect_code([&] { (void)decode_rpc(reader,8,true,0); },"invalid_limit");
        CHECK(reader.position() == 0);
        expect_code([] { (void)encode_rpc({3,std::nullopt},8,true,0); },"invalid_limit");
    }
}
TEST_CASE("Pawn RPC invalid field IDs and object ranges reject") {
    expect_code([] { (void)encode_rpc({8,std::nullopt},8,true,64); },"invalid_value");
    expect_code([] { (void)encode_rpc({1,std::nullopt},1,false,64); },"invalid_value");
    expect_code([] { (void)encode_rpc({0xffffffffU,std::nullopt},0xffffffffU,true,64); },"invalid_value");
    expect_code([] { (void)encode_rpc({3,ActorChannelReference{2048}},8,true,64); },"invalid_value");
    expect_code([] { (void)encode_rpc({3,PackageExportReference{0x80000000U}},8,true,64); },"invalid_value");
}
TEST_CASE("Pawn RPC oversized positive budget does not pad the logical field") {
    literal({3,std::nullopt},8,true,{3},4,std::numeric_limits<std::size_t>::max());
}
TEST_CASE("Pawn RPC failures preserve absolute cursor after a caller prefix") {
    // prefix101 followed by bb00; same independent nonaligned success vector.
    const std::array<std::uint8_t,3> bytes{0xdd,5,0x28};
    SUBCASE("truncated field leaves caller prefix consumed") {
        BitReader reader(bytes,5);
        CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
        expect_code([&] { (void)decode_rpc(reader,8,true,16); },"truncated");
        CHECK(reader.position() == 3);
    }
    SUBCASE("truncated reference preserves its absolute partial cursor") {
        BitReader reader(bytes,13);
        CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
        expect_code([&] { (void)decode_rpc(reader,8,true,16); },"truncated");
        CHECK(reader.position() == 13);
    }
    SUBCASE("reference budget is relative to RPC start") {
        BitReader reader(bytes,22);
        CHECK(reader.read_bits(3) == std::vector<std::uint8_t>{5});
        expect_code([&] { (void)decode_rpc(reader,8,true,15); },"field_too_large");
        CHECK(reader.position() == 8);
    }
}
