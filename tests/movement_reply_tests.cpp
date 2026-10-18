#include "hawken/movement_reply.hpp"
#include <doctest/doctest.h>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace {
using hawken::control::Endian;
using hawken::wire::BitReader;
using hawken::wire::BitWriter;
using hawken::wire::WireError;
using hawken::wire::movement::encode_client_ack_good_move;
using Bytes=std::vector<std::uint8_t>;
struct Golden {
    std::uint32_t ieee_bits;
    std::array<std::uint8_t,6> little,big;
};
// Independent bit arithmetic: tag66 takes8bits under294, presence is bit8,
// then four IEEE bytes start atbit9. No production writer derives these bytes.
constexpr Golden goldens[]{
    {0x3f800000,{{0x42,0x01,0x00,0x00,0x7f,0x00}},{{0x42,0x7f,0x00,0x01,0x00,0x00}}},
    {0xbf800000,{{0x42,0x01,0x00,0x00,0x7f,0x01}},{{0x42,0x7f,0x01,0x01,0x00,0x00}}},
    {0x3fc00000,{{0x42,0x01,0x00,0x80,0x7f,0x00}},{{0x42,0x7f,0x80,0x01,0x00,0x00}}},
    {0x7f7fffff,{{0x42,0xff,0xff,0xff,0xfe,0x00}},{{0x42,0xff,0xfe,0xfe,0xff,0x01}}},
    {0x00000001,{{0x42,0x03,0x00,0x00,0x00,0x00}},{{0x42,0x01,0x00,0x00,0x02,0x00}}},
    {0x80000001,{{0x42,0x03,0x00,0x00,0x00,0x01}},{{0x42,0x01,0x01,0x00,0x02,0x00}}},
};
template<class Operation>
void expect_code(const char* expected,Operation operation) {
    bool caught=false;
    try {operation();}
    catch(const WireError& error) {
        caught=true;CHECK(error.code==expected);
        CHECK(std::string(error.what())=="Game wire data could not be read or validated.");
    }
    CHECK(caught);
}
}

TEST_CASE("Movement reply ACK matches independent finite scalar endian goldens") {
    for(const auto& golden:goldens) {
        CAPTURE(golden.ieee_bits);
        for(const auto order:{Endian::little,Endian::big}) {
            const auto encoded=encode_client_ack_good_move(std::bit_cast<float>(golden.ieee_bits),66,294,order,41);
            CHECK(encoded.logical_bits==41);
            const auto& expected=order==Endian::little?golden.little:golden.big;
            CHECK(encoded.payload==Bytes(expected.begin(),expected.end()));
            REQUIRE(encoded.payload.size()==6);
            CHECK((encoded.payload.back()&0xfeU)==0);
        }
    }
}
TEST_CASE("Movement reply ACK omits both signed zero timestamp values") {
    for(const auto bits:{0U,0x80000000U}) {
        for(const auto order:{Endian::little,Endian::big}) {
            const auto encoded=encode_client_ack_good_move(std::bit_cast<float>(bits),66,294,order,9);
            CHECK(encoded.logical_bits==9);CHECK(encoded.payload==Bytes{0x42,0x00});
        }
    }
}
TEST_CASE("Movement reply ACK uses actual value dependent bounded tag widths") {
    struct Field {std::uint32_t id,bound;std::size_t bits;Bytes expected;};
    const Field fields[]{
        {0,1,1,{0x00}},{1,2,2,{0x01}},
        {0,294,10,{0x00,0x00}},{66,294,9,{0x42,0x00}},
        {293,294,10,{0x25,0x01}},
        {0xfffffffeU,0xffffffffU,33,{0xfe,0xff,0xff,0xff,0x00}}};
    for(const auto& field:fields) {
        CAPTURE(field.id);
        const auto encoded=encode_client_ack_good_move(0.0F,field.id,field.bound,Endian::little,field.bits);
        CHECK(encoded.logical_bits==field.bits);CHECK(encoded.payload==field.expected);
    }
}
TEST_CASE("Movement reply ACK rejects every insufficient aggregate output budget") {
    for(const float timestamp:{0.0F,1.0F}) {
        const std::size_t required=timestamp==0.0F?9:41;
        for(std::size_t budget=0;budget<required;++budget) {
            CAPTURE(budget);CAPTURE(required);
            expect_code(budget==0?"invalid_limit":"field_too_large",[&] {
                static_cast<void>(encode_client_ack_good_move(timestamp,66,294,Endian::little,budget));
            });
        }
        const auto encoded=encode_client_ack_good_move(timestamp,66,294,Endian::little,
            std::numeric_limits<std::size_t>::max());
        CHECK(encoded.logical_bits==required);
        CHECK(encoded.payload==(timestamp==0.0F?Bytes{0x42,0x00}:Bytes{0x42,0x01,0x00,0x00,0x7f,0x00}));
    }
}
TEST_CASE("Movement reply ACK rejects invalid limits fields endian and nonfinite scalars") {
    expect_code("invalid_limit",[] {static_cast<void>(encode_client_ack_good_move(1.0F,0,0,Endian::little,64));});
    for(const auto field:{294U,0xffffffffU})
        expect_code("invalid_value",[&] {static_cast<void>(encode_client_ack_good_move(1.0F,field,294,Endian::little,64));});
    expect_code("invalid_value",[] {
        static_cast<void>(encode_client_ack_good_move(1.0F,66,294,static_cast<Endian>(99),64));
    });
    for(const auto bits:{0x7f800000U,0xff800000U,0x7fc00001U,0xffc00001U,0x7f800001U}) {
        CAPTURE(bits);
        expect_code("invalid_value",[&] {
            static_cast<void>(encode_client_ack_good_move(std::bit_cast<float>(bits),66,294,Endian::little,64));
        });
    }
}
TEST_CASE("Movement reply ACK fragments compose unaligned without a terminator or suffix consumption") {
    for(const auto order:{Endian::little,Endian::big}) {
        const auto encoded=encode_client_ack_good_move(1.0F,66,294,order,41);
        REQUIRE(encoded.logical_bits==41);
        for(std::size_t offset=0;offset<8;++offset) {
            CAPTURE(offset);
            BitWriter combined;
            for(std::size_t bit=0;bit<offset;++bit) combined.write_bit(true);
            combined.write_bits(encoded.payload,encoded.logical_bits);
            combined.write_bits(std::array<std::uint8_t,1>{0x0d},4);
            const auto bytes=combined.payload_bytes();
            BitReader reader(bytes,offset+45);
            for(std::size_t bit=0;bit<offset;++bit) REQUIRE(reader.read_bit());
            CHECK(reader.read_bounded(294)==66);REQUIRE(reader.read_bit());
            const Bytes scalar=order==Endian::little?Bytes{0x00,0x00,0x80,0x3f}:Bytes{0x3f,0x80,0x00,0x00};
            CHECK(reader.read_bits(32)==scalar);CHECK(reader.position()==offset+41);
            CHECK(reader.read_bits(4)==Bytes{0x0d});CHECK(reader.at_end());
        }
    }
}
