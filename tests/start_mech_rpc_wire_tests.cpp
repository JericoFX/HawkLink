#include "hawken/start_mech_rpc_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::rpc;
using hawken::control::Endian;
constexpr std::string_view guid = "abcdef01-2345-6789-abcd-ef0123456789";
constexpr std::string_view narrow =
    "4b000000828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e7072008c00000000";
constexpr std::string_view wide =
    "b7ffffff83008400860088008a008c00600062005a006400660068006a005a006c006e00700072005a0082008400860088005a008a008c00600062006400660068006a006c006e00700072000000feffffff0700000004";

std::vector<std::uint8_t> hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i=0;i<text.size();i+=2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(i,2)),nullptr,16)));
    return out;
}
void put_bits(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value,std::size_t count) {
    for (std::size_t i=0;i<count;++i) {
        const auto mask=static_cast<std::uint8_t>(1U<<((offset+i)%8));
        if ((value>>i)&1U) bytes[(offset+i)/8]|=mask;
        else bytes[(offset+i)/8]&=static_cast<std::uint8_t>(~mask);
    }
}
// Relocates an already literal bit oracle. No RPC/scalar/string encoding here.
std::vector<std::uint8_t> relocate(const std::vector<std::uint8_t>& bytes,
                                 std::size_t bits,std::size_t offset,std::size_t suffix) {
    std::vector<std::uint8_t> out((offset+bits+suffix+7)/8,0);
    put_bits(out,0,0x55,offset);
    for (std::size_t i=0;i<bits;++i)
        put_bits(out,offset+i,(bytes[i/8]>>(i%8))&1U,1);
    put_bits(out,offset+bits,0x13,suffix);
    return out;
}
template<class Action>
void expect_code(Action action,const char* code) {
    try { action(); }
    catch (const WireError& error) { CHECK(error.code==code); return; }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("accepted invalid start-mech arguments");
}
void literal(std::string_view bytes,std::size_t bits,Endian endian,std::string_view instance,
             std::int32_t primary,std::int32_t holo) {
    const auto input=hex(bytes);
    BitReader reader(input,bits);
    const auto result=read_start_mech_arguments(reader,endian,bits);
    CHECK(result.value.mech_instance_id==instance);
    CHECK(result.value.primary_weapon==primary);
    CHECK(result.value.holo_taunt_type==holo);
    CHECK(result.consumed_bits==bits);
    CHECK(reader.position()==bits);
    CHECK(reader.at_end());
}
}

