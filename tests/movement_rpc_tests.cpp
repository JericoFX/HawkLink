#include "hawken/movement_rpc.hpp"
#include <doctest/doctest.h>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <ostream>
#include <vector>

namespace {
using namespace hawken;
constexpr wire::movement::Eligibility eligible{true,true,true,true};
// Independent test-only LSB fixture writer, not BitWriter or a production codec.
struct Bits {
    std::vector<std::uint8_t> bytes;
    std::size_t size=0;
    void raw(std::uint32_t value,std::size_t width) {
        for (std::size_t i=0;i<width;++i) {
            if (size%8==0) bytes.push_back(0);
            if ((value&(std::uint32_t{1}<<i))!=0)
                bytes.back()|=static_cast<std::uint8_t>(1U<<(size%8));
            ++size;
        }
    }
    void scalar(std::uint32_t value,control::Endian order) {
        for (std::size_t i=0;i<4;++i) {
            const auto shift=(order==control::Endian::little?i:3-i)*8;
            raw((value>>shift)&255U,8);
        }
    }
    void floating(float value,control::Endian order) {raw(1,1);scalar(std::bit_cast<std::uint32_t>(value),order);}
    void integer(std::uint32_t value,control::Endian order) {raw(1,1);scalar(value,order);}
    void byte(std::uint8_t value) {raw(1,1);raw(value,8);}
    void vector_small(std::array<std::int32_t,3> value) {
        raw(1,1);raw(0,5); // Presence1; bounded20 n0 takes5bits, bias2, component bound4.
        for (const auto component:value) raw(static_cast<std::uint32_t>(component+2),2);
    }
};
void single(Bits& bits,control::Endian order) {
    bits.raw(61,8); // Under bound294,59..61 use8bits: adding bit256 would exceed the bound.
    bits.floating(1.5F,order);bits.vector_small({1,-1,0});bits.vector_small({-2,0,1});
    bits.byte(0x28);bits.byte(0xab);bits.integer(0x80011234U,order);
}
void check_empty(const wire::movement::Move& move) {
    CHECK(move.time_stamp==0);CHECK(move.packed_acceleration==wire::actor::QuantizedPosition{});
    CHECK(move.client_location==wire::actor::QuantizedPosition{});CHECK(move.move_flags==0);
    CHECK(move.client_roll==0);CHECK(move.view==0);CHECK_FALSE(move.jump);
}
}

TEST_CASE("Movement RPC decodes three literal all-absent forms without consuming their suffix") {
    struct Fixture {std::array<std::uint8_t,3> bytes;std::size_t bits;wire::movement::Kind kind;};
    const Fixture fixtures[]{{{0x3d,0,0},14,wire::movement::Kind::server_move},
        {{0x3c,0,0},13,wire::movement::Kind::old_server_move},
        {{0x3b,0,0},18,wire::movement::Kind::dual_server_move}};
    for (const auto& fixture:fixtures) {
        CAPTURE(fixture.bits);wire::BitReader reader(fixture.bytes,24);
        const auto result=wire::movement::read_rpc(reader,control::Endian::little,eligible,fixture.bits);
        CHECK(result.kind==fixture.kind);CHECK(result.consumed_bits==fixture.bits);CHECK(reader.position()==fixture.bits);
        check_empty(result.current);CHECK(result.old_acceleration==std::array<std::uint8_t,3>{});
        if (fixture.kind==wire::movement::Kind::dual_server_move) {REQUIRE(result.pending);check_empty(*result.pending);}
        else CHECK_FALSE(result.pending);
        CHECK_FALSE(reader.read_bit());
    }
}

TEST_CASE("Movement RPC preserves packed vectors scalar endian jump roll and signed view at every alignment") {
    for (const auto order:{control::Endian::little,control::Endian::big}) {
        for (std::size_t offset=0;offset<8;++offset) {
            CAPTURE(offset);Bits bits;bits.raw(0,offset);single(bits,order);const auto end=bits.size;bits.raw(0x5a,7);
            wire::BitReader reader(bits.bytes,bits.size);for (std::size_t i=0;i<offset;++i) reader.read_bit();
            const auto result=wire::movement::read_rpc(reader,order,eligible,end-offset);
            CHECK(result.kind==wire::movement::Kind::server_move);CHECK(result.current.time_stamp==1.5F);
            CHECK(result.current.packed_acceleration==wire::actor::QuantizedPosition{1,-1,0});
            CHECK(result.current.client_location==wire::actor::QuantizedPosition{-2,0,1});
            CHECK(result.current.move_flags==0x28);CHECK(result.current.jump);CHECK(result.current.client_roll==0xab);
            CHECK(result.current.view==std::bit_cast<std::int32_t>(0x80011234U));CHECK_FALSE(result.pending);
            CHECK(result.consumed_bits==end-offset);CHECK(reader.position()==end);
            CHECK(reader.read_bits(7)==std::vector<std::uint8_t>{0x5a});
        }
    }
}

