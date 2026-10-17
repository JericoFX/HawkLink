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
