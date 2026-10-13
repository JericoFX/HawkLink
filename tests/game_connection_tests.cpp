#include "hawken/game_connection.hpp"
#include "hawken/packet_writer.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace hawken;
using game::ConnectionErrorCode;
using control::HandshakeState;
namespace login=control::login;
namespace post=control::posthello;
using Bytes=std::vector<std::uint8_t>;
static_assert(!std::is_copy_constructible_v<game::GameConnection>);
static_assert(!std::is_move_constructible_v<game::GameConnection>);
static_assert(!std::is_copy_assignable_v<game::GameConnection>);
static_assert(!std::is_move_assignable_v<game::GameConnection>);

game::ConnectionConfig config() {
    session::SessionKey key{};
    for (std::size_t i=0;i<key.size();++i) key[i]=static_cast<std::uint8_t>(i);
    // Explicit SYNTHETIC seeds/limits, not native effective defaults.
    return {{0,0x11,0x22,0x33,0x44,0x55,0x66,0x77},key,
        {0,0xffffffffU,0xffffffffU,0,0,false,64,{16,16,16,1024,16,16},5.0},
        control::Endian::little,0,7,11,{false,false},{112,16,16,1024},
        {{128,64},0x1234abcdU,30000}};
}
Bytes hex(std::string_view text) {
    auto nibble=[](char c)->std::uint8_t {
        if (c>='0' && c<='9') return static_cast<std::uint8_t>(c-'0');
        if (c>='a' && c<='f') return static_cast<std::uint8_t>(c-'a'+10);
        throw std::logic_error("invalid public fixture");
    };
    if (text.size()%2!=0) throw std::logic_error("invalid public fixture");
    Bytes result;
    for (std::size_t i=0;i<text.size();i+=2)
        result.push_back(static_cast<std::uint8_t>((nibble(text[i])<<4)|nibble(text[i+1])));
    return result;
}
// Independent first nonce fixtures: integer LSB placement + verified existing
// OpenSSL3.5.7 keystream + stdlib HMAC, not GameConnection/BitWriter/ChaCha copy.
// Exact fields/source hashes: evidence/game-connection-contract/literal-vectors.json.
Bytes initial_oracle() {
    return hex("00112233445566770100000000000000"
        "2f24f44250c12ff8a25231f5"
        "b86b27563d1079c30df13edd292c38e9f3f505020a5095234f85a371ac3df356");
}
Bytes first_ack_oracle() {
    return hex("00112233445566770100000000000080"
        "5af2f637"
        "312e29776e1451e8b400bf7a9ee0e58db3633c394b608e51ae04847de1b406f6");
}
Bytes coalesced_oracle() {
    return hex("00112233445566770100000000000000"
        "2f24f44250412bf8a25231253eb39d3379465f21efdeef784cbf8442eb058f"
        "7bc5dce5aec7b86006c2323f16e9cf194c7cdc25d5ed4155c81c674ed536e2");
}

