#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "hawken/local_udp_owner.hpp"
#include "hawken/packet_writer.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace hawken;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
constexpr session::SessionId public_id{0,0x11,0x22,0x33,0x44,0x55,0x66,0x77};
constexpr session::SessionKey public_key{
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
constexpr std::string_view server_guid = "12345678-1234-5678-9012-123456789abc";
static_assert(!std::is_copy_constructible_v<udp::LocalUdpOwner>);
static_assert(!std::is_move_constructible_v<udp::LocalUdpOwner>);

game::ConnectionConfig skeleton() {
    // Explicit SYNTHETIC limits/seeds, public zero challenge, NOT native defaults.
    // Real session ID/key are supplied ONLY by handoff, never this skeleton.
    return {{},{},{0,0xffffffffU,0xffffffffU,0,0,false,64,{16,16,16,1024,16,16},5.0},
        control::Endian::little,0,7,11,{false,false},{112,16,16,1024},
        {{128,64},0x1234abcdU,30000}};
}
assignment::Destination destination(std::uint16_t port) {
    return {std::string(server_guid),"127.0.0.1",port,"EU","1.6.0.154767"};
}
assignment::SessionProvision provision(std::uint16_t port) {
    return {{"udp-fixture",1},"11223344-5566-7788-99aa-bbccddeeff00",public_id,public_key,
        destination(port),130};
}
Bytes hex(std::string_view text) {
    auto digit=[](char c)->std::uint8_t {
        if (c>='0' && c<='9') return static_cast<std::uint8_t>(c-'0');
        if (c>='a' && c<='f') return static_cast<std::uint8_t>(c-'a'+10);
        throw std::logic_error("Invalid public UDP fixture.");
    };
    if (text.size()%2) throw std::logic_error("Invalid public UDP fixture.");
    Bytes bytes;
    for (std::size_t i=0;i<text.size();i+=2)
        bytes.push_back(static_cast<std::uint8_t>((digit(text[i])<<4)|digit(text[i+1])));
    return bytes;
}
// Independent literal bytes reused from GREEN GameConnection's public oracles.
// implementation/evidence/game-connection-contract/literal-vectors.json records
// their independent OpenSSL/HMAC derivation. These are NOT runtime credentials.
Bytes initial_oracle() {
    return hex("001122334455667701000000000000002f24f44250c12ff8a25231f5"
        "b86b27563d1079c30df13edd292c38e9f3f505020a5095234f85a371ac3df356");
}
Bytes initial_preopen_oracle() {
    // Same independently derived artifact, vector initial-no-open-le.
    return hex("001122334455667701000000000000002fa4f01210d0e695a25231e1"
        "1e3a01527ea6d241d7d79800086fedccd5cc815b5e964fba68cceceff2dc2ba5");
}
Bytes ack_oracle() {
    return hex("001122334455667701000000000000805af2f637"
        "312e29776e1451e8b400bf7a9ee0e58db3633c394b608e51ae04847de1b406f6");
}
Bytes request(session::Context& client,std::uint16_t pid,std::uint16_t seq,
              const Bytes& logical,bool open=true) {
    wire::Packet packet; packet.packet_id=pid;
    wire::PacketRecord record; auto& bunch=record.bunch;
    bunch.control=open; bunch.open=open; bunch.reliable=true;
    bunch.channel_type=1; bunch.sequence=seq; bunch.payload=logical;
    bunch.payload_bits=logical.size()*8;
    packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,64));
}
template<class Predicate>
bool wait_until(Predicate predicate) {
    const auto end=std::chrono::steady_clock::now()+2000ms;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now()<end);
    return predicate();
}
#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2,2),&data)!=0)
            throw std::runtime_error("Public UDP fixture could not initialize.");
    }
    ~SocketRuntime() { WSACleanup(); }
};
using NativeSocket=SOCKET;
constexpr NativeSocket invalid_socket=INVALID_SOCKET;
#else
struct SocketRuntime {};
using NativeSocket=int;
constexpr NativeSocket invalid_socket=-1;
#endif
void close_socket(NativeSocket socket) noexcept {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}
class Peer {
public:
    explicit Peer(std::uint16_t port=0,std::uint32_t local_address=INADDR_LOOPBACK) {
        socket_=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if (socket_==invalid_socket) throw std::runtime_error("Public UDP fixture could not open.");
        try {
#ifdef _WIN32
            const int exclusive=1;
            if (setsockopt(socket_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),static_cast<int>(sizeof(exclusive)))!=0)
                throw std::runtime_error("Public UDP fixture could not configure.");
#endif
            sockaddr_in address{}; address.sin_family=AF_INET;
            address.sin_addr.s_addr=htonl(local_address); address.sin_port=htons(port);
            if (::bind(socket_,reinterpret_cast<const sockaddr*>(&address),static_cast<int>(sizeof(address)))!=0)
                throw std::runtime_error("Public UDP fixture could not bind.");
#ifdef _WIN32
            int size=static_cast<int>(sizeof(address));
#else
            socklen_t size=sizeof(address);
#endif
            if (getsockname(socket_,reinterpret_cast<sockaddr*>(&address),&size)!=0)
                throw std::runtime_error("Public UDP fixture could not inspect bind.");
            port_=ntohs(address.sin_port);
        } catch (...) { close_socket(socket_); socket_=invalid_socket; throw; }
    }
    ~Peer() { if (socket_!=invalid_socket) close_socket(socket_); }
    Peer(const Peer&)=delete;
    Peer& operator=(const Peer&)=delete;
    std::uint16_t port() const { return port_; }
    void send(std::uint16_t target,const Bytes& bytes) {
        sockaddr_in address{}; address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(target);
#ifdef _WIN32
        const auto count=sendto(socket_,reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),0,reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address)));
#else
        const auto count=sendto(socket_,bytes.data(),bytes.size(),0,
            reinterpret_cast<const sockaddr*>(&address),sizeof(address));
#endif
        if (count<0 || static_cast<std::size_t>(count)!=bytes.size())
            throw std::runtime_error("Public UDP fixture could not send.");
    }
    std::optional<Bytes> receive(std::chrono::milliseconds timeout=2000ms) {
        fd_set readable; FD_ZERO(&readable); FD_SET(socket_,&readable);
        const auto milliseconds=timeout.count();
        timeval time{static_cast<long>(milliseconds/1000),static_cast<long>((milliseconds%1000)*1000)};
#ifdef _WIN32
        const int ready=select(0,&readable,nullptr,nullptr,&time);
#else
        const int ready=select(socket_+1,&readable,nullptr,nullptr,&time);
#endif
        if (ready<0) throw std::runtime_error("Public UDP fixture could not wait.");
        if (ready==0) return std::nullopt;
        std::array<std::uint8_t,65507> buffer{};
        sockaddr_in source{};
#ifdef _WIN32
        int size=static_cast<int>(sizeof(source));
        const auto count=recvfrom(socket_,reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),0,reinterpret_cast<sockaddr*>(&source),&size);
#else
        socklen_t size=sizeof(source);
        const auto count=recvfrom(socket_,buffer.data(),buffer.size(),0,
            reinterpret_cast<sockaddr*>(&source),&size);
#endif
        if (count<0) throw std::runtime_error("Public UDP fixture could not receive.");
        CHECK(source.sin_addr.s_addr==htonl(INADDR_LOOPBACK));
        return Bytes(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(count));
    }
    void drain() {
        const auto end=std::chrono::steady_clock::now()+2000ms;
        do { if (!receive(50ms)) return; }
        while (std::chrono::steady_clock::now()<end);
        throw std::runtime_error("Public UDP fixture did not become idle.");
    }
