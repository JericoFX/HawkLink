#include "hawken/reliable_control_transport.hpp"
#include "hawken/packet_writer.hpp"
#include <doctest/doctest.h>
#include <limits>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::reliable;
TransportConfig config() {
    return {100,0xffffffffU,99,10,20,false,64,{16,16,16,1024,16,64},5.0};
}
Bunch data(std::uint16_t channel,std::uint8_t byte,bool open=false,bool close=false) {
    Bunch b;
    b.control=open||close; b.open=open; b.close=close; b.reliable=true;
    b.channel=channel; b.channel_type=channel==0?1:2;
    b.payload_bits=8; b.payload={byte};
    return b;
}
Bunch plain(std::uint16_t channel,std::uint8_t byte) {
    auto b=data(channel,byte); b.reliable=false; b.channel_type=0; return b;
}
Bunch incoming(std::uint16_t channel,std::uint16_t sequence,std::uint8_t byte,
               bool open=false,bool close=false) {
    auto b=data(channel,byte,open,close); b.sequence=sequence; return b;
}
Packet packet(std::uint16_t id,std::initializer_list<Bunch> bunches) {
    Packet p; p.packet_id=id;
    for (const auto& b:bunches) p.records.push_back({false,0,b});
    return p;
}
Packet ack(std::uint16_t id,std::uint16_t acknowledged) {
    Packet p; p.packet_id=id; PacketRecord r;
    r.is_ack=true; r.ack_packet_id=acknowledged; p.records.push_back(r); return p;
}
std::vector<Bunch> sent(const TransportStep& step) {
    std::vector<Bunch> result;
    for (const auto& p:step.outgoing)
        for (const auto& r:p.records) if (!r.is_ack) result.push_back(r.bunch);
    return result;
}
std::vector<std::uint16_t> acknowledgements(const TransportStep& step) {
    std::vector<std::uint16_t> result;
    for (const auto& p:step.outgoing)
        for (const auto& r:p.records) if (r.is_ack) result.push_back(r.ack_packet_id);
    return result;
}
ChannelSnapshot channel(const ControlTransport& transport,std::uint16_t id) {
    const auto result=transport.channel_snapshot(id);
    REQUIRE(result.has_value()); return *result;
}
template<class F> void error(F action,TransportErrorCode code) {
    try { action(); }
    catch (const TransportError& e) {
        CHECK(e.code()==code);
        CHECK(std::string(e.what())=="Reliable control transport operation could not be completed.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed TransportError"); return; }
    FAIL_CHECK("invalid transport operation accepted");
}
}