wire::Packet inner(std::uint16_t pid,std::uint16_t seq,const Bytes& logical,
                   bool open=true,bool close=false) {
    wire::Packet p; p.packet_id=pid;
    wire::PacketRecord r; auto& b=r.bunch;
    b.control=open||close; b.open=open; b.close=close; b.reliable=true;
    b.channel_type=1; b.sequence=seq; b.payload=logical; b.payload_bits=logical.size()*8;
    p.records.push_back(std::move(r));
    return p;
}
Bytes request(session::Context& client,std::uint16_t pid,std::uint16_t seq,
              const Bytes& logical,bool open=true,bool close=false,std::uint32_t max_packet_bytes=64) {
    // Additional fixtures deliberately use already GREEN lower components;
    // expected replies/first envelopes remain independent literal bytes.
    return client.encode(wire::encode_packet(inner(pid,seq,logical,open,close),max_packet_bytes));
}
Bytes ack_request(session::Context& client,std::uint16_t pid,std::uint16_t ack_id) {
    wire::Packet p; p.packet_id=pid;
    wire::PacketRecord r; r.is_ack=true; r.ack_packet_id=ack_id; p.records.push_back(r);
    return client.encode(wire::encode_packet(p,64));
}
struct Inspected { std::vector<Bytes> plain; std::vector<wire::Packet> packets; std::vector<wire::Bunch> bunches; };
Inspected inspect(session::Context& client,const game::ConnectionStep& step,std::uint32_t max_packet_bytes=64) {
    Inspected out;
    for (const auto& datagram:step.datagrams) {
        out.plain.push_back(client.decode(datagram));
        out.packets.push_back(wire::decode_packet(out.plain.back(),max_packet_bytes));
        for (const auto& record:out.packets.back().records)
            if (!record.is_ack) out.bunches.push_back(record.bunch);
    }
    return out;
}
void initial(game::GameConnection& c,session::Context& client,bool open=true,std::uint32_t max_packet_bytes=64) {
    const auto step=c.receive(request(client,0,1,hex("1a07000000"),open,false,max_packet_bytes),0);
    REQUIRE_FALSE(step.closed);
    REQUIRE(c.snapshot().handshake==HandshakeState::awaiting_response);
    const auto out=inspect(client,step,max_packet_bytes);
    REQUIRE(out.bunches.size()==1); CHECK(out.bunches[0].payload==hex("1b00000000"));
}
void ready(game::GameConnection& c,session::Context& client,std::uint32_t max_packet_bytes=64) {
    initial(c,client,true,max_packet_bytes);
    const auto step=c.receive(request(client,1,2,hex("1cc3363f8d"),false,false,max_packet_bytes),1);
    REQUIRE_FALSE(step.closed); REQUIRE(c.snapshot().handshake==HandshakeState::awaiting_hello);
    const auto out=inspect(client,step,max_packet_bytes);
    REQUIRE(out.bunches.size()==1); CHECK(out.bunches[0].payload==hex("1d"));
}
void hello(game::GameConnection& c,session::Context& client,std::uint32_t max_packet_bytes=64) {
    ready(c,client,max_packet_bytes);
    const auto step=c.receive(request(client,2,3,hex("00080000001400000098badcfe"),false,false,max_packet_bytes),2);
    REQUIRE(step.hello.has_value()); CHECK(step.hello->version==11); CHECK(step.hello->caps==0xfedcba98U);
    REQUIRE_FALSE(step.closed); REQUIRE(c.snapshot().handshake==HandshakeState::hello_validated);
    const auto out=inspect(client,step,max_packet_bytes);
    REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("030b00000009000000313233344142434400"));
    CHECK(out.bunches[0].sequence==3); CHECK(out.bunches[0].reliable);
    CHECK_FALSE(step.login_candidate.has_value());
    CHECK(c.snapshot().login==login::Snapshot{login::State::awaiting_login,0xfedcba98U,std::nullopt});
}
template<class Action>
void expect_error(Action action,ConnectionErrorCode code) {
    try { action(); }
    catch (const game::ConnectionError& e) {
        CHECK(e.code()==code);
        CHECK(std::string(e.what())=="Game connection operation could not be completed.");
        CHECK(std::string(e.what()).find("0011223344556677")==std::string::npos);
        CHECK(std::string(e.what()).find("never-echo-private")==std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed ConnectionError"); return; }
    FAIL_CHECK("connection accepted rejected input");
}
}

TEST_CASE("Game connection fresh state has no admission or implicit handshake progress") {
    game::GameConnection c(config());
    CHECK(c.snapshot()==game::ConnectionSnapshot{HandshakeState::awaiting_initial,false,
        {login::State::awaiting_hello,std::nullopt,std::nullopt}});
    CHECK(c.tick(0).datagrams.empty()); CHECK_FALSE(c.snapshot().closed);
}
TEST_CASE("Game connection rejects invalid scalar and wrapper configuration") {
    auto reject=[](const game::ConnectionConfig& cfg) {
        expect_error([&]{game::GameConnection c(cfg);},ConnectionErrorCode::invalid_configuration);
    };
    auto cfg=config(); cfg.endian=static_cast<control::Endian>(99); reject(cfg);
    cfg=config(); cfg.server_min=12; reject(cfg);
    for (std::size_t bytes:{0U,111U,65508U}) { cfg=config(); cfg.limits.max_datagram_bytes=bytes; reject(cfg); }
    cfg=config(); cfg.transport.max_packet_bytes=65507; cfg.limits.max_datagram_bytes=65507; reject(cfg);
    cfg=config(); cfg.transport.max_packet_bytes=1; reject(cfg);
    cfg=config(); cfg.transport.incoming_timeout=0; reject(cfg);
    cfg=config(); cfg.transport.limits.max_incoming_bunches=0; reject(cfg);
}
TEST_CASE("Game connection validates own aggregate work and output quotas") {
    using Limits=game::ConnectionLimits;
    for (std::size_t Limits::*member:{&game::ConnectionLimits::max_logical_messages_per_step,
        &game::ConnectionLimits::max_datagrams_per_step}) {
        for (std::size_t value:{0U,1025U}) {
            auto cfg=config(); cfg.limits.*member=value;
            expect_error([&]{game::GameConnection c(cfg);},ConnectionErrorCode::invalid_configuration);
        }
    }
    for (std::size_t value:{0U,1048577U}) {
        auto cfg=config(); cfg.limits.max_output_bytes_per_step=value;
        expect_error([&]{game::GameConnection c(cfg);},ConnectionErrorCode::invalid_configuration);
    }
}
TEST_CASE("Game connection accepts exact public zero credentials and highest interior cap") {
    auto cfg=config(); cfg.session_id={}; cfg.session_key={};
    game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto step=c.receive(request(client,0,1,hex("1a07000000")),0);
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_response); CHECK_FALSE(step.closed);
    cfg=config(); cfg.transport.max_packet_bytes=65459; cfg.limits.max_datagram_bytes=65507;
    game::GameConnection high(cfg); CHECK_FALSE(high.snapshot().closed);
}
TEST_CASE("Game connection first exchange matches independent full envelope and packet literals") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    // Check fixture input exactly, rather than symmetric encode/decode alone.
    CHECK(request(client,0,1,hex("1a07000000"))==initial_oracle());
    const auto step=c.receive(initial_oracle(),0);
    REQUIRE(step.datagrams.size()==2); CHECK(step.datagrams[0]==first_ack_oracle());
    const auto out=inspect(client,step);
    REQUIRE(out.plain.size()==2);
    CHECK(out.plain[0]==hex("00400020"));
    CHECK(out.plain[1]==hex("0100011040506c0000000004"));
    REQUIRE(out.bunches.size()==1); CHECK(out.bunches[0].sequence==1);
    CHECK(out.bunches[0].payload==hex("1b00000000"));
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_response);
    CHECK_FALSE(step.hello.has_value()); CHECK_FALSE(step.closed);
}
TEST_CASE("Game connection initial scalar is consumed without imposing a native value") {
    for (const auto& logical:std::vector<Bytes>{hex("1a00000000"),hex("1affffff7f"),
        hex("1a00000080"),hex("1affffffff")}) {
        const auto cfg=config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        const auto step=c.receive(request(client,0,1,logical),0);
        CHECK(c.snapshot().handshake==HandshakeState::awaiting_response);
        const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
        CHECK(out.bunches[0].payload==hex("1b00000000"));
    }
}
TEST_CASE("Game connection preserves caller packet and reliable seeds instead of resetting them") {
    auto cfg=config(); cfg.transport={100,7,99,10,20,true,64,{16,16,16,1024,16,16},5.0};
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto step=c.receive(request(client,8,21,hex("1a07000000"),false),0);
    const auto out=inspect(client,step); REQUIRE(out.packets.size()==2); REQUIRE(out.bunches.size()==1);
    CHECK(out.packets[0].packet_id==100); CHECK(out.packets[1].packet_id==101);
    CHECK(out.bunches[0].sequence==11); CHECK(out.bunches[0].payload==hex("1b00000000"));
}
TEST_CASE("Game connection ACK only input before initial does not synthesize a handshake message") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto before=c.receive(ack_request(client,0,0x3fff),0);
    CHECK(inspect(client,before).bunches.empty());
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_initial);
    const auto first=c.receive(request(client,1,1,hex("1a07000000")),1);
    REQUIRE(inspect(client,first).bunches.size()==1); CHECK_FALSE(first.closed);
}
TEST_CASE("Game connection preserves persistent session counters through ready and Hello") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    initial(c,client);
    const auto response=c.receive(request(client,1,2,hex("1cc3363f8d"),false),1);
    REQUIRE(response.datagrams.size()==2);
    const auto out=inspect(client,response);
    CHECK(out.plain[0]==hex("024000600010")); CHECK(out.plain[1]==hex("0300012040107404"));
    CHECK(client.last_received()==session::direction_bit+4);
    const auto last=c.receive(request(client,2,3,hex("00080000001400000098badcfe"),false),2);
    REQUIRE(last.hello.has_value()); CHECK(last.hello->version==11); CHECK(last.hello->caps==0xfedcba98U);
    const auto final=inspect(client,last);
    REQUIRE(final.plain.size()==2); CHECK(final.plain[0]==hex("04c000a00010"));
    REQUIRE(final.bunches.size()==1);
    CHECK(final.bunches[0].payload==hex("030b00000009000000313233344142434400"));
    CHECK(client.last_received()==session::direction_bit+6); CHECK_FALSE(last.closed);
}
TEST_CASE("Game connection consumes independent coalesced prefixes in one reliable bunch") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto step=c.receive(coalesced_oracle(),0);
    REQUIRE(step.hello.has_value()); CHECK(step.hello->version==11); CHECK(step.hello->caps==0xfedcba98U);
    CHECK(c.snapshot().handshake==HandshakeState::hello_validated);
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==3);
    CHECK(out.bunches[0].payload==hex("1b00000000")); CHECK(out.bunches[1].payload==hex("1d"));
    CHECK(out.bunches[2].payload==hex("030b00000009000000313233344142434400"));
    CHECK(out.bunches[0].sequence==1); CHECK(out.bunches[1].sequence==2); CHECK_FALSE(step.closed);
}
TEST_CASE("Game connection consumes multiple ordered bunch records in the same packet") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,hex("1a07000000"));
    p.records.push_back(inner(0,2,hex("1cc3363f8d"),false).records[0]);
    p.records.push_back(inner(0,3,hex("00080000001400000098badcfe"),false).records[0]);
    const auto step=c.receive(client.encode(wire::encode_packet(p,64)),0);
    REQUIRE(step.hello.has_value()); CHECK(step.hello->version==11);
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==3);
    CHECK(out.bunches[0].payload==hex("1b00000000")); CHECK(out.bunches[1].payload==hex("1d"));
    CHECK(out.bunches[2].payload==hex("030b00000009000000313233344142434400"));
}
TEST_CASE("Game connection reliable reordering delays logical delivery until consecutive drain") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto future=c.receive(request(client,1,2,hex("1cc3363f8d"),false),0);
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_initial);
    CHECK(inspect(client,future).bunches.empty());
    const auto first=c.receive(request(client,0,1,hex("1a07000000")),1);
    const auto out=inspect(client,first); REQUIRE(out.bunches.size()==2);
    CHECK(out.bunches[0].payload==hex("1b00000000")); CHECK(out.bunches[1].payload==hex("1d"));
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_hello);
}
TEST_CASE("Game connection reliable duplicate with a fresh nonce does not repeat challenge") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    initial(c,client);
    const auto duplicate=c.receive(request(client,1,1,hex("1a07000000")),1);
    CHECK(inspect(client,duplicate).bunches.empty());
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_response); CHECK_FALSE(duplicate.closed);
}
TEST_CASE("Game connection explicit endian and signed challenge CRC are retained") {
    auto cfg=config(); cfg.endian=control::Endian::big; cfg.challenge=0x7fffffffU;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto first=c.receive(request(client,0,1,hex("1a00000007")),0);
    const auto out=inspect(client,first); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("1b7fffffff"));
    const auto response=c.receive(request(client,1,2,hex("1c8b9008b6"),false),1);
    CHECK_FALSE(response.closed); const auto ready_out=inspect(client,response);
    REQUIRE(ready_out.bunches.size()==1); CHECK(ready_out.bunches[0].payload==hex("1d"));
    const auto last=c.receive(request(client,2,3,hex("000000000800000014fedcba98"),false),2);
    REQUIRE(last.hello.has_value()); CHECK(last.hello->version==11); CHECK(last.hello->caps==0xfedcba98U);
    const auto final=inspect(client,last); REQUIRE(final.bunches.size()==1);
    CHECK(final.bunches[0].payload==hex("030000000b00000009313233344142434400"));
}
TEST_CASE("Game connection CRC mismatch closes without Ready or partial logical success") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); initial(c,client);
    const auto step=c.receive(request(client,1,2,hex("1cc2363f8d"),false),1);
    CHECK(step.closed); CHECK(c.snapshot().closed); CHECK(c.snapshot().handshake==HandshakeState::closed);
    CHECK(inspect(client,step).bunches.empty()); CHECK_FALSE(step.hello.has_value());
}
TEST_CASE("Game connection incompatible Hello emits exact Upgrade and closes") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); ready(c,client);
    const auto step=c.receive(request(client,2,3,hex("000100000006000000ffffffff"),false),2);
    CHECK(step.closed); CHECK_FALSE(step.hello.has_value());
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("02070000000b000000")); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection preserves native inverted Hello comparisons and full caps") {
    auto cfg=config(); cfg.server_min=1; cfg.server_max=100;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    ready(c,client);
    const auto step=c.receive(request(client,2,3,hex("00460000001e000000ffffffff"),false),2);
    REQUIRE(step.hello.has_value()); CHECK(step.hello->version==30); CHECK(step.hello->caps==0xffffffffU);
    CHECK_FALSE(step.closed);
}
TEST_CASE("Game connection explicit signed version boundary does not normalize u32 bits") {
    auto cfg=config(); cfg.server_min=std::numeric_limits<std::int32_t>::min(); cfg.server_max=-1;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    ready(c,client);
    const auto step=c.receive(request(client,2,3,hex("0000000080ffffff7fffffffff"),false),2);
    REQUIRE(step.hello.has_value()); CHECK(step.hello->version==-1); CHECK(step.hello->caps==0xffffffffU);
}
TEST_CASE("Game connection malformed authentication is rejected before handshake with no nonce consumption") {
    game::GameConnection c(config()); const auto before=c.snapshot();
    auto bad=initial_oracle(); bad.back()^=1;
    expect_error([&]{c.receive(bad,10);},ConnectionErrorCode::authentication_failed); CHECK(c.snapshot()==before);
    bad=initial_oracle(); bad[0]^=1;
    expect_error([&]{c.receive(bad,10);},ConnectionErrorCode::authentication_failed); CHECK(c.snapshot()==before);
    const auto accepted=c.receive(initial_oracle(),0); CHECK_FALSE(accepted.closed);
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_response);
}
TEST_CASE("Game connection opposite direction cannot enter the logical dispatcher") {
    const auto cfg=config(); game::GameConnection c(cfg); const auto before=c.snapshot();
    session::Context wrong(session::Role::server,cfg.session_id,cfg.session_key);
    const auto packet=wrong.encode(hex("008005400041a17100000010"));
    expect_error([&]{c.receive(packet,0);},ConnectionErrorCode::authentication_failed); CHECK(c.snapshot()==before);
    CHECK_FALSE(c.receive(initial_oracle(),0).closed);
}
TEST_CASE("Game connection replay rejection does not abort the live handshake") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto first=request(client,0,1,hex("1a07000000")); c.receive(first,0);
    const auto before=c.snapshot(); expect_error([&]{c.receive(first,1);},ConnectionErrorCode::replay);
    CHECK(c.snapshot()==before);
    CHECK_FALSE(c.receive(request(client,1,2,hex("1cc3363f8d"),false),1).closed);
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_hello);
}
TEST_CASE("Game connection preauthentication size limits preserve the context") {
    game::GameConnection c(config()); const auto before=c.snapshot();
    for (const auto& bad:std::vector<Bytes>{Bytes{},Bytes(48),Bytes(113,0xff)}) {
        expect_error([&]{c.receive(bad,9);},ConnectionErrorCode::invalid_datagram); CHECK(c.snapshot()==before);
    }
    CHECK_FALSE(c.receive(initial_oracle(),0).closed);
}
TEST_CASE("Game connection invalid time is checked before consuming the next nonce") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    c.receive(request(client,0,1,hex("1a07000000")),10);
    const auto next=request(client,1,2,hex("1cc3363f8d"),false); const auto before=c.snapshot();
    for (double now:{-1.0,9.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        expect_error([&]{c.receive(next,now);},ConnectionErrorCode::invalid_time); CHECK(c.snapshot()==before);
        expect_error([&]{c.tick(now);},ConnectionErrorCode::invalid_time); CHECK(c.snapshot()==before);
    }
    CHECK_FALSE(c.receive(next,10).closed); CHECK(c.snapshot().handshake==HandshakeState::awaiting_hello);
}
TEST_CASE("Game connection authenticated invalid marker closes permanently") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    expect_error([&]{c.receive(client.encode(Bytes{0}),0);},ConnectionErrorCode::invalid_packet);
    CHECK(c.snapshot().closed);
    expect_error([&]{c.receive(initial_oracle(),1);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Game connection authenticated unsupported channel closes without first reply") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,hex("1a07000000")); p.records[0].bunch.channel=1;
    expect_error([&]{c.receive(client.encode(wire::encode_packet(p,64)),0);},ConnectionErrorCode::invalid_packet);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection refuses partial archive carry across bunches") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,hex("1a0700")); p.records.push_back(inner(0,2,hex("0000"),false).records[0]);
    expect_error([&]{c.receive(client.encode(wire::encode_packet(p,64)),0);},ConnectionErrorCode::invalid_archive);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection refuses nonbyte logical payload without changing the lower codec") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,Bytes{0x1a}); p.records[0].bunch.payload_bits=7;
    expect_error([&]{c.receive(client.encode(wire::encode_packet(p,64)),0);},ConnectionErrorCode::invalid_archive);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection out of order known archive fails closed with fixed error") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    expect_error([&]{c.receive(request(client,0,1,hex("1cc3363f8d")),0);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot().closed); expect_error([&]{c.tick(1);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Game connection valid prefix followed by unsupported suffix returns no partial result") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto logical=hex("1a07000000"); const std::string secret="never-echo-private";
    logical.insert(logical.end(),secret.begin(),secret.end());
    expect_error([&]{c.receive(request(client,0,1,logical),0);},ConnectionErrorCode::invalid_archive);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection coalesced logical message quota closes instead of returning partial replies") {
    auto cfg=config(); cfg.limits.max_logical_messages_per_step=1; game::GameConnection c(cfg);
    expect_error([&]{c.receive(coalesced_oracle(),0);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection logical message quota counts across delivered bunches") {
    auto cfg=config(); cfg.limits.max_logical_messages_per_step=1; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,hex("1a07000000")); p.records.push_back(inner(0,2,hex("1cc3363f8d"),false).records[0]);
    expect_error([&]{c.receive(client.encode(wire::encode_packet(p,64)),0);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection aggregate output datagram quota closes after authenticated input") {
    auto cfg=config(); cfg.limits.max_datagrams_per_step=1; game::GameConnection c(cfg);
    expect_error([&]{c.receive(initial_oracle(),0);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection output byte budget includes both complete session wrappers exactly") {
    SUBCASE("exact112") {
        auto cfg=config(); cfg.limits.max_output_bytes_per_step=112; game::GameConnection c(cfg);
        const auto step=c.receive(initial_oracle(),0); REQUIRE(step.datagrams.size()==2);
        CHECK(step.datagrams[0].size()+step.datagrams[1].size()==112); CHECK_FALSE(step.closed);
    }
    SUBCASE("onebelow") {
        auto cfg=config(); cfg.limits.max_output_bytes_per_step=111; game::GameConnection c(cfg);
        expect_error([&]{c.receive(initial_oracle(),0);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
    }
}
TEST_CASE("Game connection lower record quota remains enforced and is a fatal composition error") {
    auto cfg=config(); cfg.transport.limits.max_records_per_packet=1; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    auto p=inner(0,1,hex("1a07000000")); p.records.push_back(inner(0,2,hex("1cc3363f8d"),false).records[0]);
    expect_error([&]{c.receive(client.encode(wire::encode_packet(p,64)),0);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection preopen tick retries the same sequence in a newly wrapped packet") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); initial(c,client,false);
    const auto at_one=c.tick(1); CHECK(inspect(client,at_one).bunches.empty());
    const auto retry=c.tick(1.1); const auto out=inspect(client,retry);
    REQUIRE(out.bunches.size()==1); CHECK(out.bunches[0].sequence==1);
    CHECK(out.bunches[0].payload==hex("1b00000000")); CHECK(out.packets.back().packet_id==3);
    CHECK(client.last_received()==session::direction_bit+4);
    CHECK(c.snapshot().handshake==HandshakeState::awaiting_response);
    CHECK(c.tick(1.5).datagrams.empty());
}
TEST_CASE("Game connection late old ACK does not suppress retry of the new association") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); initial(c,client,false);
    inspect(client,c.tick(1.1)); // ACK replayPacket2; retryPacket3, not original1.
    inspect(client,c.receive(ack_request(client,1,1),1.2));
    const auto next=inspect(client,c.tick(2.3)); REQUIRE(next.bunches.size()==1);
    CHECK(next.bunches[0].sequence==1); CHECK(next.bunches[0].payload==hex("1b00000000"));
}
TEST_CASE("Game connection open confirmation disables invented universal retry") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); initial(c,client,true);
    CHECK(inspect(client,c.tick(100)).bunches.empty()); CHECK_FALSE(c.snapshot().closed);
}
TEST_CASE("Game connection incoming timeout preempts outputs and terminates the context") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    c.receive(request(client,1,2,hex("1cc3363f8d"),false),0);
    CHECK_FALSE(c.tick(5).closed);
    const auto end=c.tick(5.01); CHECK(end.closed); CHECK(end.datagrams.empty()); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection explicit reply flags do not infer an open default") {
    auto cfg=config(); cfg.reply_flags={true,true}; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto first=c.receive(request(client,0,1,hex("1a07000000")),0);
    const auto out=inspect(client,first); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].open); CHECK(out.bunches[0].close); CHECK(out.bunches[0].control);
    CHECK_FALSE(first.closed);
    const auto end=c.receive(ack_request(client,1,1),1); CHECK(end.closed); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection peer close prevents creation of a logical challenge reply") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto end=c.receive(request(client,0,1,hex("1a07000000"),true,true),0);
    CHECK(end.closed); CHECK(inspect(client,end).bunches.empty()); CHECK(c.snapshot().closed);
}
TEST_CASE("Game connection postHello ACKs confirm all three replies without repeating Challenge3") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    // ACKs are exact, not cumulative: confirm Challenge1, Ready3, Challenge3packet5.
    wire::Packet packet; packet.packet_id=3;
    for (const auto id:{std::uint16_t{1},std::uint16_t{3},std::uint16_t{5}}) {
        wire::PacketRecord record; record.is_ack=true; record.ack_packet_id=id;
        packet.records.push_back(record);
    }
    const auto acks=inspect(client,c.receive(client.encode(wire::encode_packet(packet,64)),3));
    CHECK(acks.bunches.empty()); REQUIRE(acks.packets.size()==1);
    CHECK(acks.packets[0].packet_id==6); REQUIRE(acks.packets[0].records.size()==2);
    CHECK(acks.packets[0].records[0].is_ack); CHECK(acks.packets[0].records[0].ack_packet_id==2);
    CHECK(acks.packets[0].records[1].is_ack); CHECK(acks.packets[0].records[1].ack_packet_id==3);
    const auto after=inspect(client,c.tick(4)); CHECK(after.bunches.empty());
    REQUIRE(after.packets.size()==1); CHECK(after.packets[0].packet_id==7);
    REQUIRE(after.packets[0].records.size()==1); CHECK(after.packets[0].records[0].is_ack);
    CHECK(after.packets[0].records[0].ack_packet_id==3);
    CHECK(c.snapshot().handshake==HandshakeState::hello_validated); CHECK_FALSE(c.snapshot().closed);
}
TEST_CASE("Game connection HelloValidated ACK3 alone retransmits unconfirmed Challenge1") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    const auto out=inspect(client,c.receive(ack_request(client,3,3),3));
    REQUIRE(out.packets.size()==2); CHECK(out.packets[0].packet_id==6);
    REQUIRE(out.packets[0].records.size()==1); CHECK_FALSE(out.packets[0].records[0].is_ack);
    REQUIRE(out.bunches.size()==1); CHECK(out.bunches[0].sequence==1);
    CHECK(out.bunches[0].reliable); CHECK(out.bunches[0].channel==0);
    CHECK(out.bunches[0].channel_type==1); CHECK(out.bunches[0].payload_bits==40);
    CHECK(out.bunches[0].payload==hex("1b00000000")); // Retransmit old1b, not another logical Challenge3.
    CHECK(out.packets[1].packet_id==7); REQUIRE(out.packets[1].records.size()==2);
    CHECK(out.packets[1].records[0].is_ack); CHECK(out.packets[1].records[0].ack_packet_id==2);
    CHECK(out.packets[1].records[1].is_ack); CHECK(out.packets[1].records[1].ack_packet_id==3);
    CHECK(c.snapshot().handshake==HandshakeState::hello_validated); CHECK_FALSE(c.snapshot().closed);
}
TEST_CASE("Game connection suppressed reliable duplicate after HelloValidated only requests ACK") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    const auto duplicate=c.receive(request(client,3,3,hex("00080000001400000098badcfe"),false),3);
    CHECK(inspect(client,duplicate).bunches.empty()); CHECK_FALSE(duplicate.closed);
}
TEST_CASE("Game connection postHello NetSpeed updates speed without admission") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    const auto speed=c.receive(request(client,3,4,hex("0410270000"),false),3);
    CHECK_FALSE(speed.closed); CHECK_FALSE(speed.login_candidate.has_value());
    CHECK(inspect(client,speed).bunches.empty());
    CHECK(c.snapshot().login==login::Snapshot{login::State::awaiting_login,0xfedcba98U,10000});
}
TEST_CASE("Game connection terminal operations cannot reactivate or replay its session") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); initial(c,client);
    c.receive(request(client,1,2,hex("1c00000000"),false),1); const auto before=c.snapshot(); REQUIRE(before.closed);
    expect_error([&]{c.receive(initial_oracle(),2);},ConnectionErrorCode::terminal_state); CHECK(c.snapshot()==before);
    expect_error([&]{c.tick(2);},ConnectionErrorCode::terminal_state); CHECK(c.snapshot()==before);
}

