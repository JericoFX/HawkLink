#include "hawken/weapon_fire_rpc.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {
using hawken::wire::BitReader;
using hawken::wire::WireError;
namespace fire=hawken::game::weapon_fire;
constexpr fire::Schema dual_schema{35,26,27,32,true,true,true,true};

// Independent fixture bit copier; no production writer or decoder builds oracles.
struct Bits {
    std::vector<std::uint8_t> bytes;
    std::size_t size=0;
    void raw(std::uint32_t value,std::size_t width) {
        for (std::size_t bit=0;bit<width;++bit) {
            if (size%8==0) bytes.push_back(0);
            if ((value&(std::uint32_t{1}<<bit))!=0)
                bytes.back()|=static_cast<std::uint8_t>(1U<<(size%8));
            ++size;
        }
    }
};
struct Literal {
    std::array<std::uint8_t,2> bytes;
    std::size_t bits;
    fire::Kind kind;
    std::uint8_t mode;
    std::optional<bool> recent;
};
// Dual bound35: 26/27 take five tag bits; 32 takes six. BYTE has presence,
// then eight LSB-first bits. Extended stop has one additional raw Bool bit.
constexpr Literal literals[]{
    {{0x1a,0x00},6,fire::Kind::start_fire,0,std::nullopt},
    {{0x1b,0x00},6,fire::Kind::stop_fire,0,std::nullopt},
    {{0x3a,0x00},14,fire::Kind::start_fire,0,std::nullopt},
    {{0x7a,0x00},14,fire::Kind::start_fire,1,std::nullopt},
    {{0x3a,0x20},14,fire::Kind::start_fire,128,std::nullopt},
    {{0xfa,0x3f},14,fire::Kind::start_fire,255,std::nullopt},
    {{0xfb,0x1f},14,fire::Kind::stop_fire,127,std::nullopt},
    {{0x20,0x00},8,fire::Kind::stop_fire_extended,0,false},
    {{0xa0,0x00},8,fire::Kind::stop_fire_extended,0,true},
    {{0xe0,0x00},16,fire::Kind::stop_fire_extended,1,false},
    {{0xe0,0x80},16,fire::Kind::stop_fire_extended,1,true},
    {{0xe0,0xff},16,fire::Kind::stop_fire_extended,255,true},
};
void append(Bits& destination,const Literal& literal) {
    for (std::size_t bit=0;bit<literal.bits;++bit)
        destination.raw((literal.bytes[bit/8]>>(bit%8))&1U,1);
}
void skip(BitReader& reader,std::size_t count) {
    for (std::size_t bit=0;bit<count;++bit) static_cast<void>(reader.read_bit());
}
void check_value(const fire::Result& result,const Literal& literal) {
    CHECK(result.kind==literal.kind);
    CHECK(result.fire_mode==literal.mode);
    CHECK(result.recent_fire==literal.recent);
    CHECK(result.consumed_bits==literal.bits);
}
template<class Operation>
void expect_code(const char* code,Operation operation) {
    bool caught=false;
    try {operation();}
    catch (const WireError& error) {
        caught=true;
        CHECK(error.code==code);
        CHECK(std::string(error.what())=="Game wire data could not be read or validated.");
    }
    CHECK(caught);
}
void reject_unchanged(BitReader& reader,const fire::Schema& schema,
                      std::size_t budget,const char* code) {
    const auto before=reader.position();
    expect_code(code,[&] {static_cast<void>(fire::read_rpc(reader,schema,budget));});
    CHECK(reader.position()==before);
}
}

TEST_CASE("Weapon fire RPC decodes independent literal BYTE and raw Bool forms") {
    for (const auto& literal:literals) {
        CAPTURE(literal.bits);
        CAPTURE(literal.mode);
        BitReader reader(literal.bytes,literal.bits);
        check_value(fire::read_rpc(reader,dual_schema,literal.bits),literal);
        CHECK(reader.at_end());
    }
}

TEST_CASE("Weapon fire RPC preserves all BYTE modes and both extended Bool values") {
    for (std::uint32_t mode=0;mode<256;++mode) {
        CAPTURE(mode);
        for (unsigned variant=0;variant<4;++variant) {
            CAPTURE(variant);
            Bits input;
            const bool extended=variant>=2;
            input.raw(extended?32U:(variant==0?26U:27U),extended?6:5);
            input.raw(1,1);
            input.raw(mode,8);
            if (extended) input.raw(variant==3?1U:0U,1);
            BitReader reader(input.bytes,input.size);
            const auto result=fire::read_rpc(reader,dual_schema,input.size);
            CHECK(result.kind==(extended?fire::Kind::stop_fire_extended:
                (variant==0?fire::Kind::start_fire:fire::Kind::stop_fire)));
            CHECK(result.fire_mode==mode);
            CHECK(result.recent_fire==(extended?std::optional<bool>{variant==3}:std::nullopt));
            CHECK(result.consumed_bits==input.size);
            CHECK(reader.at_end());
        }
    }
}

TEST_CASE("Weapon fire RPC accepts unaligned prefixes and leaves opaque suffix bits intact") {
    for (std::size_t offset=0;offset<8;++offset) {
        CAPTURE(offset);
        for (const auto& literal:literals) {
            Bits input;
            input.raw(0x55,offset);
            append(input,literal);
            input.raw(0x65,7);
            BitReader reader(input.bytes,input.size);
            skip(reader,offset);
            check_value(fire::read_rpc(reader,dual_schema,literal.bits),literal);
            CHECK(reader.position()==offset+literal.bits);
            for (unsigned bit=0;bit<7;++bit)
                CHECK(reader.read_bit()==((0x65U&(1U<<bit))!=0));
            CHECK(reader.at_end());
        }
    }
}

