#include "hawken/reliable_control_transport.hpp"
#include "hawken/packet_writer.hpp"
#include <doctest/doctest.h>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::reliable;

TransportConfig config() {
    // Synthetic explicit seeds. First outgoing sequence11, NOT an inferred1.
    return {100,0xffffffffU,99,10,0,false,16,{16,16,16,1024,16,64},5.0};
}
Bunch payload(std::uint8_t byte, bool open=false, bool close=false) {
    Bunch b;
    b.control=open||close; b.open=open; b.close=close;
    b.reliable=true; b.channel_type=1; b.payload_bits=8; b.payload={byte};
    return b;
}
Packet input(std::uint16_t pid, std::uint16_t sequence, std::uint8_t byte,
             bool open=false, bool close=false) {
    Packet p; p.packet_id=pid;
    PacketRecord r; r.bunch=payload(byte,open,close); r.bunch.sequence=sequence;
    p.records.push_back(r);
    return p;
}
Packet ack(std::uint16_t pid, std::uint16_t acknowledged) {
    Packet p; p.packet_id=pid;
    PacketRecord r; r.is_ack=true; r.ack_packet_id=acknowledged;
    p.records.push_back(r);
    return p;
}
std::vector<const Bunch*> sent_bunches(const TransportStep& step) {
    std::vector<const Bunch*> result;
    for (const auto& p:step.outgoing)
        for (const auto& r:p.records)
            if (!r.is_ack) result.push_back(&r.bunch);
    return result;
}
std::vector<std::uint16_t> sent_acks(const TransportStep& step) {
    std::vector<std::uint16_t> result;
    for (const auto& p:step.outgoing)
        for (const auto& r:p.records)
            if (r.is_ack) result.push_back(r.ack_packet_id);
    return result;
}
template<class Action>
void expect_error(Action action, TransportErrorCode expected) {
    try { action(); }
    catch (const TransportError& error) {
        CHECK(error.code()==expected);
        CHECK(std::string(error.what())=="Reliable control transport operation could not be completed.");
        CHECK(std::string(error.what()).find("never-echo-private")==std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed TransportError"); return; }
    FAIL_CHECK("transport accepted rejected input");
}
}

// Expected bytes are independent integer-placement literals, with complete
// field offsets in implementation/evidence/reliable-control-implementation/
// literal-vectors.json. No expected vector uses this transport or BitWriter.
TEST_CASE("Reliable transport preserves explicit seeds and empty fresh queues") {
    auto c=config(); c.next_packet_id=0x89abcdef; c.last_incoming_packet_id=0x7ffffffe;
    c.last_ack_packet_id=0xffffffff; c.outgoing_sequence=0x7fffffff;
    c.incoming_sequence=0x80000000; c.open_confirmed=true;
    ControlTransport t(c);
    const auto s=t.snapshot();
    CHECK(s.next_packet_id==c.next_packet_id); CHECK(s.last_incoming_packet_id==c.last_incoming_packet_id);
    CHECK(s.last_ack_packet_id==c.last_ack_packet_id); CHECK(s.outgoing_sequence==c.outgoing_sequence);
    CHECK(s.incoming_sequence==c.incoming_sequence); CHECK(s.open_confirmed);
    CHECK_FALSE(s.closed); CHECK_FALSE(s.opening_packet_id.has_value()); CHECK_FALSE(s.last_time.has_value());
    CHECK(s.outgoing_bunches==0); CHECK(s.incoming_bunches==0); CHECK(s.queued_payload_bytes==0);
}
TEST_CASE("Reliable transport rejects invalid configuration without native defaults") {
    auto reject=[](const TransportConfig& c) {
        expect_error([&]{ControlTransport t(c);},TransportErrorCode::invalid_configuration);
    };
    for (auto bytes:{0U,1U,65508U}) { auto c=config(); c.max_packet_bytes=bytes; reject(c); }
    for (auto timeout:{0.0,-1.0,std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        auto c=config(); c.incoming_timeout=timeout; reject(c);
    }
    for (std::size_t QueueLimits::*field:{&QueueLimits::max_outgoing_bunches,&QueueLimits::max_incoming_bunches,
         &QueueLimits::max_ack_ids,&QueueLimits::max_records_per_packet,&QueueLimits::max_packets_per_step}) {
        for (std::size_t bad:{0U,1025U}) { auto c=config(); c.limits.*field=bad; reject(c); }
    }
    for (std::size_t bad:{0U,1048577U}) { auto c=config(); c.limits.max_payload_bytes=bad; reject(c); }
}
TEST_CASE("Reliable transport expands packet IDs with wrap and halfturn backward") {
    CHECK(expand_packet_id(0,0x3fff)==0x4000);
    CHECK(expand_packet_id(0x3fff,0)==0xffffffffU);
    CHECK(expand_packet_id(0x2000,0)==0xffffe000U);
    CHECK(expand_packet_id(0,0x2000)==0);
    CHECK(expand_packet_id(0,0xffffffffU)==0);
    CHECK(expand_packet_id(0x3fff,0x7fffffffU)==0x7fffffffU);
    CHECK(expand_packet_id(0,0x7fffffffU)==0x80000000U);
    CHECK(expand_packet_id(0x3fff,0x80000000U)==0x7fffffffU);
}
TEST_CASE("Reliable transport expands sequence bits with signed32 boundaries") {
    CHECK(expand_sequence(0,0x3ff)==0x400);
    CHECK(expand_sequence(0x3ff,0)==0xffffffffU);
    CHECK(expand_sequence(0x200,0)==0xfffffe00U);
    CHECK(expand_sequence(0,0x200)==0);
    CHECK(expand_sequence(0,0xffffffffU)==0);
    CHECK(expand_sequence(0,0x7fffffffU)==0x80000000U);
    CHECK(expand_sequence(0x3ff,0x80000000U)==0x7fffffffU);
    CHECK(std::bit_cast<std::int32_t>(expand_sequence(0,0x7fffffffU))<0);
}
TEST_CASE("Reliable transport rejects raw identifiers beyond recovered widths") {
    expect_error([]{expand_packet_id(0x4000,0);},TransportErrorCode::invalid_packet);
    expect_error([]{expand_sequence(0x400,0);},TransportErrorCode::invalid_packet);
}
TEST_CASE("Reliable transport send anchors literal open packet and assigned sequence") {
    ControlTransport t(config());
    const auto step=t.send(payload(0xa5,true),0.0);
    REQUIRE(step.outgoing.size()==1); CHECK(step.delivered.empty()); CHECK_FALSE(step.closed);
    // ID100[0..13], selector0[14], flags1[15], open1[16],close0[17],
    // reliable1[18], channel0[19..29], seq11[30..39],type1[40..42],
    // len8[43..49],payloada5[50..57],marker[58].
    const std::vector<std::uint8_t> expected{0x64,0x80,0x05,0xc0,0x02,0x41,0x94,0x06};
    CHECK(encode_packet(step.outgoing[0],16)==expected);
    CHECK(t.snapshot().next_packet_id==101); CHECK(t.snapshot().outgoing_sequence==11);
    CHECK(t.snapshot().outgoing_bunches==1); CHECK(t.snapshot().queued_payload_bytes==1);
    CHECK(t.snapshot().opening_packet_id==100); CHECK_FALSE(t.snapshot().open_confirmed);
}
TEST_CASE("Reliable transport send increments full u32 counters without signed UB") {
    auto c=config(); c.next_packet_id=0xffffffffU; c.outgoing_sequence=0x7fffffffU;
    ControlTransport t(c);
    const auto step=t.send(payload(0xa5),0);
    REQUIRE(step.outgoing.size()==1); REQUIRE(step.outgoing[0].records.size()==1);
    CHECK(step.outgoing[0].packet_id==0x3fff); CHECK(step.outgoing[0].records[0].bunch.sequence==0);
    CHECK(t.snapshot().next_packet_id==0); CHECK(t.snapshot().outgoing_sequence==0x80000000U);
}
TEST_CASE("Reliable transport receives literal bunch and emits literal ACK") {
    ControlTransport t(config());
    const std::vector<std::uint8_t> incoming{0x01,0,0x01,0x10,0x40,0x10,0xa1,0x01};
    const auto step=t.receive(decode_packet(incoming,16),0);
    REQUIRE(step.delivered.size()==1); CHECK(step.delivered[0].sequence==1);
    CHECK(step.delivered[0].payload==std::vector<std::uint8_t>{0xa1});
    REQUIRE(step.outgoing.size()==1);
    // ID100[0..13],ACK selector1[14],ACK1[15..28],marker[29].
    CHECK(encode_packet(step.outgoing[0],16)==std::vector<std::uint8_t>{0x64,0xc0,0,0x20});
    CHECK(t.snapshot().incoming_sequence==1); CHECK(t.snapshot().last_incoming_packet_id==1);
    CHECK(t.snapshot().prior_ack_ids==1); CHECK(t.snapshot().current_ack_ids==0);
}
TEST_CASE("Reliable transport orders future bunches and drains consecutive sequences") {
    ControlTransport t(config());
    CHECK(t.receive(input(3,3,0xc3),0).delivered.empty());
    CHECK(t.receive(input(2,2,0xb2),1).delivered.empty());
    CHECK(t.snapshot().incoming_bunches==2);
    const auto step=t.receive(input(1,1,0xa1),2);
    REQUIRE(step.delivered.size()==3);
    CHECK(step.delivered[0].payload==std::vector<std::uint8_t>{0xa1});
    CHECK(step.delivered[1].payload==std::vector<std::uint8_t>{0xb2});
    CHECK(step.delivered[2].payload==std::vector<std::uint8_t>{0xc3});
    CHECK(t.snapshot().incoming_sequence==3); CHECK(t.snapshot().incoming_bunches==0);
    CHECK(t.snapshot().last_incoming_packet_id==3); CHECK(t.snapshot().queued_payload_bytes==0);
}
TEST_CASE("Reliable transport stale packet still delivers a new reliable record") {
    ControlTransport t(config());
    REQUIRE(t.receive(input(9,1,0xa1),0).delivered.size()==1);
    const auto step=t.receive(input(4,2,0xb2),1);
    REQUIRE(step.delivered.size()==1); CHECK(step.delivered[0].sequence==2);
    CHECK(t.snapshot().last_incoming_packet_id==9);
    CHECK(sent_acks(step)==std::vector<std::uint16_t>{9,4});
}
TEST_CASE("Reliable transport reliable duplicate never delivers twice") {
    ControlTransport t(config());
    t.receive(input(1,1,0xa1),0);
    const auto step=t.receive(input(2,1,0xff),1);
    CHECK(step.delivered.empty()); CHECK(t.snapshot().incoming_sequence==1);
    CHECK(t.snapshot().incoming_bunches==0); CHECK(sent_acks(step)==std::vector<std::uint16_t>{1,2});
}
TEST_CASE("Reliable transport queued duplicate retains the first payload") {
    ControlTransport t(config());
    t.receive(input(3,3,0xc3),0);
    t.receive(input(4,3,0xff),1);
    CHECK(t.snapshot().incoming_bunches==1); CHECK(t.snapshot().queued_payload_bytes==1);
    t.receive(input(1,1,0xa1),2);
    const auto step=t.receive(input(2,2,0xb2),3);
    REQUIRE(step.delivered.size()==2);
    CHECK(step.delivered[1].payload==std::vector<std::uint8_t>{0xc3});
}
TEST_CASE("Reliable transport packet14 and sequence10 wrap independently") {
    auto c=config(); c.last_incoming_packet_id=0x3ffe; c.incoming_sequence=0x3fe;
    ControlTransport t(c);
    const std::vector<std::uint8_t> first{0xff,0x3f,0x01,0xf0,0x7f,0x10,0xa1,0x01};
    const std::vector<std::uint8_t> next{0,0,0x01,0,0x40,0x10,0xb2,0x01};
    REQUIRE(t.receive(decode_packet(first,16),0).delivered.size()==1);
    REQUIRE(t.receive(decode_packet(next,16),1).delivered.size()==1);
    CHECK(t.snapshot().incoming_sequence==0x400); CHECK(t.snapshot().last_incoming_packet_id==0x4000);
}
TEST_CASE("Reliable transport signed32 boundary is not an infinite reliable epoch") {
    auto c=config(); c.last_incoming_packet_id=0x7fffffffU; c.incoming_sequence=0x7fffffffU;
    ControlTransport t(c);
    const auto step=t.receive(input(0,0,0xa1),0);
    CHECK(step.delivered.empty()); CHECK(t.snapshot().incoming_sequence==0x7fffffffU);
    CHECK(t.snapshot().last_incoming_packet_id==0x7fffffffU);
}
TEST_CASE("Reliable transport incoming signed negative1 advances to zero") {
    auto c=config(); c.incoming_sequence=0xffffffffU;
    ControlTransport t(c);
    const auto step=t.receive(input(1,0,0xa1),0);
    REQUIRE(step.delivered.size()==1); CHECK(t.snapshot().incoming_sequence==0);
}
TEST_CASE("Reliable transport halfturn incoming sequence goes backward not future") {
    ControlTransport t(config());
    const auto step=t.receive(input(1,0x200,0xa1),0);
    CHECK(step.delivered.empty()); CHECK(t.snapshot().incoming_bunches==0);
    CHECK(t.snapshot().incoming_sequence==0);
}
TEST_CASE("Reliable transport preserves unaligned complete bunch payload bits") {
    ControlTransport t(config());
    auto p=input(1,1,0x95); p.records[0].bunch.payload_bits=9;
    p.records[0].bunch.payload={0x95,0x01};
    const auto step=t.receive(p,0);
    REQUIRE(step.delivered.size()==1); CHECK(step.delivered[0].payload_bits==9);
    CHECK(step.delivered[0].payload==std::vector<std::uint8_t>{0x95,0x01});
}
TEST_CASE("Reliable transport ACK is exact and head pruning is not cumulative") {
    ControlTransport t(config());
    t.send(payload(0xa5,true),0); t.send(payload(0xb6),0);
    const std::vector<std::uint8_t> peer{0x14,0xc0,0x32,0x20}; // Packet20 ACK101.
    const auto step=t.receive(decode_packet(peer,16),0.5);
    REQUIRE(step.outgoing.size()==2); // Retry102, then ACK packet103.
    CHECK(encode_packet(step.outgoing[0],16)==std::vector<std::uint8_t>{0x66,0x80,0x05,0xc0,0x02,0x41,0x94,0x06});
    CHECK(t.snapshot().outgoing_bunches==2); // Head100 now102 pending; tail101 ACKed.
    CHECK(t.snapshot().outgoing_sequence==12); CHECK(t.snapshot().last_ack_packet_id==101);
    CHECK(t.snapshot().next_packet_id==104);
    REQUIRE(sent_bunches(step).size()==1); CHECK(sent_bunches(step)[0]->sequence==11);
}
TEST_CASE("Reliable transport late old ACK can confirm open but not retransmitted entry") {
    ControlTransport t(config());
    t.send(payload(0xa5,true),0); t.send(payload(0xb6),0);
    t.receive(ack(20,101),0.5); // Packet100 lost; its entry now belongs to102.
    const auto old=t.receive(ack(21,100),0.6);
    CHECK(sent_bunches(old).empty()); CHECK(t.snapshot().outgoing_bunches==2);
    CHECK(t.snapshot().open_confirmed); // Separate Channel+74 match for old100.
    CHECK(t.snapshot().opening_packet_id==100); CHECK(t.snapshot().last_ack_packet_id==101);
    t.receive(ack(22,102),0.7);
    CHECK(t.snapshot().outgoing_bunches==0); CHECK(t.snapshot().queued_payload_bytes==0);
}
TEST_CASE("Reliable transport a later gap retries current association and not an ACKed tail") {
    ControlTransport t(config());
    t.send(payload(0xa5,true),0); t.send(payload(0xb6),0);
    t.receive(ack(20,101),0.1); // Pending head now102; tail101 ACKed but not pruned.
    const auto step=t.receive(ack(21,103),0.2); // Local NAK102, not old100 or tail101.
    const auto retries=sent_bunches(step);
    REQUIRE(retries.size()==1); CHECK(retries[0]->sequence==11);
    CHECK(retries[0]->payload==std::vector<std::uint8_t>{0xa5});
    CHECK(step.outgoing[0].packet_id==104); CHECK(t.snapshot().outgoing_bunches==2);
}
TEST_CASE("Reliable transport ACK signed wrap matches full packet association") {
    auto c=config(); c.next_packet_id=0xffffffffU; c.last_ack_packet_id=0xfffffffeU;
    ControlTransport t(c);
    t.send(payload(0xa5),0); t.receive(ack(1,0x3fff),0.1);
    CHECK(t.snapshot().outgoing_bunches==0); CHECK(t.snapshot().last_ack_packet_id==0xffffffffU);
    // The ACK reply consumed outgoing Packet0, so the next bunch is Packet1.
    t.send(payload(0xb6),0.2); t.receive(ack(2,1),0.3);
    CHECK(t.snapshot().last_ack_packet_id==1); CHECK(t.snapshot().outgoing_bunches==0);
}
TEST_CASE("Reliable transport halfturn ACK stays backward but still exact matches pending") {
    auto c=config(); c.next_packet_id=0xffffe000U; c.last_ack_packet_id=0;
    ControlTransport t(c); t.send(payload(0xa5),0);
    const auto step=t.receive(ack(1,0x2000),0.1);
    CHECK(t.snapshot().last_ack_packet_id==0); CHECK(t.snapshot().outgoing_bunches==0);
    CHECK(sent_bunches(step).empty()); // No forward ACK gap or invented window.
}
TEST_CASE("Reliable transport ACK replay rotates lists without reinserting replay IDs") {
    ControlTransport t(config());
    t.receive(input(1,1,0xa1),0);
    const auto second=t.receive(input(2,2,0xb2),0.1);
    REQUIRE(second.outgoing.size()==1);
    // ID101, selector1+ACK1, selector1+ACK2, final marker; no inserted ACK payload.
    CHECK(encode_packet(second.outgoing[0],16)==std::vector<std::uint8_t>{0x65,0xc0,0,0xa0,0,0x10});
    CHECK(t.snapshot().current_ack_ids==0); CHECK(t.snapshot().prior_ack_ids==1);
    const auto replay=t.tick(0.2);
    REQUIRE(replay.outgoing.size()==1);
    CHECK(encode_packet(replay.outgoing[0],16)==std::vector<std::uint8_t>{0x66,0x40,0x01,0x20});
    CHECK(t.snapshot().prior_ack_ids==0); CHECK(t.snapshot().current_ack_ids==0);
    CHECK(t.tick(0.3).outgoing.empty());
    CHECK(sent_acks(t.receive(input(3,3,0xc3),0.4))==std::vector<std::uint16_t>{3});
}
TEST_CASE("Reliable transport stale ACK-only packet still emits current ACK") {
    ControlTransport t(config());
    t.receive(input(9,1,0xa1),0);
    const auto step=t.receive(ack(4,99),0.1);
    CHECK(step.delivered.empty()); CHECK(sent_acks(step)==std::vector<std::uint16_t>{9,4});
    CHECK(t.snapshot().last_incoming_packet_id==9);
}
TEST_CASE("Reliable transport incoming open confirms channel before retry") {
    ControlTransport t(config());
    t.send(payload(0xa5,true),0);
    t.receive(input(1,1,0xa1,true),0.1);
    CHECK(t.snapshot().open_confirmed); CHECK(t.snapshot().opening_packet_id==1);
    CHECK(sent_bunches(t.tick(2)).empty());
}
TEST_CASE("Reliable transport preopen retry uses strict float age greater than one") {
    ControlTransport t(config()); t.send(payload(0xa5,true),0);
    CHECK(t.tick(1).outgoing.empty());
    CHECK(t.tick(std::nextafter(1.0,2.0)).outgoing.empty()); // Still rounds to1.0F.
    const auto step=t.tick(static_cast<double>(std::nextafter(1.0F,2.0F)));
    REQUIRE(sent_bunches(step).size()==1); CHECK(sent_bunches(step)[0]->sequence==11);
    CHECK(t.snapshot().outgoing_sequence==11); CHECK(t.snapshot().outgoing_bunches==1);
    CHECK(t.snapshot().next_packet_id==102); // Original100, retransmit101.
    CHECK(t.tick(1.5).outgoing.empty()); // Retry updated timestamp, not original sent time.
}
TEST_CASE("Reliable transport preopen pending8 retries while pending9 does not") {
    SUBCASE("eight") {
        ControlTransport t(config()); for (unsigned n=0;n<8;++n) t.send(payload(0xa5),0);
        CHECK(sent_bunches(t.tick(1.1)).size()==8); CHECK(t.snapshot().outgoing_bunches==8);
        CHECK(t.snapshot().outgoing_sequence==18);
    }
    SUBCASE("nine") {
        ControlTransport t(config()); for (unsigned n=0;n<9;++n) t.send(payload(0xa5),0);
        CHECK(sent_bunches(t.tick(1.1)).empty()); CHECK(t.snapshot().outgoing_bunches==9);
        CHECK(t.snapshot().next_packet_id==109);
    }
}
TEST_CASE("Reliable transport confirmed open has no invented universal timer retry") {
    auto c=config(); c.open_confirmed=true;
    ControlTransport t(c); t.send(payload(0xa5),0);
    CHECK(t.tick(100).outgoing.empty()); CHECK(t.snapshot().outgoing_bunches==1);
}
TEST_CASE("Reliable transport blocked incoming timeout preempts ACK replay and retry") {
    auto c=config(); c.incoming_timeout=1.5;
    ControlTransport t(c);
    t.receive(input(2,2,0xb2),0); t.send(payload(0xa5,true),0);
    const auto timeout=t.tick(1.5001);
    CHECK(timeout.closed); CHECK(timeout.outgoing.empty()); CHECK(timeout.delivered.empty());
    CHECK(t.snapshot().closed); CHECK(t.snapshot().incoming_bunches==0);
    CHECK(t.snapshot().outgoing_bunches==0); CHECK(t.snapshot().queued_payload_bytes==0);
}
TEST_CASE("Reliable transport incoming timeout equality is accepted and drain refreshes age") {
    ControlTransport t(config());
    t.receive(input(2,2,0xb2),0); t.receive(input(4,4,0xd4),1);
    CHECK_FALSE(t.tick(5).closed); // strict timeout, not>=.
    // Fastpath1 drains queued2, refreshing age; queued4 remains blocked on3.
    REQUIRE(t.receive(input(1,1,0xa1),5).delivered.size()==2);
    CHECK_FALSE(t.tick(9).closed);
    CHECK(t.tick(10.001).closed);
}
TEST_CASE("Reliable transport immediate progress without queued drain does not refresh timeout") {
    ControlTransport t(config()); t.receive(input(3,3,0xc3),0);
    REQUIRE(t.receive(input(1,1,0xa1),4).delivered.size()==1); // Queued3 still lacks2.
    CHECK(t.tick(5.001).closed);
}
TEST_CASE("Reliable transport accepts finite large time without out of range float conversion") {
    ControlTransport t(config()); t.send(payload(0xa5),0);
    REQUIRE(sent_bunches(t.tick(std::numeric_limits<double>::max())).size()==1);
}
TEST_CASE("Reliable transport incoming close delivers then stops queued drain") {
    ControlTransport t(config());
    t.receive(input(3,3,0xc3),0); t.receive(input(2,2,0xb2,false,true),0.1);
    const auto step=t.receive(input(1,1,0xa1),0.2);
    REQUIRE(step.delivered.size()==2); CHECK(step.delivered[1].close);
    CHECK(step.delivered[1].payload==std::vector<std::uint8_t>{0xb2});
    CHECK(step.closed); CHECK(t.snapshot().incoming_sequence==2);
    CHECK(t.snapshot().incoming_bunches==0); CHECK(t.snapshot().queued_payload_bytes==0);
    CHECK(sent_acks(step)==std::vector<std::uint16_t>{2,1});
}
TEST_CASE("Reliable transport close stops later valid records in same packet") {
    ControlTransport t(config());
    auto p=input(1,1,0xa1,false,true);
    p.records.push_back(input(1,2,0xb2).records[0]);
    const auto step=t.receive(p,0);
    REQUIRE(step.delivered.size()==1); CHECK(step.delivered[0].close);
    CHECK(step.closed); CHECK(t.snapshot().incoming_sequence==1);
}
TEST_CASE("Reliable transport outgoing close becomes terminal only when ACKed prefix prunes") {
    ControlTransport t(config());
    t.send(payload(0xa5),0); t.send(payload(0xb6,false,true),0);
    CHECK_FALSE(t.receive(ack(20,101),0.1).closed); // Tail close ACKed; head now retry102.
    CHECK(t.snapshot().outgoing_bunches==2); CHECK_FALSE(t.snapshot().closed);
    const auto step=t.receive(ack(21,102),0.2);
    CHECK(step.closed); CHECK(t.snapshot().closed); CHECK(t.snapshot().outgoing_bunches==0);
}
TEST_CASE("Reliable transport rejects nonfinite negative and decreasing time atomically") {
    ControlTransport t(config()); t.send(payload(0xa5),2);
    const auto before=t.snapshot();
    for (auto bad:{-1.0,1.0,std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::quiet_NaN()}) {
        expect_error([&]{t.send(payload(0xb6),bad);},TransportErrorCode::invalid_time);
        CHECK(t.snapshot()==before);
        expect_error([&]{t.receive(input(1,1,0xa1),bad);},TransportErrorCode::invalid_time);
        CHECK(t.snapshot()==before);
        expect_error([&]{t.tick(bad);},TransportErrorCode::invalid_time);
        CHECK(t.snapshot()==before);
    }
    CHECK(t.snapshot().last_time==2.0);
}
TEST_CASE("Reliable transport queue limits reject before counters or output commit") {
    auto c=config(); c.limits.max_outgoing_bunches=1;
    ControlTransport t(c); t.send(payload(0xa5),0);
    const auto before=t.snapshot();
    expect_error([&]{t.send(payload(0xb6),1);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before);
    t.receive(ack(1,100),1);
    CHECK(t.snapshot().outgoing_bunches==0);
}
TEST_CASE("Reliable transport incoming queue bound failure retains earlier future entry") {
    auto c=config(); c.limits.max_incoming_bunches=1;
    ControlTransport t(c); t.receive(input(2,2,0xb2),0);
    const auto before=t.snapshot();
    expect_error([&]{t.receive(input(3,3,0xc3),1);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before);
    const auto step=t.receive(input(1,1,0xa1),1);
    REQUIRE(step.delivered.size()==2); CHECK(step.delivered[1].payload==std::vector<std::uint8_t>{0xb2});
}
TEST_CASE("Reliable transport combined queued payload bound is exact and atomic") {
    auto c=config(); c.limits.max_payload_bytes=2;
    ControlTransport t(c); t.send(payload(0xa5),0); t.receive(input(2,2,0xb2),0);
    const auto before=t.snapshot(); CHECK(before.queued_payload_bytes==2);
    expect_error([&]{t.send(payload(0xc3),1);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before);
    expect_error([&]{t.receive(input(3,3,0xc3),1);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before);
}
TEST_CASE("Reliable transport ACKs split by record limit without native window32") {
    auto c=config(); c.limits.max_records_per_packet=1; c.limits.max_ack_ids=1;
    ControlTransport t(c); t.receive(input(1,1,0xa1),0);
    const auto step=t.receive(input(2,2,0xb2),1);
    REQUIRE(step.outgoing.size()==2); CHECK(step.outgoing[0].packet_id==101); CHECK(step.outgoing[1].packet_id==102);
    CHECK(sent_acks(step)==std::vector<std::uint16_t>{1,2});
    CHECK(t.snapshot().prior_ack_ids==1); CHECK(t.snapshot().current_ack_ids==0);
}
TEST_CASE("Reliable transport output packet quota failure discards complete staged result") {
    auto c=config(); c.limits.max_records_per_packet=1; c.limits.max_packets_per_step=1;
    ControlTransport t(c); t.receive(input(1,1,0xa1),0);
    const auto before=t.snapshot();
    expect_error([&]{t.receive(input(2,2,0xb2),1);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before); // No delivery,ACK rotation,ID/seq/clock mutation.
    t.tick(1); // Replay+clear previous ACK, making the original valid input fit.
    REQUIRE(t.receive(input(2,2,0xb2),2).delivered.size()==1);
}
TEST_CASE("Reliable transport records quota preflights even a later ignored record") {
    auto c=config(); c.limits.max_records_per_packet=1;
    ControlTransport t(c); const auto before=t.snapshot();
    auto p=input(1,1,0xa1,false,true); p.records.push_back(input(1,2,0xb2).records[0]);
    expect_error([&]{t.receive(p,0);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before);
}
TEST_CASE("Reliable transport unsupported channel type and unreliable never partially mutate") {
    ControlTransport t(config()); const auto before=t.snapshot();
    for (unsigned variant=0;variant<3;++variant) {
        auto p=input(1,1,0xa1);
        auto bad=input(1,2,0xb2).records[0];
        if (variant==0) bad.bunch.channel=1;
        if (variant==1) bad.bunch.channel_type=2;
        if (variant==2) { bad.bunch.reliable=false; bad.bunch.sequence=0; bad.bunch.channel_type=0; }
        p.records.push_back(bad);
        expect_error([&]{t.receive(p,0);},TransportErrorCode::unsupported_bunch);
        CHECK(t.snapshot()==before);
    }
}
TEST_CASE("Reliable transport rejects malformed canonical fields and redacts errors") {
    ControlTransport t(config()); const auto before=t.snapshot();
    for (unsigned variant=0;variant<6;++variant) {
        auto p=input(1,1,0xa1);
        if (variant==0) p.packet_id=0x4000;
        if (variant==1) p.records[0].bunch.sequence=0x400;
        if (variant==2) p.records[0].bunch.control=true;
        if (variant==3) p.records[0].ack_packet_id=1;
        if (variant==4) p.records[0].bunch.payload_bits=9;
        if (variant==5) { p.records[0].bunch.payload_bits=1; p.records[0].bunch.payload={0xff}; }
        expect_error([&]{t.receive(p,0);},TransportErrorCode::invalid_packet);
        CHECK(t.snapshot()==before);
    }
    auto bad=payload(0xa5); bad.payload.assign({'n','e','v','e','r','-','e','c','h','o','-','p','r','i','v','a','t','e'});
    expect_error([&]{t.send(bad,0);},TransportErrorCode::invalid_packet); CHECK(t.snapshot()==before);
    auto assigned=payload(0xa5); assigned.sequence=11;
    expect_error([&]{t.send(assigned,0);},TransportErrorCode::invalid_packet); CHECK(t.snapshot()==before);
}
TEST_CASE("Reliable transport bounds encoded interior including marker before mutation") {
    auto c=config(); c.max_packet_bytes=2;
    ControlTransport t(c); const auto before=t.snapshot();
    expect_error([&]{t.send(payload(0xa5),0);},TransportErrorCode::invalid_packet);
    CHECK(t.snapshot()==before);
    Packet empty; empty.packet_id=1;
    // Input header fits2; required ACK record cannot fit the same configured interior.
    expect_error([&]{t.receive(empty,0);},TransportErrorCode::invalid_packet);
    CHECK(t.snapshot()==before);
}
TEST_CASE("Reliable transport terminal operations have fixed errors and no side effects") {
    ControlTransport t(config()); t.receive(input(1,1,0xa1,false,true),0);
    const auto before=t.snapshot(); REQUIRE(before.closed);
    expect_error([&]{t.send(payload(0xa5),1);},TransportErrorCode::terminal_state); CHECK(t.snapshot()==before);
    expect_error([&]{t.receive(input(2,2,0xb2),1);},TransportErrorCode::terminal_state); CHECK(t.snapshot()==before);
    expect_error([&]{t.tick(1);},TransportErrorCode::terminal_state); CHECK(t.snapshot()==before);
}
TEST_CASE("Reliable transport quiet fresh tick creates no invented keepalive") {
    ControlTransport t(config());
    const auto step=t.tick(0);
    CHECK(step.outgoing.empty()); CHECK(step.delivered.empty()); CHECK_FALSE(step.closed);
    CHECK(t.snapshot().next_packet_id==100); CHECK(t.snapshot().outgoing_sequence==10);
}