// Independent LSB bit literals assembled from the original v3 wire schedule,
// without production BitWriter/encoder. Uppercase input must normalize to guid.
TEST_CASE("Start mech arguments preserve all eight parameter presence combinations") {
    struct Fixture { const char* bytes; std::size_t bits; bool string; std::int32_t primary,holo; };
    const std::array<Fixture,8> fixtures{{
        {"00",3,false,0,0},
        {"8c00000000",35,false,0,17},
        {"feffffff03",35,false,-1,0},
        {"feffffff8f00000000",67,false,-1,17},
        {"4b000000828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e70720000",331,true,0,0},
        {"4b000000828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e7072008c00000000",363,true,0,17},
        {"4b000000828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e707200feffffff03",363,true,-1,0},
        {"4b000000828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e707200feffffff8f00000000",395,true,-1,17},
    }};
    for (const auto& f:fixtures) {
        CAPTURE(f.string);
        CAPTURE(f.primary);
        CAPTURE(f.holo);
        literal(f.bytes,f.bits,Endian::little,f.string?guid:std::string_view{},f.primary,f.holo);
    }
}
TEST_CASE("Start mech arguments honor big scalar endian and wide UTF16 little units") {
    literal("0100004a828486888a8c60625a6466686a5a6c6e70725a828486885a8a8c60626466686a6c6e7072000200004400",
            363,Endian::big,guid,17,0);
    literal(wide,691,Endian::little,guid,-1,std::numeric_limits<std::int32_t>::min());
    literal("ffffffb783008400860088008a008c00600062005a006400660068006a005a006c006e00700072005a0082008400860088005a008a008c00600062006400660068006a006c006e00700072000000feffffff0704000000",
            691,Endian::big,guid,-1,std::numeric_limits<std::int32_t>::min());
}
TEST_CASE("Start mech arguments accept the original present empty FString forms") {
    literal("010000001e000000c4ffffff07",99,Endian::little,"",7,-8);
    literal("03000000001e000000c4ffffff07",107,Endian::little,"",7,-8);
    literal("ffffffff01001e000000c4ffffff07",115,Endian::little,"",7,-8);
    literal("010000000200001cfcffffc707",99,Endian::big,"",7,-8);
    literal("01000002000200001cfcffffc707",107,Endian::big,"",7,-8);
    literal("ffffffff01000200001cfcffffc707",115,Endian::big,"",7,-8);
}
TEST_CASE("Start mech arguments decode at offsets zero through seven and preserve suffix") {
    const auto oracle=hex(narrow);
    for (std::size_t offset=0;offset<8;++offset) {
        CAPTURE(offset);
        const auto bytes=relocate(oracle,363,offset,5);
        BitReader reader(bytes,offset+363+5);
        (void)reader.read_bits(offset);
        const auto result=read_start_mech_arguments(reader,Endian::little,363);
        CHECK(result.value.mech_instance_id==guid);
        CHECK(result.value.primary_weapon==0);
        CHECK(result.value.holo_taunt_type==17);
        CHECK(result.consumed_bits==363);
        CHECK(reader.position()==offset+363);
        CHECK(reader.read_bits(5)==std::vector<std::uint8_t>{0x13});
        CHECK(reader.at_end());
    }
}
TEST_CASE("Start mech arguments retain all-zero UUID as a syntactic selector only") {
    auto bytes=hex(narrow);
    for (std::size_t i=0;i<36;++i) {
        const auto character=(i==8 || i==13 || i==18 || i==23)?'-':'0';
        put_bits(bytes,33+8*i,static_cast<std::uint32_t>(character),8);
    }
    BitReader reader(bytes,363);
    const auto result=read_start_mech_arguments(reader,Endian::little,363);
    CHECK(result.value.mech_instance_id=="00000000-0000-0000-0000-000000000000");
    CHECK(result.value.holo_taunt_type==17);
}
TEST_CASE("Start mech arguments reject unsupported counts transactionally") {
    for (const auto count:std::array<std::int32_t,7>{2,36,38,-2,-36,-38,std::numeric_limits<std::int32_t>::min()}) {
        CAPTURE(count);
        auto bytes=hex(narrow);
        put_bits(bytes,1,static_cast<std::uint32_t>(count),32);
        BitReader reader(bytes,363);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,363);},"invalid_value");
        CHECK(reader.position()==0);
    }
}
TEST_CASE("Start mech arguments reject malformed GUID units and empty terminators without cursor changes") {
    for (const auto index:std::array<std::size_t,3>{0,8,36}) {
        auto bytes=hex(narrow);
        put_bits(bytes,33+8*index,index==0?'Z':(index==8?'x':'!'),8);
        BitReader reader(bytes,363);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,363);},"invalid_value");
        CHECK(reader.position()==0);
    }
    for (const auto unit:std::array<std::uint32_t,3>{0x100,0xd800,0xdc00}) {
        auto bytes=hex(wide);
        put_bits(bytes,33,unit,16);
        BitReader reader(bytes,691);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,691);},"invalid_value");
        CHECK(reader.position()==0);
    }
    for (const auto& fixture:std::array<std::pair<const char*,std::size_t>,2>{{
        {"03000000001e000000c4ffffff07",107},{"ffffffff01001e000000c4ffffff07",115}}}) {
        auto bytes=hex(fixture.first);
        put_bits(bytes,33,'X',8);
        BitReader reader(bytes,fixture.second);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,fixture.second);},"invalid_value");
        CHECK(reader.position()==0);
    }
}
TEST_CASE("Start mech arguments preserve the cursor for every physical truncation") {
    const auto bytes=hex(wide);
    for (std::size_t cut=0;cut<691;++cut) {
        CAPTURE(cut);
        BitReader reader(bytes,cut);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,691);},"truncated");
        CHECK(reader.position()==0);
    }
}
TEST_CASE("Start mech arguments enforce exact budgets without consuming unrelated bits") {
    const auto bytes=hex(wide);
    for (std::size_t budget=1;budget<691;++budget) {
        CAPTURE(budget);
        BitReader reader(bytes,691);
        expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,budget);},"field_too_large");
        CHECK(reader.position()==0);
    }
    BitReader reader(bytes,691);
    const auto result=read_start_mech_arguments(reader,Endian::little,std::numeric_limits<std::size_t>::max());
    CHECK(result.consumed_bits==691);
    CHECK(reader.position()==691);
}
TEST_CASE("Start mech arguments reject invalid configuration with an unaligned cursor intact") {
    const auto bytes=relocate(hex(narrow),363,5,0);
    BitReader reader(bytes,368);
    (void)reader.read_bits(5);
    expect_code([&] {(void)read_start_mech_arguments(reader,Endian::little,0);},"invalid_limit");
    CHECK(reader.position()==5);
    expect_code([&] {(void)read_start_mech_arguments(reader,static_cast<Endian>(7),363);},"invalid_value");
    CHECK(reader.position()==5);
}