TEST_CASE("Weapon fire RPC uses caller schemas instead of global extended field assumptions") {
    for (const std::uint32_t bound:{32U,34U}) {
        auto schema=dual_schema;
        schema.exclusive_bound=bound;
        schema.extended_stop_field_id.reset();
        const std::array<std::uint8_t,2> input{0xfa,0x3f};
        BitReader reader(input,14);
        const auto result=fire::read_rpc(reader,schema,14);
        CHECK(result.kind==fire::Kind::start_fire);
        CHECK(result.fire_mode==255);
        CHECK_FALSE(result.recent_fire);
        CHECK(result.consumed_bits==14);
    }
    // Secondary's field32 is LastShot, not an extended stop RPC.
    auto secondary=dual_schema;
    secondary.exclusive_bound=34;
    secondary.extended_stop_field_id.reset();
    const std::array<std::uint8_t,1> last_shot{0x20};
    BitReader unknown(last_shot,6);
    reject_unchanged(unknown,secondary,6,"invalid_value");

    const fire::Schema custom{8,1,2,4,true,true,true,true};
    const std::array<std::uint8_t,1> custom_stop{0x02}; // Three tag bits + absent BYTE.
    BitReader custom_reader(custom_stop,4);
    const auto result=fire::read_rpc(custom_reader,custom,4);
    CHECK(result.kind==fire::Kind::stop_fire);
    CHECK(result.fire_mode==0);
    CHECK(result.consumed_bits==4);
}

TEST_CASE("Weapon fire RPC rejects invalid schemas and unknown tags without advancing") {
    std::vector<fire::Schema> invalid;
    for (unsigned premise=0;premise<4;++premise) {
        auto schema=dual_schema;
        if (premise==0) schema.primary_package_map=false;
        if (premise==1) schema.weapon_cache_correct=false;
        if (premise==2) schema.parameter_order_correct=false;
        if (premise==3) schema.all_parameters_eligible=false;
        invalid.push_back(schema);
    }
    for (unsigned field=0;field<3;++field) {
        auto schema=dual_schema;
        if (field==0) schema.start_field_id=35;
        if (field==1) schema.stop_field_id=35;
        if (field==2) schema.extended_stop_field_id=35;
        invalid.push_back(schema);
    }
    auto schema=dual_schema;
    schema.stop_field_id=schema.start_field_id;invalid.push_back(schema);
    schema=dual_schema;schema.extended_stop_field_id=schema.start_field_id;invalid.push_back(schema);
    schema=dual_schema;schema.extended_stop_field_id=schema.stop_field_id;invalid.push_back(schema);
    schema=dual_schema;schema.exclusive_bound=32;invalid.push_back(schema);
    const std::array<std::uint8_t,1> input{0x1a};
    for (const auto& bad:invalid) {
        BitReader reader(input,6);
        reject_unchanged(reader,bad,64,"invalid_value");
    }
    schema=dual_schema;schema.exclusive_bound=0;
    BitReader zero_bound(input,6);
    reject_unchanged(zero_bound,schema,64,"invalid_limit");
    const std::array<std::uint8_t,1> unknown_tag{0x1c};
    BitReader unknown(unknown_tag,5);
    reject_unchanged(unknown,dual_schema,64,"invalid_value");
}

TEST_CASE("Weapon fire RPC enforces the entire tag and argument budget transactionally") {
    for (const auto& literal:literals) {
        for (std::size_t budget=0;budget<literal.bits;++budget) {
            CAPTURE(budget);
            CAPTURE(literal.bits);
            Bits input;input.raw(5,3);append(input,literal);
            BitReader reader(input.bytes,input.size);skip(reader,3);
            reject_unchanged(reader,dual_schema,budget,budget==0?"invalid_limit":"field_too_large");
        }
        BitReader reader(literal.bytes,literal.bits);
        check_value(fire::read_rpc(reader,dual_schema,std::numeric_limits<std::size_t>::max()),literal);
        CHECK(reader.at_end());
    }
}

TEST_CASE("Weapon fire RPC rolls back every physical truncation including unaligned input") {
    for (const auto& literal:literals) {
        for (std::size_t cut=0;cut<literal.bits;++cut) {
            CAPTURE(cut);
            CAPTURE(literal.bits);
            Bits input;input.raw(3,2);append(input,literal);
            BitReader reader(input.bytes,2+cut);skip(reader,2);
            reject_unchanged(reader,dual_schema,literal.bits,"truncated");
            if (cut>0) CHECK(reader.read_bit()==((literal.bytes[0]&1U)!=0));
        }
    }
}

TEST_CASE("Weapon fire RPC consumes one prefix rather than searching an opaque suffix") {
    Bits input;
    append(input,literals[0]); // Six bits, absent start BYTE.
    append(input,literals[8]); // Eight bits, absent extended BYTE + true Bool.
    input.raw(28,5); // Unknown next field remains opaque until explicitly decoded.
    BitReader reader(input.bytes,input.size);
    check_value(fire::read_rpc(reader,dual_schema,6),literals[0]);
    CHECK(reader.position()==6);
    check_value(fire::read_rpc(reader,dual_schema,8),literals[8]);
    CHECK(reader.position()==14);
    reject_unchanged(reader,dual_schema,5,"invalid_value");
    CHECK(reader.read_bounded(35)==28);
    CHECK(reader.at_end());
}
