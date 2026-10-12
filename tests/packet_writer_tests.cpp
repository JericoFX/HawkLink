#include "hawken/packet_writer.hpp"
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace hawken::wire;

template<class Action>
void expect_code(Action action, const char* expected) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == expected);
        CHECK(std::string(error.what()).find("never-echo-private") == std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("packet writer accepted rejected input");
}

PacketRecord ack(std::uint16_t id) {
    PacketRecord record;
    record.is_ack=true;
    record.ack_packet_id=id;
    return record;
}
PacketRecord data(const Bunch& bunch) {
    PacketRecord record;
    record.bunch=bunch;
    return record;
}
Packet single(std::uint16_t id, const PacketRecord& record) {
    Packet packet;
    packet.packet_id=id;
    packet.records.push_back(record);
    return packet;
}
}

// Independent literals: implementation/evidence/packet-writer-red/
// literal-vectors.json contains every field's bit offset/width/value.
// Oracle uses integer placement plus a closed bounded-width formula, never
// C++ BitWriter/encode_packet/decode_packet. Public synthetic inputs only.
TEST_CASE("Packet writer encodes empty packet with header and final marker") {
    const std::vector<std::uint8_t> expected{0,0x40}; // ID0[0..13], marker[14].
    CHECK(encode_packet(Packet{},2) == expected);
    CHECK(encode_packet(Packet{},65507) == expected);
}
TEST_CASE("Packet writer anchors literal ACK fields without bunch metadata") {
    // ID123[0..13], selector1[14], ACK234[15..28], marker[29].
    const std::vector<std::uint8_t> expected{0x23,0x41,0x1a,0x21};
    CHECK(encode_packet(single(0x123,ack(0x234)),16) == expected);
    const auto read=decode_packet(expected,16);
    CHECK(read.packet_id == 0x123);
    REQUIRE(read.records.size() == 1);
    CHECK(read.records[0].is_ack);
    CHECK(read.records[0].ack_packet_id == 0x234);
}
TEST_CASE("Packet writer preserves maximum packet and ACK identifiers") {
    const std::vector<std::uint8_t> expected{0xff,0xff,0xff,0x3f};
    CHECK(encode_packet(single(0x3fff,ack(0x3fff)),4) == expected);
}
TEST_CASE("Packet writer anchors reliable open bunch with conditional fields") {
    Bunch bunch;
    bunch.control=true; bunch.open=true; bunch.reliable=true;
    bunch.channel=3; bunch.sequence=7; bunch.channel_type=2;
    bunch.payload_bits=5; bunch.payload={0x15};
    // ID5[0..13]; selector0[14]; flags1[15]; open1[16]; close0[17];
    // reliable1[18]; channel3[19..29]; seq7[30..39]; type2[40..42];
    // length5/bound128[43..49]; payload15[50..54]; marker[55].
    const std::vector<std::uint8_t> expected{0x05,0x80,0x1d,0xc0,0x01,0x2a,0xd4};
    CHECK(encode_packet(single(5,data(bunch)),16) == expected);
    const auto read=decode_packet(expected,16);
    REQUIRE(read.records.size() == 1);
    const auto& decoded=read.records[0].bunch;
    CHECK(decoded.control); CHECK(decoded.open); CHECK_FALSE(decoded.close);
    CHECK(decoded.reliable); CHECK(decoded.channel == 3);
    CHECK(decoded.sequence == 7); CHECK(decoded.channel_type == 2);
    CHECK(decoded.payload_bits == 5); CHECK(decoded.payload == bunch.payload);
}
TEST_CASE("Packet writer omits absent conditional fields and permits empty payload") {
    Bunch bunch;
    bunch.channel=1;
    const std::vector<std::uint8_t> expected{0x03,0x00,0x02,0x00,0x08};
    CHECK(encode_packet(single(3,data(bunch)),16) == expected);
}
TEST_CASE("Packet writer copies unaligned ninebit payload with canonical tail") {
    Bunch bunch;
    bunch.control=true; bunch.open=true; bunch.reliable=true;
    bunch.channel=2; bunch.sequence=0x3ff; bunch.channel_type=1;
    bunch.payload_bits=9; bunch.payload={0x95,0x01};
    // Payload starts[50], ends[58], marker[59]. Header/counters are independent.
    const std::vector<std::uint8_t> expected{0x07,0x80,0x15,0xc0,0xff,0x49,0x54,0x0e};
    CHECK(encode_packet(single(7,data(bunch)),16) == expected);
    const auto read=decode_packet(expected,16);
    REQUIRE(read.records.size() == 1);
    CHECK(read.records[0].bunch.sequence == 0x3ff);
    CHECK(read.records[0].bunch.payload_bits == 9);
    CHECK(read.records[0].bunch.payload == bunch.payload);
}
TEST_CASE("Packet writer concatenates mixed records without byte alignment") {
    Bunch first;
    first.control=true; first.open=true; first.close=true;
    first.channel=0x7ff; first.channel_type=7;
    first.payload_bits=9; first.payload={0x95,1};
    Bunch second;
    second.reliable=true; second.channel=2; second.sequence=0x3ff;
    Packet packet;
    packet.packet_id=0x3fff;
    packet.records={ack(7),data(first),ack(0x3fff),data(second)};
    const std::vector<std::uint8_t> expected{
        0xff,0xff,0x03,0xc0,0xfd,0xff,0x09,0x95,0xff,0xff,0x14,0xc0,0xff,0x00,0x08};
    CHECK(encode_packet(packet,32) == expected);
    const auto read=decode_packet(expected,32);
    REQUIRE(read.records.size() == 4);
    CHECK(read.records[0].is_ack); CHECK(read.records[2].is_ack);
    CHECK(read.records[1].bunch.channel == 0x7ff);
    CHECK(read.records[1].bunch.channel_type == 7);
    CHECK(read.records[1].bunch.open); CHECK(read.records[1].bunch.close);
    CHECK(read.records[3].bunch.sequence == 0x3ff);
}
TEST_CASE("Packet writer adds a separate marker byte after byte aligned data") {
    Bunch bunch;
    bunch.payload_bits=5; bunch.payload={0x15};
    // Data40bits, marker bit40: an additional byte01, not payload padding.
    const std::vector<std::uint8_t> expected{0,0,0,0x50,0xa8,0x01};
    CHECK(encode_packet(single(0,data(bunch)),16) == expected);
}
TEST_CASE("Packet writer encodes value dependent lengths for nonpower payload bound") {
    Bunch eight;
    eight.channel=1; eight.payload_bits=8; eight.payload={0x95};
    Bunch seven=eight;
    seven.payload_bits=7; seven.payload={0x15};
    // Bound136: length8 uses7bits, length7 uses8bits. Payload ends[42] both.
    const std::vector<std::uint8_t> short_length{0x01,0x00,0x02,0x80,0xa8,0x0c};
    const std::vector<std::uint8_t> long_length{0x01,0x00,0x02,0x70,0x50,0x09};
    CHECK(encode_packet(single(1,data(eight)),17) == short_length);
    CHECK(encode_packet(single(1,data(seven)),17) == long_length);
    const auto short_read=decode_packet(short_length,17);
    const auto long_read=decode_packet(long_length,17);
    REQUIRE(short_read.records.size() == 1);
    REQUIRE(long_read.records.size() == 1);
    CHECK(short_read.records[0].bunch.payload_bits == 8);
    CHECK(long_read.records[0].bunch.payload_bits == 7);
}
TEST_CASE("Packet writer close without open or reliable omits channel type") {
    Bunch bunch;
    bunch.control=true; bunch.close=true; bunch.channel=1;
    const std::vector<std::uint8_t> expected{0x02,0x80,0x0a,0x00,0x20};
    CHECK(encode_packet(single(2,data(bunch)),16) == expected);
}
TEST_CASE("Packet writer includes marker in exact final capacity and rejects plus one") {
    Bunch bunch;
    bunch.payload_bits=29; bunch.payload={0x55,0x55,0x55,0x15};
    // data63bits + marker63 =64bits exactly8bytes.
    const std::vector<std::uint8_t> expected{0,0,0,0xd0,0x55,0x55,0x55,0xd5};
    CHECK(encode_packet(single(0,data(bunch)),8) == expected);
    // data64bits would fit without marker, but marker64 needs byte9.
    bunch.payload_bits=30;
    expect_code([&] { encode_packet(single(0,data(bunch)),8); }, "packet_too_large");
}
TEST_CASE("Packet writer rejects limits beyond its explicit bounded buffer policy") {
    for (const auto limit: {0u,65508u,0x20000000u,0xffffffffu})
        expect_code([&] { encode_packet(Packet{},limit); }, "invalid_limit");
    expect_code([] { encode_packet(Packet{},1); }, "packet_too_large");
}
TEST_CASE("Packet writer rejects out of range physical scalar fields") {
    expect_code([] { encode_packet(single(0x4000,ack(0)),16); }, "invalid_value");
    expect_code([] { encode_packet(single(0,ack(0x4000)),16); }, "invalid_value");
    Bunch bunch;
    bunch.channel=0x800;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_value");
    bunch.channel=0; bunch.reliable=true; bunch.sequence=0x400;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_value");
    bunch.sequence=0; bunch.channel_type=8;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_value");
}
TEST_CASE("Packet writer rejects payload count outside exclusive wire bound") {
    Bunch bunch;
    bunch.payload_bits=128; bunch.payload.resize(16);
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_value");
    bunch.payload_bits=std::numeric_limits<std::size_t>::max();
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_value");
}
TEST_CASE("Packet writer rejects truncated payload storage before copy") {
    Bunch bunch;
    bunch.payload_bits=1;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_length");
    bunch.payload_bits=9; bunch.payload={0x95};
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_length");
}
TEST_CASE("Packet writer rejects extra unencoded payload storage") {
    Bunch bunch;
    bunch.payload={0};
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_length");
    bunch.payload_bits=1; bunch.payload={1,0};
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "invalid_length");
}
TEST_CASE("Packet writer rejects nonzero bits outside declared payload length") {
    Bunch bunch;
    bunch.payload_bits=5; bunch.payload={0x95};
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
    bunch.payload_bits=9; bunch.payload={0x95,0x03};
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
}
TEST_CASE("Packet writer requires native flags present relationship") {
    Bunch bunch;
    bunch.control=true;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
    bunch.control=false; bunch.open=true;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
    bunch.open=false; bunch.close=true;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
}
TEST_CASE("Packet writer rejects values in absent conditional fields") {
    Bunch bunch;
    bunch.sequence=1;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
    bunch.sequence=0; bunch.channel_type=1;
    expect_code([&] { encode_packet(single(0,data(bunch)),16); }, "inconsistent_record");
}
TEST_CASE("Packet writer never silently discards bunch fields of an ACK") {
    std::vector<PacketRecord> invalid(9,ack(0));
    invalid[0].bunch.control=true;
    invalid[1].bunch.open=true;
    invalid[2].bunch.close=true;
    invalid[3].bunch.reliable=true;
    invalid[4].bunch.channel=1;
    invalid[5].bunch.sequence=1;
    invalid[6].bunch.channel_type=1;
    invalid[7].bunch.payload_bits=1;
    invalid[8].bunch.payload={1};
    for (std::size_t i=0; i<invalid.size(); ++i) {
        CAPTURE(i);
        expect_code([&] { encode_packet(single(0,invalid[i]),16); }, "inconsistent_record");
    }
}
TEST_CASE("Packet writer never silently discards ACK ID of a bunch") {
    auto record=data(Bunch{});
    record.ack_packet_id=1;
    expect_code([&] { encode_packet(single(0,record),16); }, "inconsistent_record");
}
TEST_CASE("Packet writer rejects aggregate record capacity without unbounded output") {
    Packet packet;
    packet.records.assign(8,ack(0)); //14 +8*15 +marker=135bits, exceeds16bytes.
    expect_code([&] { encode_packet(packet,16); }, "packet_too_large");
    packet.records.assign(10000,ack(0));
    expect_code([&] { encode_packet(packet,2); }, "packet_too_large");
}
TEST_CASE("Packet writer permits the record count boundary that still fits") {
    Packet packet;
    packet.records.assign(7,ack(0)); //14+7*15=119data bits; marker119 ->15bytes.
    const std::vector<std::uint8_t> expected{
        0x00,0x40,0x00,0x20,0x00,0x10,0x00,0x08,0x00,0x04,0x00,0x02,0x00,0x01,0x80};
    CHECK(encode_packet(packet,16) == expected);
}
TEST_CASE("Packet writer fixed errors do not include discarded payload text") {
    const std::string private_text="never-echo-private";
    auto record=ack(0);
    record.bunch.payload.assign(private_text.begin(),private_text.end());
    expect_code([&] { encode_packet(single(0,record),16); }, "inconsistent_record");
}
