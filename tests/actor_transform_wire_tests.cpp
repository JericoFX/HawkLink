#include "hawken/actor_transform_wire.hpp"
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::actor;
template<class Action>
void expect_code(Action action, const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("actor transform accepted rejected input");
}
void position_literal(const QuantizedPosition& value,
                      const std::vector<std::uint8_t>& bytes, std::size_t bits) {
    BitWriter writer;
    CHECK(write_position(writer,value,bits) == bits);
    CHECK(writer.bit_count() == bits);
    CHECK(writer.payload_bytes() == bytes);
    BitReader reader(bytes,bits);
    const auto result=read_position(reader,bits);
    CHECK(result.value == value);
    CHECK(result.consumed_bits == bits);
    CHECK(reader.position() == bits);
}
void rotation_literal(const Rotation& value,
                      const std::vector<std::uint8_t>& bytes, std::size_t bits) {
    BitWriter writer;
    CHECK(write_rotation(writer,value,bits) == bits);
    CHECK(writer.bit_count() == bits);
    CHECK(writer.payload_bytes() == bytes);
    BitReader reader(bytes,bits);
    const auto result=read_rotation(reader,bits);
    CHECK(result.value == value);
    CHECK(result.consumed_bits == bits);
    CHECK(reader.position() == bits);
}
}

// Literal expectations come from literal-vectors.json and its independent
// fixed-width string derivation, never from BitWriter or the codec under test.
// zero: n0 header00000; biased xyz2,2,2 ->10,10,10 in LSB order =11bits.
TEST_CASE("Actor position zero has literal five-bit n0 header") {
    position_literal({0,0,0},{0x40,0x05},11);
}
TEST_CASE("Actor position preserves signed units in literal biased fields") {
    position_literal({1,-1,0},{0xe0,0x04},11);
}
TEST_CASE("Actor position n3 uses five header bits") {
    position_literal({8,-8,0},{0x03,0x23,0x08},20);
}
TEST_CASE("Actor position n4 uses four header bits") {
    position_literal({16,-16,1},{0x04,0x43,0x21},22);
}
TEST_CASE("Actor position n15 retains four-bit header at transition") {
    position_literal({32768,-32768,0},{0x0f,0x00,0x18,0x00,0x10,0x00,0x40},55);
}
TEST_CASE("Actor position n16 restores fifth header bit") {
    position_literal({65536,-65536,1},{0x10,0x00,0x60,0x00,0x80,0x02,0x00,0x04},59);
}
TEST_CASE("Actor position includes exact signed range endpoints") {
    position_literal({1048575,-1048576,0},{0xf3,0xff,0xff,0x03,0,0,0,0,0x08},68);
}
TEST_CASE("Actor position accepts native oversized decoded width") {
    const std::vector<std::uint8_t> bytes{0x13,0,0,0x02,0,0x40,0,0,0x08};
    BitReader reader(bytes,68);
    const auto result=read_position(reader,68);
    const QuantizedPosition zero{0,0,0};
    CHECK(result.value == zero);
    CHECK(result.consumed_bits == 68);
    CHECK(reader.position() == 68);
}
TEST_CASE("Actor position composes at every bit offset and preserves suffix") {
    struct Fixture { std::size_t offset; std::vector<std::uint8_t> bytes; };
    const std::vector<Fixture> fixtures{
        {0,{0xe0,0x2c}}, {1,{0xc1,0x59}}, {2,{0x81,0xb3}}, {3,{0x05,0x67,0x01}},
        {4,{0x0d,0xce,0x02}}, {5,{0x0d,0x9c,0x05}}, {6,{0x2d,0x38,0x0b}}, {7,{0x2d,0x70,0x16}}};
    const std::array<bool,7> prefix{true,false,true,true,false,true,false};
    const QuantizedPosition value{1,-1,0};
    for (const auto& fixture:fixtures) {
        CAPTURE(fixture.offset);
        BitReader reader(fixture.bytes,fixture.offset+14);
        for (std::size_t i=0;i<fixture.offset;++i) CHECK(reader.read_bit() == prefix[i]);
        const auto result=read_position(reader,11);
        CHECK(result.value == value);
        CHECK(result.consumed_bits == 11);
        CHECK(reader.position() == fixture.offset+11);
        CHECK(reader.read_bit()); CHECK_FALSE(reader.read_bit()); CHECK(reader.read_bit());
        BitWriter writer;
        for (std::size_t i=0;i<fixture.offset;++i) writer.write_bit(prefix[i]);
        CHECK(write_position(writer,value,11) == 11);
        writer.write_bit(true); writer.write_bit(false); writer.write_bit(true);
        CHECK(writer.payload_bytes() == fixture.bytes);
        CHECK(writer.bit_count() == fixture.offset+14);
    }
}
TEST_CASE("Actor position all truncated prefixes fail at native partial cursor") {
    const std::vector<std::uint8_t> bytes{0xe0,0x04};
    for (std::size_t length=0;length<11;++length) {
        CAPTURE(length);
        BitReader reader(bytes,length);
        expect_code([&]{ (void)read_position(reader,512); },"truncated");
        CHECK(reader.position() == length);
    }
}
TEST_CASE("Actor position budget rejects coordinates before reading them") {
    const std::vector<std::uint8_t> bytes{0xe0,0x04};
    BitReader reader(bytes,11);
    expect_code([&]{ (void)read_position(reader,10); },"field_too_large");
    CHECK(reader.position() == 5);
}
TEST_CASE("Actor position four-bit header is measured for budget") {
    const std::vector<std::uint8_t> bytes{0x04,0x43,0x21};
    BitReader reader(bytes,22);
    expect_code([&]{ (void)read_position(reader,21); },"field_too_large");
    CHECK(reader.position() == 4);
}
TEST_CASE("Actor position tiny header budget preserves reader") {
    const std::vector<std::uint8_t> bytes{0x40,0x05};
    BitReader reader(bytes,11);
    expect_code([&]{ (void)read_position(reader,4); },"field_too_large");
    CHECK(reader.position() == 0);
}
TEST_CASE("Actor position zero read budget preserves reader") {
    const std::vector<std::uint8_t> bytes{0x40,0x05};
    BitReader reader(bytes,11);
    expect_code([&]{ (void)read_position(reader,0); },"invalid_limit");
    CHECK(reader.position() == 0);
}
TEST_CASE("Actor position invalid values leave existing writer unchanged") {
    const std::array<std::int32_t,4> values{1048576,-1048577,
        std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()};
    for (const auto value:values) {
        for (std::size_t component=0;component<3;++component) {
            BitWriter writer; writer.write_bit(true);
            const auto before=writer.packet_bytes();
            QuantizedPosition xyz{0,0,0}; xyz[component]=value;
            expect_code([&]{ (void)write_position(writer,xyz,512); },"invalid_value");
            CHECK(writer.bit_count() == 1); CHECK(writer.packet_bytes() == before);
        }
    }
}
TEST_CASE("Actor position insufficient write budget preserves existing bits") {
    BitWriter writer; writer.write_bit(true); writer.write_bit(false);
    const auto before=writer.packet_bytes();
    expect_code([&]{ (void)write_position(writer,{0,0,0},10); },"field_too_large");
    CHECK(writer.bit_count() == 2); CHECK(writer.packet_bytes() == before);
}
TEST_CASE("Actor position zero write budget preserves existing bits") {
    BitWriter writer; writer.write_bit(true);
    const auto before=writer.packet_bytes();
    expect_code([&]{ (void)write_position(writer,{0,0,0},0); },"invalid_limit");
    CHECK(writer.bit_count() == 1); CHECK(writer.packet_bytes() == before);
}
TEST_CASE("Actor position caller budgets need no artificial upper ceiling") {
    BitWriter writer;
    CHECK(write_position(writer,{0,0,0},std::numeric_limits<std::size_t>::max()) == 11);
    CHECK(writer.payload_bytes() == std::vector<std::uint8_t>{0x40,0x05});
    const std::vector<std::uint8_t> bytes{0x40,0x05};
    BitReader reader(bytes,11);
    CHECK(read_position(reader,512).consumed_bits == 11);
}