TEST_CASE("Actor registration preserves connection clock IDs and legacy control seeds") {
    ControlTransport t(config()); const auto before=t.snapshot();
    t.register_actor_channel({7,100,200,true});
    CHECK(t.snapshot()==before);
    const auto a=channel(t,7); CHECK(a.channel==7);
    CHECK(a.outgoing_sequence==100); CHECK(a.incoming_sequence==200);
    CHECK(a.open_confirmed); CHECK_FALSE(a.closed); CHECK_FALSE(a.opening_packet_id.has_value());
    CHECK(a.outgoing_bunches==0); CHECK(a.incoming_bunches==0); CHECK(a.queued_payload_bytes==0);
    CHECK_FALSE(a.incoming_blocked_since.has_value());
    CHECK(channel(t,0).outgoing_sequence==10); CHECK(channel(t,0).incoming_sequence==20);
    CHECK_FALSE(t.channel_snapshot(8).has_value()); CHECK_FALSE(t.channel_snapshot(2048).has_value());
    error([&]{t.register_actor_channel({0,0,0,false});},TransportErrorCode::invalid_configuration);
    error([&]{t.register_actor_channel({2048,0,0,false});},TransportErrorCode::invalid_configuration);
    error([&]{t.register_actor_channel({7,0,0,false});},TransportErrorCode::invalid_configuration);
    CHECK(t.snapshot()==before); CHECK(channel(t,7)==a);
}
TEST_CASE("Actor send matches independent LSB literal and shares packet namespace") {
    auto c=config(); c.max_packet_bytes=16; ControlTransport t(c);
    t.register_actor_channel({7,20,0,false}); t.register_actor_channel({9,40,0,true});
    const auto first=t.send(data(7,0xa5,true),0);
    REQUIRE(first.outgoing.size()==1);
    // Independent placement: PID100[0:14], selectors/flags[14:19], channel7
    // [19:30], seq21[30:40], type2[40:43], length8[43:50], A5[50:58], marker58.
    const std::vector<std::uint8_t> literal{0x64,0x80,0x3d,0x40,0x05,0x42,0x94,0x06};
    CHECK(encode_packet(first.outgoing[0],16)==literal);
    const auto second=t.send(data(0,0x11),0);
    const auto third=t.send(data(9,0x22),0);
    REQUIRE(second.outgoing.size()==1); REQUIRE(third.outgoing.size()==1);
    CHECK(first.outgoing[0].packet_id==100); CHECK(second.outgoing[0].packet_id==101);
    CHECK(third.outgoing[0].packet_id==102);
    CHECK(sent(second)[0].sequence==11); CHECK(sent(third)[0].sequence==41);
    CHECK(t.snapshot().next_packet_id==103); CHECK(t.snapshot().outgoing_sequence==11);
    CHECK(channel(t,7).outgoing_sequence==21); CHECK(channel(t,9).outgoing_sequence==41);
    CHECK(t.snapshot().outgoing_bunches==3); CHECK(t.snapshot().queued_payload_bytes==3);
}
TEST_CASE("One packet independently expands and delivers interleaved channel sequences") {
    ControlTransport t(config()); t.register_actor_channel({1,0,40,false});
    t.register_actor_channel({2,0,90,false});
    const auto step=t.receive(packet(0,{incoming(1,41,0xa1,true),incoming(0,21,0xc0),incoming(2,91,0xa2)}),0);
    REQUIRE(step.delivered.size()==3);
    CHECK(step.delivered[0].channel==1); CHECK(step.delivered[1].channel==0);
    CHECK(step.delivered[2].channel==2); CHECK(step.closed_channels.empty()); CHECK_FALSE(step.closed);
    CHECK(t.snapshot().incoming_sequence==21); CHECK(channel(t,1).incoming_sequence==41);
    CHECK(channel(t,2).incoming_sequence==91); CHECK(channel(t,1).open_confirmed);
    CHECK(channel(t,1).opening_packet_id==0);
    CHECK(acknowledgements(step)==std::vector<std::uint16_t>{0});
}
TEST_CASE("Future reliable queues and drains belong to their channel with stale suppression") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true}); t.register_actor_channel({2,0,0,true});
    const auto future=t.receive(packet(0,{incoming(1,2,0x12),incoming(2,1,0x21)}),0);
    REQUIRE(future.delivered.size()==1); CHECK(future.delivered[0].channel==2);
    CHECK(channel(t,1).incoming_bunches==1); CHECK(channel(t,2).incoming_bunches==0);
    const auto drain=t.receive(packet(0,{incoming(2,1,0xff),incoming(1,1,0x11)}),1);
    REQUIRE(drain.delivered.size()==2); CHECK(drain.delivered[0].payload[0]==0x11);
    CHECK(drain.delivered[1].payload[0]==0x12); CHECK(channel(t,1).incoming_sequence==2);
    CHECK(t.snapshot().incoming_sequence==20); CHECK(t.snapshot().incoming_bunches==0);
    const auto duplicate=t.receive(packet(1,{incoming(1,2,0xee)}),1);
    CHECK(duplicate.delivered.empty()); CHECK_FALSE(acknowledgements(duplicate).empty());
}
TEST_CASE("Exact ACK prunes each channel prefix without confirming other pending channels") {
    auto c=config(); c.last_ack_packet_id=100; ControlTransport t(c);
    t.register_actor_channel({1,0,0,true}); t.register_actor_channel({2,0,0,true});
    t.send(data(0,0xc0),0); t.send(data(1,0xa1),0); t.send(data(2,0xa2),0);
    const auto actor=t.receive(ack(0,101),1); CHECK(sent(actor).empty());
    CHECK(channel(t,1).outgoing_bunches==0); CHECK(channel(t,0).outgoing_bunches==1);
    CHECK(channel(t,2).outgoing_bunches==1); CHECK(t.snapshot().outgoing_bunches==2);
    t.receive(ack(1,100),1); CHECK(channel(t,0).outgoing_bunches==0);
    CHECK(channel(t,2).outgoing_bunches==1); CHECK(t.snapshot().last_ack_packet_id==101);
    t.receive(ack(2,102),1); CHECK(t.snapshot().outgoing_bunches==0);
}
TEST_CASE("Connection ACK gaps NAK actors with original sequence and new packet association") {
    ControlTransport t(config()); t.register_actor_channel({1,20,0,false});
    t.send(data(0,0xc0),0); t.send(data(1,0xa1),0);
    const auto retry=t.receive(ack(0,102),1); const auto bunches=sent(retry);
    REQUIRE(bunches.size()==2); REQUIRE(retry.outgoing.size()>=2);
    CHECK(retry.outgoing[0].packet_id==102); CHECK(retry.outgoing[1].packet_id==103);
    CHECK(bunches[0].channel==0); CHECK(bunches[0].sequence==11); CHECK(bunches[0].payload[0]==0xc0);
    CHECK(bunches[1].channel==1); CHECK(bunches[1].sequence==21); CHECK(bunches[1].payload[0]==0xa1);
    CHECK(channel(t,0).outgoing_bunches==0); CHECK(channel(t,1).outgoing_bunches==1);
    t.receive(ack(1,101),1); CHECK(channel(t,1).outgoing_bunches==1);
    t.receive(ack(2,103),1); CHECK(channel(t,1).outgoing_bunches==0);
}
TEST_CASE("Incoming actor close clears only its queues and continues other channel records") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false}); t.register_actor_channel({2,0,0,true});
    t.receive(packet(0,{incoming(1,3,0x13)}),0);
    const auto closed=t.receive(packet(1,{incoming(1,1,0x11,false,true),incoming(2,1,0x21)}),1);
    REQUIRE(closed.delivered.size()==2); CHECK(closed.delivered[0].close);
    CHECK(closed.delivered[1].channel==2); CHECK_FALSE(closed.closed);
    CHECK(closed.closed_channels==std::vector<std::uint16_t>{1});
    CHECK(channel(t,1).closed); CHECK(channel(t,1).incoming_bunches==0);
    CHECK(channel(t,1).queued_payload_bytes==0); CHECK(t.snapshot().queued_payload_bytes==0);
    const auto later=t.receive(packet(2,{incoming(1,2,0xee,true),incoming(2,2,0x22)}),1);
    REQUIRE(later.delivered.size()==1); CHECK(later.delivered[0].channel==2);
    CHECK(later.closed_channels.empty()); CHECK_FALSE(later.closed); CHECK(channel(t,1).closed);
    CHECK(channel(t,1).incoming_sequence==1); CHECK_FALSE(acknowledgements(later).empty());
    error([&]{t.send(data(1,0xab),1);},TransportErrorCode::terminal_state);
}
TEST_CASE("Outgoing actor close waits for its own prefix ACK and never closes control") {
    auto c=config(); c.last_ack_packet_id=100; ControlTransport t(c);
    t.register_actor_channel({1,0,0,true});
    t.send(data(1,0xa1),0); t.send(data(1,0xa2,false,true),0); t.send(data(0,0xc0),0);
    const auto tail=t.receive(ack(0,101),1); CHECK(tail.closed_channels.empty());
    CHECK(channel(t,1).outgoing_bunches==2); CHECK_FALSE(channel(t,1).closed);
    const auto close=t.receive(ack(1,100),1);
    CHECK(close.closed_channels==std::vector<std::uint16_t>{1}); CHECK_FALSE(close.closed);
    CHECK(channel(t,1).outgoing_bunches==0); CHECK(channel(t,1).closed);
    CHECK(channel(t,0).outgoing_bunches==1);
    CHECK(t.receive(ack(2,101),1).closed_channels.empty());
}
TEST_CASE("Pending outgoing actor close rejects further sends without connection mutation") {
    auto c=config(); c.last_ack_packet_id=100; ControlTransport t(c);
    t.register_actor_channel({1,0,0,true}); t.register_actor_channel({2,0,0,true});
    t.send(data(1,0xa1),0); t.send(data(1,0xa2,false,true),0);
    SUBCASE("Close awaits its ACK") {}
    SUBCASE("Close ACKed but preceding bunch still pending") {
        t.receive(ack(0,101),1);
    }
    const auto before=t.snapshot(); const auto actor=channel(t,1);
    const auto control=channel(t,0); const auto other=channel(t,2);
    REQUIRE_FALSE(actor.closed); REQUIRE(actor.outgoing_bunches==2);
    error([&]{t.send(data(1,0xa3),2);},TransportErrorCode::terminal_state);
    CHECK(t.snapshot()==before); CHECK(channel(t,1)==actor);
    CHECK(channel(t,0)==control); CHECK(channel(t,2)==other);
    const auto continued=t.send(data(0,0xc0),2);
    REQUIRE(continued.outgoing.size()==1);
    CHECK(continued.outgoing[0].packet_id==(before.next_packet_id&0x3fffU));
    CHECK(sent(continued)[0].sequence==11); CHECK_FALSE(continued.closed);
}
TEST_CASE("Queued actor close drains safely then continues control in the same packet") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true});
    t.receive(packet(0,{incoming(1,2,0xa2,false,true)}),0);
    const auto step=t.receive(packet(1,{incoming(1,1,0xa1),incoming(0,21,0xc0)}),1);
    REQUIRE(step.delivered.size()==3);
    CHECK(step.delivered[0].channel==1); CHECK(step.delivered[0].payload==std::vector<std::uint8_t>{0xa1});
    CHECK(step.delivered[1].channel==1); CHECK(step.delivered[1].close);
    CHECK(step.delivered[2].channel==0); CHECK(step.delivered[2].payload==std::vector<std::uint8_t>{0xc0});
    CHECK(step.closed_channels==std::vector<std::uint16_t>{1}); CHECK_FALSE(step.closed);
    CHECK(channel(t,1).closed); CHECK(channel(t,1).incoming_sequence==2);
    CHECK(channel(t,1).incoming_bunches==0); CHECK(channel(t,1).queued_payload_bytes==0);
    CHECK(t.snapshot().incoming_sequence==21); CHECK(t.snapshot().incoming_bunches==0);
    CHECK(t.snapshot().queued_payload_bytes==0);
    CHECK(acknowledgements(step)==std::vector<std::uint16_t>{0,1});
}
TEST_CASE("Control close remains connection terminal including actor queues and registration") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true});
    t.send(data(1,0xa1),0); t.receive(packet(0,{incoming(1,2,0xa2)}),0);
    const auto close=t.receive(packet(1,{incoming(0,21,0xc0,false,true)}),1);
    CHECK(close.closed); CHECK(t.snapshot().outgoing_bunches==0);
    CHECK(t.snapshot().incoming_bunches==0); CHECK(t.snapshot().queued_payload_bytes==0);
    error([&]{t.register_actor_channel({2,0,0,true});},TransportErrorCode::terminal_state);
    error([&]{t.tick(2);},TransportErrorCode::terminal_state);
}
TEST_CASE("Preopen age retry is control-only and counts only control pending entries") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.send(data(0,0xc0),0);
    for (unsigned n=0;n<9;++n) t.send(data(1,static_cast<std::uint8_t>(n)),0);
    CHECK(sent(t.tick(1)).empty());
    const auto retry=sent(t.tick(1.01)); REQUIRE(retry.size()==1);
    CHECK(retry[0].channel==0); CHECK(retry[0].sequence==11);
    CHECK(channel(t,1).outgoing_bunches==9); CHECK_FALSE(channel(t,1).open_confirmed);
}
TEST_CASE("Actor blocked incoming timeout is global and precedes control retry") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true});
    t.send(data(0,0xc0),0); t.receive(packet(0,{incoming(1,2,0xa2)}),0);
    CHECK(channel(t,1).incoming_blocked_since==0);
    const auto step=t.tick(5.01); CHECK(step.closed); CHECK(step.outgoing.empty());
    CHECK(t.snapshot().incoming_bunches==0); CHECK(t.snapshot().outgoing_bunches==0);
}
TEST_CASE("Aggregate channel queue and byte quotas roll back whole rejected operations") {
    auto c=config();
    SUBCASE("Outgoing count shared") { c.limits.max_outgoing_bunches=1; }
    SUBCASE("Incoming count shared") { c.limits.max_incoming_bunches=1; }
    SUBCASE("Combined payload bytes shared") { c.limits.max_payload_bytes=1; }
    ControlTransport t(c); t.register_actor_channel({1,0,0,true}); t.register_actor_channel({2,0,0,true});
    if (c.limits.max_incoming_bunches==1) {
        t.receive(packet(0,{incoming(1,2,0xa1)}),0);
        const auto before=t.snapshot(); const auto a=channel(t,1); const auto b=channel(t,2);
        error([&]{t.receive(packet(1,{incoming(2,2,0xa2)}),1);},TransportErrorCode::resource_limit);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a); CHECK(channel(t,2)==b);
    } else {
        t.send(data(1,0xa1),0); const auto before=t.snapshot(); const auto a=channel(t,1);
        if (c.limits.max_outgoing_bunches==1)
            error([&]{t.send(data(0,0xc0),1);},TransportErrorCode::resource_limit);
        else error([&]{t.receive(packet(0,{incoming(2,2,0xa2)}),1);},TransportErrorCode::resource_limit);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a); CHECK(channel(t,2).incoming_bunches==0);
    }
}
TEST_CASE("Unknown wrong-type unreliable and nonfinite inputs reject without channel progress") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true});
    const auto before=t.snapshot(); const auto a=channel(t,1);
    error([&]{t.receive(packet(0,{incoming(1,1,0xa1),incoming(2,1,0xa2)}),1);},TransportErrorCode::unsupported_bunch);
    auto wrong=incoming(1,1,0xa1); wrong.channel_type=1;
    error([&]{t.receive(packet(0,{wrong}),1);},TransportErrorCode::unsupported_bunch);
    auto unreliable=data(0,0xa1); unreliable.reliable=false; unreliable.channel_type=0;
    error([&]{t.send(unreliable,1);},TransportErrorCode::unsupported_bunch);
    for (const auto now:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        error([&]{t.send(data(1,0xa1),now);},TransportErrorCode::invalid_time);
    CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
    t.receive(packet(0,{incoming(1,1,0xa1,false,true)}),1);
    const auto closed=t.snapshot(); auto malformed=incoming(1,2,0xee); malformed.control=true;
    error([&]{t.receive(packet(1,{malformed}),2);},TransportErrorCode::invalid_packet);
    CHECK(t.snapshot()==closed); CHECK(channel(t,1).closed);
}
TEST_CASE("Actor sequence wrap and halfturn retain independent signed32 seed behavior") {
    auto c=config(); c.next_packet_id=0xffffffffU; ControlTransport t(c);
    t.register_actor_channel({1,0xffffffffU,0x3ffU,true}); t.register_actor_channel({2,0,0,true});
    const auto first=t.send(data(1,0xa1),0); const auto second=t.send(data(2,0xa2),0);
    CHECK(first.outgoing[0].packet_id==0x3fff); CHECK(second.outgoing[0].packet_id==0);
    CHECK(sent(first)[0].sequence==0); CHECK(sent(second)[0].sequence==1);
    const auto wrapped=t.receive(packet(0,{incoming(1,0,0x11),incoming(2,0x200,0x22)}),1);
    REQUIRE(wrapped.delivered.size()==1); CHECK(wrapped.delivered[0].channel==1);
    CHECK(channel(t,1).incoming_sequence==0x400); CHECK(channel(t,2).incoming_sequence==0);
    CHECK(t.snapshot().incoming_sequence==20);
}
TEST_CASE("Registration cap includes closed tombstones and never permits index reuse") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,true});
    t.receive(packet(0,{incoming(1,1,0xa1,false,true)}),0); REQUIRE(channel(t,1).closed);
    error([&]{t.register_actor_channel({1,99,99,false});},TransportErrorCode::invalid_configuration);
    for (std::uint16_t id=2;id<=64;++id) t.register_actor_channel({id,0,0,true});
    const auto before=t.snapshot();
    error([&]{t.register_actor_channel({65,0,0,true});},TransportErrorCode::resource_limit);
    CHECK_FALSE(t.channel_snapshot(65).has_value()); CHECK(t.snapshot()==before);
    CHECK(channel(t,1).closed); CHECK(channel(t,64).outgoing_sequence==0);
}