TEST_CASE("Game connection requires valid explicit postHello limits and driver speed") {
    for (const auto limits:std::array<post::Limits,3>{{{0,64},{128,0},
        {128,static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1}}}) {
        auto cfg=config(); cfg.login.limits=limits;
        expect_error([&]{game::GameConnection c(cfg);},ConnectionErrorCode::invalid_configuration);
    }
    auto cfg=config(); cfg.login.driver_max_speed=1799;
    expect_error([&]{game::GameConnection c(cfg);},ConnectionErrorCode::invalid_configuration);
}
TEST_CASE("Game connection Login preserves literal raw fields without admission or prior NetSpeed") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    auto step=c.receive(request(client,3,4,
        hex("05030000005200fffdffffff000100d80000040302017856341200000080ffffffff"),false),3);
    REQUIRE(step.login_candidate.has_value());
    const post::Login expected{
        {post::StringForm::narrow,std::u16string{u'R',u'\0',u'\u00ff'}},
        {post::StringForm::wide,std::u16string{u'\u0100',static_cast<char16_t>(0xd800),u'\0'}},
        {0x01020304U,0x12345678U,0x80000000U,0xffffffffU}};
    CHECK(*step.login_candidate==expected); CHECK_FALSE(step.closed);
    CHECK(inspect(client,step).bunches.empty()); // No Welcome or implicit world.
    CHECK(c.snapshot().login==login::Snapshot{login::State::login_received,0xfedcba98U,std::nullopt});
    const auto before=c.snapshot(); step.login_candidate->response.units.clear();
    CHECK(c.snapshot()==before); CHECK_FALSE(c.tick(3).login_candidate.has_value());
}
TEST_CASE("Game connection coalesced Hello NetSpeed Login consumes prefixes in one bunch") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); ready(c,client);
    const auto step=c.receive(request(client,2,3,
        hex("00080000001400000098badcfe04581b0000"
            "05030000005200fffdffffff000100d80000040302017856341200000080ffffffff"),false),2);
    REQUIRE(step.hello.has_value()); REQUIRE(step.login_candidate.has_value());
    CHECK(step.login_candidate->unique_id==std::array<std::uint32_t,4>{0x01020304U,0x12345678U,0x80000000U,0xffffffffU});
    CHECK(c.snapshot().login==login::Snapshot{login::State::login_received,0xfedcba98U,7000});
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("030b00000009000000313233344142434400")); CHECK_FALSE(step.closed);
}
TEST_CASE("Game connection postHello big endian swaps scalars but preserves raw wide units") {
    auto cfg=config(); cfg.endian=control::Endian::big;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    inspect(client,c.receive(request(client,0,1,hex("1a00000007")),0));
    inspect(client,c.receive(request(client,1,2,hex("1c8d3f36c3"),false),1));
    const auto begin=c.receive(request(client,2,3,hex("000000000800000014fedcba98"),false),2);
    const auto out=inspect(client,begin); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("030000000b00000009313233344142434400"));
    const auto step=c.receive(request(client,3,4,
        hex("048000000005000000035200fffffffffd000100d80000010203041234567880000000ffffffff"),false),3);
    REQUIRE(step.login_candidate.has_value());
    CHECK(step.login_candidate->url==post::RawString{post::StringForm::wide,
        std::u16string{u'\u0100',static_cast<char16_t>(0xd800),u'\0'}});
    CHECK(step.login_candidate->unique_id==std::array<std::uint32_t,4>{0x01020304U,0x12345678U,0x80000000U,0xffffffffU});
    CHECK(c.snapshot().login==login::Snapshot{login::State::login_received,0xfedcba98U,1800});
    CHECK_FALSE(step.closed); CHECK(inspect(client,step).bunches.empty());
}
TEST_CASE("Game connection peer Failure afterHello is terminal without reflecting text") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    const auto step=c.receive(request(client,3,4,hex("0604000000500049ff"),false),3);
    CHECK(step.closed); CHECK_FALSE(step.login_candidate.has_value());
    CHECK(inspect(client,step).bunches.empty()); CHECK(c.snapshot().login.state==login::State::closed);
    const auto before=c.snapshot();
    expect_error([&]{c.tick(4);},ConnectionErrorCode::terminal_state); CHECK(c.snapshot()==before);
}
TEST_CASE("Game connection malformed postHello prefix closes without partial candidate") {
    for (const auto archive:std::array<std::string_view,4>{"04","0500000080",
        "0500000000000000000403020178563412","ff"}) {
        const auto cfg=config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
        expect_error([&]{c.receive(request(client,3,4,hex(archive),false),3);},ConnectionErrorCode::invalid_archive);
        CHECK(c.snapshot().closed);
    }
}
TEST_CASE("Game connection postHello quotas close instead of returning candidate or pending replies") {
    SUBCASE("challenge encoding budget") {
        auto cfg=config(); cfg.login.limits={17,9}; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); ready(c,client);
        expect_error([&]{c.receive(request(client,2,3,hex("00080000001400000098badcfe"),false),2);},
            ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
    }
    SUBCASE("Login whole-message budget") {
        auto cfg=config(); cfg.login.limits={33,64}; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
        expect_error([&]{c.receive(request(client,3,4,
            hex("05030000005200fffdffffff000100d80000040302017856341200000080ffffffff"),false),3);},
            ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
    }
    SUBCASE("coalesced logical count") {
        auto cfg=config(); cfg.limits.max_logical_messages_per_step=2; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); ready(c,client);
        expect_error([&]{c.receive(request(client,2,3,
            hex("00080000001400000098badcfe04581b0000050000000000000000040302017856341200000080ffffffff"),false),2);},
            ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
    }
}
TEST_CASE("Game connection Hello response output cap includes newly encrypted Challenge3") {
    // Literal Hello ACK interior 04c000a00010:6+48 wrapper=54 bytes.
    // Challenge3:14 packet-ID +36 reliable metadata +144 payload +1 stop
    // bit ->25 interior bytes +48 wrapper=73. Total54+73=127, not a native MTU.
    for (const auto limit:std::array<std::size_t,2>{126,127}) {
        auto cfg=config(); cfg.limits.max_output_bytes_per_step=limit; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); ready(c,client);
        const auto packet=request(client,2,3,hex("00080000001400000098badcfe"),false);
        if (limit==126) {
            expect_error([&]{c.receive(packet,2);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
        } else {
            const auto step=c.receive(packet,2); REQUIRE(step.datagrams.size()==2);
            CHECK(step.datagrams[0].size()+step.datagrams[1].size()==127); CHECK_FALSE(step.closed);
        }
    }
}
TEST_CASE("Game connection Login followed by invalid suffix or second Login cannot escape a candidate") {
    for (const auto suffix:std::array<std::string_view,2>{"ff",
        "050000000000000000040302017856341200000080ffffffff"}) {
        const auto cfg=config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
        auto raw=hex("050000000000000000040302017856341200000080ffffffff");
        const auto tail=hex(suffix); raw.insert(raw.end(),tail.begin(),tail.end());
        expect_error([&]{c.receive(request(client,3,4,raw,false),3);},
            suffix=="ff"?ConnectionErrorCode::invalid_archive:ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot().closed);
        expect_error([&]{c.tick(4);},ConnectionErrorCode::terminal_state);
    }
}
TEST_CASE("Game connection duplicate Login reliable sequence cannot deliver a second candidate") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    const auto raw=hex("050000000000000000040302017856341200000080ffffffff");
    const auto packet=request(client,3,4,raw,false);
    REQUIRE(c.receive(packet,3).login_candidate.has_value()); const auto before=c.snapshot();
    expect_error([&]{c.receive(packet,4);},ConnectionErrorCode::replay); CHECK(c.snapshot()==before);
    const auto duplicate=c.receive(request(client,4,4,raw,false),4);
    CHECK_FALSE(duplicate.login_candidate.has_value()); CHECK_FALSE(duplicate.closed);
    CHECK(c.snapshot()==before);
    expect_error([&]{c.receive(request(client,5,5,hex("0408070000"),false),5);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot().closed);
}

namespace {
void login_for_rejection(game::GameConnection& c,session::Context& client,control::Endian endian,std::uint32_t max_packet_bytes=64) {
    if (endian==control::Endian::little) {
        hello(c,client,max_packet_bytes);
    } else {
        inspect(client,c.receive(request(client,0,1,hex("1a00000007"),true,false,max_packet_bytes),0),max_packet_bytes);
        inspect(client,c.receive(request(client,1,2,hex("1c8d3f36c3"),false,false,max_packet_bytes),1),max_packet_bytes);
        const auto out=inspect(client,c.receive(request(client,2,3,hex("000000000800000014fedcba98"),false,false,max_packet_bytes),2),max_packet_bytes);
        REQUIRE(out.bunches.size()==1);
        CHECK(out.bunches[0].payload==hex("030000000b00000009313233344142434400"));
    }
    // Public raw Login fixture with zero-form strings: no retained credentials.
    // NetSpeed7000 first, then modern fouru32; expected bytes are independent.
    const auto logical=endian==control::Endian::little
        ? hex("04581b0000050000000000000000040302017856341200000080ffffffff")
        : hex("0400001b58050000000000000000010203041234567880000000ffffffff");
    const auto step=c.receive(request(client,3,4,logical,false,false,max_packet_bytes),3);
    REQUIRE(step.login_candidate.has_value()); REQUIRE_FALSE(step.closed);
    const auto out=inspect(client,step,max_packet_bytes); CHECK(out.bunches.empty());
    REQUIRE(c.snapshot().login==login::Snapshot{login::State::login_received,0xfedcba98U,7000});
}
std::uint64_t rejection_counter(const Bytes& datagram) {
    if (datagram.size()<16) throw std::logic_error("incomplete public envelope");
    std::uint64_t value=0;
    for (std::size_t i=0;i<8;++i) value|=static_cast<std::uint64_t>(datagram[8+i])<<(8*i);
    return value;
}
}

TEST_CASE("Game connection rejection emits fixed Failure through its existing reliable session") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian); const auto before=c.snapshot();
    const auto step=c.reject_login(4);
    REQUIRE(step.closed); CHECK_FALSE(step.hello.has_value()); CHECK_FALSE(step.login_candidate.has_value());
    REQUIRE(step.datagrams.size()==1); CHECK(step.datagrams[0].size()==76);
    CHECK(rejection_counter(step.datagrams[0])==(session::direction_bit|8));
    CHECK(std::equal(cfg.session_id.begin(),cfg.session_id.end(),step.datagrams[0].begin()));
    const auto out=inspect(client,step); REQUIRE(out.packets.size()==1); REQUIRE(out.bunches.size()==1);
    CHECK(out.packets[0].packet_id==7); CHECK(out.packets[0].records.size()==1);
    CHECK(out.bunches[0].sequence==4); CHECK(out.bunches[0].reliable);
    CHECK(out.bunches[0].channel==0); CHECK(out.bunches[0].channel_type==1);
    CHECK_FALSE(out.bunches[0].open); CHECK_FALSE(out.bunches[0].close); CHECK_FALSE(out.bunches[0].control);
    CHECK(out.bunches[0].payload==hex("06100000004c6f67696e2072656a65637465642e00"));
    const auto decoded=post::decode_exact(out.bunches[0].payload,cfg.endian,cfg.login.limits);
    REQUIRE(std::holds_alternative<post::Failure>(decoded));
    CHECK(std::get<post::Failure>(decoded).error==post::RawString{post::StringForm::narrow,
        std::u16string{u'L',u'o',u'g',u'i',u'n',u' ',u'r',u'e',u'j',u'e',u'c',u't',u'e',u'd',u'.',u'\0'}});
    CHECK(c.snapshot().closed); CHECK(c.snapshot().handshake==HandshakeState::closed);
    CHECK(c.snapshot().login==before.login); // Historical candidate stage, not admission.
}
TEST_CASE("Game connection rejection big endian changes only the Failure count") {
    auto cfg=config(); cfg.endian=control::Endian::big; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian);
    const auto step=c.reject_login(3); REQUIRE(step.closed); REQUIRE(step.datagrams.size()==1);
    CHECK(rejection_counter(step.datagrams[0])==(session::direction_bit|8));
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("06000000104c6f67696e2072656a65637465642e00"));
    CHECK(out.bunches[0].sequence==4); CHECK(out.packets[0].packet_id==7);
    CHECK(c.snapshot().login.state==login::State::login_received);
}
TEST_CASE("Game connection rejection before Login preserves each live handshake stage") {
    for (unsigned stage=0;stage<4;++stage) {
        const auto cfg=config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        if (stage==1) initial(c,client);
        if (stage==2) ready(c,client);
        if (stage==3) hello(c,client);
        const auto before=c.snapshot();
        expect_error([&]{c.reject_login(3);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot()==before);
    }
}
TEST_CASE("Game connection rejection invalid clocks preserve state and do not consume a send nonce") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian); const auto before=c.snapshot();
    for (const double now:std::array<double,6>{-1,2.999,std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),0}) {
        expect_error([&]{c.reject_login(now);},ConnectionErrorCode::invalid_time);
        CHECK(c.snapshot()==before);
    }
    const auto step=c.reject_login(3); REQUIRE(step.closed); REQUIRE(step.datagrams.size()==1);
    CHECK(rejection_counter(step.datagrams[0])==(session::direction_bit|8));
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].sequence==4); CHECK(out.packets[0].packet_id==7);
}
TEST_CASE("Game connection rejected owner cannot send a duplicate Failure or continue ticking") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian); REQUIRE(c.reject_login(4).closed);
    const auto before=c.snapshot();
    expect_error([&]{c.reject_login(5);},ConnectionErrorCode::terminal_state);
    expect_error([&]{c.tick(5);},ConnectionErrorCode::terminal_state);
    expect_error([&]{c.receive(ack_request(client,4,7),5);},ConnectionErrorCode::terminal_state);
    CHECK(c.snapshot()==before);
}
TEST_CASE("Game connection rejection after peer Failure preserves the existing terminal snapshot") {
    const auto cfg=config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); hello(c,client);
    REQUIRE(c.receive(request(client,3,4,hex("0600000000"),false),3).closed);
    const auto before=c.snapshot();
    expect_error([&]{c.reject_login(4);},ConnectionErrorCode::terminal_state);
    CHECK(c.snapshot()==before);
}
TEST_CASE("Game connection rejection string quota closes without a partial Failure") {
    auto cfg=config(); cfg.login.limits.max_string_units=15; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian); const auto before=c.snapshot().login;
    expect_error([&]{c.reject_login(4);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed); CHECK(c.snapshot().login==before);
    expect_error([&]{c.reject_login(5);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Game connection rejection exact sixteen-unit string quota is sufficient") {
    auto cfg=config(); cfg.login.limits.max_string_units=16; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian);
    const auto step=c.reject_login(4); REQUIRE(step.closed);
    const auto out=inspect(client,step); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].payload==hex("06100000004c6f67696e2072656a65637465642e00"));
}
TEST_CASE("Game connection rejection pending-bunch quota is terminal without a partial flush") {
    // Challenge1b, Ready1d and Challenge3 occupy three unacknowledged entries.
    auto cfg=config(); cfg.transport.limits.max_outgoing_bunches=3; game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian); const auto before=c.snapshot().login;
    expect_error([&]{c.reject_login(4);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed); CHECK(c.snapshot().login==before);
    expect_error([&]{c.tick(5);},ConnectionErrorCode::terminal_state);
}