TEST_CASE("Movement RPC keeps Dual first and current movement fields in original Children order") {
    for (const auto order:{control::Endian::little,control::Endian::big}) {
        Bits bits;bits.raw(59,8);bits.floating(2.0F,order);bits.vector_small({0,1,-1});
        bits.byte(0x20);bits.integer(0x12345678,order);bits.floating(3.0F,order);
        bits.vector_small({1,0,-2});bits.vector_small({-1,1,0});bits.byte(0x10);bits.byte(0x7f);
        bits.integer(0xfedcba98,order);
        wire::BitReader reader(bits.bytes,bits.size);const auto result=wire::movement::read_rpc(reader,order,eligible,bits.size);
        REQUIRE(result.pending);CHECK(result.pending->time_stamp==2.0F);CHECK(result.pending->jump);
        CHECK(result.pending->move_flags==0x20);CHECK(result.pending->view==0x12345678);
        CHECK(result.pending->packed_acceleration==wire::actor::QuantizedPosition{0,1,-1});
        CHECK(result.pending->client_location==wire::actor::QuantizedPosition{});CHECK(result.pending->client_roll==0);
        CHECK(result.current.time_stamp==3.0F);CHECK_FALSE(result.current.jump);CHECK(result.current.move_flags==0x10);
        CHECK(result.current.packed_acceleration==wire::actor::QuantizedPosition{1,0,-2});
        CHECK(result.current.client_location==wire::actor::QuantizedPosition{-1,1,0});
        CHECK(result.current.client_roll==0x7f);CHECK(result.current.view==std::bit_cast<std::int32_t>(0xfedcba98U));
        CHECK(result.consumed_bits==bits.size);CHECK(reader.at_end());
    }
}

TEST_CASE("Movement RPC keeps Old acceleration bytes raw and jump separate from other flags") {
    for (const auto order:{control::Endian::little,control::Endian::big}) {
        for (const auto flags:{std::uint8_t{0},std::uint8_t{0x20},std::uint8_t{0xdf},std::uint8_t{0xff}}) {
            Bits bits;bits.raw(60,8);bits.floating(-0.5F,order);bits.byte(1);bits.byte(128);bits.byte(255);bits.byte(flags);
            wire::BitReader reader(bits.bytes,bits.size);const auto result=wire::movement::read_rpc(reader,order,eligible,bits.size);
            CHECK(result.kind==wire::movement::Kind::old_server_move);CHECK(result.current.time_stamp==-0.5F);
            CHECK(result.old_acceleration==std::array<std::uint8_t,3>{1,128,255});CHECK(result.current.move_flags==flags);
            CHECK(result.current.jump==(flags==0x20 || flags==0xff));CHECK_FALSE(result.pending);
            CHECK(result.current.packed_acceleration==wire::actor::QuantizedPosition{});
            CHECK(result.current.client_location==wire::actor::QuantizedPosition{});CHECK(result.current.view==0);
        }
    }
}

TEST_CASE("Movement RPC preserves cursor for every truncated input and insufficient aggregate budget") {
    Bits bits;bits.raw(5,3);single(bits,control::Endian::little);const auto full=bits.size;
    for (std::size_t end=3;end<full;++end) {
        CAPTURE(end);wire::BitReader reader(bits.bytes,end);reader.read_bits(3);
        CHECK_THROWS_AS(wire::movement::read_rpc(reader,control::Endian::little,eligible,full-3),wire::WireError);
        CHECK(reader.position()==3);
    }
    for (std::size_t budget=0;budget<full-3;++budget) {
        CAPTURE(budget);wire::BitReader reader(bits.bytes,full);reader.read_bits(3);
        CHECK_THROWS_AS(wire::movement::read_rpc(reader,control::Endian::little,eligible,budget),wire::WireError);
        CHECK(reader.position()==3);
    }
}

TEST_CASE("Movement RPC rejects unknown fields false premises invalid endian and nonfinite time transactionally") {
    for (const auto field:{0U,58U,62U,293U,294U}) {
        // Bound294 field293 has9bits. Attempted raw294 cannot become an
        // out-of-bound decoded ID: it maps to unsupported38 plus an opaque bit.
        Bits bits;bits.raw(field,field>=293?9:8);bits.raw(0,10);
        wire::BitReader reader(bits.bytes,bits.size);
        CHECK_THROWS_AS(wire::movement::read_rpc(reader,control::Endian::little,eligible,bits.size),wire::WireError);
        CHECK(reader.position()==0);
    }
    for (unsigned missing=0;missing<4;++missing) {
        auto policy=eligible;if(missing==0)policy.primary_package_map=false;
        if(missing==1)policy.controller_cache_correct=false;if(missing==2)policy.parameter_order_correct=false;
        if(missing==3)policy.all_parameters_eligible=false;
        const std::array<std::uint8_t,2> bytes{0x3d,0};wire::BitReader reader(bytes,14);
        CHECK_THROWS_AS(wire::movement::read_rpc(reader,control::Endian::little,policy,14),wire::WireError);
        CHECK(reader.position()==0);
    }
    const std::array<std::uint8_t,2> bytes{0x3d,0};wire::BitReader bad_order(bytes,14);
    CHECK_THROWS_AS(wire::movement::read_rpc(bad_order,static_cast<control::Endian>(99),eligible,14),wire::WireError);
    CHECK(bad_order.position()==0);
    for (const auto storage:{0x7f800000U,0xff800000U,0x7fc00001U}) {
        Bits bits;bits.raw(61,8);bits.raw(1,1);bits.scalar(storage,control::Endian::little);bits.raw(0,5);
        wire::BitReader reader(bits.bytes,bits.size);
        CHECK_THROWS_AS(wire::movement::read_rpc(reader,control::Endian::little,eligible,bits.size),wire::WireError);
        CHECK(reader.position()==0);
    }
}