private:
    SocketRuntime runtime_;
    NativeSocket socket_=invalid_socket;
    std::uint16_t port_=0;
};
struct Fixture {
    std::atomic<double> now{100};
    std::atomic<bool> allowed{true};
    std::atomic<int> authorizations{0};
    udp::LocalUdpOwner owner{0,skeleton(),[this]{return now.load();},5ms};
    Fixture() {
        owner.start([this](const session::SessionId& id) {
            ++authorizations; return allowed.load() && id==public_id;
        });
    }
    ~Fixture() { owner.stop(); }
};
template<class Action>
void error(Action action,udp::LocalUdpErrorCode code) {
    try { action(); }
    catch (const udp::LocalUdpError& e) {
        CHECK(e.code()==code);
        CHECK(std::string(e.what())=="Local UDP operation could not be completed.");
        CHECK(std::string(e.what()).find("private-")==std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("Expected redacted LocalUdpError."); return; }
    FAIL_CHECK("Local UDP owner accepted invalid input.");
}
void first(Peer& peer,Fixture& fixture,session::Context& client,bool open=true) {
    const auto initial=open?initial_oracle():initial_preopen_oracle();
    CHECK(request(client,0,1,hex("1a07000000"),open)==initial);
    peer.send(fixture.owner.port(),initial);
    const auto ack=peer.receive(); REQUIRE(ack); CHECK(*ack==ack_oracle());
    const auto challenge=peer.receive(); REQUIRE(challenge);
    CHECK(client.decode(*ack)==hex("00400020"));
    const auto plain=client.decode(*challenge);
    CHECK(plain==hex("0100011040506c0000000004"));
    peer.drain(); // Existing transport tick may replay its prior ACK once.
}
std::vector<Bytes> logical_replies(Peer& peer,session::Context& client) {
    std::vector<Bytes> logical;
    const auto end=std::chrono::steady_clock::now()+2000ms;
    while (std::chrono::steady_clock::now()<end) {
        const auto packet=peer.receive(100ms);
        if (!packet) continue;
        const auto decoded=wire::decode_packet(client.decode(*packet),64);
        for (const auto& record:decoded.records)
            if (!record.is_ack) logical.push_back(record.bunch.payload);
        if (!logical.empty()) break;
    }
    return logical;
}
void through_hello(Peer& peer,Fixture& fixture,session::Context& client) {
    first(peer,fixture,client);
    peer.send(fixture.owner.port(),request(client,1,2,hex("1cc3363f8d"),false));
    REQUIRE(logical_replies(peer,client)==std::vector<Bytes>{hex("1d")}); peer.drain();
    peer.send(fixture.owner.port(),request(client,2,3,hex("00080000001400000098badcfe"),false));
    REQUIRE(logical_replies(peer,client)==std::vector<Bytes>{hex("030b00000009000000313233344142434400")});
    peer.drain();
}
void acknowledgements_only(Peer& peer,session::Context& client) {
    const auto end=std::chrono::steady_clock::now()+200ms;
    while (std::chrono::steady_clock::now()<end) {
        const auto packet=peer.receive(40ms);
        if (!packet) continue;
        const auto decoded=wire::decode_packet(client.decode(*packet),64);
        for (const auto& record:decoded.records) CHECK(record.is_ack);
    }
}
class FakeVerifier final : public auth::SteamVerifier {
public:
    std::atomic<bool> active{false};
    auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        active=ticket.size()==4 && ticket[0]==0xaa && ticket[1]==0xbb && ticket[2]==1 && ticket[3]==0;
        return {active.load()?auth::SteamAuthStatus::authenticated:auth::SteamAuthStatus::invalid_ticket,
            active.load()?76561197960265729ULL:0};
    }
    bool is_authenticated(std::uint64_t id) const override { return active.load() && id==76561197960265729ULL; }
    void end_session(std::uint64_t) override { active=false; }
};
struct AuthFixture {
    test::TempDirectory temporary;
    store::LocalStore store{temporary.path()/"accounts.sqlite","udp-auth-fixture"};
    FakeVerifier verifier;
    std::atomic<std::int64_t> auth_now{1000},monotonic{100};
    std::atomic<bool> authorization_rejected{false};
    auth::AuthService auth{store,verifier,[this]{return auth_now.load();}};
    udp::LocalUdpOwner owner{0,skeleton(),[this]{return static_cast<double>(monotonic.load());},5ms};
    std::unique_ptr<assignment::SessionAssignmentService> service;
    AuthFixture() {
        owner.start([this](const session::SessionId& id){
            const bool allowed=service && service->authorize_udp(id);
            if (!allowed) authorization_rejected=true;
            return allowed;
        });
    }
    ~AuthFixture() { owner.stop(); service.reset(); }
    void construct_assignment() {
        service=std::make_unique<assignment::SessionAssignmentService>(auth,destination(owner.port()),
            assignment::AssignmentPolicy{30,1200,800},[this]{return monotonic.load();},[]{return 1704067200LL;},
            [](std::span<std::uint8_t> bytes) {
                if (bytes.size()==public_id.size()) std::copy(public_id.begin(),public_id.end(),bytes.begin());
                else if (bytes.size()==public_key.size()) std::copy(public_key.begin(),public_key.end(),bytes.begin());
                else return false;
                return true;
            },[this](const assignment::Destination& d){return owner.destination_bound(d);},
            [this](const assignment::SessionProvision& p){return owner.handoff(p);});
    }
    auth::AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
std::string authorization(const auth::AuthReply& reply) {
    return "Basic "+reply.result.at("AccessGrant").get<std::string>();
}
// Releasing on every unwind prevents a failed assertion from trapping worker/join.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered=false,released=false;
    void enter() {
        std::unique_lock lock(mutex); entered=true; cv.notify_all();
        cv.wait_for(lock,5000ms,[this]{return released;});
    }
    bool wait_entered() {
        std::unique_lock lock(mutex); return cv.wait_for(lock,2000ms,[this]{return entered;});
    }
    void release() { std::lock_guard lock(mutex); released=true; cv.notify_all(); }
    ~Gate() { release(); }
};
struct GateRelease {
    Gate& gate;
    ~GateRelease() { gate.release(); }
};
}