namespace {
using PublicationState=game::PublicationState;
game::ConnectionConfig publication_config() {
    auto cfg=config();
    // Public synthetic composition budget, not an observed native world/MTU.
    // The payload-bit SerializeInt bound is wire-significant: handshake
    // request/inspection helpers must use512 too, not their old64 default.
    cfg.transport.max_packet_bytes=512;
    cfg.limits.max_datagram_bytes=560;
    cfg.limits.max_output_bytes_per_step=8192;
    cfg.login.limits={512,256};
    return cfg;
}
game::WorldPublication publication() {
    return {{{"A",{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},{100,120}},
             {"B",{9,8,7,6},{77}}},
        {{post::StringForm::narrow,std::u16string{u'L',u'\0'}},
         {post::StringForm::narrow,std::u16string{u'G',u'\0'}}}};
}
Bytes publication_request(session::Context& client,std::uint16_t pid,std::uint16_t seq,const Bytes& raw) {
    return client.encode(wire::encode_packet(inner(pid,seq,raw,false),512));
}
Inspected publication_inspect(session::Context& client,const game::ConnectionStep& step) {
    Inspected out;
    for (const auto& datagram:step.datagrams) {
        out.plain.push_back(client.decode(datagram));
        out.packets.push_back(wire::decode_packet(out.plain.back(),512));
        for (const auto& record:out.packets.back().records)
            if (!record.is_ack) out.bunches.push_back(record.bunch);
    }
    return out;
}
Bytes first_package_literal(control::Endian endian) {
    return endian==control::Endian::little
        ? hex("07040302017856341200000080ffffffff0200000041000100000000"
              "000000000000000002000000010000000000")
        : hex("07010203041234567880000000ffffffff0000000241000000000100"
              "000000000000000000000002000000010000");
}
Bytes second_package_literal(control::Endian endian) {
    return endian==control::Endian::little
        ? hex("07090000000800000007000000060000000200000042000100000000"
              "000000000000000001000000010000000000")
        : hex("07000000090000000800000007000000060000000242000000000100"
              "000000000000000000000001000000010000");
}
Bytes package_ack_literal(unsigned which,control::Endian endian) {
    if (which==0) return endian==control::Endian::little
        ? hex("08040302017856341200000080ffffffff02000000")
        : hex("08010203041234567880000000ffffffff00000002");
    return endian==control::Endian::little
        ? hex("080900000008000000070000000600000001000000")
        : hex("080000000900000008000000070000000600000001");
}
void append(Bytes& to,const Bytes& from) { to.insert(to.end(),from.begin(),from.end()); }
void published(game::GameConnection& c,session::Context& client,control::Endian endian) {
    login_for_rejection(c,client,endian,512);
    const auto step=c.publish_world(publication(),4);
    REQUIRE_FALSE(step.closed); CHECK_FALSE(step.join_requested);
    const auto out=publication_inspect(client,step); REQUIRE(out.bunches.size()==3);
    REQUIRE(c.snapshot().publication==PublicationState::awaiting_package_acks);
    REQUIRE(c.snapshot().acknowledged_packages==0);
}
}