TEST_CASE("Actor rotation absent components occupy three presence bits") {
    rotation_literal({0,0,0},{0x00},3);
}
TEST_CASE("Actor rotation first byte follows presence without alignment") {
    rotation_literal({256,0,0},{0x03,0x00},11);
}
TEST_CASE("Actor rotation middle component retains full byte") {
    rotation_literal({0,65280,0},{0xfe,0x03},11);
}
TEST_CASE("Actor rotation three bytes retain high-bit and max units") {
    rotation_literal({256,32768,65280},{0x03,0x02,0xfe,0x07},27);
}
TEST_CASE("Actor rotation accepts present zero without canonical rejection") {
    const std::vector<std::uint8_t> bytes{0x01,0x00};
    BitReader reader(bytes,11);
    const auto result=read_rotation(reader,11);
    const Rotation zero{0,0,0};
    CHECK(result.value == zero); CHECK(result.consumed_bits == 11);
    CHECK(reader.position() == 11);
}
TEST_CASE("Actor rotation raw byte truncation preserves cursor after presence") {
    const std::vector<std::uint8_t> bytes{0x03,0x00};
    for (std::size_t length=1;length<=8;++length) {
        CAPTURE(length);
        BitReader reader(bytes,length);
        expect_code([&]{ (void)read_rotation(reader,512); },"truncated");
        CHECK(reader.position() == 1);
    }
}
TEST_CASE("Actor rotation absent tail truncation retains consumed prefix") {
    const std::vector<std::uint8_t> bytes{0x03,0x00};
    for (const std::size_t length:std::array<std::size_t,3>{0,9,10}) {
        BitReader reader(bytes,length);
        expect_code([&]{ (void)read_rotation(reader,512); },"truncated");
        CHECK(reader.position() == length);
    }
}
TEST_CASE("Actor rotation byte budget checked before raw read") {
    const std::vector<std::uint8_t> bytes{0x03,0x00};
    BitReader reader(bytes,11);
    expect_code([&]{ (void)read_rotation(reader,8); },"field_too_large");
    CHECK(reader.position() == 1);
}
TEST_CASE("Actor rotation checks budget before every presence bit") {
    const std::vector<std::uint8_t> bytes{0x00};
    BitReader reader(bytes,3);
    expect_code([&]{ (void)read_rotation(reader,2); },"field_too_large");
    CHECK(reader.position() == 2);
}
TEST_CASE("Actor rotation zero read budget preserves reader") {
    const std::vector<std::uint8_t> bytes{0x00};
    BitReader reader(bytes,3);
    expect_code([&]{ (void)read_rotation(reader,0); },"invalid_limit");
    CHECK(reader.position() == 0);
}
TEST_CASE("Actor rotation rejects unaligned units in every component") {
    const std::array<std::uint16_t,4> values{1,255,257,65535};
    for (const auto value:values) {
        for (std::size_t component=0;component<3;++component) {
            BitWriter writer; writer.write_bit(true);
            const auto before=writer.packet_bytes();
            Rotation rotation{0,0,0}; rotation[component]=value;
            expect_code([&]{ (void)write_rotation(writer,rotation,512); },"invalid_value");
            CHECK(writer.bit_count() == 1); CHECK(writer.packet_bytes() == before);
        }
    }
}
TEST_CASE("Actor rotation whole write budget preflight prevents partial output") {
    BitWriter writer; writer.write_bit(true);
    const auto before=writer.packet_bytes();
    expect_code([&]{ (void)write_rotation(writer,{256,32768,65280},26); },"field_too_large");
    CHECK(writer.bit_count() == 1); CHECK(writer.packet_bytes() == before);
}
TEST_CASE("Actor rotation zero write budget preserves writer") {
    BitWriter writer; writer.write_bit(true);
    const auto before=writer.packet_bytes();
    expect_code([&]{ (void)write_rotation(writer,{0,0,0},0); },"invalid_limit");
    CHECK(writer.bit_count() == 1); CHECK(writer.packet_bytes() == before);
}
TEST_CASE("Actor rotation budgets may exceed field maximum") {
    BitWriter writer;
    CHECK(write_rotation(writer,{0,0,0},std::numeric_limits<std::size_t>::max()) == 3);
    const std::vector<std::uint8_t> bytes{0};
    BitReader reader(bytes,3);
    CHECK(read_rotation(reader,512).consumed_bits == 3);
}
TEST_CASE("Actor position and rotation share unaligned cursor with literal payload") {
    // 11bits position1,-1,0 followed immediately by27bits rotation1,80,ff.
    const std::vector<std::uint8_t> bytes{0xe0,0x1c,0x10,0xf0,0x3f};
    BitReader reader(bytes,38);
    const auto position=read_position(reader,11);
    const QuantizedPosition expected_position{1,-1,0};
    CHECK(position.value == expected_position); CHECK(reader.position() == 11);
    const auto rotation=read_rotation(reader,27);
    const Rotation expected_rotation{256,32768,65280};
    CHECK(rotation.value == expected_rotation); CHECK(rotation.consumed_bits == 27);
    CHECK(reader.position() == 38);
    BitWriter writer;
    CHECK(write_position(writer,expected_position,11) == 11);
    CHECK(write_rotation(writer,expected_rotation,27) == 27);
    CHECK(writer.bit_count() == 38); CHECK(writer.payload_bytes() == bytes);
}