TEST_CASE("UDP owner publishes actual exclusive loopback bind and authenticates first literal exchange") {
    Fixture f; REQUIRE(f.owner.port()>0); REQUIRE(f.owner.running());
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{});
    CHECK(f.owner.destination_bound(destination(f.owner.port())));
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); REQUIRE(lease->active());
    Peer peer; session::Context client(session::Role::client,public_id,public_key);
    first(peer,f,client);
    CHECK(lease->active()); // Challenge1b is not admission or Welcome.
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{1,false,false});
}
TEST_CASE("UDP bound callback rejects unstarted stopped wrong IP and wrong port") {
    udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms);
    REQUIRE(owner.port()>0); CHECK_FALSE(owner.destination_bound(destination(owner.port())));
    CHECK_FALSE(owner.handoff(provision(owner.port())));
    owner.start([](const session::SessionId&){return true;});
    CHECK(owner.destination_bound(destination(owner.port())));
    auto d=destination(owner.port()); d.ip="127.0.0.2"; CHECK_FALSE(owner.destination_bound(d));
    d=destination(owner.port()); d.port=owner.port()==65535?1:owner.port()+1;
    CHECK_FALSE(owner.destination_bound(d));
    owner.stop(); CHECK_FALSE(owner.destination_bound(destination(owner.port())));
}
TEST_CASE("UDP bind rejects collision without adopting or disturbing existing socket") {
    Peer occupied;
    error([&]{udp::LocalUdpOwner owner(occupied.port(),skeleton(),[]{return 100.0;},5ms);},
        udp::LocalUdpErrorCode::bind_failed);
    Fixture f; REQUIRE(f.owner.port()>0);
    error([&]{udp::LocalUdpOwner owner(f.owner.port(),skeleton(),[]{return 100.0;},5ms);},
        udp::LocalUdpErrorCode::bind_failed);
    CHECK(f.owner.running());
}
TEST_CASE("UDP owner rejects missing callback repeat start and restart after stop") {
    udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms);
    error([&]{owner.start({});},udp::LocalUdpErrorCode::invalid_lifecycle);
    owner.start([](const session::SessionId&){return false;});
    CHECK(owner.running());
    error([&]{owner.start([](const session::SessionId&){return true;});},udp::LocalUdpErrorCode::invalid_lifecycle);
    owner.stop(); owner.stop();
    error([&]{owner.start([](const session::SessionId&){return true;});},udp::LocalUdpErrorCode::invalid_lifecycle);
}
TEST_CASE("UDP owner validates explicit configuration without provisioning a context") {
    error([]{udp::LocalUdpOwner o(0,skeleton(),{},5ms);},udp::LocalUdpErrorCode::invalid_configuration);
    for (auto interval:{0ms,1001ms})
        error([&]{udp::LocalUdpOwner o(0,skeleton(),[]{return 100.0;},interval);},
            udp::LocalUdpErrorCode::invalid_configuration);
    for (int mutation=0;mutation<4;++mutation) {
        auto cfg=skeleton();
        if (mutation==0) cfg.transport.max_packet_bytes=1;
        if (mutation==1) cfg.limits.max_datagram_bytes=111;
        if (mutation==2) cfg.server_min=12;
        if (mutation==3) cfg.endian=static_cast<control::Endian>(99);
        error([&]{udp::LocalUdpOwner o(0,cfg,[]{return 100.0;},5ms);},udp::LocalUdpErrorCode::invalid_configuration);
    }
}
TEST_CASE("UDP skeleton cannot retain real credential material") {
    auto cfg=skeleton(); cfg.session_id=public_id;
    error([&]{udp::LocalUdpOwner o(0,cfg,[]{return 100.0;},5ms);},udp::LocalUdpErrorCode::invalid_configuration);
    cfg=skeleton(); cfg.session_key=public_key;
    error([&]{udp::LocalUdpOwner o(0,cfg,[]{return 100.0;},5ms);},udp::LocalUdpErrorCode::invalid_configuration);
}
TEST_CASE("UDP failed handoffs preserve successful installation budget") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto p=provision(f.owner.port()); p.destination.ip="127.0.0.2";
    CHECK_FALSE(f.owner.handoff(p));
    p=provision(f.owner.port()); p.expires_at_monotonic=100;
    CHECK_FALSE(f.owner.handoff(p));
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); CHECK(lease->active());
}
TEST_CASE("UDP first-trial single installation does not clone or reset retired context") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
    CHECK_FALSE(f.owner.handoff(provision(f.owner.port())));
    lease.reset();
    CHECK_FALSE(f.owner.handoff(provision(f.owner.port())));
    auto different=provision(f.owner.port()); ++different.id[0]; ++different.key[0];
    CHECK_FALSE(f.owner.handoff(different)); // Temporary limitation, not final multiplayer policy.
}
TEST_CASE("UDP owner ignores absent installation and unauthorized foreign IDs") {
    Fixture f; REQUIRE(f.owner.port()>0); Peer peer;
    peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms));
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
    auto foreign=initial_oracle(); foreign[0]^=0x80; peer.send(f.owner.port(),foreign);
    CHECK_FALSE(peer.receive(200ms)); CHECK(lease->active());
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
}
TEST_CASE("UDP invalid MAC cannot pin attacker peer or consume valid receive nonce") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
    Peer attacker,peer; auto invalid=initial_oracle(); invalid.back()^=1;
    attacker.send(f.owner.port(),invalid); CHECK_FALSE(attacker.receive(200ms)); CHECK(lease->active());
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{});
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    CHECK_FALSE(attacker.receive(200ms));
}
TEST_CASE("UDP size and truncated datagrams do not poison a later valid request") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    peer.send(f.owner.port(),Bytes{1,2,3});
    peer.send(f.owner.port(),Bytes(session::wrapper_bytes,0));
    auto too_large=initial_oracle(); too_large.resize(65507,0x55); peer.send(f.owner.port(),too_large);
    auto truncated=initial_oracle(); truncated.pop_back(); peer.send(f.owner.port(),truncated);
    CHECK_FALSE(peer.receive(200ms)); CHECK(lease->active());
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
}
TEST_CASE("UDP replay is dropped while persistent counters permit following CRC exchange") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    CHECK(f.owner.diagnostics().authenticated_datagrams==1);
    peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms)); CHECK(lease->active());
    CHECK(f.owner.diagnostics().authenticated_datagrams==1);
    peer.send(f.owner.port(),request(client,1,2,hex("1cc3363f8d"),false));
    const auto replies=logical_replies(peer,client);
    REQUIRE(replies.size()==1); CHECK(replies.front()==hex("1d"));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{2,false,false});
}
TEST_CASE("UDP authenticated peer IP and port cannot be replaced or consume counters") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer,other;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    const auto response=request(client,1,2,hex("1cc3363f8d"),false);
    other.send(f.owner.port(),response); CHECK_FALSE(other.receive(200ms));
    peer.send(f.owner.port(),response);
    const auto replies=logical_replies(peer,client);
    REQUIRE(replies.size()==1); CHECK(replies.front()==hex("1d")); CHECK(lease->active());
}
TEST_CASE("UDP authenticated malformed interior retires connection without partial output") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key);
    peer.send(f.owner.port(),client.encode(Bytes{0}));
    CHECK(wait_until([&]{return !lease->active();})); CHECK_FALSE(peer.receive(200ms));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{0,false,true});
    peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms));
}
TEST_CASE("UDP authenticated peer cannot be replaced by another loopback IP at same port") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    Peer other(peer.port(),0x7f000002U);
    const auto response=request(client,1,2,hex("1cc3363f8d"),false);
    other.send(f.owner.port(),response); CHECK_FALSE(other.receive(200ms));
    peer.send(f.owner.port(),response);
    const auto replies=logical_replies(peer,client);
    REQUIRE(replies.size()==1); CHECK(replies.front()==hex("1d")); CHECK(lease->active());
}
TEST_CASE("UDP owner ticks retry without resetting reliable sequence or nonce") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client,false);
    f.now=102;
    const auto retries=logical_replies(peer,client);
    REQUIRE(retries.size()==1); CHECK(retries.front()==hex("1b00000000"));
    CHECK(client.last_received()>session::direction_bit+2); CHECK(lease->active());
}
TEST_CASE("UDP owner does not invent retries after open confirmation") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    f.now=102;
    CHECK_FALSE(peer.receive(200ms)); CHECK(lease->active());
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{1,false,false});
}
TEST_CASE("UDP Hello sends original Challenge3 without Welcome Login or admission") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    peer.send(f.owner.port(),request(client,1,2,hex("1cc3363f8d"),false));
    REQUIRE(logical_replies(peer,client)==std::vector<Bytes>{hex("1d")}); peer.drain();
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{2,false,false});
    peer.send(f.owner.port(),request(client,2,3,hex("00080000001400000098badcfe"),false));
    REQUIRE(logical_replies(peer,client)==std::vector<Bytes>{hex("030b00000009000000313233344142434400")});
    CHECK(lease->active()); peer.drain(); CHECK_FALSE(peer.receive(200ms));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{3,true,false,false});
    f.owner.stop(); CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{3,true,true,false});
}
TEST_CASE("UDP Login observation remains pending with active lease and no Welcome") {
    Fixture f; auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); through_hello(peer,f,client);
    CHECK_FALSE(f.owner.diagnostics().login_received);
    // Public raw Login fixture from GREEN posthello codec; this ID is not admitted.
    const auto packet=request(client,3,4,
        hex("050000000000000000040302017856341200000080ffffffff"),false);
    peer.send(f.owner.port(),packet);
    REQUIRE(wait_until([&]{return f.owner.diagnostics().login_received;}));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,false,true});
    CHECK(lease->active()); CHECK(f.owner.running()); acknowledgements_only(peer,client);
    peer.send(f.owner.port(),packet); CHECK_FALSE(peer.receive(200ms)); // Wrapper replay is dropped.
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,false,true}); CHECK(lease->active());
    f.allowed=false; REQUIRE(wait_until([&]{return !lease->active();}));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,true,true});
}
TEST_CASE("UDP bad MAC cannot mark Login or consume its valid provisioned receive") {
    Fixture f; auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); through_hello(peer,f,client);
    const auto packet=request(client,3,4,
        hex("050000000000000000040302017856341200000080ffffffff"),false);
    auto corrupted=packet; corrupted.back()^=1;
    peer.send(f.owner.port(),corrupted); CHECK_FALSE(peer.receive(200ms));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{3,true,false,false}); CHECK(lease->active());
    peer.send(f.owner.port(),packet);
    REQUIRE(wait_until([&]{return f.owner.diagnostics().login_received;}));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,false,true});
    acknowledgements_only(peer,client); CHECK(lease->active());
    // Strict pending policy still rejects a NEW logical message, without admission.
    peer.send(f.owner.port(),request(client,4,5,hex("0408070000"),false));
    REQUIRE(wait_until([&]{return !lease->active();}));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,true,true});
}
TEST_CASE("UDP valid postHello Failure retires without Login or echoed failure text") {
    Fixture f; auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); Peer peer;
    session::Context client(session::Role::client,public_id,public_key); through_hello(peer,f,client);
    peer.send(f.owner.port(),request(client,3,4,hex("0604000000500049ff"),false));
    REQUIRE(wait_until([&]{return !lease->active();}));
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,true,false});
    acknowledgements_only(peer,client); CHECK(f.owner.running());
    f.owner.stop(); CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{4,true,true,false});
}
TEST_CASE("UDP scalar diagnostics stay monotonic during concurrent reads and shutdown") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
    std::atomic<bool> regressed{false};
    std::jthread observer([&](std::stop_token token) {
        std::uint64_t previous=0; bool hello=false,closed=false;
        while (!token.stop_requested()) {
            const auto values=f.owner.diagnostics();
            if (values.authenticated_datagrams<previous || (hello && !values.hello_validated)
                    || (closed && !values.closed)) regressed=true;
            previous=values.authenticated_datagrams; hello=values.hello_validated; closed=values.closed;
            std::this_thread::sleep_for(1ms);
        }
    });
    Peer peer; session::Context client(session::Role::client,public_id,public_key); first(peer,f,client);
    f.owner.stop(); observer.request_stop(); observer.join();
    CHECK_FALSE(regressed.load());
    CHECK(f.owner.diagnostics()==udp::LocalUdpDiagnostics{1,false,true});
}
TEST_CASE("UDP idle authorization retires a revoked session without network input") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease); f.allowed=false;
    CHECK(wait_until([&]{return !lease->active();}));
    Peer peer; peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms));
}
TEST_CASE("UDP throwing authorizer is contained and retires session without echoing exception") {
    udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms); REQUIRE(owner.port()>0);
    owner.start([](const session::SessionId&)->bool{throw std::runtime_error("private-authorizer");});
    auto lease=owner.handoff(provision(owner.port())); REQUIRE(lease);
    CHECK(wait_until([&]{return !lease->active();}));
    Peer peer; peer.send(owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms)); owner.stop();
}
TEST_CASE("UDP invalid or regressed monotonic clock closes listener and lease") {
    for (double invalid:{-1.0,99.0,std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        Fixture f; REQUIRE(f.owner.port()>0);
        auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
        REQUIRE(wait_until([&]{return f.authorizations.load()>0;}));
        f.now=invalid;
        CHECK(wait_until([&]{return !f.owner.running();})); CHECK_FALSE(lease->active());
        CHECK_FALSE(f.owner.destination_bound(destination(f.owner.port())));
    }
}
TEST_CASE("UDP throwing clock cannot escape worker or keep stale listener active") {
    std::atomic<bool> fail{false};
    udp::LocalUdpOwner owner(0,skeleton(),[&]{
        if (fail.load()) throw std::runtime_error("private-clock"); return 100.0;
    },5ms);
    REQUIRE(owner.port()>0); owner.start([](const session::SessionId&){return true;});
    auto lease=owner.handoff(provision(owner.port())); REQUIRE(lease); fail=true;
    CHECK(wait_until([&]{return !owner.running();})); CHECK_FALSE(lease->active()); owner.stop();
}
TEST_CASE("UDP retirement finishes before blocked authorization can pin removed context") {
    Gate gate; udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms);
    const GateRelease release{gate};
    REQUIRE(owner.port()>0); owner.start([&](const session::SessionId&){gate.enter(); return true;});
    auto lease=owner.handoff(provision(owner.port())); REQUIRE(lease);
    // Idle can enter the barrier before the packet. Either path MUST authorize
    // before pin; retire must NOT wait for this not-yet-authorized worker.
    Peer peer; peer.send(owner.port(),initial_oracle());
    const bool entered=gate.wait_entered();
    std::promise<void> done; auto result=done.get_future();
    std::jthread retire([&]{lease.reset(); done.set_value();});
    const bool retired=result.wait_for(2000ms)==std::future_status::ready;
    gate.release(); retire.join(); owner.stop();
    CHECK(entered); CHECK(retired); CHECK_FALSE(peer.receive(200ms));
}
TEST_CASE("UDP stop joins idle worker releases exclusive port and invalidates surviving lease") {
    Fixture f; REQUIRE(f.owner.port()>0);
    auto lease=f.owner.handoff(provision(f.owner.port())); REQUIRE(lease);
    const auto port=f.owner.port(); const auto begin=std::chrono::steady_clock::now();
    f.owner.stop(); f.owner.stop();
    CHECK(std::chrono::steady_clock::now()-begin<2000ms);
    CHECK_FALSE(f.owner.running()); CHECK_FALSE(lease->active());
    CHECK(f.owner.diagnostics().closed);
    Peer rebound(port); CHECK(rebound.port()==port);
}
TEST_CASE("UDP inactive lease can safely outlive owner without dangling registry") {
    std::unique_ptr<assignment::UdpSessionLease> lease;
    {
        udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms); REQUIRE(owner.port()>0);
        owner.start([](const session::SessionId&){return true;});
        lease=owner.handoff(provision(owner.port())); REQUIRE(lease); CHECK(lease->active());
    }
    CHECK_FALSE(lease->active()); lease.reset();
}
TEST_CASE("UDP stop waits owned callback completion before releasing callback lifetime") {
    Gate gate; udp::LocalUdpOwner owner(0,skeleton(),[]{return 100.0;},5ms);
    const GateRelease release{gate};
    REQUIRE(owner.port()>0); owner.start([&](const session::SessionId&){gate.enter(); return true;});
    auto lease=owner.handoff(provision(owner.port())); REQUIRE(lease);
    const bool entered=gate.wait_entered();
    std::promise<void> done; auto result=done.get_future();
    std::jthread shutdown([&]{owner.stop(); done.set_value();});
    const bool returned_early=result.wait_for(50ms)==std::future_status::ready;
    gate.release(); const bool finished=result.wait_for(2000ms)==std::future_status::ready;
    shutdown.join(); CHECK(entered); CHECK_FALSE(returned_early); CHECK(finished);
    CHECK_FALSE(lease->active());
}
TEST_CASE("UDP real assignment rejects unauthenticated provision and serves authorized same material") {
    AuthFixture f; REQUIRE(f.owner.port()>0); f.construct_assignment();
    CHECK(f.service->select("Basic invalid",server_guid).status==401);
    Peer peer; peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms));
    const auto login=f.login(); REQUIRE(login.status==200);
    REQUIRE(f.service->select(authorization(login),server_guid).status==200);
    const auto dto=f.service->handle("GET","/session",authorization(login)); REQUIRE(dto.status==200);
    CHECK(dto.result.at("SessionId")=="ABEiM0RVZnc");
    peer.send(f.owner.port(),initial_oracle()); const auto ack=peer.receive(); REQUIRE(ack); CHECK(*ack==ack_oracle());
    CHECK(f.service->handle("GET","/session",authorization(login)).result==dto.result);
    CHECK_FALSE(f.service->authorize_udp(session::SessionId{}));
    CHECK(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP real assignment expiry ban grant and Steam revocation retire idle transport") {
    for (int reason=0;reason<4;++reason) {
        AuthFixture f; REQUIRE(f.owner.port()>0); f.construct_assignment();
        const auto login=f.login(); REQUIRE(login.status==200);
        const auto header=authorization(login);
        REQUIRE(f.service->select(header,server_guid).status==200);
        REQUIRE(f.service->handle("GET","/session",header).status==200);
        f.authorization_rejected=false;
        if (reason==0) f.monotonic=130;
        if (reason==1) {
            const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
            f.store.set_ban(account->ref,{1000,std::nullopt,"public fixture ban"});
        }
        if (reason==2) f.auth_now=1900;
        if (reason==3) f.verifier.active=false;
        CHECK(wait_until([&]{return f.authorization_rejected.load();}));
        Peer peer; peer.send(f.owner.port(),initial_oracle()); CHECK_FALSE(peer.receive(200ms));
        CHECK(f.service->handle("GET","/session",header).status!=200);
    }
}