TEST_CASE("World publication emits ordered literal packages then Welcome without resetting session") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        login_for_rejection(c,client,endian,512);
        const auto step=c.publish_world(publication(),4);
        REQUIRE(step.datagrams.size()==3); CHECK_FALSE(step.closed);
        CHECK_FALSE(step.hello.has_value()); CHECK_FALSE(step.login_candidate.has_value());
        CHECK_FALSE(step.join_requested);
        const auto out=publication_inspect(client,step); REQUIRE(out.bunches.size()==3);
        CHECK(out.bunches[0].payload==first_package_literal(endian));
        CHECK(out.bunches[1].payload==second_package_literal(endian));
        CHECK(out.bunches[2].payload==(endian==control::Endian::little
            ? hex("01020000004c00020000004700") : hex("01000000024c00000000024700")));
        for (std::size_t i=0;i<3;++i) {
            CHECK(out.bunches[i].sequence==static_cast<std::uint16_t>(4+i));
            CHECK(out.packets[i].packet_id==static_cast<std::uint16_t>(7+i));
            CHECK(out.bunches[i].channel==0); CHECK(out.bunches[i].channel_type==1);
            CHECK(out.bunches[i].reliable); CHECK_FALSE(out.bunches[i].open);
            CHECK_FALSE(out.bunches[i].close);
            CHECK(rejection_counter(step.datagrams[i])==(session::direction_bit|static_cast<std::uint64_t>(8+i)));
        }
        CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks);
        CHECK(c.snapshot().acknowledged_packages==0);
        CHECK(c.snapshot().login.state==login::State::login_received);
    }
}
TEST_CASE("World publication accepts coalesced package ACKs and emits Join event only once") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        published(c,client,endian);
        auto raw=package_ack_literal(0,endian); append(raw,package_ack_literal(1,endian)); raw.push_back(9);
        const auto datagram=publication_request(client,4,5,raw);
        const auto step=c.receive(datagram,5);
        CHECK(step.join_requested); CHECK_FALSE(step.closed); CHECK_FALSE(step.login_candidate.has_value());
        CHECK(c.snapshot().publication==PublicationState::join_requested);
        CHECK(c.snapshot().acknowledged_packages==2);
        CHECK(publication_inspect(client,step).bunches.empty());
        const auto before=c.snapshot();
        expect_error([&]{c.receive(datagram,6);},ConnectionErrorCode::replay); CHECK(c.snapshot()==before);
        const auto duplicate=c.receive(publication_request(client,5,5,raw),6);
        CHECK_FALSE(duplicate.join_requested); CHECK_FALSE(duplicate.closed);
        CHECK(publication_inspect(client,duplicate).bunches.empty()); CHECK(c.snapshot()==before);
        const auto tick=c.tick(6); CHECK_FALSE(tick.join_requested); CHECK_FALSE(tick.closed);
        CHECK(publication_inspect(client,tick).bunches.empty()); CHECK(c.snapshot()==before);
    }
}
TEST_CASE("World publication exact duplicate package ACK is idempotent and all ACKs precede Join") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
    auto raw=package_ack_literal(1,cfg.endian); const auto duplicate=raw; append(raw,duplicate);
    const auto first=c.receive(publication_request(client,4,5,raw),5);
    CHECK_FALSE(first.join_requested); CHECK_FALSE(first.closed); publication_inspect(client,first);
    CHECK(c.snapshot().acknowledged_packages==1); CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks);
    const auto second=c.receive(publication_request(client,5,6,package_ack_literal(0,cfg.endian)),6);
    CHECK_FALSE(second.join_requested); publication_inspect(client,second);
    CHECK(c.snapshot().acknowledged_packages==2); CHECK(c.snapshot().publication==PublicationState::awaiting_join);
    const auto join=c.receive(publication_request(client,6,7,Bytes{9}),7);
    CHECK(join.join_requested); CHECK_FALSE(join.closed); publication_inspect(client,join);
    CHECK(c.snapshot().publication==PublicationState::join_requested);
}
TEST_CASE("World publication Join before every package ACK is terminal") {
    for (const bool one_ack:{false,true}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
        auto raw=one_ack?package_ack_literal(0,cfg.endian):Bytes{}; raw.push_back(9);
        expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot().closed); CHECK(c.snapshot().publication!=PublicationState::join_requested);
        expect_error([&]{c.tick(6);},ConnectionErrorCode::terminal_state);
    }
}
TEST_CASE("World publication unknown or mismatched generation ACK cannot unlock Join") {
    for (const bool unknown:{false,true}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
        auto raw=package_ack_literal(0,cfg.endian);
        raw[unknown?1U:17U]^=1; // Independently corrupt identity or generation2->3.
        expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot().closed); CHECK(c.snapshot().acknowledged_packages==0);
        CHECK(c.snapshot().publication!=PublicationState::join_requested);
    }
}
TEST_CASE("World publication malformed suffix prevents a partial Join event") {
    for (const auto tail:std::array<Bytes,3>{Bytes{0xff},Bytes{9},Bytes{8,1}}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
        auto raw=package_ack_literal(0,cfg.endian); append(raw,package_ack_literal(1,cfg.endian)); raw.push_back(9); append(raw,tail);
        const auto expected=tail==Bytes{9}?ConnectionErrorCode::unexpected_message:ConnectionErrorCode::invalid_archive;
        expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},expected);
        CHECK(c.snapshot().closed);
        // State may retain a historical transition, but no returned event or
        // partial output is exposed by a throwing authenticated operation.
        expect_error([&]{c.tick(6);},ConnectionErrorCode::terminal_state);
    }
}
TEST_CASE("World publication second newly delivered Join is terminal without another event") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
    auto raw=package_ack_literal(0,cfg.endian); append(raw,package_ack_literal(1,cfg.endian)); raw.push_back(9);
    REQUIRE(c.receive(publication_request(client,4,5,raw),5).join_requested);
    expect_error([&]{c.receive(publication_request(client,5,6,Bytes{9}),6);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot().closed);
}
TEST_CASE("World publication peer Failure closes without echoing text or a Join event") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
    const auto step=c.receive(publication_request(client,4,5,hex("0604000000500049ff")),5);
    CHECK(step.closed); CHECK_FALSE(step.join_requested); CHECK_FALSE(step.login_candidate.has_value());
    CHECK(publication_inspect(client,step).bunches.empty()); CHECK(c.snapshot().closed);
}
TEST_CASE("World publication unexpected postpublication logical messages remain terminal") {
    for (const auto& raw:std::array<Bytes,2>{hex("0408070000"),hex("050000000000000000040302017856341200000080ffffffff")}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
        expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot().closed);
    }
}
TEST_CASE("World publication invalid roots duplicates and Welcome preserve state and send counters") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); login_for_rejection(c,client,cfg.endian,512);
    const auto before=c.snapshot();
    for (unsigned variant=0;variant<7;++variant) {
        auto bad=publication();
        switch (variant) {
        case 0: bad.packages.clear(); break;
        case 1: bad.packages[1].name="a"; break;
        case 2: bad.packages[1].identity=bad.packages[0].identity; break;
        case 3: bad.packages[0].identity={}; break;
        case 4: bad.packages[0].generation_net_counts.clear(); break;
        case 5: bad.packages[0].name="../private"; break;
        default: bad.welcome.level={post::StringForm::zero,std::u16string{u'x'}}; break;
        }
        expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::invalid_archive);
        CHECK(c.snapshot()==before);
    }
    const auto step=c.publish_world(publication(),4); REQUIRE(step.datagrams.size()==3);
    CHECK(rejection_counter(step.datagrams.front())==(session::direction_bit|8));
    const auto out=publication_inspect(client,step); REQUIRE(out.bunches.size()==3);
    CHECK(out.bunches[0].sequence==4); CHECK(out.packets[0].packet_id==7);
}
TEST_CASE("World publication validates clocks before sending and preserves rejected clock") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); login_for_rejection(c,client,cfg.endian,512);
    const auto before=c.snapshot();
    for (const double now:std::array<double,4>{2.99,-1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        expect_error([&]{c.publish_world(publication(),now);},ConnectionErrorCode::invalid_time); CHECK(c.snapshot()==before);
    }
    const auto step=c.publish_world(publication(),3); REQUIRE_FALSE(step.closed);
    CHECK(rejection_counter(step.datagrams.front())==(session::direction_bit|8));
    publication_inspect(client,step);
}
TEST_CASE("World publication requires received Login and one pending publication") {
    for (unsigned stage=0;stage<4;++stage) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        if (stage==1) initial(c,client,true,512);
        if (stage==2) ready(c,client,512);
        if (stage==3) hello(c,client,512);
        const auto before=c.snapshot();
        expect_error([&]{c.publish_world(publication(),3);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot()==before);
    }
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
    const auto before=c.snapshot();
    expect_error([&]{c.publish_world(publication(),99);},ConnectionErrorCode::unexpected_message); CHECK(c.snapshot()==before);
    expect_error([&]{c.reject_login(99);},ConnectionErrorCode::unexpected_message); CHECK(c.snapshot()==before);
    CHECK_FALSE(c.tick(4).closed); // Invalid operations did not commit time99.
}
TEST_CASE("World publication list cardinality and complete encoding preflight retain pending state") {
    auto cfg=publication_config(); cfg.limits.max_logical_messages_per_step=3;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian,512); const auto before=c.snapshot();
    auto bad=publication(); bad.packages.push_back({"C",{10,11,12,13},{1}});
    expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot()==before);
    bad=publication(); bad.welcome.game={post::StringForm::narrow,std::u16string(257,u'x')};
    expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot()==before);
    bad=publication(); bad.packages[1].name=std::string(256,'A');
    expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot()==before);
    const auto step=c.publish_world(publication(),4); REQUIRE(step.datagrams.size()==3);
    CHECK(rejection_counter(step.datagrams.front())==(session::direction_bit|8)); publication_inspect(client,step);
}
TEST_CASE("World publication started send quota failure is terminal without a partial flush") {
    // Three existing unacknowledged handshake replies leave room for one
    // Package7 but not the second; preflight itself must not pretend delivery.
    auto cfg=publication_config(); cfg.transport.limits.max_outgoing_bunches=4;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian,512);
    expect_error([&]{c.publish_world(publication(),4);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
    expect_error([&]{c.publish_world(publication(),5);},ConnectionErrorCode::terminal_state);
    expect_error([&]{c.tick(5);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("World publication ACK and tick retain the publication phase without replaying packages") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key); published(c,client,cfg.endian);
    wire::Packet packet; packet.packet_id=4;
    for (const auto id:{std::uint16_t{1},std::uint16_t{3},std::uint16_t{5},std::uint16_t{7},std::uint16_t{8},std::uint16_t{9}}) {
        wire::PacketRecord record; record.is_ack=true; record.ack_packet_id=id; packet.records.push_back(record);
    }
    const auto step=c.receive(client.encode(wire::encode_packet(packet,512)),5);
    CHECK_FALSE(step.join_requested); CHECK(publication_inspect(client,step).bunches.empty());
    CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks); CHECK(c.snapshot().acknowledged_packages==0);
    const auto tick=c.tick(6); CHECK_FALSE(tick.join_requested); CHECK(publication_inspect(client,tick).bunches.empty());
    CHECK_FALSE(tick.closed); CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks);
}
TEST_CASE("World publication coalesced ACK quota prevents a partial Join event") {
    auto cfg=publication_config(); cfg.limits.max_logical_messages_per_step=3;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    published(c,client,cfg.endian);
    auto raw=package_ack_literal(0,cfg.endian); append(raw,package_ack_literal(1,cfg.endian));
    append(raw,package_ack_literal(1,cfg.endian)); raw.push_back(9); // Four messages, own cap3.
    expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
    expect_error([&]{c.tick(6);},ConnectionErrorCode::terminal_state);
}

TEST_CASE("World publication coalesced Join then valid Failure closes without delivering Join") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        published(c,client,endian);
        auto raw=package_ack_literal(0,endian); append(raw,package_ack_literal(1,endian));
        raw.push_back(9); append(raw,hex("0600000000")); // Valid Failure6 with zero FString.
        const auto step=c.receive(publication_request(client,4,5,raw),5);
        CHECK(step.closed); CHECK_FALSE(step.join_requested);
        CHECK_FALSE(step.hello.has_value()); CHECK_FALSE(step.login_candidate.has_value());
        CHECK(publication_inspect(client,step).bunches.empty());
        CHECK(c.snapshot().closed); CHECK(c.snapshot().acknowledged_packages==2);
        // Publication snapshot may retain Join history; closed wins for delivery.
        expect_error([&]{c.tick(6);},ConnectionErrorCode::terminal_state);
    }
}