TEST_CASE("BitWriter raw empty payload omits packet terminator") {
    BitWriter writer;
    CHECK(writer.payload_bytes().empty());
    CHECK(writer.packet_bytes() == std::vector<std::uint8_t>{1});
    CHECK(writer.bit_count() == 0);
}
TEST_CASE("BitWriter raw aligned payload preserves bytes and marker method") {
    BitWriter writer;
    const std::vector<std::uint8_t> input{0xa5};
    writer.write_bits(input,8);
    CHECK(writer.payload_bytes() == input);
    CHECK(writer.packet_bytes() == std::vector<std::uint8_t>{0xa5,1});
    CHECK(writer.bit_count() == 8);
}
TEST_CASE("BitWriter raw partial payload keeps zero high bits without mutation") {
    BitWriter writer; writer.write_bit(true); writer.write_bit(false); writer.write_bit(true);
    const std::vector<std::uint8_t> expected{5};
    CHECK(writer.payload_bytes() == expected); CHECK(writer.payload_bytes() == expected);
    CHECK(writer.packet_bytes() == std::vector<std::uint8_t>{13});
    CHECK(writer.bit_count() == 3);
    writer.write_bit(true);
    CHECK(writer.payload_bytes() == std::vector<std::uint8_t>{13});
    CHECK(writer.bit_count() == 4);
}