namespace {
game::ConnectionConfig authority_skeleton() {
    auto config=skeleton();
    // NEW public fixture budget: GUID36+NUL narrow Login is62 archive bytes,
    // wide is99; neither fits the old64 packet after bunch framing. No fragment.
    config.transport.max_packet_bytes=256;
    config.limits.max_datagram_bytes=304;
    config.limits.max_output_bytes_per_step=8192;
    config.login.limits={256,128};
    return config;
}
struct LoginAuthorityFixture {
    using Decision=std::function<bool(LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::RawString&)>;
    using PublicationDecision=std::function<std::optional<game::WorldPublication>(
        LoginAuthorityFixture&,const session::SessionId&,const control::posthello::Login&)>;
    test::TempDirectory temporary;
    store::LocalStore store{temporary.path()/"accounts.sqlite","udp-login-authority"};
    FakeVerifier verifier;
    std::atomic<std::int64_t> auth_now{1000},monotonic{100};
    // Same monotonic epoch, separately fault-injectable UDP clock reader.
    std::atomic<double> udp_now{100};
    std::atomic<int> validations{0};
    auth::AuthService auth{store,verifier,[this]{return auth_now.load();}};
    Decision decision;
    PublicationDecision publication_decision;
    std::atomic<int> publications{0};
    udp::LocalUdpOwner owner{0,authority_skeleton(),
        [this]{return udp_now.load();},5ms};
    std::unique_ptr<assignment::SessionAssignmentService> service;
    explicit LoginAuthorityFixture(Decision action={},bool with_validator=true,
                                   PublicationDecision publisher={},
                                   std::optional<udp::InitialControllerPlan> controller=std::nullopt,
                                   std::optional<game::ConnectionConfig> config=std::nullopt)
        : decision(std::move(action)),publication_decision(std::move(publisher)),
          owner(0,config.value_or(authority_skeleton()),[this]{return udp_now.load();},5ms) {
        service=std::make_unique<assignment::SessionAssignmentService>(auth,destination(owner.port()),
            assignment::AssignmentPolicy{30,1200,800},[this]{return monotonic.load();},[]{return 1704067200LL;},
            [](std::span<std::uint8_t> bytes) {
                if (bytes.size()==public_id.size()) std::copy(public_id.begin(),public_id.end(),bytes.begin());
                else if (bytes.size()==public_key.size()) std::copy(public_key.begin(),public_key.end(),bytes.begin());
                else return false;
                return true;
            },[this](const assignment::Destination& d){return owner.destination_bound(d);},
            [this](const assignment::SessionProvision& provision){return owner.handoff(provision);});
        auto authorize=[this](const session::SessionId& id){return service && service->authorize_udp(id);};
        udp::LocalUdpOwner::PublicationProvider publish;
        if (publication_decision)
            publish=[this](const session::SessionId& id,const control::posthello::Login& candidate) {
                ++publications;
                return publication_decision(*this,id,candidate);
            };
        if (with_validator)
            owner.start(std::move(authorize),[this](const session::SessionId& id,
                const control::posthello::RawString& response) {
                ++validations;
                return decision ? decision(*this,id,response) : service->validate_login_response(id,response);
            },std::move(publish),std::move(controller));
        else owner.start(std::move(authorize),{},std::move(publish),std::move(controller));
    }
    ~LoginAuthorityFixture() { owner.stop(); service.reset(); }
    auth::AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
Bytes authority_request(session::Context& client,std::uint16_t pid,std::uint16_t seq,
                        const Bytes& logical,bool open=false) {
    wire::Packet packet; packet.packet_id=pid;
    wire::PacketRecord record; auto& bunch=record.bunch;
    bunch.control=open; bunch.open=open; bunch.reliable=true;
    bunch.channel_type=1; bunch.sequence=seq; bunch.payload=logical;
    bunch.payload_bits=logical.size()*8;
    packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
std::vector<Bytes> authority_messages(Peer& peer,session::Context& client,
                                     std::chrono::milliseconds budget=2000ms) {
    std::vector<Bytes> logical;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto packet=peer.receive(40ms);
        if (!packet) continue;
        const auto decoded=wire::decode_packet(client.decode(*packet),256);
        for (const auto& record:decoded.records)
            if (!record.is_ack) logical.push_back(record.bunch.payload);
        if (!logical.empty()) break;
    }
    return logical;
}
void authority_hello(LoginAuthorityFixture& fixture,Peer& peer,session::Context& client) {
    // These short archives still reproduce the old independent plaintext oracles.
    peer.send(fixture.owner.port(),authority_request(client,0,1,hex("1a07000000"),true));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("1b00000000")}); peer.drain();
    peer.send(fixture.owner.port(),authority_request(client,1,2,hex("1cc3363f8d")));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("1d")}); peer.drain();
    peer.send(fixture.owner.port(),authority_request(client,2,3,hex("00080000001400000098badcfe")));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("030b00000009000000313233344142434400")});
    peer.drain();
}
Bytes authority_login(std::string_view guid,bool wide=false) {
    if (guid.size()!=36) throw std::logic_error("Invalid public Login fixture.");
    // Manual archive oracle: Login5, signed32 +/-37, ASCII GUID and finalNUL,
    // empty URL, four PUBLIC arbitrary ID words. This does not validate PlayerID.
    Bytes logical{5};
    const std::uint32_t length=wide?0xffffffdbU:37U;
    for (unsigned shift=0;shift<32;shift+=8) logical.push_back(static_cast<std::uint8_t>(length>>shift));
    for (const char unit:guid) {
        logical.push_back(static_cast<std::uint8_t>(unit));
        if (wide) logical.push_back(0);
    }
    logical.push_back(0); if (wide) logical.push_back(0);
    const auto tail=hex("00000000040302017856341200000080ffffffff");
    logical.insert(logical.end(),tail.begin(),tail.end());
    return logical;
}
void authority_select(LoginAuthorityFixture& fixture,const auth::AuthReply& login) {
    REQUIRE(login.status==200);
    REQUIRE(fixture.service->select(authorization(login),server_guid).status==200);
}
}

