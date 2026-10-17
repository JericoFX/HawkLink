#include "hawken/inventory_rpc_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <string>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::rpc;

template<class Action>
void inventory_error(Action action,const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code==code);
        CHECK(std::string(error.what())=="Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError");return; }
    FAIL_CHECK("ClientGivenTo encoder accepted rejected input");
}
void inventory_literal(std::uint32_t field,std::uint32_t bound,std::uint16_t pawn,bool inactive,
                       const std::vector<std::uint8_t>& bytes,std::size_t bits,std::size_t budget) {
    const auto result=encode_client_given_to(field,bound,pawn,inactive,budget);
    CHECK(result.payload==bytes);
    CHECK(result.logical_bits==bits);
}
}
// Independent native layout: tag23/b35 is5bits; presence1, selector1+channel2
// is12bits. Non-Bool NewOwner has presence; BoolNET has NO presence evenfalse.
// 23+(1<<5)+((1+(2<<1))<<6)=0x177; booltrue adds1<<18.
TEST_CASE("Inventory ClientGivenTo false bool literal includes its data bit without bool presence") {
    inventory_literal(23,35,2,false,{0x77,1,0},19,19);
}
TEST_CASE("Inventory ClientGivenTo true bool literal changes only bit eighteen") {
    inventory_literal(23,35,2,true,{0x77,1,4},19,19);
}
TEST_CASE("Inventory ClientGivenTo null owner omits only the reference not the bool") {
    // Sender Property2b8(value,NULL,0) sees null: presence0, no reference.
    // tag23, presence0 at5, false/true value at6 =>0x17/0x57,7bits.
    inventory_literal(23,35,0,false,{0x17},7,7);
    inventory_literal(23,35,0,true,{0x57},7,7);
    inventory_error([]{(void)encode_client_given_to(23,35,0,false,6);},"field_too_large");
}
TEST_CASE("Inventory ClientGivenTo actor channel maxima keep every bit and zero unused tail") {
    // tag23 +presence1 +ref4095 at6 +booltrue at18 =0x7fff7 (19bits).
    inventory_literal(23,35,2047,true,{0xf7,0xff,7},19,19);
    inventory_error([]{(void)encode_client_given_to(23,35,2048,false,64);},"invalid_value");
    inventory_error([]{(void)encode_client_given_to(23,35,65535,false,64);},"invalid_value");
}
TEST_CASE("Inventory ClientGivenTo exact cursor preserves unknown opaque suffix") {
    const auto result=encode_client_given_to(23,35,2,false,19);
    BitWriter joined;joined.write_bits(result.payload,result.logical_bits);
    joined.write_bits(std::array<std::uint8_t,1>{0x2d},6);
    const auto bytes=joined.payload_bytes();BitReader reader(bytes,joined.bit_count());
    CHECK(reader.read_bounded(35)==23);
    CHECK(reader.read_bit());
    CHECK(object::read_reference(reader,12).value==object::Reference{object::ActorChannelReference{2}});
    CHECK_FALSE(reader.read_bit());
    CHECK(reader.position()==19);
    CHECK(reader.read_bits(6)==std::vector<std::uint8_t>{0x2d});
    CHECK(reader.position()==25);
}
TEST_CASE("Inventory ClientGivenTo bound one omits only field bits and never treats ID zero as sentinel") {
    inventory_literal(0,1,0,false,{0},2,2);
    inventory_literal(0,1,0,true,{2},2,2);
    inventory_literal(0,1,2,false,{0x0b,0},14,14);
    inventory_literal(0,1,2,true,{0x0b,0x20},14,14);
    inventory_error([]{(void)encode_client_given_to(0,1,0,false,1);},"field_too_large");
}
TEST_CASE("Inventory ClientGivenTo non power bounds retain actual native field width") {
    // bound3/id1 takes1bit, id0/id2 take2; ref2code5 follows presence1.
    inventory_literal(1,3,2,false,{0x17,0},15,15);
    inventory_literal(0,3,2,false,{0x2c,0},16,16);
    inventory_literal(2,3,2,false,{0x2e,0},16,16);
    inventory_error([]{(void)encode_client_given_to(1,3,2,false,14);},"field_too_large");
}
TEST_CASE("Inventory ClientGivenTo global budget includes tag presence reference and trailing false bool") {
    for (const auto short_budget:{1U,5U,6U,17U,18U}) {
        CAPTURE(short_budget);
        inventory_error([&]{(void)encode_client_given_to(23,35,2,false,short_budget);},"field_too_large");
    }
    inventory_literal(23,35,2,false,{0x77,1,0},19,std::numeric_limits<std::size_t>::max());
}
TEST_CASE("Inventory ClientGivenTo positive limits and field range are mandatory") {
    inventory_error([]{(void)encode_client_given_to(0,0,0,false,64);},"invalid_limit");
    inventory_error([]{(void)encode_client_given_to(0,1,0,false,0);},"invalid_limit");
    inventory_error([]{(void)encode_client_given_to(35,35,2,false,64);},"invalid_value");
    inventory_error([]{(void)encode_client_given_to(0xffffffffU,35,2,false,64);},"invalid_value");
}
TEST_CASE("Inventory ClientGivenTo full uint32 field bound remains an owned bit payload") {
    inventory_literal(0xfffffffeU,0xffffffffU,2047,true,
        {0xfe,0xff,0xff,0xff,0xff,0x3f},46,46);
}