namespace {
game::WorldPublication contained_publication() {
    auto world=publication();
    // The containing root identity is SYNTHETIC composition metadata. Only
    // HGC export180 GUID and generations{533} below reuse original static data;
    // this is an explicit local trial descriptor, not a live linker observation.
    world.packages[1].name="HawkenDM_SF";
    world.contained_packages.push_back({{"HawkenGameContent",
        {1557215687U,1252474388U,3172763027U,4167063261U},{533}},"HawkenDM_SF"});
    return world;
}
Bytes container_root_literal(control::Endian endian) {
    return endian==control::Endian::little
        ? hex("07090000000800000007000000060000000c0000004861776b656e444d5f534600"
              "0100000000000000000000000001000000010000000000")
        : hex("07000000090000000800000007000000060000000c4861776b656e444d5f534600"
              "0000000100000000000000000000000001000000010000");
}
Bytes contained_package_literal(control::Endian endian) {
    // Independent archive oracle: GUID words; name0 HGC+NUL (18 units);
    // name1 ONE NUL; text2 zero; value0=0; value1 generation COUNT1, not533;
    // name3 HawkenDM_SF+NUL (12 units); flags0. Zero fields are own trial policy.
    return endian==control::Endian::little
        ? hex("07c739d15c143ea74a93851cbddd5660f8120000004861776b656e47616d65436f6e74656e7400"
              "01000000000000000000000000010000000c0000004861776b656e444d5f53460000")
        : hex("075cd139c74aa73e14bd1c8593f86056dd000000124861776b656e47616d65436f6e74656e7400"
              "00000001000000000000000000000000010000000c4861776b656e444d5f53460000");
}
Bytes contained_ack_literal(control::Endian endian) {
    return endian==control::Endian::little
        ? hex("08c739d15c143ea74a93851cbddd5660f801000000")
        : hex("085cd139c74aa73e14bd1c8593f86056dd00000001");
}
void contained_published(game::GameConnection& c,session::Context& client,control::Endian endian) {
    login_for_rejection(c,client,endian,512);
    const auto step=c.publish_world(contained_publication(),4);
    REQUIRE_FALSE(step.closed);
    const auto out=publication_inspect(client,step);
    REQUIRE(out.bunches.size()==4);
    REQUIRE(c.snapshot().publication==PublicationState::awaiting_package_acks);
}
}

TEST_CASE("Contained publication sends roots then contained Package7 then Welcome on same Context") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        login_for_rejection(c,client,endian,512);
        const auto step=c.publish_world(contained_publication(),4);
        REQUIRE(step.datagrams.size()==4); CHECK_FALSE(step.closed); CHECK_FALSE(step.join_requested);
        const auto out=publication_inspect(client,step); REQUIRE(out.bunches.size()==4);
        CHECK(out.bunches[0].payload==first_package_literal(endian));
        CHECK(out.bunches[1].payload==container_root_literal(endian));
        CHECK(out.bunches[2].payload==contained_package_literal(endian));
        CHECK(out.bunches[3].payload==(endian==control::Endian::little
            ? hex("01020000004c00020000004700") : hex("01000000024c00000000024700")));
        const auto decoded=post::decode_exact(out.bunches[2].payload,endian,cfg.login.limits);
        const auto& descriptor=std::get<post::Package>(decoded);
        CHECK(descriptor.name0==(post::RawString{post::StringForm::narrow,std::u16string(u"HawkenGameContent",18)}));
        CHECK(descriptor.name3==(post::RawString{post::StringForm::narrow,std::u16string(u"HawkenDM_SF",12)}));
        CHECK(descriptor.value1==1); CHECK(descriptor.value0==0); CHECK(descriptor.flags==0);
        for (std::size_t i=0;i<4;++i) {
            CHECK(out.bunches[i].sequence==static_cast<std::uint16_t>(4+i));
            CHECK(out.packets[i].packet_id==static_cast<std::uint16_t>(7+i));
            CHECK(rejection_counter(step.datagrams[i])==(session::direction_bit|static_cast<std::uint64_t>(8+i)));
            CHECK(out.bunches[i].reliable); CHECK(out.bunches[i].channel==0);
        }
        CHECK(c.snapshot().acknowledged_packages==0);
        CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks);
    }
}
TEST_CASE("Contained publication requires HGC COUNT1 ACK after roots and retains duplicate suppression") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        contained_published(c,client,endian);
        auto roots=package_ack_literal(0,endian); append(roots,package_ack_literal(1,endian));
        const auto first=c.receive(publication_request(client,4,5,roots),5);
        CHECK_FALSE(first.join_requested); CHECK_FALSE(first.closed); publication_inspect(client,first);
        CHECK(c.snapshot().acknowledged_packages==2);
        CHECK(c.snapshot().publication==PublicationState::awaiting_package_acks);
        auto raw=contained_ack_literal(endian); append(raw,contained_ack_literal(endian)); raw.push_back(9);
        const auto datagram=publication_request(client,5,6,raw);
        const auto joined=c.receive(datagram,6);
        CHECK(joined.join_requested); CHECK_FALSE(joined.closed); publication_inspect(client,joined);
        CHECK(c.snapshot().acknowledged_packages==3);
        CHECK(c.snapshot().publication==PublicationState::join_requested);
        const auto before=c.snapshot();
        expect_error([&]{c.receive(datagram,7);},ConnectionErrorCode::replay); CHECK(c.snapshot()==before);
        const auto duplicate=c.receive(publication_request(client,6,6,raw),7);
        CHECK_FALSE(duplicate.join_requested); CHECK_FALSE(duplicate.closed);
        CHECK(publication_inspect(client,duplicate).bunches.empty()); CHECK(c.snapshot()==before);
    }
}
TEST_CASE("Contained publication rejects Join when only the root packages have been acknowledged") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    contained_published(c,client,cfg.endian);
    auto raw=package_ack_literal(0,cfg.endian); append(raw,package_ack_literal(1,cfg.endian)); raw.push_back(9);
    expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot().closed); CHECK(c.snapshot().acknowledged_packages==2);
    CHECK(c.snapshot().publication!=PublicationState::join_requested);
}
TEST_CASE("Contained publication does not accept HGC object count533 as ACK generation") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        contained_published(c,client,endian);
        const auto raw=endian==control::Endian::little
            ? hex("08c739d15c143ea74a93851cbddd5660f815020000")
            : hex("085cd139c74aa73e14bd1c8593f86056dd00000215");
        expect_error([&]{c.receive(publication_request(client,4,5,raw),5);},ConnectionErrorCode::unexpected_message);
        CHECK(c.snapshot().closed); CHECK(c.snapshot().acknowledged_packages==0);
    }
}
TEST_CASE("Contained publication detects names and GUIDs duplicated across the combined list before sending") {
    for (unsigned variant=0;variant<4;++variant) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        login_for_rejection(c,client,cfg.endian,512); const auto before=c.snapshot();
        auto bad=contained_publication();
        if (variant==0) bad.contained_packages[0].package.name="a";
        if (variant==1) bad.contained_packages[0].package.identity=bad.packages[0].identity;
        if (variant>=2) {
            auto extra=bad.contained_packages[0];
            extra.package.name="OtherContained"; extra.package.identity={42,43,44,45};
            if (variant==2) extra.package.name="hawkengamecontent";
            else extra.package.identity=bad.contained_packages[0].package.identity;
            bad.contained_packages.push_back(std::move(extra));
        }
        expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::invalid_archive);
        REQUIRE(c.snapshot()==before);
        // Retry time4 demonstrates failed preflight did not commit99; exact
        // first counter/packet/sequence demonstrate no publication send spent.
        const auto good=c.publish_world(contained_publication(),4);
        REQUIRE(good.datagrams.size()==4);
        CHECK(rejection_counter(good.datagrams.front())==(session::direction_bit|8));
        const auto out=publication_inspect(client,good); REQUIRE(out.bunches.size()==4);
        CHECK(out.packets.front().packet_id==7); CHECK(out.bunches.front().sequence==4);
        CHECK(out.bunches[2].payload==contained_package_literal(cfg.endian));
    }
}
TEST_CASE("Contained publication total logical quota includes roots contained descriptors and Welcome") {
    for (const std::size_t limit:{std::size_t{3},std::size_t{4}}) {
        auto cfg=publication_config(); cfg.limits.max_logical_messages_per_step=limit;
        game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        login_for_rejection(c,client,cfg.endian,512); const auto before=c.snapshot();
        auto bad=contained_publication();
        if (limit==4) bad.contained_packages.push_back({{"OtherContained",{42,43,44,45},{1}},"HawkenDM_SF"});
        expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::resource_limit);
        REQUIRE(c.snapshot()==before);
        // Boundary fits: two roots+Welcome at3, or two roots+HGC+Welcome at4.
        auto good=limit==3?publication():contained_publication();
        const auto step=c.publish_world(good,4); REQUIRE(step.datagrams.size()==limit);
        CHECK(rejection_counter(step.datagrams.front())==(session::direction_bit|8));
        const auto out=publication_inspect(client,step); REQUIRE(out.bunches.size()==limit);
        CHECK(out.packets.front().packet_id==7); CHECK(out.bunches.front().sequence==4);
        CHECK_FALSE(step.closed); CHECK_FALSE(step.join_requested);
    }
}

namespace {
void reference_join(game::GameConnection& connection,session::Context& client,
                    control::Endian endian,bool contained) {
    auto raw=package_ack_literal(0,endian);
    append(raw,package_ack_literal(1,endian));
    if (contained) append(raw,contained_ack_literal(endian));
    raw.push_back(9);
    const auto step=connection.receive(publication_request(client,4,5,raw),5);
    REQUIRE(step.join_requested); REQUIRE_FALSE(step.closed);
    CHECK(publication_inspect(client,step).bunches.empty());
    REQUIRE(connection.snapshot().publication==PublicationState::join_requested);
}
}

TEST_CASE("Package reference query requires open connection after all ACKs and Join") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    const auto identity=publication().packages[0].identity;
    auto before=c.snapshot();
    expect_error([&]{(void)c.resolve_package_reference(identity,0);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot()==before);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    published(c,client,cfg.endian); before=c.snapshot();
    expect_error([&]{(void)c.resolve_package_reference(identity,0);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot()==before);
    auto raw=package_ack_literal(0,cfg.endian); append(raw,package_ack_literal(1,cfg.endian));
    const auto ack=c.receive(publication_request(client,4,5,raw),5);
    publication_inspect(client,ack); REQUIRE(c.snapshot().publication==PublicationState::awaiting_join);
    before=c.snapshot();
    expect_error([&]{(void)c.resolve_package_reference(identity,0);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot()==before);
    const auto join=c.receive(publication_request(client,5,6,Bytes{9}),6);
    REQUIRE(join.join_requested); publication_inspect(client,join);
    CHECK(c.resolve_package_reference(identity,0).index==0);
}
TEST_CASE("Package reference coordinates use roots then contained negotiated network counts") {
    for (const auto endian:{control::Endian::little,control::Endian::big}) {
        auto cfg=publication_config(); cfg.endian=endian; game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        contained_published(c,client,endian); reference_join(c,client,endian,true);
        const auto plan=contained_publication(); const auto before=c.snapshot();
        // Latest network counts120,77,533; generation COUNTs2,1,1.
        // Independent arithmetic: bases0,120,197; total730.
        CHECK(c.resolve_package_reference(plan.packages[0].identity,0).index==0);
        CHECK(c.resolve_package_reference(plan.packages[0].identity,119).index==119);
        CHECK(c.resolve_package_reference(plan.packages[1].identity,0).index==120);
        CHECK(c.resolve_package_reference(plan.packages[1].identity,76).index==196);
        CHECK(c.resolve_package_reference(plan.contained_packages[0].package.identity,0).index==197);
        CHECK(c.resolve_package_reference(plan.contained_packages[0].package.identity,532).index==729);
        CHECK(c.snapshot()==before);
    }
}
TEST_CASE("Package references retain their own metadata despite caller mutation and destruction") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto original=contained_publication();
    login_for_rejection(c,client,cfg.endian,512);
    {
        auto plan=original;
        const auto step=c.publish_world(plan,4); publication_inspect(client,step);
        plan.packages[0].generation_net_counts={1};
        plan.packages[1].identity={10,11,12,13};
        plan.contained_packages[0].package.generation_net_counts={0};
        plan.contained_packages.clear(); plan.packages.clear();
    }
    reference_join(c,client,cfg.endian,true);
    CHECK(c.resolve_package_reference(original.packages[1].identity,76).index==196);
    CHECK(c.resolve_package_reference(original.contained_packages[0].package.identity,532).index==729);
}
TEST_CASE("Package reference unknown GUID and invalid local index preserve joined state") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    published(c,client,cfg.endian); reference_join(c,client,cfg.endian,false);
    const auto identity=publication().packages[0].identity; const auto before=c.snapshot();
    expect_error([&]{(void)c.resolve_package_reference({1,2,3,4},0);},ConnectionErrorCode::invalid_archive);
    for (const std::int32_t local:{-1,-2,120,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()})
        expect_error([&]{(void)c.resolve_package_reference(identity,local);},ConnectionErrorCode::invalid_archive);
    CHECK(c.snapshot()==before); CHECK_FALSE(c.snapshot().closed);
    CHECK(c.resolve_package_reference(identity,119).index==119);
}
TEST_CASE("Closed connection cannot resolve even if snapshot retains historical Join") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    published(c,client,cfg.endian); reference_join(c,client,cfg.endian,false);
    const auto failure=c.receive(publication_request(client,5,6,hex("0600000000")),6);
    REQUIRE(failure.closed); REQUIRE(c.snapshot().publication==PublicationState::join_requested);
    const auto before=c.snapshot();
    expect_error([&]{(void)c.resolve_package_reference(publication().packages[0].identity,0);},ConnectionErrorCode::terminal_state);
    CHECK(c.snapshot()==before);
}
TEST_CASE("Zero network count package has no reference and does not shift following base") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian,512);
    auto plan=publication(); plan.packages[0].generation_net_counts={100,0};
    const auto step=c.publish_world(plan,4); publication_inspect(client,step);
    reference_join(c,client,cfg.endian,false);
    expect_error([&]{(void)c.resolve_package_reference(plan.packages[0].identity,0);},ConnectionErrorCode::invalid_archive);
    CHECK(c.resolve_package_reference(plan.packages[1].identity,0).index==0);
    CHECK(c.resolve_package_reference(plan.packages[1].identity,76).index==76);
}
TEST_CASE("Signed-invalid network counts fail publication preflight without spending time or nonce") {
    for (const auto invalid:{0x80000000U,0xffffffffU}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        login_for_rejection(c,client,cfg.endian,512); const auto before=c.snapshot();
        auto bad=publication(); bad.packages[0].generation_net_counts[0]=invalid;
        // Validate even the unselected older count, not merely the latest.
        expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::invalid_archive);
        REQUIRE(c.snapshot()==before);
        const auto good=c.publish_world(publication(),4); REQUIRE(good.datagrams.size()==3);
        CHECK(rejection_counter(good.datagrams.front())==(session::direction_bit|8));
        const auto out=publication_inspect(client,good); REQUIRE(out.packets.size()==3);
        CHECK(out.packets.front().packet_id==7); CHECK(out.bunches.front().sequence==4);
    }
}
TEST_CASE("Cumulative coordinate overflow in roots and contained is rejected before publication send") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    login_for_rejection(c,client,cfg.endian,512); const auto before=c.snapshot();
    auto bad=contained_publication();
    bad.packages[0].generation_net_counts={0x7fffffffU,0x7fffffffU};
    bad.packages[1].generation_net_counts={1};
    bad.contained_packages[0].package.generation_net_counts={1};
    expect_error([&]{c.publish_world(bad,99);},ConnectionErrorCode::invalid_archive);
    REQUIRE(c.snapshot()==before);
    const auto good=c.publish_world(contained_publication(),4); REQUIRE(good.datagrams.size()==4);
    CHECK(rejection_counter(good.datagrams.front())==(session::direction_bit|8));
    const auto out=publication_inspect(client,good); REQUIRE(out.packets.size()==4);
    CHECK(out.packets.front().packet_id==7); CHECK(out.bunches.front().sequence==4);
}