TEST_CASE("UDP matching Login Response is validated against real session without admission") {
    for (const bool wide:{false,true}) {
        LoginAuthorityFixture f; const auto login=f.login(); authority_select(f,login);
        const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
        const auto wallet=f.store.wallet(account->ref);
        const auto dto=f.service->handle("GET","/session",authorization(login)); REQUIRE(dto.status==200);
        Peer peer; session::Context client(session::Role::client,public_id,public_key);
        authority_hello(f,peer,client);
        peer.send(f.owner.port(),authority_request(client,3,4,
            authority_login(login.result.at("UserId").get<std::string>(),wide)));
        CHECK(wait_until([&]{return f.owner.diagnostics().login_response_validated;}));
        CHECK(f.validations.load()==1); CHECK(f.owner.diagnostics().login_received);
        CHECK_FALSE(f.owner.diagnostics().closed); CHECK(f.owner.running());
        CHECK(authority_messages(peer,client,200ms).empty()); // ACK only, no Welcome.
        CHECK(f.service->authorize_udp(public_id));
        CHECK(f.service->handle("GET","/session",authorization(login)).result==dto.result);
        CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    }
}
TEST_CASE("UDP mismatching Login Response emits fixed Failure with existing session nonces") {
    LoginAuthorityFixture f; const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key);
    authority_hello(f,peer,client); const auto nonce=client.last_received();
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login("abcdef01-2345-6789-abcd-ef0123456789")));
    CHECK(authority_messages(peer,client)==std::vector<Bytes>{hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(client.last_received()>nonce); CHECK(f.validations.load()==1);
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().login_response_validated); CHECK(f.owner.running());
    CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP throwing Login validator cannot echo exception or validate identity") {
    LoginAuthorityFixture f([](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::RawString&)->bool { throw std::runtime_error("private-validator"); });
    const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    CHECK(authority_messages(peer,client)==std::vector<Bytes>{hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(f.validations.load()==1); CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().login_response_validated);
}
TEST_CASE("UDP absent Login validator retains only candidate observation and no admission") {
    LoginAuthorityFixture f({},false); const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&]{return f.owner.diagnostics().login_received;}));
    CHECK_FALSE(f.owner.diagnostics().login_response_validated); CHECK(f.validations.load()==0);
    CHECK(authority_messages(peer,client,200ms).empty()); CHECK(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP fresh authorization discards accepted Response after callback revocation") {
    LoginAuthorityFixture f([](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::RawString&) { fixture.verifier.active=false; return true; });
    const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;})); CHECK(f.validations.load()==1);
    CHECK_FALSE(f.owner.diagnostics().login_response_validated);
    CHECK(authority_messages(peer,client,200ms).empty()); CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP validator may synchronously destroy assignment lease without self deadlock") {
    LoginAuthorityFixture f([](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::RawString&) { fixture.service.reset(); return true; });
    const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;})); CHECK(f.validations.load()==1);
    CHECK_FALSE(f.owner.diagnostics().login_response_validated);
    CHECK(authority_messages(peer,client,200ms).empty()); CHECK(f.owner.running());
}
TEST_CASE("UDP concurrent retirement discards late Login decision outside every pin") {
    Gate gate;
    LoginAuthorityFixture f([&](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::RawString&) { gate.enter(); return true; });
    GateRelease release{gate}; // Releases BEFORE fixture destructor joins on assertion unwind.
    const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    const bool entered=gate.wait_entered(); CHECK(entered);
    auto retired=std::async(std::launch::async,[&] {
        f.verifier.active=false;
        return f.service->authorize_udp(public_id);
    });
    const bool before_release=retired.wait_for(2000ms)==std::future_status::ready;
    gate.release(); // Always release BEFORE any failure assertion or future destruction.
    CHECK(before_release);
    const bool finished=retired.wait_for(2000ms)==std::future_status::ready; CHECK(finished);
    if (finished) CHECK_FALSE(retired.get());
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().login_response_validated); CHECK(f.validations.load()==1);
    CHECK(authority_messages(peer,client,200ms).empty());
}
TEST_CASE("UDP wrapper replay reliable duplicate and bad MAC do not repeat Login validation") {
    LoginAuthorityFixture f; const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    const auto archive=authority_login(login.result.at("UserId").get<std::string>());
    const auto packet=authority_request(client,3,4,archive);
    auto bad=packet; bad.back()^=1; peer.send(f.owner.port(),bad);
    CHECK(authority_messages(peer,client,200ms).empty()); CHECK(f.validations.load()==0);
    peer.send(f.owner.port(),packet);
    CHECK(wait_until([&]{return f.owner.diagnostics().login_response_validated;}));
    CHECK(authority_messages(peer,client,200ms).empty());
    peer.send(f.owner.port(),packet); // Exact encrypted replay must consume nothing.
    CHECK(authority_messages(peer,client,200ms).empty());
    CHECK(f.owner.diagnostics().authenticated_datagrams==4);
    peer.send(f.owner.port(),authority_request(client,3,4,archive)); // Fresh nonce, same reliable seq.
    CHECK(wait_until([&]{return f.owner.diagnostics().authenticated_datagrams==5;}));
    CHECK(authority_messages(peer,client,200ms).empty()); CHECK(f.validations.load()==1);
    CHECK(f.owner.diagnostics().login_response_validated); CHECK(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP rereads clock after Login callback and fails closed on regression") {
    LoginAuthorityFixture f([](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::RawString&) { fixture.udp_now=99; return true; });
    const auto login=f.login(); authority_select(f,login);
    Peer peer; session::Context client(session::Role::client,public_id,public_key); authority_hello(f,peer,client);
    peer.send(f.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&]{return !f.owner.running();})); CHECK(f.validations.load()==1);
    CHECK(f.owner.diagnostics().closed); CHECK_FALSE(f.owner.diagnostics().login_response_validated);
    CHECK(authority_messages(peer,client,200ms).empty());
}