TEST_CASE("Unreliable actor requires an opening PacketID even with a confirmed seed") {
    for (const bool confirmed:{false,true}) {
        ControlTransport t(config()); t.register_actor_channel({1,7,9,confirmed});
        const auto before=t.snapshot(); const auto a=channel(t,1);
        error([&]{t.send(plain(1,0xa1),0);},TransportErrorCode::unsupported_bunch);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
        const auto ignored=t.receive(packet(0,{plain(1,0xa2)}),0);
        CHECK(ignored.delivered.empty()); CHECK_FALSE(ignored.closed);
        CHECK(acknowledgements(ignored)==std::vector<std::uint16_t>{0});
        CHECK(channel(t,1)==a); CHECK_FALSE(channel(t,1).opening_packet_id.has_value());
        const auto opened=t.receive(packet(1,{incoming(1,10,0x11,true),plain(1,0xa3)}),1);
        REQUIRE(opened.delivered.size()==2);
        CHECK(opened.delivered[0].reliable); CHECK_FALSE(opened.delivered[1].reliable);
        CHECK(opened.delivered[1].payload==std::vector<std::uint8_t>{0xa3});
        CHECK(channel(t,1).opening_packet_id==1); CHECK(channel(t,1).incoming_sequence==10);
    }
}
TEST_CASE("Unreliable actor literal omits sequence and type without advancing reliable state") {
    auto c=config(); c.max_packet_bytes=16; ControlTransport t(c);
    t.register_actor_channel({7,20,30,false}); t.send(data(7,0x11,true),0);
    const auto before=t.snapshot(); const auto a=channel(t,7);
    REQUIRE(a.opening_packet_id==100); REQUIRE_FALSE(a.open_confirmed);
    const auto step=t.send(plain(7,0xa5),1);
    REQUIRE(step.outgoing.size()==1); REQUIRE(sent(step).size()==1);
    // Independent LSB placement: PID101[0:14], selector/control/reliable zeros
    // [14:17], channel7[17:28], length8[28:35], A5[35:43], marker43.
    const std::vector<std::uint8_t> literal{0x65,0x00,0x0e,0x80,0x28,0x0d};
    CHECK(encode_packet(step.outgoing[0],16)==literal);
    CHECK_FALSE(sent(step)[0].reliable); CHECK(sent(step)[0].sequence==0);
    CHECK(sent(step)[0].channel_type==0); CHECK(step.delivered.empty());
    CHECK(channel(t,7)==a); CHECK(t.snapshot().next_packet_id==before.next_packet_id+1);
    CHECK(t.snapshot().outgoing_bunches==before.outgoing_bunches);
    CHECK(t.snapshot().queued_payload_bytes==before.queued_payload_bytes);
    CHECK(t.snapshot().outgoing_sequence==10); CHECK(t.snapshot().last_time==1);
}
TEST_CASE("Unreliable actor delivers stale packets and duplicates alongside reliable control") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.receive(packet(2,{incoming(1,1,0x11,true)}),0);
    const auto first=t.receive(packet(3,{plain(1,0xa1),incoming(0,21,0xc0),plain(1,0xa2)}),1);
    REQUIRE(first.delivered.size()==3);
    CHECK(first.delivered[0].payload==std::vector<std::uint8_t>{0xa1});
    CHECK(first.delivered[1].channel==0); CHECK(first.delivered[1].reliable);
    CHECK(first.delivered[2].payload==std::vector<std::uint8_t>{0xa2});
    const auto again=t.receive(packet(1,{plain(1,0xa1),plain(1,0xa1)}),2);
    REQUIRE(again.delivered.size()==2); CHECK_FALSE(again.delivered[0].reliable);
    CHECK_FALSE(again.delivered[1].reliable); CHECK_FALSE(again.closed);
    CHECK(channel(t,1).incoming_sequence==1); CHECK(channel(t,1).incoming_bunches==0);
    CHECK(t.snapshot().incoming_sequence==21); CHECK(t.snapshot().last_incoming_packet_id==3);
    CHECK(acknowledgements(again)==std::vector<std::uint16_t>{3,1});
}
TEST_CASE("Unreliable actor preserves a reliable gap and subsequent shared queue drain") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.receive(packet(0,{incoming(1,1,0x11,true)}),0);
    t.receive(packet(1,{incoming(1,3,0x13)}),1);
    const auto before=channel(t,1); REQUIRE(before.incoming_bunches==1);
    const auto step=t.receive(packet(2,{plain(1,0xaa)}),2);
    REQUIRE(step.delivered.size()==1); CHECK_FALSE(step.delivered[0].reliable);
    CHECK(channel(t,1)==before); CHECK(t.snapshot().queued_payload_bytes==1);
    const auto drain=t.receive(packet(3,{plain(1,0xab),incoming(1,2,0x12),plain(1,0xac)}),3);
    REQUIRE(drain.delivered.size()==4);
    CHECK(drain.delivered[0].payload==std::vector<std::uint8_t>{0xab});
    CHECK(drain.delivered[1].payload==std::vector<std::uint8_t>{0x12});
    CHECK(drain.delivered[2].payload==std::vector<std::uint8_t>{0x13});
    CHECK(drain.delivered[3].payload==std::vector<std::uint8_t>{0xac});
    CHECK(channel(t,1).incoming_sequence==3); CHECK(channel(t,1).incoming_bunches==0);
    CHECK_FALSE(channel(t,1).incoming_blocked_since.has_value());
    CHECK(t.snapshot().queued_payload_bytes==0);
}
TEST_CASE("NAK gaps retry only reliable actor entries not intervening unreliable packets") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.send(data(1,0x11,true),0); t.send(plain(1,0xaa),0); t.send(data(1,0x12),0);
    REQUIRE(t.snapshot().outgoing_bunches==2); REQUIRE(channel(t,1).outgoing_sequence==2);
    const auto retry=t.receive(ack(0,102),1); const auto bunches=sent(retry);
    REQUIRE(bunches.size()==1);
    CHECK(retry.outgoing[0].packet_id==103);
    CHECK(bunches[0].reliable); CHECK(bunches[0].sequence==1);
    CHECK(bunches[0].payload==std::vector<std::uint8_t>{0x11});
    CHECK(channel(t,1).outgoing_sequence==2); CHECK(channel(t,1).outgoing_bunches==2);
    t.receive(ack(1,103),1); REQUIRE(channel(t,1).outgoing_bunches==0);
    t.send(plain(1,0xab),2);
    CHECK(sent(t.tick(4)).empty()); CHECK(channel(t,1).outgoing_bunches==0);
}
TEST_CASE("Unreliable actor obeys close tombstones and pending outgoing close") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.receive(packet(0,{incoming(1,1,0x11,true)}),0);
    SUBCASE("Outgoing close awaiting ACK") {
        t.send(data(1,0x12,false,true),1);
        const auto before=t.snapshot(); const auto a=channel(t,1);
        error([&]{t.send(plain(1,0xaa),2);},TransportErrorCode::terminal_state);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
    }
    SUBCASE("Incoming close ignores later unreliable but continues control") {
        t.receive(packet(1,{incoming(1,2,0x12,false,true)}),1);
        const auto a=channel(t,1); REQUIRE(a.closed);
        const auto step=t.receive(packet(2,{plain(1,0xaa),incoming(0,21,0xc0)}),2);
        REQUIRE(step.delivered.size()==1); CHECK(step.delivered[0].channel==0);
        CHECK_FALSE(step.closed); CHECK(step.closed_channels.empty()); CHECK(channel(t,1)==a);
        CHECK(acknowledgements(step)==std::vector<std::uint16_t>{1,2});
        const auto before=t.snapshot();
        error([&]{t.send(plain(1,0xab),3);},TransportErrorCode::terminal_state);
        CHECK(t.snapshot()==before);
    }
}
TEST_CASE("Unreliable actor rejects noncanonical flags and fields transactionally") {
    ControlTransport t(config()); t.register_actor_channel({1,0,0,false});
    t.send(data(1,0x11,true),0); const auto before=t.snapshot(); const auto a=channel(t,1);
    for (unsigned variant=0;variant<7;++variant) {
        auto b=plain(1,0xaa); auto code=TransportErrorCode::invalid_packet;
        switch (variant) {
        case 0: b.channel=0; code=TransportErrorCode::unsupported_bunch; break;
        case 1: b.open=true; b.control=true; b.channel_type=2; code=TransportErrorCode::unsupported_bunch; break;
        case 2: b.close=true; b.control=true; code=TransportErrorCode::unsupported_bunch; break;
        case 3: b.control=true; break;
        case 4: b.sequence=1; break;
        case 5: b.channel_type=2; break;
        case 6: b.channel=2; code=TransportErrorCode::unsupported_bunch; break;
        }
        error([&]{t.send(b,1);},code);
        error([&]{t.receive(packet(0,{b}),1);},code);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
    }
    for (const auto now:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        error([&]{t.send(plain(1,0xaa),now);},TransportErrorCode::invalid_time);
        CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
    }
}
TEST_CASE("Unreliable actor exact bit budget excludes absent sequence and type fields") {
    auto c=config(); c.max_packet_bytes=8; ControlTransport t(c);
    t.register_actor_channel({7,0,0,false}); t.send(data(7,0x11,true),0);
    auto b=plain(7,0); b.payload_bits=29; b.payload={0xff,0xff,0xff,0x1f};
    const auto step=t.send(b,1); REQUIRE(step.outgoing.size()==1);
    // PID101 + 14 bunch flag/channel bits + length29 in6 bits + 29 ones
    // occupy63 bits; marker63 fills the eighth byte exactly.
    const std::vector<std::uint8_t> literal{0x65,0x00,0x0e,0xd0,0xfd,0xff,0xff,0xff};
    CHECK(encode_packet(step.outgoing[0],8)==literal);
    const auto before=t.snapshot(); const auto a=channel(t,7);
    b.payload_bits=30; b.payload.back()=0x3f;
    error([&]{t.send(b,2);},TransportErrorCode::invalid_packet);
    error([&]{t.receive(packet(0,{b}),2);},TransportErrorCode::invalid_packet);
    b.payload_bits=29; b.payload.back()=0xff;
    error([&]{t.send(b,2);},TransportErrorCode::invalid_packet);
    CHECK(t.snapshot()==before); CHECK(channel(t,7)==a);
}
TEST_CASE("Unreliable actor bypasses reliable queue quotas but preserves packet limits") {
    auto c=config(); c.limits.max_outgoing_bunches=1; c.limits.max_payload_bytes=1;
    c.limits.max_records_per_packet=1; c.limits.max_packets_per_step=1;
    ControlTransport t(c); t.register_actor_channel({1,0,0,false});
    t.send(data(1,0x11,true),0); const auto a=channel(t,1);
    const auto direct=t.send(plain(1,0xaa),1); REQUIRE(direct.outgoing.size()==1);
    CHECK(channel(t,1)==a); CHECK(t.snapshot().queued_payload_bytes==1);
    CHECK(t.snapshot().outgoing_bunches==1);
    const auto before=t.snapshot();
    error([&]{t.receive(packet(0,{plain(1,0xaa),plain(1,0xab)}),2);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==before); CHECK(channel(t,1)==a);
    const auto accepted=t.receive(packet(0,{plain(1,0xac)}),2);
    REQUIRE(accepted.delivered.size()==1); CHECK(channel(t,1)==a);
    CHECK(acknowledgements(accepted)==std::vector<std::uint16_t>{0});
    // A second packet needs both prior and current ACK output, over this quota.
    const auto committed=t.snapshot();
    error([&]{t.receive(packet(1,{plain(1,0xad)}),3);},TransportErrorCode::resource_limit);
    CHECK(t.snapshot()==committed); CHECK(channel(t,1)==a);
}