namespace {
wire::reliable::ActorChannelConfig actor_config() { return {7,41,0,false}; }
wire::Bunch actor_payload(std::uint16_t sequence=0,bool open=true,bool close=false) {
    wire::Bunch bunch;
    bunch.control=open||close; bunch.open=open; bunch.close=close;
    bunch.reliable=true; bunch.channel=7; bunch.sequence=sequence; bunch.channel_type=2;
    // Independent initial-controller fixture: index0, position0, no rotation,
    // raw netbyte0. 51 logical bits, not a byte-aligned control archive.
    bunch.payload=hex("00000000400500"); bunch.payload_bits=51;
    return bunch;
}
Bytes actor_request(session::Context& client,std::uint16_t pid,
                    const std::vector<wire::Bunch>& bunches) {
    wire::Packet packet; packet.packet_id=pid;
    for (const auto& bunch:bunches) {
        wire::PacketRecord record; record.bunch=bunch; packet.records.push_back(record);
    }
    return client.encode(wire::encode_packet(packet,512));
}
struct ActorContinuation { std::uint16_t packet_id; std::uint64_t nonce; };
ActorContinuation joined_actor(game::GameConnection& connection,session::Context& client,
                               control::Endian endian) {
    published(connection,client,endian);
    auto raw=package_ack_literal(0,endian); append(raw,package_ack_literal(1,endian)); raw.push_back(9);
    const auto step=connection.receive(publication_request(client,4,5,raw),5);
    REQUIRE(step.join_requested); REQUIRE_FALSE(step.closed);
    REQUIRE_FALSE(step.datagrams.empty());
    const auto out=publication_inspect(client,step); REQUIRE_FALSE(out.packets.empty());
    REQUIRE(out.bunches.empty());
    return {static_cast<std::uint16_t>((out.packets.back().packet_id+1U)&0x3fffU),
        rejection_counter(step.datagrams.back())+1U};
}
void ack_all_control(game::GameConnection& connection,session::Context& client,double now=6) {
    wire::Packet packet; packet.packet_id=5;
    // Public fixture outbound reliable packets: challenges1/3/5, packages7/8,
    // Welcome9. Ascending exact ACKs avoid intentionally requesting gap retries.
    for (const auto id:{1,3,5,7,8,9}) {
        wire::PacketRecord record; record.is_ack=true;
        record.ack_packet_id=static_cast<std::uint16_t>(id); packet.records.push_back(record);
    }
    const auto step=connection.receive(client.encode(wire::encode_packet(packet,512)),now);
    REQUIRE_FALSE(step.closed); CHECK(publication_inspect(client,step).bunches.empty());
}
void actor_fields(const wire::Bunch& actual,const wire::Bunch& expected) {
    CHECK(actual.payload==expected.payload); CHECK(actual.payload_bits==expected.payload_bits);
    CHECK(actual.channel==expected.channel); CHECK(actual.channel_type==2);
    CHECK(actual.reliable); CHECK(actual.open==expected.open); CHECK(actual.close==expected.close);
    CHECK(actual.control==expected.control); CHECK(actual.sequence==expected.sequence);
}
}