namespace {
game::WorldPublication public_world() {
    // Entirely synthetic caller-owned metadata, NOT an original playable map.
    using control::posthello::StringForm;
    return {{{"FRoot",{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},{0,3}}},
        {{StringForm::narrow,std::u16string{u'M',u'a',u'p',0}},
         {StringForm::narrow,std::u16string{u'G',u'a',u'm',u'e',0}}}};
}
Bytes public_package_archive() {
    // Independently written fields: opcode7, GUID fourLEwords, FRoot+NUL,
    // name1 oneNUL, text2 zero, value0=0, generationCOUNT=2 (not netcount3),
    // name3 oneNUL, flags0. No encoder supplies this expectation.
    return hex("07040302017856341200000080ffffffff0600000046526f6f7400"
        "0100000000000000000000000002000000010000000000");
}
Bytes public_welcome_archive() { return hex("01040000004d6170000500000047616d6500"); }
struct PublicationPackets {
    std::vector<Bytes> logical;
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint16_t> packet_ids;
};
PublicationPackets publication_packets(Peer& peer,session::Context& client,
                                      std::size_t wanted=2) {
    PublicationPackets result;
    const auto end=std::chrono::steady_clock::now()+500ms;
    while (std::chrono::steady_clock::now()<end) {
        const auto datagram=peer.receive(20ms);
        if (!datagram) continue;
        const auto packet=wire::decode_packet(client.decode(*datagram),256);
        for (const auto& record:packet.records) if (!record.is_ack) {
            result.logical.push_back(record.bunch.payload);
            result.sequences.push_back(record.bunch.sequence);
            result.packet_ids.push_back(packet.packet_id);
            CHECK(record.bunch.reliable); CHECK(record.bunch.channel==0);
            CHECK(record.bunch.channel_type==1);
        }
        if (result.logical.size()>=wanted) break;
    }
    return result;
}
void publication_login(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
                       Peer& peer,session::Context& client) {
    authority_select(fixture,login); authority_hello(fixture,peer,client);
    peer.send(fixture.owner.port(),authority_request(client,3,4,
        authority_login(login.result.at("UserId").get<std::string>())));
}
}