// Original Manager.ServerSetCurrentWeapon has ONE ObjectProperty80 ArrayDim1
// ExpectedClass Engine.Weapon. Helper starts AFTER caller's conditional tag.
TEST_CASE("Inventory ServerSetCurrentWeapon reads independent actor argument literals") {
    struct Fixture { std::array<std::uint8_t,2> bytes; std::uint16_t channel; };
    const Fixture fixtures[]{ {{{0x1b,0}},6}, {{{3,0}},0}, {{{0xff,0x1f}},2047} };
    for (const auto& fixture:fixtures) {
        CAPTURE(fixture.channel);
        BitReader reader(fixture.bytes,13);
        const auto result=read_server_set_current_weapon_arguments(reader,13);
        CHECK(result.desired_weapon==object::Reference{object::ActorChannelReference{fixture.channel}});
        CHECK(result.consumed_bits==13);
        CHECK(reader.position()==13);
    }
}
TEST_CASE("Inventory ServerSetCurrentWeapon absent object defaults to null without consuming a reference") {
    const std::array<std::uint8_t,1> bytes{0x16}; // presence0 then opaque1011.
    BitReader reader(bytes,5);
    const auto result=read_server_set_current_weapon_arguments(reader,1);
    CHECK(result.desired_weapon==object::Reference{object::ActorChannelReference{0}});
    CHECK(result.consumed_bits==1);
    CHECK(reader.position()==1);
    CHECK(reader.read_bits(4)==std::vector<std::uint8_t>{0x0b});
}
TEST_CASE("Inventory ServerSetCurrentWeapon preserves package variant including export zero and maximum") {
    struct Fixture { std::array<std::uint8_t,5> bytes; std::uint32_t index; };
    // presence1 | (selector0 + index31bits) =>1+(index<<2),33bits.
    const Fixture fixtures[]{ {{{0x25,0,0,0,0}},9}, {{{1,0,0,0,0}},0},
        {{{0xfd,0xff,0xff,0xff,1}},0x7fffffffU} };
    for (const auto& fixture:fixtures) {
        CAPTURE(fixture.index);
        BitReader reader(fixture.bytes,33);
        const auto result=read_server_set_current_weapon_arguments(reader,33);
        CHECK(result.desired_weapon==object::Reference{object::PackageExportReference{fixture.index}});
        CHECK(result.consumed_bits==33);
        CHECK(reader.position()==33);
    }
}
TEST_CASE("Inventory ServerSetCurrentWeapon caller consumes Manager tag before the argument prefix") {
    // field23/b24 takes5bits; ch6args0x1b starts at5 =>0x377,18bits.
    const std::array<std::uint8_t,3> present{0x77,3,0};
    BitReader reader(present,18);
    REQUIRE(reader.read_bounded(24)==23);
    REQUIRE(reader.position()==5);
    const auto value=read_server_set_current_weapon_arguments(reader,13);
    CHECK(value.desired_weapon==object::Reference{object::ActorChannelReference{6}});
    CHECK(value.consumed_bits==13);
    CHECK(reader.position()==18);
    const std::array<std::uint8_t,1> absent{0x17};
    BitReader no_owner(absent,6);
    REQUIRE(no_owner.read_bounded(24)==23);
    const auto zero=read_server_set_current_weapon_arguments(no_owner,1);
    CHECK(zero.desired_weapon==object::Reference{object::ActorChannelReference{0}});
    CHECK(zero.consumed_bits==1);
    CHECK(no_owner.position()==6);
}
TEST_CASE("Inventory ServerSetCurrentWeapon preserves unaligned cursors and opaque suffixes") {
    for (std::size_t offset=0;offset<8;++offset) {
        CAPTURE(offset);
        const auto word=(0x55ULL&((1ULL<<offset)-1)) | (0x1bULL<<offset) |
            (0x2dULL<<(offset+13));
        std::array<std::uint8_t,4> bytes{};
        for (std::size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<std::uint8_t>(word>>(8*i));
        BitReader reader(bytes,offset+19);
        for (std::size_t i=0;i<offset;++i) reader.read_bit();
        const auto value=read_server_set_current_weapon_arguments(reader,13);
        CHECK(value.desired_weapon==object::Reference{object::ActorChannelReference{6}});
        CHECK(value.consumed_bits==13);
        CHECK(reader.position()==offset+13);
        CHECK(reader.read_bits(6)==std::vector<std::uint8_t>{0x2d});
        CHECK(reader.position()==offset+19);
    }
}
TEST_CASE("Inventory ServerSetCurrentWeapon rolls back every physically truncated actor and package argument") {
    const std::array<std::uint8_t,2> actor{0x1b,0};
    const std::array<std::uint8_t,5> package{0x25,0,0,0,0};
    for (const bool is_package:{false,true}) {
        const std::size_t bits=is_package?33:13;
        BitWriter joined;joined.write_bits(std::array<std::uint8_t,1>{5},3);
        if (is_package) joined.write_bits(package,bits); else joined.write_bits(actor,bits);
        const auto bytes=joined.payload_bytes();
        for (std::size_t cut=0;cut<bits;++cut) {
            CAPTURE(is_package);CAPTURE(cut);
            BitReader reader(bytes,3+cut);
            REQUIRE(reader.read_bits(3)==std::vector<std::uint8_t>{5});
            inventory_error([&]{(void)read_server_set_current_weapon_arguments(reader,bits);},"truncated");
            CHECK(reader.position()==3);
            if (cut!=0) CHECK(reader.read_bit());
        }
    }
}
TEST_CASE("Inventory ServerSetCurrentWeapon positive aggregate budgets include presence and roll back") {
    const std::array<std::uint8_t,2> actor{0x1b,0};
    const std::array<std::uint8_t,5> package{0x25,0,0,0,0};
    for (const bool is_package:{false,true}) {
        const std::size_t bits=is_package?33:13;
        for (std::size_t budget=0;budget<bits;++budget) {
            CAPTURE(is_package);CAPTURE(budget);
            BitReader reader(is_package?std::span<const std::uint8_t>{package}:
                std::span<const std::uint8_t>{actor},bits);
            inventory_error([&]{(void)read_server_set_current_weapon_arguments(reader,budget);},
                budget==0?"invalid_limit":"field_too_large");
            CHECK(reader.position()==0);
            CHECK(reader.read_bit());
        }
    }
    BitReader reader(actor,13);
    CHECK(read_server_set_current_weapon_arguments(reader,std::numeric_limits<std::size_t>::max()).consumed_bits==13);
}
TEST_CASE("Inventory ServerSetCurrentWeapon missing presence is truncated and invalid budget wins without mutation") {
    const std::array<std::uint8_t,0> bytes{};
    BitReader reader(bytes,0);
    inventory_error([&]{(void)read_server_set_current_weapon_arguments(reader,1);},"truncated");
    CHECK(reader.position()==0);
    inventory_error([&]{(void)read_server_set_current_weapon_arguments(reader,0);},"invalid_limit");
    CHECK(reader.position()==0);
}