TEST_CASE("Actor registration and send require open post Join state without clock changes") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    const auto fresh=c.snapshot();
    expect_error([&]{c.register_actor_channel(actor_config());},ConnectionErrorCode::unexpected_message);
    expect_error([&]{c.send_actor(actor_payload(),99);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot()==fresh);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    published(c,client,cfg.endian); const auto before=c.snapshot();
    expect_error([&]{c.register_actor_channel(actor_config());},ConnectionErrorCode::unexpected_message);
    expect_error([&]{c.send_actor(actor_payload(),99);},ConnectionErrorCode::unexpected_message);
    CHECK(c.snapshot()==before);
    reference_join(c,client,cfg.endian,false);
    c.register_actor_channel(actor_config());
    CHECK(c.snapshot().publication==PublicationState::join_requested);
    const auto failure=c.receive(publication_request(client,5,6,hex("0600000000")),6);
    REQUIRE(failure.closed);
    expect_error([&]{c.register_actor_channel({8,0,0,false});},ConnectionErrorCode::terminal_state);
    expect_error([&]{c.send_actor(actor_payload(),7);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Actor send continues packet nonce and explicit actor sequence without resetting control") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto next=joined_actor(c,client,cfg.endian); const auto before=c.snapshot();
    c.register_actor_channel(actor_config()); CHECK(c.snapshot()==before);
    const auto step=c.send_actor(actor_payload(),6);
    REQUIRE(step.datagrams.size()==1); CHECK(rejection_counter(step.datagrams.front())==next.nonce);
    const auto out=publication_inspect(client,step); REQUIRE(out.packets.size()==1); REQUIRE(out.bunches.size()==1);
    CHECK(out.packets.front().packet_id==next.packet_id);
    auto expected=actor_payload(42); actor_fields(out.bunches.front(),expected);
    CHECK(step.actor_bunches.empty()); CHECK(step.closed_actor_channels.empty()); CHECK_FALSE(step.closed);
    CHECK_FALSE(step.join_requested); CHECK_FALSE(step.login_candidate); CHECK(c.snapshot()==before);
}
TEST_CASE("Incoming nonbyte actor dispatch precedes opcode parsing and mixes with valid control") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    auto control_bunch=inner(5,6,package_ack_literal(0,cfg.endian),false).records.front().bunch;
    const auto expected=actor_payload(1);
    const auto step=c.receive(actor_request(client,5,{expected,control_bunch}),6);
    REQUIRE(step.actor_bunches.size()==1); actor_fields(step.actor_bunches.front(),expected);
    CHECK(step.closed_actor_channels.empty()); CHECK_FALSE(step.closed); CHECK_FALSE(step.join_requested);
    CHECK(publication_inspect(client,step).bunches.empty());
    CHECK(c.snapshot().publication==PublicationState::join_requested);
}
TEST_CASE("Incoming actor reorder duplicate and authenticated replay never repeat delivery") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    const auto future=c.receive(actor_request(client,5,{actor_payload(2,false)}),6);
    CHECK(future.actor_bunches.empty()); publication_inspect(client,future);
    const auto first_packet=actor_request(client,6,{actor_payload(1)});
    const auto drained=c.receive(first_packet,7); REQUIRE(drained.actor_bunches.size()==2);
    CHECK(drained.actor_bunches[0].sequence==1); CHECK(drained.actor_bunches[1].sequence==2);
    publication_inspect(client,drained); const auto before=c.snapshot();
    expect_error([&]{c.receive(first_packet,8);},ConnectionErrorCode::replay); CHECK(c.snapshot()==before);
    const auto duplicate=c.receive(actor_request(client,7,{actor_payload(2,false)}),8);
    CHECK(duplicate.actor_bunches.empty()); CHECK_FALSE(duplicate.closed); publication_inspect(client,duplicate);
}
TEST_CASE("Actor gap retry retains payload and reliable sequence on same Context") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); ack_all_control(c,client); c.register_actor_channel(actor_config());
    const auto first=c.send_actor(actor_payload(),7); const auto a=publication_inspect(client,first);
    const auto second=c.send_actor(actor_payload(0,false),8); const auto b=publication_inspect(client,second);
    REQUIRE(a.bunches.size()==1); REQUIRE(b.bunches.size()==1);
    wire::Packet packet; packet.packet_id=6; wire::PacketRecord ack; ack.is_ack=true;
    ack.ack_packet_id=b.packets.front().packet_id; packet.records.push_back(ack);
    const auto step=c.receive(client.encode(wire::encode_packet(packet,512)),9);
    const auto retry=publication_inspect(client,step); REQUIRE(retry.bunches.size()==1);
    actor_fields(retry.bunches.front(),a.bunches.front());
    CHECK(retry.packets.front().packet_id>b.packets.front().packet_id);
    CHECK(rejection_counter(step.datagrams.front())==rejection_counter(second.datagrams.back())+1);
    CHECK(step.actor_bunches.empty()); CHECK_FALSE(step.closed);
}
TEST_CASE("Incoming actor close emits one channel event and preserves control connection") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    const auto closing=actor_payload(1,true,true);
    auto control_bunch=inner(5,6,package_ack_literal(0,cfg.endian),false).records.front().bunch;
    const auto step=c.receive(actor_request(client,5,{closing,control_bunch}),6);
    REQUIRE(step.actor_bunches.size()==1); actor_fields(step.actor_bunches.front(),closing);
    CHECK(step.closed_actor_channels==std::vector<std::uint16_t>{7}); CHECK_FALSE(step.closed);
    publication_inspect(client,step); const auto before=c.snapshot();
    expect_error([&]{c.send_actor(actor_payload(),99);},ConnectionErrorCode::terminal_state);
    CHECK(c.snapshot()==before);
    const auto next=c.receive(publication_request(client,6,7,package_ack_literal(1,cfg.endian)),7);
    CHECK_FALSE(next.closed); CHECK(next.actor_bunches.empty()); CHECK(next.closed_actor_channels.empty());
    publication_inspect(client,next);
}
TEST_CASE("Outgoing actor close is pending until exact ACK and never closes control") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); ack_all_control(c,client); c.register_actor_channel(actor_config());
    const auto sent=c.send_actor(actor_payload(0,true,true),7);
    CHECK(sent.closed_actor_channels.empty()); CHECK_FALSE(sent.closed);
    const auto out=publication_inspect(client,sent); REQUIRE(out.packets.size()==1);
    const auto before=c.snapshot();
    expect_error([&]{c.send_actor(actor_payload(),99);},ConnectionErrorCode::terminal_state);
    CHECK(c.snapshot()==before);
    wire::Packet packet; packet.packet_id=6; wire::PacketRecord ack; ack.is_ack=true;
    ack.ack_packet_id=out.packets.front().packet_id; packet.records.push_back(ack);
    const auto closed=c.receive(client.encode(wire::encode_packet(packet,512)),8);
    CHECK(closed.closed_actor_channels==std::vector<std::uint16_t>{7}); CHECK_FALSE(closed.closed);
    publication_inspect(client,closed);
    const auto tick=c.tick(9); CHECK_FALSE(tick.closed); CHECK(tick.closed_actor_channels.empty());
    publication_inspect(client,tick);
}
TEST_CASE("Later control Failure or global close clears all transient actor events") {
    for (const bool transport_close:{false,true}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
        auto closing=actor_payload(1,true,true);
        auto control_bunch=inner(5,6,hex("0600000000"),false,transport_close).records.front().bunch;
        const auto step=c.receive(actor_request(client,5,{closing,control_bunch}),6);
        CHECK(step.closed); CHECK(c.snapshot().closed); CHECK(step.actor_bunches.empty());
        CHECK(step.closed_actor_channels.empty()); CHECK_FALSE(step.join_requested);
        publication_inspect(client,step);
    }
}
TEST_CASE("Actor preflight invalid time registration shape and unknown channel preserve state and nonce") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto next=joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    const auto before=c.snapshot();
    for (const std::uint16_t channel:{std::uint16_t{0},std::uint16_t{2048},std::uint16_t{7}})
        expect_error([&]{c.register_actor_channel({channel,0,0,false});},ConnectionErrorCode::invalid_configuration);
    for (const auto now:{-1.0,4.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        expect_error([&]{c.send_actor(actor_payload(),now);},ConnectionErrorCode::invalid_time);
    for (unsigned invalid=0;invalid<8;++invalid) {
        auto bunch=actor_payload();
        switch (invalid) {
        case 0: bunch.channel=0; bunch.channel_type=1; break;
        case 1: bunch.channel=8; break;
        case 2: bunch.channel_type=1; break;
        case 3: bunch.sequence=1; break;
        case 4: bunch.reliable=false; break;
        case 5: bunch.control=false; break;
        case 6: bunch.payload_bits=52; bunch.payload.back()=0x80; break;
        case 7: bunch.payload.pop_back(); break;
        }
        expect_error([&]{c.send_actor(bunch,99);},ConnectionErrorCode::invalid_packet);
        CHECK(c.snapshot()==before);
    }
    const auto good=c.send_actor(actor_payload(),6); REQUIRE(good.datagrams.size()==1);
    CHECK(rejection_counter(good.datagrams.front())==next.nonce);
    const auto out=publication_inspect(client,good); CHECK(out.packets.front().packet_id==next.packet_id);
}
TEST_CASE("Actor transport quota failure preserves connection and permits later exact ACK recovery") {
    auto cfg=publication_config(); cfg.transport.limits.max_outgoing_bunches=6;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config()); const auto before=c.snapshot();
    expect_error([&]{c.send_actor(actor_payload(),99);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot()==before); CHECK_FALSE(c.snapshot().closed);
    ack_all_control(c,client,6);
    const auto good=c.send_actor(actor_payload(),7); CHECK_FALSE(good.closed);
    CHECK(publication_inspect(client,good).bunches.size()==1);
}
TEST_CASE("Actor output failure after transport commit is terminal with no partial result") {
    auto cfg=publication_config(); cfg.limits.max_output_bytes_per_step=500;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    auto large=actor_payload(); large.payload.assign(500,0); large.payload_bits=4000;
    // Interior fits512; its session-wrapped output exceeds this own500-byte cap.
    expect_error([&]{c.send_actor(large,6);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
    expect_error([&]{c.tick(7);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Actor bunches consume shared delivered message quota with control and no partial events") {
    auto cfg=publication_config(); cfg.limits.max_logical_messages_per_step=3;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    const auto control_a=inner(5,6,package_ack_literal(0,cfg.endian),false).records.front().bunch;
    const auto control_b=inner(5,7,package_ack_literal(1,cfg.endian),false).records.front().bunch;
    expect_error([&]{c.receive(actor_request(client,5,{actor_payload(1),control_a,
        actor_payload(2,false),control_b}),6);},ConnectionErrorCode::resource_limit);
    CHECK(c.snapshot().closed);
}

namespace {
wire::Bunch unreliable_actor_payload() {
    wire::Bunch bunch;
    bunch.channel=7; bunch.payload_bits=13; bunch.payload={0x06,0x01};
    // Opaque nonbyte actor bits, deliberately starting with control Failure6.
    // Unreliable omits both sequence and channel-type from its wire header.
    return bunch;
}
void unreliable_actor_fields(const wire::Bunch& actual,const wire::Bunch& expected) {
    CHECK(actual.payload==expected.payload); CHECK(actual.payload_bits==expected.payload_bits);
    CHECK(actual.channel==7); CHECK(actual.channel_type==0); CHECK(actual.sequence==0);
    CHECK_FALSE(actual.reliable); CHECK_FALSE(actual.control);
    CHECK_FALSE(actual.open); CHECK_FALSE(actual.close);
}
}

TEST_CASE("Unreliable actor sends share Context without advancing reliable sequence") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto next=joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    const auto before=c.snapshot(); const auto payload=unreliable_actor_payload();
    const auto first=c.send_actor(payload,7); REQUIRE(first.datagrams.size()==1);
    CHECK(rejection_counter(first.datagrams.front())==next.nonce+1U);
    const auto a=publication_inspect(client,first); REQUIRE(a.bunches.size()==1);
    CHECK(a.packets.front().packet_id==((next.packet_id+1U)&0x3fffU)); unreliable_actor_fields(a.bunches[0],payload);
    CHECK(first.actor_bunches.empty()); CHECK_FALSE(first.closed); CHECK(c.snapshot()==before);
    const auto second=c.send_actor(actor_payload(0,false),8); REQUIRE(second.datagrams.size()==1);
    CHECK(rejection_counter(second.datagrams.front())==next.nonce+2U);
    const auto b=publication_inspect(client,second); REQUIRE(b.bunches.size()==1);
    CHECK(b.packets.front().packet_id==((next.packet_id+2U)&0x3fffU));
    actor_fields(b.bunches[0],actor_payload(43,false));
}
TEST_CASE("Unreliable actor stale packets bypass a reliable gap while wrapper replay is rejected") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    const auto future=c.receive(actor_request(client,5,{actor_payload(2,false)}),6);
    CHECK(future.actor_bunches.empty()); publication_inspect(client,future);
    const auto payload=unreliable_actor_payload(); const auto raw=actor_request(client,4,{payload});
    const auto stale=c.receive(raw,7); REQUIRE(stale.actor_bunches.size()==1);
    unreliable_actor_fields(stale.actor_bunches[0],payload); CHECK_FALSE(stale.closed);
    publication_inspect(client,stale); const auto before=c.snapshot();
    expect_error([&]{c.receive(raw,8);},ConnectionErrorCode::replay); CHECK(c.snapshot()==before);
    const auto drain=c.receive(actor_request(client,6,{actor_payload(1,false)}),8);
    REQUIRE(drain.actor_bunches.size()==2); CHECK(drain.actor_bunches[0].sequence==1);
    CHECK(drain.actor_bunches[1].sequence==2); publication_inspect(client,drain);
}
TEST_CASE("Unreliable actor input before opening is ignored and accepts after send without its ACK") {
    for (const bool confirmed:{false,true}) {
        const auto cfg=publication_config(); game::GameConnection c(cfg);
        session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
        joined_actor(c,client,cfg.endian); c.register_actor_channel({7,41,0,confirmed});
        const auto payload=unreliable_actor_payload();
        const auto early=c.receive(actor_request(client,5,{payload}),6);
        CHECK(early.actor_bunches.empty()); CHECK_FALSE(early.closed);
        CHECK_FALSE(early.datagrams.empty()); publication_inspect(client,early);
        const auto opening=c.send_actor(actor_payload(),7); publication_inspect(client,opening);
        const auto accepted=c.receive(actor_request(client,6,{payload}),8);
        REQUIRE(accepted.actor_bunches.size()==1); unreliable_actor_fields(accepted.actor_bunches[0],payload);
        CHECK_FALSE(accepted.closed); publication_inspect(client,accepted);
    }
}
TEST_CASE("Unreliable actor dispatch and valid control share a packet without opcode confusion") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    const auto control_bunch=inner(5,6,package_ack_literal(0,cfg.endian),false).records.front().bunch;
    const auto payload=unreliable_actor_payload();
    const auto step=c.receive(actor_request(client,5,{payload,control_bunch,payload}),6);
    REQUIRE(step.actor_bunches.size()==2); unreliable_actor_fields(step.actor_bunches[0],payload);
    unreliable_actor_fields(step.actor_bunches[1],payload);
    CHECK_FALSE(step.closed); CHECK_FALSE(step.join_requested); CHECK_FALSE(step.login_candidate);
    CHECK(publication_inspect(client,step).bunches.empty());
}
TEST_CASE("Unreliable actor send rejects unready and noncanonical shapes without consuming nonce") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    const auto next=joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    const auto before=c.snapshot(); const auto payload=unreliable_actor_payload();
    expect_error([&]{c.send_actor(payload,99);},ConnectionErrorCode::invalid_packet); CHECK(c.snapshot()==before);
    c.register_actor_channel({8,0,0,true});
    for (unsigned invalid=0;invalid<7;++invalid) {
        auto bad=payload; bad.channel=8;
        switch (invalid) {
        case 0: bad.sequence=1; break;
        case 1: bad.channel_type=2; break;
        case 2: bad.control=true; bad.open=true; bad.channel_type=2; break;
        case 3: bad.control=true; bad.close=true; break;
        case 4: bad.channel=0; break;
        case 5: bad.channel=9; break;
        case 6: bad.payload.back()=0x80; break;
        }
        expect_error([&]{c.send_actor(bad,99);},ConnectionErrorCode::invalid_packet); CHECK(c.snapshot()==before);
    }
    const auto opening=c.send_actor(actor_payload(),6); REQUIRE(opening.datagrams.size()==1);
    CHECK(rejection_counter(opening.datagrams.front())==next.nonce); publication_inspect(client,opening);
    const auto good=c.send_actor(payload,7); REQUIRE(good.datagrams.size()==1);
    CHECK(rejection_counter(good.datagrams.front())==next.nonce+1U); publication_inspect(client,good);
}
TEST_CASE("Unreliable actor bunches consume shared message quota without partial events") {
    auto cfg=publication_config(); cfg.limits.max_logical_messages_per_step=3;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    const auto payload=unreliable_actor_payload();
    expect_error([&]{c.receive(actor_request(client,5,{payload,payload,payload,payload}),6);},
        ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
}
TEST_CASE("Unreliable actor output quota after transport commit closes the same Context") {
    auto cfg=publication_config(); cfg.limits.max_output_bytes_per_step=500;
    game::GameConnection c(cfg); session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    auto large=unreliable_actor_payload(); large.payload.assign(500,0); large.payload_bits=4000;
    expect_error([&]{c.send_actor(large,7);},ConnectionErrorCode::resource_limit); CHECK(c.snapshot().closed);
    expect_error([&]{c.tick(7);},ConnectionErrorCode::terminal_state);
}
TEST_CASE("Unreliable actor traffic cannot reopen a closed reliable actor channel") {
    const auto cfg=publication_config(); game::GameConnection c(cfg);
    session::Context client(session::Role::client,cfg.session_id,cfg.session_key);
    joined_actor(c,client,cfg.endian); c.register_actor_channel(actor_config());
    publication_inspect(client,c.send_actor(actor_payload(),6));
    const auto close=c.receive(actor_request(client,5,{actor_payload(1,false,true)}),6);
    CHECK(close.closed_actor_channels==std::vector<std::uint16_t>{7}); publication_inspect(client,close);
    const auto payload=unreliable_actor_payload();
    const auto ignored=c.receive(actor_request(client,6,{payload}),7);
    CHECK(ignored.actor_bunches.empty()); CHECK(ignored.closed_actor_channels.empty());
    CHECK_FALSE(ignored.closed); publication_inspect(client,ignored);
    const auto before=c.snapshot();
    expect_error([&]{c.send_actor(payload,99);},ConnectionErrorCode::terminal_state); CHECK(c.snapshot()==before);
}