TEST_CASE("UDP publication requires an identity validator before worker start") {
    udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
    error([&]{owner.start([](const session::SessionId&){return true;},{},
        [](const session::SessionId&,const control::posthello::Login&) {
            return std::optional<game::WorldPublication>{public_world()};
        });},udp::LocalUdpErrorCode::invalid_configuration);
    CHECK_FALSE(owner.running());
}
TEST_CASE("UDP validated transient Login publishes Package Welcome and accepts ACK8 Join on same context") {
    std::atomic<bool> full_candidate{false};
    LoginAuthorityFixture f({},true,[&](LoginAuthorityFixture& fixture,const session::SessionId& id,
        const control::posthello::Login& candidate) {
        full_candidate=id==public_id && candidate.url==control::posthello::RawString{
            control::posthello::StringForm::zero,{}} &&
            candidate.unique_id==std::array<std::uint32_t,4>{0x01020304U,0x12345678U,0x80000000U,0xffffffffU} &&
            fixture.service->validate_login_response(id,candidate.response);
        return std::optional<game::WorldPublication>{public_world()};
    });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    CHECK_FALSE(f.owner.diagnostics().world_published); CHECK_FALSE(f.owner.diagnostics().join_requested);
    publication_login(f,login,peer,client); const auto previous=client.last_received();
    const auto received=publication_packets(peer,client);
    CHECK(received.logical==std::vector<Bytes>{public_package_archive(),public_welcome_archive()});
    CHECK(received.sequences==std::vector<std::uint32_t>{4,5});
    // Owner's idle tick replays each preceding ACK once: IDs2,5,8.
    // Login ACK9 precedes Package10/Welcome11; reliable sequences remain4/5.
    if (received.packet_ids.size()==2) {
        CAPTURE(received.packet_ids[0]); CAPTURE(received.packet_ids[1]);
        CHECK(received.packet_ids==std::vector<std::uint16_t>{10,11});
    } else CHECK(received.packet_ids.size()==2);
    CHECK(client.last_received()>previous); CHECK(full_candidate.load()); CHECK(f.publications.load()==1);
    CHECK(wait_until([&]{return f.owner.diagnostics().world_published;}));
    CHECK(f.owner.diagnostics().login_response_validated); CHECK_FALSE(f.owner.diagnostics().closed);
    // ACK8 acknowledges GUID/generationCOUNT2, followed by fieldless Join9 in
    // ONE logical bunch. It is not a packet ACK or world-spawn instruction.
    peer.send(f.owner.port(),authority_request(client,4,5,
        hex("08040302017856341200000080ffffffff0200000009")));
    CHECK(wait_until([&]{return f.owner.diagnostics().join_requested;}));
    CHECK(f.service->authorize_udp(public_id)); CHECK_FALSE(f.owner.diagnostics().closed);
    CHECK(publication_packets(peer,client,1).logical.empty()); // ACK only, no actor.
    CHECK(f.validations.load()==1); CHECK(f.publications.load()==1);
    f.owner.stop(); CHECK(f.owner.diagnostics().world_published); CHECK(f.owner.diagnostics().join_requested);
}
TEST_CASE("UDP absent publication provider preserves identity-only continuation") {
    LoginAuthorityFixture f; const auto login=f.login(); Peer peer;
    session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(wait_until([&]{return f.owner.diagnostics().login_response_validated;}));
    CHECK(publication_packets(peer,client,1).logical.empty());
    CHECK_FALSE(f.owner.diagnostics().world_published); CHECK_FALSE(f.owner.diagnostics().join_requested);
    CHECK(f.publications.load()==0); CHECK(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP failed Response validation never invokes publication provider") {
    LoginAuthorityFixture f([](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::RawString&){return false;},true,
        [](LoginAuthorityFixture&,const session::SessionId&,const control::posthello::Login&) {
            return std::optional<game::WorldPublication>{public_world()};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(publication_packets(peer,client,1).logical==std::vector<Bytes>{
        hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(f.publications.load()==0); CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().world_published);
}
TEST_CASE("UDP publication nullopt denies with fixed Failure and retires existing session") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::Login&) { return std::optional<game::WorldPublication>{}; });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(publication_packets(peer,client,1).logical==std::vector<Bytes>{
        hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(f.publications.load()==1); CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.service->authorize_udp(public_id)); CHECK_FALSE(f.owner.diagnostics().world_published);
}
TEST_CASE("UDP publication exception is fixed Failure without reflecting private provider text") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::Login&)->std::optional<game::WorldPublication> {
            throw std::runtime_error("private-world-provider");
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(publication_packets(peer,client,1).logical==std::vector<Bytes>{
        hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(f.publications.load()==1); CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().world_published);
}
TEST_CASE("UDP fresh authorization discards world plan after provider revocation") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::Login&) {
        fixture.verifier.active=false;
        return std::optional<game::WorldPublication>{public_world()};
    });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(f.publications.load()==1); CHECK(publication_packets(peer,client,1).logical.empty());
    CHECK_FALSE(f.owner.diagnostics().world_published); CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP provider may synchronously destroy lease outside pin without publication") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::Login&) {
        fixture.service.reset();
        return std::optional<game::WorldPublication>{public_world()};
    });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;})); CHECK(f.publications.load()==1);
    CHECK(publication_packets(peer,client,1).logical.empty()); CHECK(f.owner.running());
    CHECK_FALSE(f.owner.diagnostics().world_published);
}
TEST_CASE("UDP concurrent retirement discards late world decision with exact-entry recheck") {
    Gate gate;
    LoginAuthorityFixture f({},true,[&](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::Login&) {
        gate.enter(); return std::optional<game::WorldPublication>{public_world()};
    });
    GateRelease release{gate};
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client); CHECK(gate.wait_entered());
    auto retired=std::async(std::launch::async,[&] {
        f.verifier.active=false; return f.service->authorize_udp(public_id);
    });
    const bool before_release=retired.wait_for(2000ms)==std::future_status::ready;
    gate.release(); CHECK(before_release);
    const bool finished=retired.wait_for(2000ms)==std::future_status::ready; CHECK(finished);
    if (finished) CHECK_FALSE(retired.get());
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.owner.diagnostics().world_published);
    CHECK(publication_packets(peer,client,1).logical.empty());
}
TEST_CASE("UDP invalid supplied world plan retires without partial Package Welcome publication") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture&,const session::SessionId&,
        const control::posthello::Login&) {
        auto plan=public_world(); plan.packages[0].identity={};
        return std::optional<game::WorldPublication>{std::move(plan)};
    });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;})); CHECK(f.publications.load()==1);
    CHECK(publication_packets(peer,client,1).logical.empty());
    CHECK_FALSE(f.owner.diagnostics().world_published); CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP rereads clock after publication provider and closes on regression") {
    LoginAuthorityFixture f({},true,[](LoginAuthorityFixture& fixture,const session::SessionId&,
        const control::posthello::Login&) {
        fixture.udp_now=99; return std::optional<game::WorldPublication>{public_world()};
    });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    publication_login(f,login,peer,client);
    CHECK(wait_until([&]{return !f.owner.running();})); CHECK(f.publications.load()==1);
    CHECK(f.owner.diagnostics().closed); CHECK_FALSE(f.owner.diagnostics().world_published);
    CHECK(publication_packets(peer,client,1).logical.empty());
}


namespace {
udp::InitialControllerPlan public_controller_plan() {
    // Synthetic package netcount3, local2; explicit actor seeds, no native identity.
    return {{7,41,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        2,{0,0,0},std::nullopt,0};
}
LoginAuthorityFixture::PublicationDecision controller_publisher() {
    return [](LoginAuthorityFixture&,const session::SessionId&,
              const control::posthello::Login&) { return std::optional<game::WorldPublication>{public_world()}; };
}
void controller_publication(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
                            Peer& peer,session::Context& client) {
    publication_login(fixture,login,peer,client);
    const auto world=publication_packets(peer,client);
    REQUIRE(world.logical==std::vector<Bytes>{public_package_archive(),public_welcome_archive()});
    REQUIRE(wait_until([&]{return fixture.owner.diagnostics().world_published;}));
    CHECK_FALSE(fixture.owner.diagnostics().controller_sent);
}
Bytes controller_join(session::Context& client) {
    return authority_request(client,4,5,hex("08040302017856341200000080ffffffff0200000009"));
}
struct ControllerOutput {
    std::vector<wire::Bunch> bunches;
    std::vector<std::uint16_t> packet_ids;
};
ControllerOutput controller_output(Peer& peer,session::Context& client,
                                   std::chrono::milliseconds budget=2000ms) {
    ControllerOutput result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto datagram=peer.receive(20ms); if (!datagram) continue;
        const auto packet=wire::decode_packet(client.decode(*datagram),256);
        for (const auto& record:packet.records) if (!record.is_ack && record.bunch.channel!=0) {
            result.bunches.push_back(record.bunch); result.packet_ids.push_back(packet.packet_id);
        }
        if (!result.bunches.empty()) break;
    }
    return result;
}
}

TEST_CASE("UDP controller plan requires both callbacks and failed start remains retryable") {
    for (const bool validator_missing:{false,true}) {
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        udp::LocalUdpOwner::LoginResponseValidator validator;
        udp::LocalUdpOwner::PublicationProvider publisher;
        if (!validator_missing) validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        if (validator_missing) publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan());},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().controller_sent);
        if (owner.running()) { owner.stop(); continue; } // Safe cleanup of RED no-op plan.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP controller malformed plan is rejected before worker without consuming start") {
    for (unsigned invalid=0;invalid<6;++invalid) {
        auto plan=public_controller_plan();
        switch (invalid) {
        case 0: plan.channel.channel=0; break;
        case 1: plan.channel.channel=2048; break;
        case 2: plan.archetype_local_net_index=-1; break;
        case 3: plan.position[0]=1048576; break;
        case 4: plan.position[2]=-1048577; break;
        case 5: plan.rotation=wire::actor::Rotation{1,0,0}; break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        auto publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,plan);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().controller_sent);
        if (owner.running()) { owner.stop(); continue; }
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan()));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP Join emits exact initial controller once through same session and channel seeds") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); const auto previous=client.last_received();
    const auto join=controller_join(client); peer.send(f.owner.port(),join);
    const auto out=controller_output(peer,client); REQUIRE(out.bunches.size()==1);
    const auto& b=out.bunches.front(); CHECK(b.payload==hex("04000000400500")); CHECK(b.payload_bits==51);
    CHECK(b.channel==7); CHECK(b.channel_type==2); CHECK(b.reliable); CHECK(b.control); CHECK(b.open); CHECK_FALSE(b.close);
    CHECK(b.sequence==42); CHECK(client.last_received()>previous);
    REQUIRE(out.packet_ids.size()==1); CHECK(out.packet_ids.front()>11);
    CHECK(wait_until([&]{return f.owner.diagnostics().controller_sent;}));
    CHECK(f.owner.diagnostics().join_requested); CHECK_FALSE(f.owner.diagnostics().closed);
    CHECK(f.service->authorize_udp(public_id)); CHECK(f.validations.load()==1); CHECK(f.publications.load()==1);
    peer.send(f.owner.port(),join); // Encrypted replay: no new Context counters.
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    peer.send(f.owner.port(),controller_join(client)); // Fresh nonce, duplicate reliable Join.
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP controller plan copies caller values including optional nonzero rotation") {
    auto plan=public_controller_plan(); plan.position={8,-8,0};
    plan.rotation=wire::actor::Rotation{256,0,65280}; plan.net_player_index=2;
    LoginAuthorityFixture f({},true,controller_publisher(),plan);
    plan.channel.channel=8; plan.channel.outgoing_sequence=900;
    plan.archetype_package_guid={}; plan.archetype_local_net_index=3;
    plan.position={0,0,0}; plan.rotation=std::nullopt; plan.net_player_index=255;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    const auto out=controller_output(peer,client); REQUIRE(out.bunches.size()==1);
    // Independent32+20+19+8bit literal, not a call to the composer under test.
    CHECK(out.bunches.front().payload==hex("04000000032338c07f01"));
    CHECK(out.bunches.front().payload_bits==79); CHECK(out.bunches.front().channel==7);
    CHECK(out.bunches.front().sequence==42);
    CHECK(wait_until([&]{return f.owner.diagnostics().controller_sent;}));
}
TEST_CASE("UDP controller is never emitted before Join or after authorization denial") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client);
    CHECK(controller_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().controller_sent);
    f.verifier.active=false;
    peer.send(f.owner.port(),controller_join(client));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(controller_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().controller_sent);
    CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP unknown controller package or out of range local index retires without actor output") {
    for (const bool unknown_package:{false,true}) {
        auto plan=public_controller_plan();
        if (unknown_package) plan.archetype_package_guid={1,2,3,4};
        else plan.archetype_local_net_index=3; // FRoot owns locals0..2 only.
        LoginAuthorityFixture f({},true,controller_publisher(),plan);
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(controller_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().controller_sent);
        CHECK_FALSE(f.service->authorize_udp(public_id));
    }
}
TEST_CASE("UDP controller send quota failure retires after Join without claiming emitted payload") {
    auto cfg=authority_skeleton(); cfg.transport.limits.max_outgoing_bunches=5;
    // Three challenges + Package + Welcome fit exactly. Controller needs six;
    // no artificially oversized field or packet limit that prevents Login.
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),cfg);
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(controller_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().controller_sent);
    CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP stopped owner never claims initial controller and absent plan stays unchanged") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client);
    f.owner.stop(); CHECK_FALSE(f.owner.running()); CHECK(f.owner.diagnostics().closed);
    CHECK(controller_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().controller_sent);
    // Existing publication test proves absent plan has ACK-only Join. Snapshot
    // retains false controller observation through stop as well.
    LoginAuthorityFixture absent({},true,controller_publisher());
    const auto absent_login=absent.login(); Peer absent_peer;
    session::Context absent_client(session::Role::client,public_id,public_key);
    controller_publication(absent,absent_login,absent_peer,absent_client);
    absent_peer.send(absent.owner.port(),controller_join(absent_client));
    CHECK(wait_until([&]{return absent.owner.diagnostics().join_requested;}));
    CHECK(controller_output(absent_peer,absent_client,200ms).bunches.empty());
    CHECK_FALSE(absent.owner.diagnostics().controller_sent);
    absent.owner.stop(); CHECK_FALSE(absent.owner.diagnostics().controller_sent);
}
