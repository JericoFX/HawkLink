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
#include <ostream>
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
                else if (bytes.size()==16) {
                    for (std::size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<std::uint8_t>(i*0x11);
                }
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
    std::string handed_off_user_guid; // Synthetic server provision, never Login.UniqueId.
    udp::LocalUdpOwner owner{0,authority_skeleton(),
        [this]{return udp_now.load();},5ms};
    std::unique_ptr<assignment::SessionAssignmentService> service;
    explicit LoginAuthorityFixture(Decision action={},bool with_validator=true,
                                   PublicationDecision publisher={},
                                   std::optional<udp::InitialControllerPlan> controller=std::nullopt,
                                   std::optional<game::ConnectionConfig> config=std::nullopt,
                                   std::optional<udp::InitialPawnPlan> pawn=std::nullopt,
                                   std::optional<udp::InitialPriPlan> pri=std::nullopt,
                                   std::optional<udp::InitialActorLinksPlan> links=std::nullopt,
                                   std::optional<udp::StartMechDispatchPlan> start_mech=std::nullopt,
                                   udp::LocalUdpOwner::StartMechProvider mech_provider={},
                                   std::optional<udp::SpawnValidationDispatchPlan> spawn_validation=std::nullopt,
                                   udp::LocalUdpOwner::SpawnSelectionValidator selection_validator={},
                                   std::optional<udp::SpawnAdmissionDispatchPlan> spawn_admission=std::nullopt,
                                   std::optional<udp::PriIdentityDispatchPlan> pri_identity=std::nullopt,
                                   std::optional<udp::SelectionBootstrapPlan> selection_bootstrap=std::nullopt,
                                   std::optional<udp::ReadyMatchPlan> ready_match=std::nullopt,
                                   udp::LocalUdpOwner::ReadyFactsProvider ready_facts={},double clock_epoch=100.0,
                                   std::optional<udp::InventoryBootstrapPlan> inventory_bootstrap=std::nullopt)
        : monotonic(static_cast<std::int64_t>(clock_epoch)),udp_now(clock_epoch),
          decision(std::move(action)),publication_decision(std::move(publisher)),
          owner(0,config.value_or(authority_skeleton()),[this]{return udp_now.load();},5ms) {
        service=std::make_unique<assignment::SessionAssignmentService>(auth,destination(owner.port()),
            assignment::AssignmentPolicy{30,1200,800},[this]{return monotonic.load();},[]{return 1704067200LL;},
            [](std::span<std::uint8_t> bytes) {
                if (bytes.size()==public_id.size()) std::copy(public_id.begin(),public_id.end(),bytes.begin());
                else if (bytes.size()==public_key.size()) std::copy(public_key.begin(),public_key.end(),bytes.begin());
                else return false;
                return true;
            },[this](const assignment::Destination& d){return owner.destination_bound(d);},
            [this](const assignment::SessionProvision& provision){
                handed_off_user_guid=provision.user_guid;
                return owner.handoff(provision);
            });
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
            },std::move(publish),std::move(controller),std::move(pawn),std::move(pri),std::move(links),
                std::move(start_mech),std::move(mech_provider),
                std::move(spawn_validation),std::move(selection_validator),std::move(spawn_admission),
                std::move(pri_identity),std::move(selection_bootstrap),std::move(ready_match),std::move(ready_facts),
                std::move(inventory_bootstrap));
        else owner.start(std::move(authorize),{},std::move(publish),
                            std::move(controller),std::move(pawn),std::move(pri),std::move(links),
                std::move(start_mech),std::move(mech_provider),
                std::move(spawn_validation),std::move(selection_validator),std::move(spawn_admission),
                std::move(pri_identity),std::move(selection_bootstrap),std::move(ready_match),std::move(ready_facts),
                std::move(inventory_bootstrap));
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
                                   std::chrono::milliseconds budget=2000ms,std::size_t expected=1) {
    ControllerOutput result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto datagram=peer.receive(20ms); if (!datagram) continue;
        const auto packet=wire::decode_packet(client.decode(*datagram),256);
        for (const auto& record:packet.records) if (!record.is_ack && record.bunch.channel!=0) {
            result.bunches.push_back(record.bunch); result.packet_ids.push_back(packet.packet_id);
        }
        if (result.bunches.size()>=expected) break;
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
    CHECK_FALSE(f.owner.diagnostics().pawn_sent); // Absent plan preserves Controller-only output.
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

namespace {
udp::InitialPawnPlan public_pawn_plan() {
    // Synthetic FRoot/local2, separate channel and explicit reliable seed.
    return {{8,71,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        2,{0,0,0},std::nullopt};
}
}
TEST_CASE("UDP Pawn plan requires Controller validator and publication with retryable prestart failure") {
    for (unsigned missing=0;missing<3;++missing) {
        CAPTURE(missing);
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        udp::LocalUdpOwner::LoginResponseValidator validator;
        udp::LocalUdpOwner::PublicationProvider publisher;
        std::optional<udp::InitialControllerPlan> controller=public_controller_plan();
        if (missing!=0) validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        if (missing!=1) publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        if (missing==2) controller.reset();
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            controller,public_pawn_plan());},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().pawn_sent);
        if (owner.running()) { owner.stop(); continue; } // Cleanup if RED ignores Pawn prerequisites.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP Pawn malformed plan rejects before start without consuming lifecycle") {
    for (unsigned invalid=0;invalid<7;++invalid) {
        CAPTURE(invalid);
        auto pawn=public_pawn_plan();
        switch (invalid) {
        case 0: pawn.channel.channel=0; break;
        case 1: pawn.channel.channel=2048; break;
        case 2: pawn.channel.channel=7; break; // Controller owns this channel.
        case 3: pawn.archetype_local_net_index=-1; break;
        case 4: pawn.position[0]=1048576; break;
        case 5: pawn.position[2]=-1048577; break;
        case 6: pawn.rotation=wire::actor::Rotation{1,0,0}; break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        auto publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan(),pawn);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().pawn_sent);
        if (owner.running()) { owner.stop(); continue; }
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan(),public_pawn_plan()));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP Join emits Controller then exact Pawn once on the existing Context") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},public_pawn_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client);
    CHECK_FALSE(f.owner.diagnostics().pawn_sent);
    const auto previous=client.last_received(); const auto join=controller_join(client);
    peer.send(f.owner.port(),join);
    const auto out=controller_output(peer,client,2000ms,2); REQUIRE(out.bunches.size()==2);
    CHECK(out.bunches[0].channel==7); CHECK(out.bunches[0].sequence==42);
    CHECK(out.bunches[0].payload==hex("04000000400500")); CHECK(out.bunches[0].payload_bits==51);
    const auto& pawn=out.bunches[1];
    // Independent32bit reference +11bit zero vector; no NetPlayerIndex byte.
    CHECK(pawn.payload==hex("040000004005")); CHECK(pawn.payload_bits==43);
    CHECK(pawn.channel==8); CHECK(pawn.sequence==72); CHECK(pawn.channel_type==2);
    CHECK(pawn.reliable); CHECK(pawn.control); CHECK(pawn.open); CHECK_FALSE(pawn.close);
    REQUIRE(out.packet_ids.size()==2); CHECK(out.packet_ids[0]>11);
    CHECK(out.packet_ids[1]>out.packet_ids[0]); CHECK(client.last_received()>previous);
    CHECK(wait_until([&]{return f.owner.diagnostics().pawn_sent;}));
    CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().closed);
    CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),join);
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    peer.send(f.owner.port(),controller_join(client)); // Fresh nonce, duplicate reliable Join.
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK(f.validations.load()==1); CHECK(f.publications.load()==1);
    CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP Pawn plan is copied including nonzero optional rotation") {
    auto pawn=public_pawn_plan(); pawn.position={8,-8,0};
    pawn.rotation=wire::actor::Rotation{256,0,65280};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},pawn);
    pawn.channel.channel=9; pawn.channel.outgoing_sequence=900;
    pawn.archetype_package_guid={}; pawn.archetype_local_net_index=3;
    pawn.position={0,0,0}; pawn.rotation.reset();
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    const auto out=controller_output(peer,client,2000ms,2); REQUIRE(out.bunches.size()==2);
    CHECK(out.bunches[1].payload==hex("04000000032338c07f"));
    CHECK(out.bunches[1].payload_bits==71); CHECK(out.bunches[1].channel==8);
    CHECK(out.bunches[1].sequence==72);
    CHECK(wait_until([&]{return f.owner.diagnostics().pawn_sent;}));
}
TEST_CASE("UDP Pawn never emits before Join after denial or after stop") {
    for (const bool stopped:{false,true}) {
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},public_pawn_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        controller_publication(f,login,peer,client);
        CHECK(controller_output(peer,client,200ms).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().pawn_sent);
        if (stopped) {
            f.owner.stop(); // No datagram to a closed port: Windows reports an ICMP reset.
        } else {
            f.verifier.active=false;
            peer.send(f.owner.port(),controller_join(client));
        }
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(controller_output(peer,client,200ms).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pawn_sent);
    }
}
TEST_CASE("UDP unknown Pawn package or unavailable local retires after successful Controller") {
    for (const bool unknown_package:{false,true}) {
        auto pawn=public_pawn_plan();
        if (unknown_package) pawn.archetype_package_guid={1,2,3,4};
        else pawn.archetype_local_net_index=3; // Synthetic FRoot owns only0..2.
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},pawn);
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
        const auto out=controller_output(peer,client,200ms); REQUIRE(out.bunches.size()==1);
        CHECK(out.bunches[0].channel==7);
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pawn_sent);
        CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(controller_output(peer,client,200ms).bunches.empty());
    }
}
TEST_CASE("UDP Pawn queue exhaustion retires without clearing already emitted Controller") {
    auto cfg=authority_skeleton(); cfg.transport.limits.max_outgoing_bunches=6;
    // Three challenges + Package + Welcome + Controller fit. Pawn needs seven.
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),cfg,public_pawn_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    const auto out=controller_output(peer,client,200ms); REQUIRE(out.bunches.size()==1);
    CHECK(out.bunches[0].channel==7);
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pawn_sent);
    CHECK_FALSE(f.service->authorize_udp(public_id));
    CHECK(controller_output(peer,client,200ms).bunches.empty());
}
TEST_CASE("UDP failed Controller emission cannot send a Pawn") {
    auto controller=public_controller_plan(); controller.archetype_package_guid={1,2,3,4};
    LoginAuthorityFixture f({},true,controller_publisher(),controller,{},public_pawn_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pawn_sent);
}

// Original DELETE/void framing; retirement/status behavior is new local policy.
TEST_CASE("UDP real advertisement DELETE retires accepted transport while listener survives") {
    AuthFixture f; REQUIRE(f.owner.port()>0); f.construct_assignment();
    const auto login=f.login(); REQUIRE(login.status==200);
    const auto header=authorization(login);
    const auto guid=login.result.at("UserId").get<std::string>();
    const nlohmann::json body{{"OwnerGuid",guid},{"Region","EU"},
        {"GameVersion","1.6.0.154767"},{"Users",nlohmann::json::array({guid})},
        {"RequestedServerGuid",server_guid}};
    const auto posted=f.service->handle("POST","/hawkenClientMatchmakingAdvertisements",header,body.dump());
    REQUIRE(posted.status==200);
    REQUIRE(posted.result==nlohmann::json("00112233-4455-6677-8899-aabbccddeeff"));
    REQUIRE(f.service->handle("GET","/session",header).status==200);
    Peer peer; session::Context client(session::Role::client,public_id,public_key);
    CHECK(request(client,0,1,hex("1a07000000"))==initial_oracle());
    peer.send(f.owner.port(),initial_oracle());
    const auto ack=peer.receive(); REQUIRE(ack); CHECK(*ack==ack_oracle());
    const auto challenge=peer.receive(); REQUIRE(challenge);
    CHECK(client.decode(*ack)==hex("00400020"));
    CHECK(client.decode(*challenge)==hex("0100011040506c0000000004"));
    REQUIRE(f.owner.diagnostics().authenticated_datagrams==1);
    peer.drain();
    const auto cancelled=f.service->handle("DELETE",
        "/hawkenClientMatchmakingAdvertisements/00112233-4455-6677-8899-aabbccddeeff",header);
    REQUIRE(cancelled.status==200); CHECK(cancelled.result.is_null());
    CHECK_FALSE(f.service->authorize_udp(public_id));
    CHECK(f.service->handle("GET","/session",header).status==404);
    peer.drain(); // Clear datagrams emitted before the synchronous retirement ACK.
    peer.send(f.owner.port(),request(client,1,2,hex("1cc3363f8d"),false));
    CHECK_FALSE(peer.receive(200ms));
    CHECK(f.owner.running()); CHECK(f.owner.diagnostics().authenticated_datagrams==1);
}

namespace {
udp::InitialPriPlan public_pri_plan() {
    // Synthetic non-Controller prefix; no native CDO/rotation inference.
    return {{9,101,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        2,{0,0,0},std::nullopt};
}
}
TEST_CASE("UDP PRI requires Controller validator and publication before retryable start") {
    for (unsigned missing=0;missing<3;++missing) {
        CAPTURE(missing);
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        udp::LocalUdpOwner::LoginResponseValidator validator;
        udp::LocalUdpOwner::PublicationProvider publisher;
        std::optional<udp::InitialControllerPlan> controller=public_controller_plan();
        if (missing!=0) validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        if (missing!=1) publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        if (missing==2) controller.reset();
        error([&]{owner.start([](const session::SessionId&){return true;},validator,
            publisher,controller,std::nullopt,public_pri_plan());},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().pri_sent);
        if (owner.running()) { owner.stop(); continue; } // RED cleanup if PRI is ignored.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP PRI invalid transform index or channel collision leaves start retryable") {
    for (unsigned invalid=0;invalid<8;++invalid) {
        CAPTURE(invalid);
        auto pri=public_pri_plan();
        switch (invalid) {
        case 0: pri.channel.channel=0; break;
        case 1: pri.channel.channel=2048; break;
        case 2: pri.channel.channel=7; break;
        case 3: pri.channel.channel=8; break;
        case 4: pri.archetype_local_net_index=-1; break;
        case 5: pri.position[0]=1048576; break;
        case 6: pri.position[2]=-1048577; break;
        case 7: pri.rotation=wire::actor::Rotation{1,0,0}; break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        auto publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        error([&]{owner.start([](const session::SessionId&){return true;},validator,
            publisher,public_controller_plan(),public_pawn_plan(),pri);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); CHECK_FALSE(owner.diagnostics().pri_sent);
        if (owner.running()) { owner.stop(); continue; }
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;},validator,
            publisher,public_controller_plan(),public_pawn_plan(),public_pri_plan()));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP PRI follows Controller before Pawn with exact bits and no repeated emission") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client);
    CHECK_FALSE(f.owner.diagnostics().pri_sent);
    const auto previous=client.last_received(); const auto join=controller_join(client);
    peer.send(f.owner.port(),join);
    const auto out=controller_output(peer,client,2000ms,3); REQUIRE(out.bunches.size()==3);
    CHECK(out.bunches[0].channel==7); CHECK(out.bunches[0].sequence==42);
    CHECK(out.bunches[0].payload==hex("04000000400500")); CHECK(out.bunches[0].payload_bits==51);
    const auto& pri=out.bunches[1];
    // Independent32bit primary reference +11bit zero vector, no Controller byte.
    CHECK(pri.payload==hex("040000004005")); CHECK(pri.payload_bits==43);
    CHECK(pri.channel==9); CHECK(pri.sequence==102); CHECK(pri.channel_type==2);
    CHECK(pri.reliable); CHECK(pri.control); CHECK(pri.open); CHECK_FALSE(pri.close);
    CHECK(out.bunches[2].channel==8); CHECK(out.bunches[2].sequence==72);
    CHECK(out.bunches[2].payload==hex("040000004005")); CHECK(out.bunches[2].payload_bits==43);
    REQUIRE(out.packet_ids.size()==3); CHECK(out.packet_ids[0]>11);
    CHECK(out.packet_ids[1]>out.packet_ids[0]); CHECK(out.packet_ids[2]>out.packet_ids[1]);
    CHECK(client.last_received()>previous);
    CHECK(wait_until([&]{return f.owner.diagnostics().pri_sent && f.owner.diagnostics().pawn_sent;}));
    CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().closed);
    CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),join); CHECK(controller_output(peer,client,200ms).bunches.empty());
    peer.send(f.owner.port(),controller_join(client));
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK(f.validations.load()==1); CHECK(f.publications.load()==1);
    CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP PRI copied plan preserves optional rotation and absent plan preserves two actors") {
    auto pri=public_pri_plan(); pri.position={8,-8,0};
    pri.rotation=wire::actor::Rotation{256,0,65280};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),pri);
    pri.channel.channel=10; pri.channel.outgoing_sequence=900;
    pri.archetype_package_guid={}; pri.archetype_local_net_index=3;
    pri.position={0,0,0}; pri.rotation.reset();
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    const auto out=controller_output(peer,client,2000ms,3); REQUIRE(out.bunches.size()==3);
    CHECK(out.bunches[1].channel==9); CHECK(out.bunches[1].sequence==102);
    CHECK(out.bunches[1].payload==hex("04000000032338c07f")); CHECK(out.bunches[1].payload_bits==71);
    CHECK(wait_until([&]{return f.owner.diagnostics().pri_sent;}));
    LoginAuthorityFixture absent({},true,controller_publisher(),public_controller_plan(),{},public_pawn_plan());
    const auto absent_login=absent.login(); Peer absent_peer;
    session::Context absent_client(session::Role::client,public_id,public_key);
    controller_publication(absent,absent_login,absent_peer,absent_client);
    absent_peer.send(absent.owner.port(),controller_join(absent_client));
    const auto old=controller_output(absent_peer,absent_client,2000ms,2); REQUIRE(old.bunches.size()==2);
    CHECK(old.bunches[0].channel==7); CHECK(old.bunches[1].channel==8);
    CHECK_FALSE(absent.owner.diagnostics().pri_sent); CHECK_FALSE(absent.owner.diagnostics().closed);
}
TEST_CASE("UDP PRI never emits before Join after denial or after stop") {
    for (const bool stopped:{false,true}) {
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        controller_publication(f,login,peer,client);
        CHECK(controller_output(peer,client,200ms).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().pri_sent);
        if (stopped) f.owner.stop(); // Never send to a closed Windows UDP socket.
        else { f.verifier.active=false; peer.send(f.owner.port(),controller_join(client)); }
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(controller_output(peer,client,200ms).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pri_sent);
        CHECK_FALSE(f.owner.diagnostics().pawn_sent);
    }
}
TEST_CASE("UDP PRI resolution failure retires after Controller without sending Pawn") {
    for (const bool unknown_package:{false,true}) {
        auto pri=public_pri_plan();
        if (unknown_package) pri.archetype_package_guid={1,2,3,4};
        else pri.archetype_local_net_index=3;
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),pri);
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
        const auto out=controller_output(peer,client,200ms,3); REQUIRE(out.bunches.size()==1);
        CHECK(out.bunches[0].channel==7); CHECK(out.bunches[0].payload==hex("04000000400500"));
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pri_sent);
        CHECK_FALSE(f.owner.diagnostics().pawn_sent); CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(controller_output(peer,client,200ms).bunches.empty());
    }
}
TEST_CASE("UDP failed Controller emission cannot send PRI or Pawn") {
    auto controller=public_controller_plan(); controller.archetype_package_guid={1,2,3,4};
    LoginAuthorityFixture f({},true,controller_publisher(),controller,{},public_pawn_plan(),public_pri_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client); peer.send(f.owner.port(),controller_join(client));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().controller_sent); CHECK_FALSE(f.owner.diagnostics().pri_sent);
    CHECK_FALSE(f.owner.diagnostics().pawn_sent);
}


namespace {
udp::InitialActorLinksPlan public_links_plan() {
    // Explicit SYNTHETIC effective field IDs/bounds, not native defaults.
    return {3,5,8,1,6,8};
}
Bytes links_ack(session::Context& client,std::uint16_t packet_id,
                const std::vector<std::uint16_t>& ids) {
    wire::Packet packet; packet.packet_id=packet_id;
    for (const auto id:ids) {
        wire::PacketRecord record; record.is_ack=true; record.ack_packet_id=id;
        packet.records.push_back(record);
    }
    return client.encode(wire::encode_packet(packet,256));
}
std::vector<std::uint16_t> links_opening_acks(const ControllerOutput& out) {
    std::vector<std::uint16_t> ids{1,3,5,7,8};
    ids.insert(ids.end(),out.packet_ids.begin(),out.packet_ids.end());
    return ids;
}
ControllerOutput links_openings(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
                               Peer& peer,session::Context& client) {
    controller_publication(fixture,login,peer,client);
    peer.send(fixture.owner.port(),controller_join(client));
    auto result=controller_output(peer,client,2000ms,3);
    REQUIRE(result.bunches.size()==3); REQUIRE(result.packet_ids.size()==3);
    REQUIRE(result.bunches[0].channel==7); REQUIRE(result.bunches[1].channel==9);
    REQUIRE(result.bunches[2].channel==8);
    REQUIRE(wait_until([&]{return fixture.owner.diagnostics().pawn_sent;}));
    return result;
}
ControllerOutput links_output(Peer& peer,session::Context& client,
                             std::chrono::milliseconds budget=200ms,std::size_t expected=2,
                             std::string* diagnostic_trace=nullptr,
                             std::vector<std::uint16_t>* received_open_ids=nullptr,
                             std::vector<std::uint16_t>* received_packet_ids=nullptr) {
    ControllerOutput result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto datagram=peer.receive(20ms); if (!datagram) continue;
        const auto packet=wire::decode_packet(client.decode(*datagram),256);
        if (received_packet_ids) received_packet_ids->push_back(packet.packet_id);
        for (const auto& record:packet.records) {
            if (received_open_ids && !record.is_ack && record.bunch.channel!=0 && record.bunch.open)
                received_open_ids->push_back(packet.packet_id);
            if (diagnostic_trace && !record.is_ack && record.bunch.channel!=0)
                *diagnostic_trace+=" pid="+std::to_string(packet.packet_id)+
                    "/channel="+std::to_string(record.bunch.channel)+
                    "/seq="+std::to_string(record.bunch.sequence)+
                    "/open="+std::to_string(record.bunch.open);
            if (!record.is_ack && record.bunch.channel!=0 && !record.bunch.open) {
                result.bunches.push_back(record.bunch); result.packet_ids.push_back(packet.packet_id);
            }
        }
        if (result.bunches.size()>=expected) break;
    }
    return result;
}
void links_literals(const ControllerOutput& out) {
    REQUIRE(out.bunches.size()==2);
    // Independent LSB fields: (3/8,channel9),(5/8,channel8) and
    // (1/8,channel7),(6/8,channel9). Two15-bit fields, no alignment/terminator.
    CHECK(out.bunches[0].channel==7); CHECK(out.bunches[0].sequence==43);
    CHECK(out.bunches[0].payload==hex("9b804600")); CHECK(out.bunches[0].payload_bits==30);
    CHECK(out.bunches[1].channel==8); CHECK(out.bunches[1].sequence==73);
    CHECK(out.bunches[1].payload==hex("79004f00")); CHECK(out.bunches[1].payload_bits==30);
    for (const auto& bunch:out.bunches) {
        CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
        CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    }
    REQUIRE(out.packet_ids.size()==2); CHECK(out.packet_ids[1]>out.packet_ids[0]);
}
}
TEST_CASE("UDP actor links wait for all three exact opening ACKs and send two literal batches once") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    CHECK(links_output(peer,client).bunches.empty());
    // Acknowledge Controller and Pawn, leaving PRI's exact opening pending.
    auto partial=links_opening_acks(openings); partial.erase(partial.begin()+6);
    peer.send(f.owner.port(),links_ack(client,5,partial));
    CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().closed);
    const auto previous=client.last_received();
    peer.send(f.owner.port(),links_ack(client,6,{openings.packet_ids[1]}));
    const auto out=links_output(peer,client,2000ms); links_literals(out);
    CHECK(client.last_received()>previous); CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),links_ack(client,7,links_opening_acks(openings)));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK(links_output(peer,client).bunches.empty()); // Worker ticks do not repeat batches.
    CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP actor links ACK for a control-only packet does not confirm actor openings") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    // Public fixture11 precedes all three actor openings and carries control ACKs.
    REQUIRE(openings.packet_ids.front()>11);
    peer.send(f.owner.port(),links_ack(client,5,{1,3,5,7,8,11}));
    CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().closed);
    peer.send(f.owner.port(),links_ack(client,6,openings.packet_ids));
    links_literals(links_output(peer,client,2000ms));
}
TEST_CASE("UDP actor links copied IDs survive caller mutation and confirmed seeds still wait for openings ACK") {
    auto controller=public_controller_plan(); auto pri=public_pri_plan(); auto pawn=public_pawn_plan();
    controller.channel.open_confirmed=true; pri.channel.open_confirmed=true; pawn.channel.open_confirmed=true;
    auto plan=public_links_plan();
    LoginAuthorityFixture f({},true,controller_publisher(),controller,{},pawn,pri,plan);
    plan={0,0,0,0,0,0}; controller.channel.channel=20; pri.channel.channel=21; pawn.channel.channel=22;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    CHECK(links_output(peer,client).bunches.empty());
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
}
TEST_CASE("UDP actor links absent plan preserves three initial actors without property batches") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP actor links require all plans and both callbacks before retryable start") {
    for (unsigned missing=0;missing<5;++missing) {
        CAPTURE(missing);
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        udp::LocalUdpOwner::LoginResponseValidator validator;
        udp::LocalUdpOwner::PublicationProvider publisher;
        if (missing!=0) validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        if (missing!=1) publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        std::optional<udp::InitialControllerPlan> controller=public_controller_plan();
        std::optional<udp::InitialPriPlan> pri=public_pri_plan();
        std::optional<udp::InitialPawnPlan> pawn=public_pawn_plan();
        if (missing==2) controller.reset(); if (missing==3) pri.reset(); if (missing==4) pawn.reset();
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            controller,pawn,pri,public_links_plan());},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; } // RED ignored-plan cleanup.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP actor links invalid field IDs bounds or either batch budget leave start retryable") {
    for (unsigned invalid=0;invalid<8;++invalid) {
        CAPTURE(invalid);
        auto plan=public_links_plan(); auto config=authority_skeleton();
        switch (invalid) {
        case 0: plan.controller_exclusive_bound=0; break;
        case 1: plan.pawn_exclusive_bound=0; break;
        case 2: plan.controller_pawn_field_id=plan.controller_pri_field_id; break;
        case 3: plan.pawn_pri_field_id=plan.pawn_controller_field_id; break;
        case 4: plan.controller_pri_field_id=8; break;
        case 5: plan.pawn_pri_field_id=8; break;
        case 6: config.transport.max_packet_bytes=8; plan.controller_exclusive_bound=0xffffffffU;
            plan.controller_pri_field_id=0x80000000U; plan.controller_pawn_field_id=0x80000001U; break;
        case 7: config.transport.max_packet_bytes=8; plan.pawn_exclusive_bound=0xffffffffU;
            plan.pawn_controller_field_id=0x80000000U; plan.pawn_pri_field_id=0x80000001U; break;
        }
        udp::LocalUdpOwner owner(0,config,[]{return 100.0;},5ms);
        auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        auto publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),plan);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; }
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));
        CHECK(owner.running()); owner.stop();
    }
}
TEST_CASE("UDP actor links do not send after actor close revocation control failure or stopping") {
    for (unsigned terminal=0;terminal<4;++terminal) {
        CAPTURE(terminal);
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        const auto openings=links_openings(f,login,peer,client);
        if (terminal==3) f.owner.stop();
        else {
            auto ids=links_opening_acks(openings);
            wire::Packet packet; packet.packet_id=5;
            for (const auto id:ids) { wire::PacketRecord ack; ack.is_ack=true; ack.ack_packet_id=id; packet.records.push_back(ack); }
            if (terminal==0) {
                wire::PacketRecord closing; auto& bunch=closing.bunch;
                bunch.channel=8; bunch.channel_type=2; bunch.reliable=true;
                bunch.control=true; bunch.close=true; bunch.sequence=1;
                packet.records.push_back(closing);
            } else if (terminal==1) f.verifier.active=false;
            else {
                wire::PacketRecord failure;
                failure.bunch.channel=0; failure.bunch.channel_type=1; failure.bunch.reliable=true;
                // Public control Failure6, next channel0 reliable sequence after Join.
                failure.bunch.sequence=6; failure.bunch.payload=hex("0600000000");
                failure.bunch.payload_bits=40; packet.records.push_back(failure);
            }
            peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
        }
        CHECK(links_output(peer,client).bunches.empty());
        if (terminal!=0) CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    }
}

TEST_CASE("UDP actor links inbound openings are not outgoing ACKs and original ACKs still release links") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    wire::Packet packet; packet.packet_id=5;
    for (const auto channel:{7,9,8}) {
        wire::PacketRecord record; auto& bunch=record.bunch;
        bunch.channel=static_cast<std::uint16_t>(channel); bunch.channel_type=2;
        bunch.reliable=true; bunch.control=true; bunch.open=true; bunch.sequence=1;
        bunch.payload={0}; bunch.payload_bits=8; packet.records.push_back(record);
    }
    peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
    CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().closed);
    peer.send(f.owner.port(),links_ack(client,6,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
}

namespace {
udp::StartMechDispatchPlan public_start_mech_plan() {
    // Synthetic effective class fields; no native ID/runtime eligibility claim.
    return {4,8,5,8,{true,true,true,{true,true,true},true,true,true},41};
}
wire::mech::SMechSetupStruct public_mech_result(std::uint8_t previous) {
    wire::mech::SMechSetupStruct result{};
    result.b_ready_for_setup=true;
    result.setup_id=static_cast<std::uint8_t>(previous+1);
    return result;
}
Bytes start_mech_packet(session::Context& client,std::uint16_t packet_id,
                       std::uint16_t sequence=1,std::uint16_t channel=7,
                       std::uint32_t field=4,bool suffix=false,bool malformed=false,
                       std::int32_t primary=256,std::int32_t holo=-1) {
    wire::BitWriter bits;
    bits.write_bounded(field,8);
    const auto u32=[&](std::uint32_t value) {
        const std::array<std::uint8_t,4> bytes{static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),
            static_cast<std::uint8_t>(value>>24)};
        bits.write_bits(bytes,32);
    };
    if (!malformed) {
        bits.write_bit(true); u32(37);
        constexpr std::string_view instance="20000000-0000-0000-0000-000000000001";
        for (const auto c:instance) bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(c)},8);
        bits.write_bits(std::array<std::uint8_t,1>{0},8);
        bits.write_bit(true); u32(static_cast<std::uint32_t>(primary));
        bits.write_bit(true); u32(static_cast<std::uint32_t>(holo));
        if (suffix) { bits.write_bounded(4,8); bits.write_bits(std::array<std::uint8_t,1>{0x5a},8); }
    }
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2;
    bunch.channel=channel; bunch.sequence=sequence;
    bunch.payload_bits=bits.bit_count(); bunch.payload=bits.payload_bytes();
    wire::Packet packet; packet.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
void start_mech_property_literal(const ControllerOutput& output) {
    REQUIRE(output.bunches.size()==1);
    const auto& bunch=output.bunches[0];
    CHECK(bunch.channel==9); CHECK(bunch.sequence==103); CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    CHECK_FALSE(bunch.control); CHECK(bunch.payload_bits==547);
    // Independent absolute offsets: id5 three bits, Ready526, Setup42 at527,
    // primary null selector535. All other synthetic scalar bits are zero.
    Bytes expected(69,0); expected[0]=5; expected[65]=0x40; expected[66]=0x95;
    CHECK(bunch.payload==expected);
}
}
TEST_CASE("UDP start mech waits for real actor ACKs and links then sends one literal PRI property") {
    std::atomic<int> calls{0};
    auto plan=public_start_mech_plan();
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),plan,
        [&](const session::SessionId& id,const wire::rpc::StartMechArguments& args,std::uint8_t previous) {
            CHECK(id==public_id); CHECK(args.mech_instance_id=="20000000-0000-0000-0000-000000000001");
            CHECK(args.primary_weapon==256); CHECK(args.holo_taunt_type==-1);
            CHECK(previous==41+calls.load());
            ++calls; return std::optional{public_mech_result(previous)};
        });
    plan.controller_rpc_field_id=7; plan.initial_setup_id=0; // copied immutable plan
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),start_mech_packet(client,5));
    CHECK(links_output(peer,client).bunches.empty()); CHECK(calls==0);
    peer.send(f.owner.port(),links_ack(client,6,{1,3,5,7,8,openings.packet_ids[0],openings.packet_ids[2]}));
    CHECK(links_output(peer,client).bunches.empty()); CHECK(calls==0);
    // Packet 1 is already ACKed control traffic, not an actor opening.
    peer.send(f.owner.port(),links_ack(client,7,{1}));
    CHECK(links_output(peer,client).bunches.empty()); CHECK(calls==0);
    peer.send(f.owner.port(),links_ack(client,8,{openings.packet_ids[1]}));
    auto output=links_output(peer,client,2000ms,3); REQUIRE(output.bunches.size()==3);
    CHECK(output.bunches[0].channel==7); CHECK(output.bunches[1].channel==8);
    ControllerOutput property; property.bunches.push_back(output.bunches[2]);
    start_mech_property_literal(property); CHECK(calls==1); CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),start_mech_packet(client,9)); // same reliable sequence, fresh nonce
    peer.send(f.owner.port(),links_ack(client,10,output.packet_ids));
    CHECK(links_output(peer,client).bunches.empty()); CHECK(calls==1);
    peer.send(f.owner.port(),start_mech_packet(client,11,2));
    const auto next=links_output(peer,client,2000ms,1); REQUIRE(next.bunches.size()==1);
    CHECK(next.bunches[0].sequence==104); CHECK(next.bunches[0].channel==9);
    Bytes next_expected(69,0); next_expected[0]=5; next_expected[65]=0xc0; next_expected[66]=0x95;
    CHECK(next.bunches[0].payload==next_expected); CHECK(calls==2);
}
TEST_CASE("UDP start mech does not fabricate requests or inspect other actor fields or RPC suffixes") {
    std::atomic<int> calls{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            ++calls; return std::optional{public_mech_result(previous)};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    const auto linked=links_output(peer,client,2000ms); links_literals(linked);
    CHECK(calls==0); CHECK(links_output(peer,client).bunches.empty());
    peer.send(f.owner.port(),start_mech_packet(client,6,1,8)); // registered Pawn, not Controller
    peer.send(f.owner.port(),start_mech_packet(client,7,1,7,6,true)); // unknown first ID; hidden suffix ignored
    CHECK(links_output(peer,client).bunches.empty()); CHECK(calls==0); CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),start_mech_packet(client,8,2,7,4,true)); // known first PREFIX only
    start_mech_property_literal(links_output(peer,client,2000ms,1)); CHECK(calls==1);
}
TEST_CASE("UDP start mech provider can retire its lease without a pin deadlock or stale PRI send") {
    std::atomic<int> calls{0}; LoginAuthorityFixture* active=nullptr;
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            ++calls; active->service.reset(); // synchronous lease retirement must complete outside Use
            return std::optional{public_mech_result(previous)};
        });
    active=&f;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
    peer.send(f.owner.port(),start_mech_packet(client,6));
    CHECK(wait_until([&]{return calls==1 && f.owner.diagnostics().closed;}));
    CHECK(links_output(peer,client).bunches.empty());
}
TEST_CASE("UDP start mech provider denial throw and authorization expiry retire without setup") {
    for (unsigned mode=0;mode<4;++mode) {
        CAPTURE(mode); std::atomic<int> calls{0}; LoginAuthorityFixture* active=nullptr;
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous)
                ->std::optional<wire::mech::SMechSetupStruct> {
                ++calls;
                if (mode==0) return std::nullopt;
                if (mode==1) throw std::runtime_error("synthetic provider fault");
                if (mode==2) active->verifier.active=false;
                else { active->monotonic=131; active->udp_now=131; }
                return public_mech_result(previous);
            });
        active=&f; const auto login=f.login(); Peer peer;
        session::Context client(session::Role::client,public_id,public_key);
        const auto openings=links_openings(f,login,peer,client);
        peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
        links_literals(links_output(peer,client,2000ms));
        peer.send(f.owner.port(),start_mech_packet(client,6));
        CHECK(wait_until([&]{return calls==1 && f.owner.diagnostics().closed;}));
        CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.service->authorize_udp(public_id));
    }
}
TEST_CASE("UDP start mech malformed known prefix and conflicting pending request fail closed") {
    for (const bool malformed:{false,true}) {
        std::atomic<int> calls{0};
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                ++calls; return std::optional{public_mech_result(previous)};
            });
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        (void)links_openings(f,login,peer,client);
        peer.send(f.owner.port(),start_mech_packet(client,5,1,7,4,false,malformed));
        if (!malformed) peer.send(f.owner.port(),start_mech_packet(client,6,2,7,4,false,false,256,7));
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;})); CHECK(calls==0);
        CHECK(links_output(peer,client).bunches.empty());
    }
}
TEST_CASE("UDP start mech schema activation fails before start commit and remains retryable") {
    for (unsigned bad=0;bad<16;++bad) {
        auto plan=public_start_mech_plan(); std::optional<udp::InitialActorLinksPlan> links=public_links_plan();
        auto provider=[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            return std::optional{public_mech_result(previous)};
        };
        udp::LocalUdpOwner::StartMechProvider callback=provider;
        switch(bad) {
        case 0: plan.eligibility.primary_package_map=false; break;
        case 1: plan.eligibility.controller_cache_correct=false; break;
        case 2: plan.eligibility.rpc_eligible=false; break;
        case 3: plan.eligibility.parameters_eligible[0]=false; break;
        case 4: plan.eligibility.parameters_eligible[1]=false; break;
        case 5: plan.eligibility.parameters_eligible[2]=false; break;
        case 6: plan.eligibility.pri_cache_correct=false; break;
        case 7: plan.eligibility.pri_property_eligible=false; break;
        case 8: plan.eligibility.all_members_eligible=false; break;
        case 9: plan.controller_exclusive_bound=0; break;
        case 10: plan.controller_rpc_field_id=8; break;
        case 11: plan.pri_mech_setup_field_id=8; break;
        case 12: callback={}; break;
        case 13: links.reset(); break;
        case 14: plan.pri_exclusive_bound=0; break;
        case 15: plan.controller_exclusive_bound=9; break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        auto publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional{public_world()};};
        CAPTURE(bad);
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),links,plan,callback);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (!owner.running()) {
            CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;},validator,publisher,
                public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),
                public_start_mech_plan(),provider));
            CHECK(owner.running());
        }
        owner.stop();
    }
}

TEST_CASE("UDP start mech shutdown joins its callback before releasing captured lifetime") {
    Gate gate; std::atomic<int> calls{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            ++calls; gate.enter(); return std::optional{public_mech_result(previous)};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
    peer.send(f.owner.port(),start_mech_packet(client,6));
    const bool entered=gate.wait_entered(); CHECK(entered);
    if (!entered) { gate.release(); return; }
    std::atomic<bool> stopped{false};
    std::thread shutdown([&]{f.owner.stop(); stopped=true;});
    CHECK(wait_until([&]{return !f.owner.running();})); CHECK_FALSE(stopped.load());
    gate.release(); shutdown.join();
    CHECK(stopped.load()); CHECK(calls==1); CHECK(f.owner.diagnostics().closed);
    CHECK(links_output(peer,client).bunches.empty());
}


namespace {
udp::SpawnValidationDispatchPlan public_spawn_validation_plan() {
    // Synthetic first-field IDs, not native defaults. All six premises explicit.
    return {2,6,7,8,{true,true,true,true,true,true}};
}
Bytes spawn_validation_packet(session::Context& client,std::uint16_t packet_id,
    std::uint16_t sequence=2,std::uint16_t channel=7,std::uint8_t field=2,bool suffix=false) {
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2;
    bunch.channel=channel; bunch.sequence=sequence;
    // Literal LSB tag2/bound8; an opaque suffix is never searched for another RPC.
    bunch.payload={field}; bunch.payload_bits=3;
    if (suffix) { bunch.payload={static_cast<std::uint8_t>(field|0xf8U),0x07}; bunch.payload_bits=11; }
    wire::Packet packet; packet.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
void validation_literal(const ControllerOutput& output,std::uint8_t literal,
    std::size_t bits,std::uint16_t sequence=44) {
    REQUIRE(output.bunches.size()==1); const auto& bunch=output.bunches[0];
    CHECK(bunch.channel==7); CHECK(bunch.sequence==sequence); CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    CHECK_FALSE(bunch.control); CHECK(bunch.payload_bits==bits); CHECK(bunch.payload==Bytes{literal});
}
void validation_selected(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
    Peer& peer,session::Context& client) {
    const auto opened=links_openings(fixture,login,peer,client);
    peer.send(fixture.owner.port(),links_ack(client,5,links_opening_acks(opened)));
    const auto links=links_output(peer,client,2000ms,2); links_literals(links);
    peer.send(fixture.owner.port(),start_mech_packet(client,6));
    const auto property=links_output(peer,client,2000ms,1); start_mech_property_literal(property);
    auto ids=links.packet_ids; ids.insert(ids.end(),property.packet_ids.begin(),property.packet_ids.end());
    peer.send(fixture.owner.port(),links_ack(client,7,ids));
}
}
TEST_CASE("UDP spawn validation requires explicit request and emits one literal success per accepted selection") {
    std::atomic<int> validations{0}; auto plan=public_spawn_validation_plan();
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            return std::optional{public_mech_result(previous)};
        },plan,[&](const session::SessionId& id,const wire::rpc::StartMechArguments& args,std::uint8_t setup) {
            CHECK(id==public_id); CHECK(args.mech_instance_id=="20000000-0000-0000-0000-000000000001");
            CHECK(args.primary_weapon==256); CHECK(args.holo_taunt_type==-1);
            CHECK(setup==42+validations.load()); ++validations;
            return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    plan.validated_rpc_field_id=1; plan.eligibility.primary_package_map=false;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    validation_selected(f,login,peer,client);
    CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK(validations==0);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_requested);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent); CHECK_FALSE(f.owner.diagnostics().spawn_validation_failed);
    peer.send(f.owner.port(),spawn_validation_packet(client,8,1,8)); // Pawn has no Controller dispatcher.
    peer.send(f.owner.port(),spawn_validation_packet(client,9,2,7,1)); // Unknown first field stays opaque.
    CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK(validations==0);
    peer.send(f.owner.port(),spawn_validation_packet(client,10,3,7,2,true));
    const auto accepted=links_output(peer,client,2000ms,1); validation_literal(accepted,0x06,3);
    CHECK(validations==1); CHECK(f.service->authorize_udp(public_id));
    CHECK(f.owner.diagnostics().spawn_validation_requested); CHECK(f.owner.diagnostics().spawn_validation_sent);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_failed);
    peer.send(f.owner.port(),spawn_validation_packet(client,11,3)); // Reliable duplicate, fresh authenticated nonce.
    peer.send(f.owner.port(),links_ack(client,12,accepted.packet_ids));
    peer.send(f.owner.port(),spawn_validation_packet(client,13,4)); // Same accepted setup remains idempotent.
    CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK(validations==1);
    peer.send(f.owner.port(),start_mech_packet(client,14,5));
    const auto preset=links_output(peer,client,2000ms,1); REQUIRE(preset.bunches.size()==1);
    CHECK(preset.bunches[0].channel==9); CHECK(preset.bunches[0].sequence==104);
    peer.send(f.owner.port(),links_ack(client,15,preset.packet_ids));
    peer.send(f.owner.port(),spawn_validation_packet(client,16,6));
    validation_literal(links_output(peer,client,2000ms,1),0x06,3,45); CHECK(validations==2);
}
TEST_CASE("UDP spawn validation queued before opening ACKs waits for actual selected PRI and links") {
    std::atomic<int> loaded{0},checked{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            ++loaded; return std::optional{public_mech_result(previous)};
        },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t setup) {
            CHECK(loaded==1); CHECK(setup==42); ++checked;
            return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opened=links_openings(f,login,peer,client);
    std::string diagnostic_trace="original_open_pids="+std::to_string(opened.packet_ids[0])+","+
        std::to_string(opened.packet_ids[1])+","+std::to_string(opened.packet_ids[2]);
    CAPTURE(diagnostic_trace);
    std::vector<std::uint16_t> received_open_ids;
    peer.send(f.owner.port(),start_mech_packet(client,5));
    peer.send(f.owner.port(),spawn_validation_packet(client,6));
    peer.send(f.owner.port(),links_ack(client,7,{1,3,5,7,8,opened.packet_ids[0],opened.packet_ids[2]}));
    CHECK(links_output(peer,client,200ms,2,&diagnostic_trace,&received_open_ids).bunches.empty()); CHECK(loaded==0); CHECK(checked==0);
    // The earlier Controller/Pawn ACK gap retransmitted PRI's opening.
    // ACK that actually observed copy before later property ACKs advance the gap.
    REQUIRE_FALSE(received_open_ids.empty());
    auto opening_ack_ids=received_open_ids;
    opening_ack_ids.push_back(opened.packet_ids[1]);
    peer.send(f.owner.port(),links_ack(client,8,opening_ack_ids));
    const auto output=links_output(peer,client,2000ms,3,&diagnostic_trace); REQUIRE(output.bunches.size()==3);
    CHECK(output.bunches[0].channel==7); CHECK(output.bunches[1].channel==8);
    CHECK(output.bunches[2].channel==9); CHECK(output.bunches[2].payload_bits==547);
    CHECK(links_output(peer,client,200ms,2,&diagnostic_trace).bunches.empty()); CHECK(loaded==1); CHECK(checked==0);
    CHECK(f.owner.diagnostics().spawn_validation_requested);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent); CHECK_FALSE(f.owner.diagnostics().spawn_validation_failed);
    // All openings were ACKed, but the selected reliable PRI property still is not.
    peer.send(f.owner.port(),links_ack(client,9,output.packet_ids));
    validation_literal(links_output(peer,client,2000ms,1,&diagnostic_trace),0x06,3); CHECK(checked==1);
    CHECK(f.owner.diagnostics().spawn_validation_sent); CHECK_FALSE(f.owner.diagnostics().spawn_validation_failed);
}
TEST_CASE("UDP spawn validation does not invent an accepted preset for a request before selection") {
    std::atomic<int> checked{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            return std::optional{public_mech_result(previous)};
        },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            ++checked; return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opened=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
    links_literals(links_output(peer,client,2000ms,2));
    peer.send(f.owner.port(),spawn_validation_packet(client,6,1));
    // GeneralFailure0: the required server-owned MechSetup-equivalent selection is absent.
    validation_literal(links_output(peer,client,2000ms,1),0x07,4); CHECK(checked==0);
    CHECK(f.owner.diagnostics().spawn_validation_requested); CHECK(wait_until([&]{return f.owner.diagnostics().spawn_validation_failed;}));
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent);
}
TEST_CASE("UDP spawn validation pending failed and invalid reasons never produce a success callback") {
    for (unsigned mode=0;mode<3;++mode) {
        CAPTURE(mode); std::atomic<int> checked{0};
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                ++checked;
                if (mode==0) return udp::SpawnSelectionValidationResult{{3,3,3,1,3},7};
                return udp::SpawnSelectionValidationResult{{3,3,2,3,3},static_cast<std::uint8_t>(mode==1?4:9)};
            });
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        validation_selected(f,login,peer,client); peer.send(f.owner.port(),spawn_validation_packet(client,8));
        CHECK(wait_until([&]{return checked.load()>0;}));
        if (mode==1) validation_literal(links_output(peer,client,2000ms,1),0x4f,8);
        else CHECK(links_output(peer,client,200ms).bunches.empty());
        if (mode==2) CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        else CHECK(f.service->authorize_udp(public_id));
        CHECK(f.owner.diagnostics().spawn_validation_requested);
        CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent);
        CHECK(f.owner.diagnostics().spawn_validation_failed==(mode==1));
    }
}
TEST_CASE("UDP spawn validator runs outside pins and discards retired revoked expired or throwing decisions") {
    for (unsigned mode=0;mode<4;++mode) {
        CAPTURE(mode); std::atomic<int> checked{0}; LoginAuthorityFixture* active=nullptr;
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                ++checked;
                if (mode==0) active->service.reset(); // Would deadlock if a Use still pinned its lease.
                if (mode==1) active->verifier.active=false;
                if (mode==2) { active->monotonic=131; active->udp_now=131; }
                if (mode==3) throw std::runtime_error("synthetic validation failure");
                return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            });
        active=&f; const auto login=f.login(); Peer peer;
        session::Context client(session::Role::client,public_id,public_key);
        validation_selected(f,login,peer,client); peer.send(f.owner.port(),spawn_validation_packet(client,8));
        CHECK(wait_until([&]{return checked==1 && f.owner.diagnostics().closed;}));
        CHECK(f.owner.diagnostics().spawn_validation_requested);
        CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent); CHECK_FALSE(f.owner.diagnostics().spawn_validation_failed);
        CHECK(links_output(peer,client,200ms).bunches.empty());
    }
}
TEST_CASE("UDP spawn validation schema and provider are preflighted before retryable worker start") {
    for (unsigned invalid=0;invalid<15;++invalid) {
        CAPTURE(invalid); udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto plan=public_spawn_validation_plan();
        std::optional<udp::SpawnValidationDispatchPlan> selected=plan;
        udp::LocalUdpOwner::SpawnSelectionValidator callback=[](const session::SessionId&,
            const wire::rpc::StartMechArguments&,std::uint8_t){return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};};
        switch(invalid) {
        case 0: selected.reset(); break;
        case 1: callback={}; break;
        case 2: selected->eligibility.primary_package_map=false; break;
        case 3: selected->eligibility.controller_cache_correct=false; break;
        case 4: selected->eligibility.validate_rpc_eligible=false; break;
        case 5: selected->eligibility.validated_rpc_eligible=false; break;
        case 6: selected->eligibility.failed_rpc_eligible=false; break;
        case 7: selected->eligibility.failure_reason_eligible=false; break;
        case 8: selected->controller_exclusive_bound=0; break;
        case 9: selected->validate_rpc_field_id=8; break;
        case 10: selected->validated_rpc_field_id=8; break;
        case 11: selected->failed_rpc_field_id=8; break;
        case 12: selected->validate_rpc_field_id=4; break; // Cannot overlap StartMech's first field.
        case 13: selected->validated_rpc_field_id=selected->failed_rpc_field_id; break;
        case 14: selected->controller_exclusive_bound=9; break;
        }
        error([&]{owner.start([](const session::SessionId&){return true;},
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional{public_world()};},
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t p){return std::optional{public_mech_result(p)};},
            selected,callback);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; } // Safe cleanup of ignored-plan RED scaffold.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;})); owner.stop();
    }
}
TEST_CASE("UDP spawn validation stop joins the provider and sends no stale success") {
    Gate gate;
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            return std::optional{public_mech_result(previous)};
        },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            gate.enter(); return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    validation_selected(f,login,peer,client); peer.send(f.owner.port(),spawn_validation_packet(client,8));
    const bool entered=gate.wait_entered(); CHECK(entered);
    if (!entered) { gate.release(); return; }
    std::atomic<bool> stopped{false}; std::thread shutdown([&]{f.owner.stop(); stopped=true;});
    CHECK(wait_until([&]{return !f.owner.running();})); CHECK_FALSE(stopped.load());
    gate.release(); shutdown.join(); CHECK(stopped.load());
    CHECK(links_output(peer,client,200ms).bunches.empty());
}


namespace {
Bytes coalesced_start_validation_packet(session::Context& client,std::uint16_t packet_id,bool behind_unknown) {
    wire::BitWriter bits; bits.write_bounded(4,8);
    auto u32=[&](std::uint32_t value) {
        for (unsigned shift=0;shift<32;shift+=8)
            bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(value>>shift)},8);
    };
    bits.write_bit(true); u32(37);
    for (const char c:std::string_view("20000000-0000-0000-0000-000000000001"))
        bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(c)},8);
    bits.write_bits(std::array<std::uint8_t,1>{0},8);
    bits.write_bit(true); u32(256); bits.write_bit(true); u32(0xffffffffU);
    // Known StartMech tag3 + arguments395 = exact bit398 boundary.
    // An unknown next field must stop parsing; do not search its opaque tail for2.
    if (behind_unknown) bits.write_bounded(1,8);
    bits.write_bounded(2,8);
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2;
    bunch.channel=7; bunch.sequence=1; bunch.payload=bits.payload_bytes(); bunch.payload_bits=bits.bit_count();
    wire::Packet packet; packet.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
}
TEST_CASE("UDP coalesced StartMech then Validate consumes only the known argument boundary without scanning unknown fields") {
    for (const bool behind_unknown : {false,true}) {
        CAPTURE(behind_unknown); std::atomic<int> checked{0};
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments& args,std::uint8_t setup) {
                CHECK(args.primary_weapon==256); CHECK(args.holo_taunt_type==-1); CHECK(setup==42);
                ++checked; return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            });
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        const auto opened=links_openings(f,login,peer,client);
        peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
        const auto links=links_output(peer,client,2000ms,2); links_literals(links);
        peer.send(f.owner.port(),coalesced_start_validation_packet(client,6,behind_unknown));
        const auto property=links_output(peer,client,2000ms,1); start_mech_property_literal(property);
        CHECK(checked==0); CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent);
        CHECK(f.owner.diagnostics().spawn_validation_requested==!behind_unknown);
        auto ids=links.packet_ids; ids.insert(ids.end(),property.packet_ids.begin(),property.packet_ids.end());
        peer.send(f.owner.port(),links_ack(client,7,ids));
        if (behind_unknown) {
            CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK(checked==0);
        } else {
            validation_literal(links_output(peer,client,2000ms,1),0x06,3);
            CHECK(checked==1); CHECK(f.owner.diagnostics().spawn_validation_sent);
        }
    }
}


namespace {
udp::SpawnAdmissionDispatchPlan public_spawn_admission_plan() {
    return {1,3,8,{true,true,true,true,true,true}};
}
Bytes selection_stage_packet(session::Context& client,std::uint16_t packet_id,
    std::uint16_t sequence,std::int32_t stage) {
    wire::BitWriter bits; bits.write_bounded(1,8); bits.write_bit(stage!=0);
    if (stage!=0) {
        const auto value=static_cast<std::uint32_t>(stage);
        for (unsigned shift=0;shift<32;shift+=8)
            bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(value>>shift)},8);
    }
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2; bunch.channel=7;
    bunch.sequence=sequence; bunch.payload=bits.payload_bytes(); bunch.payload_bits=bits.bit_count();
    wire::Packet packet; packet.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
void admission_literal(const ControllerOutput& output) {
    REQUIRE(output.bunches.size()==1);
    const auto& bunch=output.bunches[0];
    CHECK(bunch.channel==7); CHECK(bunch.sequence==45); CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    // Independent LSB tag3/8 + presence1 + primary selector1 + channel8/2048.
    CHECK(bunch.payload_bits==16); CHECK(bunch.payload==hex("1b01"));
}
}
TEST_CASE("UDP admission needs explicit stage3 after validated selection and reliably restarts the existing Pawn once") {
    std::atomic<int> checks{0}; auto admission=public_spawn_admission_plan();
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
            return std::optional{public_mech_result(previous)};
        },public_spawn_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            ++checks; return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        },admission);
    admission.client_restart_rpc_field_id=0; admission.eligibility.pawn_reference_eligible=false;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    validation_selected(f,login,peer,client);
    peer.send(f.owner.port(),spawn_validation_packet(client,8));
    const auto validated=links_output(peer,client,2000ms,1); validation_literal(validated,0x06,3);
    peer.send(f.owner.port(),links_ack(client,9,validated.packet_ids));
    CHECK(checks==1); CHECK_FALSE(f.owner.diagnostics().stage3_requested);
    CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
    peer.send(f.owner.port(),selection_stage_packet(client,10,3,2));
    CHECK(links_output(peer,client,200ms).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().stage3_requested); CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
    peer.send(f.owner.port(),selection_stage_packet(client,11,4,3));
    const auto restarted=links_output(peer,client,2000ms,1); admission_literal(restarted);
    CHECK(f.owner.diagnostics().stage3_requested); CHECK(f.owner.diagnostics().client_restart_sent);
    CHECK(f.service->authorize_udp(public_id)); CHECK(checks==1);
    peer.send(f.owner.port(),selection_stage_packet(client,12,4,3)); // Same reliable sequence, fresh nonce.
    peer.send(f.owner.port(),links_ack(client,13,restarted.packet_ids));
    peer.send(f.owner.port(),selection_stage_packet(client,14,5,3));
    CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK(checks==1);
}
TEST_CASE("UDP admission rejects invalid premises and never restarts without successful selected data validation") {
    for (const bool fail_validation : {false,true}) {
        CAPTURE(fail_validation);
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                return udp::SpawnSelectionValidationResult{{3,3,2,3,3},4};
            },public_spawn_admission_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        validation_selected(f,login,peer,client);
        if (fail_validation) {
            peer.send(f.owner.port(),spawn_validation_packet(client,8));
            const auto failed=links_output(peer,client,2000ms,1); validation_literal(failed,0x4f,8);
            peer.send(f.owner.port(),links_ack(client,9,failed.packet_ids));
        }
        peer.send(f.owner.port(),selection_stage_packet(client,10,static_cast<std::uint16_t>(fail_validation?3:2),3));
        CHECK(links_output(peer,client,200ms).bunches.empty());
        CHECK(f.owner.diagnostics().stage3_requested); CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
        CHECK(f.service->authorize_udp(public_id));
    }
    for (unsigned invalid=0;invalid<11;++invalid) {
        CAPTURE(invalid); auto admission=public_spawn_admission_plan();
        switch(invalid) {
        case 0: admission.eligibility.primary_package_map=false; break;
        case 1: admission.eligibility.controller_cache_correct=false; break;
        case 2: admission.eligibility.stage_rpc_eligible=false; break;
        case 3: admission.eligibility.stage_argument_eligible=false; break;
        case 4: admission.eligibility.client_restart_rpc_eligible=false; break;
        case 5: admission.eligibility.pawn_reference_eligible=false; break;
        case 6: admission.controller_exclusive_bound=0; break;
        case 7: admission.stage_rpc_field_id=8; break;
        case 8: admission.client_restart_rpc_field_id=8; break;
        case 9: admission.stage_rpc_field_id=4; break; // Collides with known StartMech.
        case 10: admission.controller_exclusive_bound=9; break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        error([&]{owner.start([](const session::SessionId&){return true;},
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional{public_world()};},
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            },admission);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; } // Safe cleanup while RED ignores the plan.
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;})); owner.stop();
    }
}


TEST_CASE("UDP admission preserves known boundaries and rejects stale selection or revoked authorization") {
    for (unsigned mode=0;mode<4;++mode) {
        CAPTURE(mode);
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),public_start_mech_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
                return std::optional{public_mech_result(previous)};
            },public_spawn_validation_plan(),[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            },public_spawn_admission_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        validation_selected(f,login,peer,client);
        peer.send(f.owner.port(),spawn_validation_packet(client,8));
        const auto validated=links_output(peer,client,2000ms,1); validation_literal(validated,0x06,3);
        peer.send(f.owner.port(),links_ack(client,9,validated.packet_ids));
        if (mode<2) {
            wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2; bunch.channel=7; bunch.sequence=3;
            // Independent unaligned fixtures: Stage0(4bits), then Stage3(36bits).
            // The negative inserts unknown field0(3bits) before the opaque Stage3.
            bunch.payload=mode==0 ? hex("9103000000") : hex("811c00000000");
            bunch.payload_bits=mode==0 ? 40 : 43;
            wire::Packet packet; packet.packet_id=10;
            wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
            peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
            if (mode==0) {
                admission_literal(links_output(peer,client,2000ms,1));
                CHECK(f.owner.diagnostics().stage3_requested); CHECK(f.owner.diagnostics().client_restart_sent);
            } else {
                CHECK(links_output(peer,client,200ms).bunches.empty());
                CHECK_FALSE(f.owner.diagnostics().stage3_requested); CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
            }
        } else if (mode==2) {
            peer.send(f.owner.port(),start_mech_packet(client,10,3));
            const auto selected=links_output(peer,client,2000ms,1);
            REQUIRE(selected.bunches.size()==1); CHECK(selected.bunches[0].channel==9);
            CHECK(selected.bunches[0].sequence==104); CHECK(selected.bunches[0].payload_bits==547);
            peer.send(f.owner.port(),selection_stage_packet(client,11,4,3));
            CHECK(links_output(peer,client,200ms).bunches.empty());
            peer.send(f.owner.port(),links_ack(client,12,selected.packet_ids));
            CHECK(links_output(peer,client,200ms).bunches.empty());
            CHECK(f.owner.diagnostics().stage3_requested); CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
            CHECK(f.service->authorize_udp(public_id)); // Old success cannot release the new revision.
        } else {
            f.verifier.active=false;
            peer.send(f.owner.port(),selection_stage_packet(client,10,3,3));
            CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
            CHECK(links_output(peer,client,200ms).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
        }
    }
}

namespace {
udp::PriIdentityDispatchPlan public_identity_plan() {
    // SYNTHETIC field2/bound8, no native cache observation.
    return {2,8,control::Endian::little,{true,true,true,true,true}};
}
Bytes expected_provision_identity(std::string_view guid) {
    REQUIRE(guid.size()==36);
    std::string compact;
    for (const auto c:guid) if (c!='-') compact.push_back(c);
    REQUIRE(compact.size()==32);
    auto bytes=hex(compact);
    // Independent native076ca0 chunks, then four little-endian u32 blocks.
    // This does not call the product GUID parser or UniqueId codec.
    for (std::size_t i=0;i<bytes.size();i+=4)
        std::reverse(bytes.begin()+static_cast<std::ptrdiff_t>(i),
            bytes.begin()+static_cast<std::ptrdiff_t>(i+4));
    return bytes;
}
void identity_property(const ControllerOutput& output,std::string_view provision_guid) {
    REQUIRE(output.bunches.size()==1);
    const auto& bunch=output.bunches.front();
    REQUIRE(bunch.channel==9);
    // Owner fixture PRI seed101 -> reliable opening102 -> first property103.
    // Runtime's separate91/92/93 fixture does not define this Owner oracle.
    CHECK(bunch.sequence==103);
    CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    REQUIRE(bunch.payload_bits==131); // tag3 + value128, no RPC presence.
    wire::BitReader reader(bunch.payload,bunch.payload_bits);
    CHECK(reader.read_bounded(8)==2);
    const auto value=reader.read_bits(128);
    CHECK(value==expected_provision_identity(provision_guid));
    // Existing Login fixture carries these conflicting client-supplied words.
    CHECK(value!=hex("040302017856341200000080ffffffff"));
    CHECK(reader.at_end());
    REQUIRE(output.packet_ids.size()==1);
}
}

TEST_CASE("UDP PRI identity uses server provision after real opening ACK and gates links until identity ACK") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan(),{},{},{},{},{},public_identity_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    REQUIRE(f.handed_off_user_guid.size()==36);
    INFO("initial opening packet IDs controller="<<openings.packet_ids[0]
        <<" pri="<<openings.packet_ids[1]<<" pawn="<<openings.packet_ids[2]);
    auto partial=links_opening_acks(openings); partial.erase(partial.begin()+6);
    std::string opening_retry_trace;
    std::vector<std::uint16_t> opening_retry_ids;
    peer.send(f.owner.port(),links_ack(client,5,partial));
    CHECK(links_output(peer,client,200ms,2,&opening_retry_trace,&opening_retry_ids).bunches.empty());
    INFO("partial ACK5 opening retry count="<<opening_retry_ids.size()<<" trace="<<opening_retry_trace);
    const auto partial_diagnostics=f.owner.diagnostics();
    CHECK(partial_diagnostics.controller_open_acked); CHECK(partial_diagnostics.pawn_open_acked);
    CHECK_FALSE(partial_diagnostics.pri_open_acked); CHECK_FALSE(partial_diagnostics.pri_identity_sent);
    CHECK_FALSE(partial_diagnostics.actor_links_sent);
    // Partial ACK's gap moved the PRI opening association to the received retry.
    // ACK that real current PacketID, not the superseded initial opening ID.
    REQUIRE_FALSE(opening_retry_ids.empty());
    peer.send(f.owner.port(),links_ack(client,6,opening_retry_ids));
    const auto identity=links_output(peer,client,2000ms,1);
    identity_property(identity,f.handed_off_user_guid);
    INFO("identity packet ID="<<identity.packet_ids.front()
        <<" preceded by ACK6 of received opening retries count="<<opening_retry_ids.size()
        <<" trace="<<opening_retry_trace);
    CHECK(wait_until([&]{return f.owner.diagnostics().pri_identity_sent;}));
    CHECK(f.owner.diagnostics().pri_open_acked);
    CHECK_FALSE(f.owner.diagnostics().pri_identity_acked); CHECK_FALSE(f.owner.diagnostics().actor_links_sent);
    // Re-ACKing only the opening cannot clear the queued identity property.
    std::vector<std::uint16_t> later_packet_ids;
    peer.send(f.owner.port(),links_ack(client,7,opening_retry_ids));
    CHECK(links_output(peer,client,200ms,2,nullptr,nullptr,&later_packet_ids).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().pri_identity_acked);
    // Accepted Actor retry is driven by a real later ACK gap. The duplicate
    // opening ACK is itself acknowledged with a later server ACK-only packet.
    const auto later=std::find_if(later_packet_ids.begin(),later_packet_ids.end(),
        [&](std::uint16_t id){return id>identity.packet_ids.front();});
    REQUIRE(later!=later_packet_ids.end());
    INFO("ACK8 gap server packet ID="<<*later);
    peer.send(f.owner.port(),links_ack(client,8,{*later}));
    const auto retry=links_output(peer,client,2000ms,1);
    identity_property(retry,f.handed_off_user_guid);
    INFO("retried identity packet ID="<<retry.packet_ids.front()<<" final ACK9 server ID="<<retry.packet_ids.front());
    CHECK(retry.packet_ids.front()>identity.packet_ids.front());
    CHECK_FALSE(f.owner.diagnostics().actor_links_sent);
    const auto before_identity_ack=f.owner.diagnostics();
    INFO("before final UID ACK controller_open="<<before_identity_ack.controller_open_acked
        <<" pri_open="<<before_identity_ack.pri_open_acked<<" pawn_open="<<before_identity_ack.pawn_open_acked
        <<" identity_sent="<<before_identity_ack.pri_identity_sent<<" identity_acked="<<before_identity_ack.pri_identity_acked
        <<" links_sent="<<before_identity_ack.actor_links_sent<<" closed="<<before_identity_ack.closed);
    peer.send(f.owner.port(),links_ack(client,9,retry.packet_ids));
    const auto linked=links_output(peer,client,2000ms);
    const auto after_identity_ack=f.owner.diagnostics();
    INFO("after final UID ACK controller_open="<<after_identity_ack.controller_open_acked
        <<" pri_open="<<after_identity_ack.pri_open_acked<<" pawn_open="<<after_identity_ack.pawn_open_acked
        <<" identity_sent="<<after_identity_ack.pri_identity_sent<<" identity_acked="<<after_identity_ack.pri_identity_acked
        <<" links_sent="<<after_identity_ack.actor_links_sent<<" closed="<<after_identity_ack.closed);
    links_literals(linked);
    CHECK(wait_until([&]{return f.owner.diagnostics().actor_links_sent;}));
    CHECK(f.owner.diagnostics().pri_identity_acked);
    CHECK(f.service->authorize_udp(public_id));
    peer.send(f.owner.port(),links_ack(client,10,retry.packet_ids));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP PRI identity copied plan ignores confirmed seeds and needs only real PRI opening before send") {
    auto controller=public_controller_plan(); auto pawn=public_pawn_plan(); auto pri=public_pri_plan();
    controller.channel.open_confirmed=true; pawn.channel.open_confirmed=true; pri.channel.open_confirmed=true;
    auto plan=public_identity_plan();
    LoginAuthorityFixture f({},true,controller_publisher(),controller,{},pawn,pri,public_links_plan(),
        {},{},{},{},{},plan);
    plan.field_id=7; plan.exclusive_bound=1; plan.integer_order=control::Endian::big;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().controller_open_acked);
    CHECK_FALSE(f.owner.diagnostics().pri_open_acked); CHECK_FALSE(f.owner.diagnostics().pawn_open_acked);
    peer.send(f.owner.port(),links_ack(client,5,{openings.packet_ids[1]}));
    const auto identity=links_output(peer,client,2000ms,1);
    identity_property(identity,f.handed_off_user_guid);
    CHECK(f.owner.diagnostics().pri_open_acked);
    CHECK_FALSE(f.owner.diagnostics().controller_open_acked); CHECK_FALSE(f.owner.diagnostics().pawn_open_acked);
    peer.send(f.owner.port(),links_ack(client,6,identity.packet_ids));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK(wait_until([&]{return f.owner.diagnostics().pri_identity_acked;}));
    CHECK_FALSE(f.owner.diagnostics().actor_links_sent);
    peer.send(f.owner.port(),links_ack(client,7,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
    CHECK(wait_until([&]{return f.owner.diagnostics().actor_links_sent;}));
}

TEST_CASE("UDP PRI identity prerequisites premises tag endian and budget fail before retryable start") {
    for (unsigned mutation=0;mutation<13;++mutation) {
        CAPTURE(mutation);
        auto config=authority_skeleton(); auto plan=public_identity_plan();
        udp::LocalUdpOwner::LoginResponseValidator validator=[](const session::SessionId&,
            const control::posthello::RawString&){return true;};
        udp::LocalUdpOwner::PublicationProvider publisher=[](const session::SessionId&,
            const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};};
        std::optional<udp::InitialPriPlan> pri=public_pri_plan();
        std::optional<udp::InitialActorLinksPlan> links=public_links_plan();
        switch (mutation) {
        case 0: validator={}; break;
        case 1: publisher={}; break;
        case 2: pri.reset(); break;
        case 3: links.reset(); break;
        case 4: plan.eligibility.primary_package_map=false; break;
        case 5: plan.eligibility.pri_cache_correct=false; break;
        case 6: plan.eligibility.unique_id_property_eligible=false; break;
        case 7: plan.eligibility.unique_net_id_serializer_correct=false; break;
        case 8: plan.eligibility.integer_archive_order_correct=false; break;
        case 9: plan.exclusive_bound=0; break;
        case 10: plan.field_id=8; break;
        case 11: plan.integer_order=static_cast<control::Endian>(9); break;
        case 12: config.transport.max_packet_bytes=16; break; //131 bits exceed128 before start.
        }
        udp::LocalUdpOwner owner(0,config,[]{return 100.0;},5ms);
        error([&]{owner.start([](const session::SessionId&){return true;},validator,publisher,
            public_controller_plan(),public_pawn_plan(),pri,links,{},{},{},{},{},plan);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; } // Safe semantic RED no-op cleanup.
        CHECK_FALSE(owner.diagnostics().pri_identity_sent); CHECK_FALSE(owner.diagnostics().actor_links_sent);
        owner.start([](const session::SessionId&){return true;}); CHECK(owner.running());
    }
}

TEST_CASE("UDP PRI identity rejects malformed or zero provision GUID without consuming handoff budget") {
    const std::string invalid[]{"", "00000000-0000-0000-0000-000000000000",
        "11223344-5566-7788-99AA-BBCCDDEEFF00", "112233445566778899aabbccddeeff00",
        "11223344_5566-7788-99aa-bbccddeeff00", "11223344-5566-7788-99aa-bbccddeeff0g"};
    for (const auto& guid:invalid) {
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        owner.start([](const session::SessionId&){return true;},
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional<game::WorldPublication>{public_world()};},
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),
            {},{},{},{},{},public_identity_plan());
        auto bad=provision(owner.port()); bad.user_guid=guid;
        auto rejected=owner.handoff(bad); CHECK_FALSE(rejected);
        CHECK(owner.running()); CHECK_FALSE(owner.diagnostics().pri_identity_sent);
        if (rejected) { rejected.reset(); owner.stop(); continue; }
        auto accepted=owner.handoff(provision(owner.port())); REQUIRE(accepted); CHECK(accepted->active());
    }
}

TEST_CASE("UDP PRI identity absent plan preserves old links and does not retain malformed unused GUID") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto openings=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(openings)));
    links_literals(links_output(peer,client,2000ms));
    CHECK(wait_until([&]{return f.owner.diagnostics().actor_links_sent;}));
    CHECK(f.owner.diagnostics().controller_open_acked); CHECK(f.owner.diagnostics().pri_open_acked);
    CHECK(f.owner.diagnostics().pawn_open_acked);
    CHECK_FALSE(f.owner.diagnostics().pri_identity_sent); CHECK_FALSE(f.owner.diagnostics().pri_identity_acked);
    udp::LocalUdpOwner absent(0,authority_skeleton(),[]{return 100.0;},5ms);
    absent.start([](const session::SessionId&){return true;});
    auto unused=provision(absent.port()); unused.user_guid="unused-invalid-guid";
    const auto lease=absent.handoff(unused); REQUIRE(lease); CHECK(lease->active());
}

TEST_CASE("UDP PRI identity revoked authorization retires before identity or links without further send") {
    for (const bool after_identity:{false,true}) {
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),public_links_plan(),{},{},{},{},{},public_identity_plan());
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        const auto openings=links_openings(f,login,peer,client);
        std::vector<std::uint16_t> pending_ack=links_opening_acks(openings);
        if (after_identity) {
            peer.send(f.owner.port(),links_ack(client,5,pending_ack));
            const auto identity=links_output(peer,client,2000ms,1);
            identity_property(identity,f.handed_off_user_guid); pending_ack=identity.packet_ids;
            CHECK(wait_until([&]{return f.owner.diagnostics().pri_identity_sent;}));
        }
        f.verifier.active=false;
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK_FALSE(f.service->authorize_udp(public_id));
        peer.send(f.owner.port(),links_ack(client,6,pending_ack));
        CHECK(links_output(peer,client).bunches.empty());
        CHECK(f.owner.diagnostics().pri_identity_sent==after_identity);
        CHECK_FALSE(f.owner.diagnostics().pri_identity_acked); CHECK_FALSE(f.owner.diagnostics().actor_links_sent);
        CHECK(f.owner.running()); // Session retire does not stop the owner socket.
    }
}

TEST_CASE("UDP PRI identity and StartMech reject contradictory same-channel cache then allow coherent retry") {
    for (const bool duplicate_field:{false,true}) {
        CAPTURE(duplicate_field);
        auto identity=public_identity_plan();
        const auto mech=public_start_mech_plan();
        identity.exclusive_bound=mech.pri_exclusive_bound;
        identity.field_id=mech.pri_mech_setup_field_id==2 ? 3U : 2U;
        const auto coherent=identity;
        if (duplicate_field) identity.field_id=mech.pri_mech_setup_field_id;
        else ++identity.exclusive_bound;
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        const auto authorize=[](const session::SessionId&){return true;};
        const auto validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        const auto publisher=[](const session::SessionId&,const control::posthello::Login&) {
            return std::optional<game::WorldPublication>{public_world()};
        };
        const auto provider=[](const session::SessionId&,const wire::rpc::StartMechArguments&,
            std::uint8_t previous) { return std::optional{public_mech_result(previous)}; };
        error([&]{owner.start(authorize,validator,publisher,
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),
            mech,provider,{},{},{},identity);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        if (owner.running()) { owner.stop(); continue; } // Preserve cleanup on semantic RED.
        CHECK_FALSE(owner.diagnostics().pri_identity_sent);
        owner.start(authorize,validator,publisher,
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),public_links_plan(),
            mech,provider,{},{},{},coherent);
        CHECK(owner.running());
    }
}

namespace {
constexpr std::array<std::uint32_t,4> bootstrap_hg{0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U};
constexpr std::array<std::uint32_t,4> bootstrap_hgc{0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU};
game::WorldPublication bootstrap_world() {
    // Synthetic metadata only: padding roots reproduce the retained PRIMARY
    // coordinates HG=38336 and HGC=104786. No original assets/SHA gates here.
    auto world=public_world();
    world.packages[0].generation_net_counts={0,1639};
    world.packages.push_back({"SPad",{10,11,12,13},{0,36697}});
    world.packages.push_back({"HawkenGame",bootstrap_hg,{0,49101}});
    world.packages.push_back({"TPad",{20,21,22,23},{0,17349}});
    world.contained_packages.push_back({{"HawkenGameContent",bootstrap_hgc,{0,533}},"TPad"});
    return world;
}
LoginAuthorityFixture::PublicationDecision bootstrap_publisher() {
    return [](LoginAuthorityFixture&,const session::SessionId&,const control::posthello::Login&) {
        return std::optional{bootstrap_world()};
    };
}
udp::InitialActorLinksPlan bootstrap_links() {
    auto links=public_links_plan(); links.controller_exclusive_bound=294; return links;
}
udp::SelectionBootstrapPlan public_bootstrap_plan() {
    return {{{10,111,0,false},bootstrap_hg,9439,{0,0,0},std::nullopt},
        bootstrap_hgc,232,bootstrap_hg,32596,31,73,42,294,
        {true,true,true,true,true,true,true,true,true,true,true,true}};
}
Bytes bootstrap_join(session::Context& client) {
    // Five independent PackageAck8 archives, generationCOUNT2, followed by Join9.
    // The netcounts above are not the ACK values.
    return authority_request(client,4,5,hex(
        "08040302017856341200000080ffffffff02000000"
        "080a0000000b0000000c0000000d00000002000000"
        "0821675abf2b093349802f52e844c18f2302000000"
        "081400000015000000160000001700000002000000"
        "08c739d15c143ea74a93851cbddd5660f80200000009"));
}
struct BootstrapOpenings {
    ControllerOutput actors;
    std::vector<std::uint16_t> control_ids;
};
BootstrapOpenings bootstrap_openings(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
                                    Peer& peer,session::Context& client,bool rotated=false) {
    publication_login(fixture,login,peer,client);
    const auto publication=publication_packets(peer,client,6);
    REQUIRE(publication.logical.size()==6);
    CHECK_FALSE(fixture.owner.diagnostics().gri_sent);
    peer.send(fixture.owner.port(),bootstrap_join(client));
    auto actors=controller_output(peer,client,2000ms,4);
    REQUIRE(actors.bunches.size()==4); REQUIRE(actors.packet_ids.size()==4);
    const std::uint16_t channels[]{7,9,8,10};
    const std::uint32_t sequences[]{42,102,72,112};
    for (std::size_t i=0;i<4;++i) {
        const auto& bunch=actors.bunches[i]; CHECK(bunch.channel==channels[i]); CHECK(bunch.sequence==sequences[i]);
        CHECK(bunch.reliable); CHECK(bunch.control); CHECK(bunch.open); CHECK_FALSE(bunch.close);
        CHECK(bunch.channel_type==2);
    }
    // GRI: primary coordinate47775 <<1, then the independently verified zero
    // position11bits. No Controller NetPlayerIndex byte; optional zero rotation3bits.
    CHECK(actors.bunches[3].payload==hex("3e7501004005"));
    CHECK(actors.bunches[3].payload_bits==(rotated ? 46U : 43U));
    CHECK(wait_until([&]{return fixture.owner.diagnostics().gri_sent;}));
    std::vector<std::uint16_t> controls{1,3,5}; // Existing Hello literal fixture replies.
    controls.insert(controls.end(),publication.packet_ids.begin(),publication.packet_ids.end());
    return {std::move(actors),std::move(controls)};
}
std::vector<std::uint16_t> bootstrap_opening_acks(const BootstrapOpenings& out,bool include_gri=true) {
    auto ids=out.control_ids;
    ids.insert(ids.end(),out.actors.packet_ids.begin(),
        out.actors.packet_ids.begin()+(include_gri ? 4 : 3));
    return ids;
}
void bootstrap_property_literal(const ControllerOutput& out) {
    REQUIRE(out.bunches.size()==1); REQUIRE(out.packet_ids.size()==1);
    const auto& bunch=out.bunches[0];
    // Field31/bound73 + PRIMARY Class coordinate105018. No RPC presence bit.
    CHECK(bunch.channel==10); CHECK(bunch.sequence==113);
    CHECK(bunch.payload==hex("1f1dcd0000")); CHECK(bunch.payload_bits==38);
    CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
}
void bootstrap_hud_literal(const ControllerOutput& out) {
    REQUIRE(out.bunches.size()==1); REQUIRE(out.packet_ids.size()==1);
    const auto& bunch=out.bunches[0];
    // Field42/bound294 + present Class parameter + PRIMARY coordinate70932.
    CHECK(bunch.channel==7); CHECK(bunch.sequence==44);
    CHECK(bunch.payload==hex("2a5154040000")); CHECK(bunch.payload_bits==41);
    CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
}
void bootstrap_start(udp::LocalUdpOwner& owner,const udp::SelectionBootstrapPlan& plan,
                     udp::LocalUdpOwner::LoginResponseValidator validator,
                     udp::LocalUdpOwner::PublicationProvider publisher,
                     std::optional<udp::InitialControllerPlan> controller=public_controller_plan(),
                     std::optional<udp::InitialPawnPlan> pawn=public_pawn_plan(),
                     std::optional<udp::InitialPriPlan> pri=public_pri_plan(),
                     std::optional<udp::InitialActorLinksPlan> links=bootstrap_links()) {
    owner.start([](const session::SessionId&){return true;},std::move(validator),std::move(publisher),
        controller,pawn,pri,links,{},{},{},{},{},{},plan);
}
}

TEST_CASE("UDP selection bootstrap opens fourth GRI then gates HUD on actual GameClass and link ACKs") {
    auto plan=public_bootstrap_plan(); plan.gri.rotation=wire::actor::Rotation{0,0,0};
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},{},plan);
    // Caller mutation after start must not change any copied reference/rotation/schema.
    plan.gri.rotation.reset(); plan.gri.channel.channel=11; plan.game_class_local_net_index=1;
    plan.hud_local_net_index=1; plan.game_class_field_id=2; plan.client_set_hud_field_id=3;
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opens=bootstrap_openings(f,login,peer,client,true);
    peer.send(f.owner.port(),links_ack(client,5,bootstrap_opening_acks(opens,false)));
    const auto links=links_output(peer,client,2000ms,2); REQUIRE(links.bunches.size()==2);
    REQUIRE(links.packet_ids.size()==2); CHECK(links.bunches[0].channel==7); CHECK(links.bunches[0].sequence==43);
    CHECK(links.bunches[1].channel==8); CHECK(links.bunches[1].sequence==73);
    CHECK(wait_until([&]{return f.owner.diagnostics().actor_links_sent;})); CHECK_FALSE(f.owner.diagnostics().gri_open_acked);
    CHECK_FALSE(f.owner.diagnostics().game_class_sent); CHECK_FALSE(f.owner.diagnostics().hud_sent);
    peer.send(f.owner.port(),links_ack(client,6,{opens.actors.packet_ids[3]}));
    const auto property=links_output(peer,client,2000ms,1); bootstrap_property_literal(property);
    CHECK(f.owner.diagnostics().gri_open_acked); CHECK(wait_until([&]{return f.owner.diagnostics().game_class_sent;}));
    CHECK_FALSE(f.owner.diagnostics().game_class_acked); CHECK_FALSE(f.owner.diagnostics().hud_sent);
    // Deliver only the Controller link. GameClass ACK is later than the still
    // pending Pawn link, so that real ACK gap must retry the Pawn link first.
    peer.send(f.owner.port(),links_ack(client,7,{links.packet_ids[0]}));
    CHECK(links_output(peer,client).bunches.empty());
    peer.send(f.owner.port(),links_ack(client,8,property.packet_ids));
    const auto pawn_retry=links_output(peer,client,2000ms,1); REQUIRE(pawn_retry.bunches.size()==1);
    REQUIRE(pawn_retry.packet_ids.size()==1); CHECK(pawn_retry.bunches[0].channel==8);
    CHECK(pawn_retry.bunches[0].sequence==73); CHECK(pawn_retry.bunches[0].payload==links.bunches[1].payload);
    CHECK(pawn_retry.packet_ids[0]>links.packet_ids[1]);
    CHECK(wait_until([&]{return f.owner.diagnostics().game_class_acked;})); CHECK_FALSE(f.owner.diagnostics().hud_sent);
    peer.send(f.owner.port(),links_ack(client,9,pawn_retry.packet_ids));
    const auto hud=links_output(peer,client,2000ms,1); bootstrap_hud_literal(hud);
    CHECK(wait_until([&]{return f.owner.diagnostics().hud_sent;})); CHECK_FALSE(f.owner.diagnostics().hud_acked);
    peer.send(f.owner.port(),links_ack(client,10,hud.packet_ids));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK(wait_until([&]{return f.owner.diagnostics().hud_acked;}));
    peer.send(f.owner.port(),links_ack(client,11,hud.packet_ids));
    CHECK(links_output(peer,client).bunches.empty());
    peer.send(f.owner.port(),bootstrap_join(client)); // Duplicate reliable Join, fresh crypto nonce.
    CHECK(controller_output(peer,client,200ms).bunches.empty());
    CHECK(f.service->authorize_udp(public_id)); CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP selection bootstrap retries GameClass on observed ACK gap before one HUD send") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},{},public_bootstrap_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opens=bootstrap_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,bootstrap_opening_acks(opens)));
    const auto initial=links_output(peer,client,2000ms,3); REQUIRE(initial.bunches.size()==3);
    REQUIRE(initial.packet_ids.size()==3); CHECK(initial.bunches[0].channel==7); CHECK(initial.bunches[1].channel==8);
    ControllerOutput property{{initial.bunches[2]},{initial.packet_ids[2]}}; bootstrap_property_literal(property);
    peer.send(f.owner.port(),links_ack(client,6,{initial.packet_ids[0],initial.packet_ids[1]}));
    CHECK(links_output(peer,client).bunches.empty()); CHECK_FALSE(f.owner.diagnostics().hud_sent);
    std::vector<std::uint16_t> later_ids;
    peer.send(f.owner.port(),links_ack(client,7,{opens.actors.packet_ids[3]}));
    CHECK(links_output(peer,client,200ms,2,nullptr,nullptr,&later_ids).bunches.empty());
    const auto later=std::find_if(later_ids.begin(),later_ids.end(),
        [&](std::uint16_t id){return id>property.packet_ids[0];});
    REQUIRE(later!=later_ids.end());
    peer.send(f.owner.port(),links_ack(client,8,{*later}));
    const auto retry=links_output(peer,client,2000ms,1); bootstrap_property_literal(retry);
    CHECK(retry.packet_ids[0]>property.packet_ids[0]); CHECK_FALSE(f.owner.diagnostics().game_class_acked);
    CHECK_FALSE(f.owner.diagnostics().hud_sent);
    peer.send(f.owner.port(),links_ack(client,9,retry.packet_ids));
    const auto hud=links_output(peer,client,2000ms,1); bootstrap_hud_literal(hud);
    peer.send(f.owner.port(),links_ack(client,10,hud.packet_ids));
    CHECK(links_output(peer,client).bunches.empty()); CHECK(f.owner.diagnostics().hud_acked);
}

TEST_CASE("UDP selection bootstrap GameClass opening gate is independent of configured PRI identity") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},public_identity_plan(),public_bootstrap_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opens=bootstrap_openings(f,login,peer,client);
    auto ids=opens.control_ids; ids.push_back(opens.actors.packet_ids[3]);
    // Later GRI ACK can retry the three unacknowledged actor openings, but the
    // class property needs neither a fabricated UID ACK nor actor-link delivery.
    peer.send(f.owner.port(),links_ack(client,5,ids));
    bootstrap_property_literal(links_output(peer,client,2000ms,1));
    CHECK_FALSE(f.owner.diagnostics().pri_identity_sent); CHECK_FALSE(f.owner.diagnostics().pri_identity_acked);
    CHECK_FALSE(f.owner.diagnostics().actor_links_sent); CHECK_FALSE(f.owner.diagnostics().hud_sent);
}

TEST_CASE("UDP selection bootstrap requires all schema premises and prior actors before retryable start") {
    using E=udp::SelectionBootstrapEligibility;
    bool E::* const premises[]{&E::primary_package_map,&E::common_gri_prefix_correct,&E::gri_cdo_correct,
        &E::gri_cache_correct,&E::game_class_property_eligible,&E::game_class_property_class_correct,
        &E::game_class_meta_class_correct,&E::controller_cache_correct,&E::client_set_hud_rpc_eligible,
        &E::hud_parameter_eligible,&E::hud_parameter_class_correct,&E::hud_meta_class_correct};
    for (unsigned missing=0;missing<18;++missing) {
        CAPTURE(missing); auto plan=public_bootstrap_plan();
        udp::LocalUdpOwner::LoginResponseValidator validator=[](const session::SessionId&,const control::posthello::RawString&){return true;};
        udp::LocalUdpOwner::PublicationProvider publisher=[](const session::SessionId&,const control::posthello::Login&){return std::optional{bootstrap_world()};};
        std::optional<udp::InitialControllerPlan> controller=public_controller_plan();
        std::optional<udp::InitialPawnPlan> pawn=public_pawn_plan(); std::optional<udp::InitialPriPlan> pri=public_pri_plan();
        std::optional<udp::InitialActorLinksPlan> links=bootstrap_links();
        if (missing==0) validator={}; if (missing==1) publisher={}; if (missing==2) controller.reset();
        if (missing==3) pawn.reset(); if (missing==4) pri.reset(); if (missing==5) links.reset();
        if (missing>=6) plan.eligibility.*premises[missing-6]=false;
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        error([&]{bootstrap_start(owner,plan,validator,publisher,controller,pawn,pri,links);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); if (owner.running()) {owner.stop();continue;} // Ignored-plan semantic RED cleanup.
        bootstrap_start(owner,public_bootstrap_plan(),
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional{bootstrap_world()};});
        CHECK(owner.running());
    }
}

TEST_CASE("UDP selection bootstrap rejects channel reference schema and budget contradictions before commit") {
    for (unsigned bad=0;bad<15;++bad) {
        CAPTURE(bad); auto plan=public_bootstrap_plan(); auto config=authority_skeleton();
        switch (bad) {
        case 0: plan.gri.channel.channel=0; break;
        case 1: plan.gri.channel.channel=2048; break;
        case 2: plan.gri.channel.channel=7; break;
        case 3: plan.gri.channel.channel=8; break;
        case 4: plan.gri.channel.channel=9; break;
        case 5: plan.gri.archetype_local_net_index=-1; break;
        case 6: plan.game_class_local_net_index=-1; break;
        case 7: plan.hud_local_net_index=-1; break;
        case 8: plan.gri_exclusive_bound=0; break;
        case 9: plan.game_class_field_id=73; break;
        case 10: plan.controller_exclusive_bound=293; break;
        case 11: plan.client_set_hud_field_id=294; break;
        case 12: plan.client_set_hud_field_id=bootstrap_links().controller_pri_field_id; break;
        case 13: config.transport.max_packet_bytes=8; plan.gri.position={1048575,1048575,1048575}; break;
        case 14: plan.gri.rotation=wire::actor::Rotation{1,0,0}; break; // Unquantized angle, not a budget failure.
        }
        udp::LocalUdpOwner owner(0,config,[]{return 100.0;},5ms);
        error([&]{bootstrap_start(owner,plan,
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional{bootstrap_world()};});},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running()); if (owner.running()) {owner.stop();continue;}
        owner.start([](const session::SessionId&){return true;}); CHECK(owner.running());
    }
}

TEST_CASE("UDP selection bootstrap never completes after publication failure revocation or stop") {
    for (unsigned terminal=0;terminal<5;++terminal) {
        CAPTURE(terminal); auto plan=public_bootstrap_plan();
        if (terminal==0) plan.gri.archetype_local_net_index=49101; // Outside HG range.
        if (terminal==1) plan.game_class_local_net_index=533; // Outside contained HGC range.
        if (terminal==4) plan.hud_local_net_index=49101; // Outside HG range.
        LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},{},plan);
        const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
        if (terminal==0) {
            publication_login(f,login,peer,client); REQUIRE(publication_packets(peer,client,6).logical.size()==6);
            peer.send(f.owner.port(),bootstrap_join(client));
            static_cast<void>(controller_output(peer,client,200ms,4));
            CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
            CHECK_FALSE(f.service->authorize_udp(public_id)); CHECK_FALSE(f.owner.diagnostics().gri_sent);
        } else {
            const auto opens=bootstrap_openings(f,login,peer,client);
            if (terminal==3) f.owner.stop();
            else {
                if (terminal==2) f.verifier.active=false;
                peer.send(f.owner.port(),links_ack(client,5,bootstrap_opening_acks(opens)));
            }
            const auto output=links_output(peer,client,200ms,3);
            if (terminal==4) {
                REQUIRE(output.bunches.size()==3); REQUIRE(output.packet_ids.size()==3);
                peer.send(f.owner.port(),links_ack(client,6,output.packet_ids));
                CHECK(links_output(peer,client).bunches.empty());
            }
            CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
            CHECK_FALSE(f.service->authorize_udp(public_id));
        }
        CHECK_FALSE(f.owner.diagnostics().hud_sent); CHECK_FALSE(f.owner.diagnostics().hud_acked);
    }
}

TEST_CASE("UDP selection bootstrap absent plan preserves three actor openings and old link payloads") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),public_links_plan());
    const auto login=f.login(); Peer peer; session::Context client(session::Role::client,public_id,public_key);
    const auto opens=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opens)));
    links_literals(links_output(peer,client,2000ms));
    const auto diagnostic=f.owner.diagnostics();
    CHECK_FALSE(diagnostic.gri_sent); CHECK_FALSE(diagnostic.gri_open_acked);
    CHECK_FALSE(diagnostic.game_class_sent); CHECK_FALSE(diagnostic.game_class_acked);
    CHECK_FALSE(diagnostic.hud_sent); CHECK_FALSE(diagnostic.hud_acked);
}


namespace {
udp::ReadyMatchPlan ready_match_plan() {
    // Conditional fixture schema supplied explicitly, no native acceptance:
    // Ready280/294; PRIReady30/Waiting31/87; GRIcounter48/49/51 + begun29/73.
    return {280,294,30,31,87,48,49,51,29,73,control::Endian::little,
        {true,1,0.55F,0,0,10,30.0F},{0.0F,0.0F,false,0,0,-1},
        true,true,true,false,{true,true,true,true,true,true,true,true,true,true,true,true,true}};
}
udp::PriIdentityDispatchPlan ready_identity_plan() {
    auto plan=public_identity_plan();plan.exclusive_bound=87;return plan;
}
udp::ReadyAccountFacts loaded_ready_facts() { return {true,true,true,true,false,false}; }
Bytes ready_request(session::Context& client,std::uint16_t pid,std::uint16_t sequence,
                    std::string_view literal,std::size_t logical_bits,std::uint16_t channel=7) {
    wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=channel;
    bunch.sequence=sequence;bunch.payload=hex(literal);bunch.payload_bits=logical_bits;
    wire::Packet packet;packet.packet_id=pid;
    wire::PacketRecord record;record.bunch=std::move(bunch);packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,256));
}
struct ReadyBootstrap {
    std::uint16_t next_packet=5;
    bool initial_counts_seen=false;
};
void ready_ack(LoginAuthorityFixture& fixture,Peer& peer,session::Context& client,
               ReadyBootstrap& state,const std::vector<std::uint16_t>& ids) {
    peer.send(fixture.owner.port(),links_ack(client,state.next_packet++,ids));
}
ReadyBootstrap ready_bootstrap(LoginAuthorityFixture& fixture,const auth::AuthReply& login,
                               Peer& peer,session::Context& client) {
    ReadyBootstrap state;
    const auto opens=bootstrap_openings(fixture,login,peer,client);
    ready_ack(fixture,peer,client,state,bootstrap_opening_acks(opens));
    std::vector<std::uint16_t> retried_opens;
    const auto props=links_output(peer,client,2000ms,2,nullptr,&retried_opens);
    REQUIRE(props.bunches.size()==2);
    bool identity=false,game_class=false;
    for (const auto& bunch:props.bunches) {
        if (bunch.channel==9) {
            identity=true;CHECK(bunch.sequence==103);CHECK(bunch.payload_bits==135);
        } else if (bunch.channel==10) {
            game_class=true;CHECK(bunch.sequence==113);CHECK(bunch.payload_bits==38);
        }
    }
    CHECK(identity);CHECK(game_class);
    auto ack=props.packet_ids;ack.insert(ack.end(),retried_opens.begin(),retried_opens.end());
    ready_ack(fixture,peer,client,state,ack);
    const auto linked=links_output(peer,client,2000ms,2,nullptr,&retried_opens);
    REQUIRE(linked.bunches.size()==2);
    ack=linked.packet_ids;ack.insert(ack.end(),retried_opens.begin(),retried_opens.end());
    ready_ack(fixture,peer,client,state,ack);
    const auto hud=links_output(peer,client,2000ms,1,nullptr,&retried_opens);
    bootstrap_hud_literal(hud);
    ack=hud.packet_ids;ack.insert(ack.end(),retried_opens.begin(),retried_opens.end());
    ready_ack(fixture,peer,client,state,ack);
    REQUIRE(wait_until([&]{return fixture.owner.diagnostics().hud_acked;}));
    const auto initial_counts=links_output(peer,client,200ms,1);
    if (!initial_counts.bunches.empty()) {
        REQUIRE(initial_counts.bunches.size()==1);
        const auto& bunch=initial_counts.bunches.front();
        CHECK(bunch.channel==10);CHECK(bunch.sequence==114);CHECK(bunch.reliable);
        CHECK(bunch.payload_bits==114);
        CHECK(bunch.payload==hex("30000000400c00000030ffffffff03"));
        ready_ack(fixture,peer,client,state,initial_counts.packet_ids);
        state.initial_counts_seen=true;
    }
    return state;
}
void ready_echo(const ControllerOutput& output,bool value,std::uint32_t sequence=104) {
    REQUIRE(output.bunches.size()==1);REQUIRE(output.packet_ids.size()==1);
    const auto& bunch=output.bunches.front();
    CHECK(bunch.channel==9);CHECK(bunch.sequence==sequence);CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2);CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);CHECK_FALSE(bunch.control);
    // tag30/bound87 +bool, tag31/bound87 +boolWaiting=true: two7-bit records.
    CHECK(bunch.payload_bits==14);CHECK(bunch.payload==hex(value ? "de2f" : "9e2f"));
}
void ready_counts(const ControllerOutput& output,std::string_view literal,std::uint32_t sequence) {
    REQUIRE(output.bunches.size()==1);REQUIRE(output.packet_ids.size()==1);
    const auto& bunch=output.bunches.front();
    CHECK(bunch.channel==10);CHECK(bunch.sequence==sequence);CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2);CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);CHECK_FALSE(bunch.control);
    CHECK(bunch.payload_bits==114);CHECK(bunch.payload==hex(literal));
}
void ready_begin(const ControllerOutput& output,std::uint32_t sequence) {
    REQUIRE(output.bunches.size()==1);REQUIRE(output.packet_ids.size()==1);
    const auto& bunch=output.bunches.front();
    CHECK(bunch.channel==10);CHECK(bunch.sequence==sequence);CHECK(bunch.reliable);
    CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);CHECK_FALSE(bunch.control);
    CHECK(bunch.payload_bits==7);CHECK(bunch.payload==hex("5d"));
}
void ready_start(udp::LocalUdpOwner& owner,std::optional<udp::ReadyMatchPlan> plan,
                 udp::LocalUdpOwner::ReadyFactsProvider source={},
                 std::optional<udp::InitialControllerPlan> controller=public_controller_plan(),
                 std::optional<udp::InitialPawnPlan> pawn=public_pawn_plan(),
                 std::optional<udp::InitialPriPlan> pri=public_pri_plan(),
                 std::optional<udp::InitialActorLinksPlan> links=bootstrap_links(),
                 std::optional<udp::PriIdentityDispatchPlan> identity=ready_identity_plan(),
                 std::optional<udp::SelectionBootstrapPlan> bootstrap=public_bootstrap_plan(),
                 bool publication=true) {
    udp::LocalUdpOwner::PublicationProvider publisher;
    if (publication) publisher=[](const session::SessionId&,const control::posthello::Login&) {
        return std::optional{bootstrap_world()};
    };
    owner.start([](const session::SessionId&){return true;},
        [](const session::SessionId&,const control::posthello::RawString&){return true;},publisher,
        controller,pawn,pri,links,{},{},{},{},{},identity,bootstrap,plan,source);
}
}

TEST_CASE("UDP Ready echoes on PRI then starts one world only after real echo ACK and all four loaded facts") {
    std::atomic<int> reads{0};LoginAuthorityFixture* current=nullptr;
    auto source=[&](const session::SessionId& id)->std::optional<udp::ReadyAccountFacts> {
        ++reads;CHECK(id==public_id);
        CHECK((current && current->service->account_for_udp(id).has_value()));
        return loaded_ready_facts(); // Explicit synthetic loaded data domains, not native async objects.
    };
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan(),source);
    current=&f;
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);REQUIRE(flow.initial_counts_seen);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
    const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,true);
    CHECK(reads.load()==0);CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    CHECK_FALSE(f.owner.diagnostics().pri_ready_acked);
    ready_ack(f,peer,client,flow,echo.packet_ids);
    const auto countdown=links_output(peer,client,2000ms,1);
    ready_counts(countdown,"30000000400c000000302b00000000",115);
    CHECK(reads.load()>0);CHECK(f.owner.diagnostics().pri_ready_acked);
    ready_ack(f,peer,client,flow,countdown.packet_ids);
    f.udp_now=110.0;f.monotonic=110;
    const auto begun=links_output(peer,client,2000ms,1);ready_begin(begun,116);
    CHECK(wait_until([&]{return f.owner.diagnostics().match_start_requested;}));
    CHECK(f.owner.diagnostics().gri_match_begun_sent);
    CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
    ready_ack(f,peer,client,flow,begun.packet_ids);
    CHECK(wait_until([&]{return f.owner.diagnostics().gri_match_begun_acked;}));
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,2,"1803",10));
    CHECK(links_output(peer,client).bunches.empty());
    f.udp_now=111.0;f.monotonic=111;
    CHECK(links_output(peer,client).bunches.empty());
    CHECK(f.service->authorize_udp(public_id));
}

TEST_CASE("UDP Ready empty provider or any unavailable data domain never fabricates synchronization or match start") {
    for (unsigned missing=0;missing<7;++missing) {
        CAPTURE(missing);
        auto facts=loaded_ready_facts();
        if (missing==1) facts.owned_collection_loaded=false;
        if (missing==2) facts.unranked_stats_available=false;
        if (missing==3) facts.item_stats_loaded=false;
        if (missing==4) facts.achievements_loaded=false;
        if (missing==5) facts.purchasing=true;
        if (missing==6) facts.saving=true;
        udp::LocalUdpOwner::ReadyFactsProvider source;
        if (missing!=0) source=[facts](const session::SessionId&) { return std::optional{facts}; };
        LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
            ready_identity_plan(),public_bootstrap_plan(),ready_match_plan(),source);
        const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
        auto flow=ready_bootstrap(f,login,peer,client);
        peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
        const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,true);
        ready_ack(f,peer,client,flow,echo.packet_ids);
        f.udp_now=120.0;f.monotonic=120;
        CHECK(links_output(peer,client).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().match_start_requested);
        CHECK_FALSE(f.owner.diagnostics().gri_match_begun_sent);
        CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
        CHECK(f.service->authorize_udp(public_id));
    }
}

TEST_CASE("UDP Ready known prefix last value wins while unknown suffix and duplicate requests stay opaque or idempotent") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan());
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);
    // unknown279 followed by Ready280,true: 279 |(0x318<<9). No suffix scan.
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"173106",19));
    CHECK(links_output(peer,client).bunches.empty());CHECK_FALSE(f.owner.diagnostics().ready_rpc_received);
    // Readytrue then Readyfalse at cursor10: 0x318 |(0x118<<10), logical20.
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,2,"186304",20));
    const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,false);
    ready_ack(f,peer,client,flow,echo.packet_ids);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,2,"186304",20));
    CHECK(links_output(peer,client).bunches.empty());
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,3,"1801",10));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().match_start_requested);CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP Ready uses a relative double world epoch and refreshes saving state during countdown") {
    std::atomic<bool> saving{false};auto plan=ready_match_plan();
    auto source=[&](const session::SessionId&) {
        auto facts=loaded_ready_facts();facts.saving=saving.load();return std::optional{facts};
    };
    constexpr double epoch=1000000000000.0;
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),plan,source,epoch);
    plan.ready_rpc_field_id=1;plan.rules.countdown_seconds=77;plan.initial_state.countdown_remaining=77;
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);REQUIRE(flow.initial_counts_seen);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
    const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,true);
    ready_ack(f,peer,client,flow,echo.packet_ids);
    const auto ten=links_output(peer,client,2000ms,1);
    ready_counts(ten,"30000000400c000000302b00000000",115);
    ready_ack(f,peer,client,flow,ten.packet_ids);
    f.udp_now=epoch+4.75;
    const auto five=links_output(peer,client,2000ms,1);
    ready_counts(five,"30000000400c000000301700000000",116);
    ready_ack(f,peer,client,flow,five.packet_ids);
    saving=true;f.udp_now=epoch+20.0;f.monotonic=static_cast<std::int64_t>(epoch)+20;
    CHECK(links_output(peer,client).bunches.empty());CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    saving=false;f.udp_now=epoch+21.0;f.monotonic=static_cast<std::int64_t>(epoch)+21;
    const auto begun=links_output(peer,client,2000ms,1);ready_begin(begun,117);
    CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
}

TEST_CASE("UDP Ready provider executes outside Use and revoked authorization discards its returned facts") {
    Gate gate;LoginAuthorityFixture* current=nullptr;
    auto source=[&](const session::SessionId& id)->std::optional<udp::ReadyAccountFacts> {
        CHECK((current && current->service->account_for_udp(id).has_value()));
        gate.enter();return loaded_ready_facts();
    };
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan(),source);
    GateRelease release{gate}; // Releases before fixture destructor even after a fatal assertion.
    current=&f;
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
    const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,true);
    ready_ack(f,peer,client,flow,echo.packet_ids);
    REQUIRE(gate.wait_entered());
    f.verifier.active=false;
    auto revoked=std::async(std::launch::async,[&]{return f.service->authorize_udp(public_id);});
    const auto status=revoked.wait_for(500ms);
    gate.release(); // Always unblock before any fatal assertion/destruction.
    REQUIRE(status==std::future_status::ready);CHECK_FALSE(revoked.get());
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().match_start_requested);CHECK_FALSE(f.owner.diagnostics().gri_match_begun_sent);
}

TEST_CASE("UDP Ready rejects truncated known bool and never discovers a request on another actor") {
    for (const bool truncated : std::array<bool,2>{false,true}) {
        LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
            public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
            ready_identity_plan(),public_bootstrap_plan(),ready_match_plan());
        const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
        auto flow=ready_bootstrap(f,login,peer,client);
        peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,truncated ? "1801" : "1803",
            truncated ? 9 : 10,truncated ? 7 : 8));
        CHECK(links_output(peer,client).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().ready_rpc_received);CHECK_FALSE(f.owner.diagnostics().pri_ready_sent);
        if (truncated) CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        else CHECK_FALSE(f.owner.diagnostics().closed);
    }
}

TEST_CASE("UDP Ready startup validates metadata premises cache ranges and shared world state before retryable worker start") {
    using Eligibility=udp::ReadyMatchEligibility;
    const std::array premise_fields{&Eligibility::primary_package_map,&Eligibility::controller_cache_correct,
        &Eligibility::pri_cache_correct,&Eligibility::gri_cache_correct,&Eligibility::ready_rpc_eligible,
        &Eligibility::ready_argument_bool_net_correct,&Eligibility::pri_ready_property_eligible,
        &Eligibility::pri_waiting_property_eligible,&Eligibility::gri_waiting_to_ready_property_eligible,
        &Eligibility::gri_waiting_for_property_eligible,&Eligibility::gri_countdown_property_eligible,
        &Eligibility::gri_match_begun_property_eligible,&Eligibility::integer_archive_order_correct};
    for (std::size_t i=0;i<premise_fields.size();++i) {
        auto plan=ready_match_plan();plan.eligibility.*premise_fields[i]=false;
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        error([&]{ready_start(owner,plan);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        ready_start(owner,ready_match_plan());CHECK(owner.running());
    }
    for (unsigned bad=0;bad<16;++bad) {
        CAPTURE(bad);auto plan=ready_match_plan();
        if (bad==0) plan.ready_rpc_field_id=294;
        if (bad==1) plan.controller_exclusive_bound=8;
        if (bad==2) plan.ready_rpc_field_id=42;
        if (bad==3) plan.ready_rpc_field_id=3;
        if (bad==4) plan.pri_ready_field_id=plan.pri_waiting_field_id;
        if (bad==5) plan.pri_ready_field_id=2;
        if (bad==6) plan.pri_exclusive_bound=8;
        if (bad==7) plan.gri_exclusive_bound=72;
        if (bad==8) plan.gri_waiting_for_field_id=31;
        if (bad==9) plan.gri_match_begun_field_id=plan.gri_countdown_field_id;
        if (bad==10) plan.gri_countdown_field_id=73;
        if (bad==11) plan.integer_order=static_cast<control::Endian>(7);
        if (bad==12) plan.rules.min_percent_ready=1.1F;
        if (bad==13) plan.rules.max_delay_seconds=-1;
        if (bad==14) plan.initial_state.waiting_for=-1;
        if (bad==15) plan.initial_state.pending_start_time=1.0F;
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        error([&]{ready_start(owner,plan);},udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
    }
    for (unsigned missing=0;missing<7;++missing) {
        auto controller=std::optional{public_controller_plan()};auto pawn=std::optional{public_pawn_plan()};
        auto pri=std::optional{public_pri_plan()};auto links=std::optional{bootstrap_links()};
        auto identity=std::optional{ready_identity_plan()};auto bootstrap=std::optional{public_bootstrap_plan()};
        if (missing==0) controller.reset();if (missing==1) pawn.reset();if (missing==2) pri.reset();
        if (missing==3) links.reset();if (missing==4) identity.reset();if (missing==5) bootstrap.reset();
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        error([&]{ready_start(owner,ready_match_plan(),{},controller,pawn,pri,links,identity,bootstrap,missing!=6);},
            udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
    }
    udp::LocalUdpOwner no_plan(0,authority_skeleton(),[]{return 100.0;},5ms);
    error([&]{ready_start(no_plan,std::nullopt,[](const session::SessionId&){return std::optional{loaded_ready_facts()};});},
        udp::LocalUdpErrorCode::invalid_configuration);
}

TEST_CASE("UDP Ready absent plan preserves opaque Ready and never adds model counters to prior bootstrap") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan());
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);
    CHECK_FALSE(flow.initial_counts_seen);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().ready_rpc_received);CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    CHECK_FALSE(f.owner.diagnostics().closed);
}


namespace {
Bytes ready_start_request(session::Context& client,std::uint16_t pid,std::uint16_t sequence,bool followed_by_ready) {
    wire::BitWriter bits;bits.write_bounded(4,294);
    const auto u32=[&](std::uint32_t value) {
        for (unsigned shift=0;shift<32;shift+=8)
            bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(value>>shift)},8);
    };
    bits.write_bit(true);u32(37);
    constexpr std::string_view instance="20000000-0000-0000-0000-000000000001";
    for (const auto c:instance)
        bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(c)},8);
    bits.write_bits(std::array<std::uint8_t,1>{0},8);
    bits.write_bit(true);u32(256);bits.write_bit(true);u32(0xffffffffU);
    if (followed_by_ready) { bits.write_bounded(280,294);bits.write_bit(true); }
    wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=7;bunch.sequence=sequence;
    bunch.payload_bits=bits.bit_count();bunch.payload=bits.payload_bytes();
    wire::Packet packet;packet.packet_id=pid;wire::PacketRecord record;record.bunch=std::move(bunch);
    packet.records.push_back(std::move(record));return client.encode(wire::encode_packet(packet,256));
}
udp::StartMechDispatchPlan ready_start_plan() {
    auto plan=public_start_mech_plan();plan.controller_exclusive_bound=294;plan.pri_exclusive_bound=87;
    return plan;
}
}

TEST_CASE("UDP Ready parses the exact known suffix after StartMech even without an admission plan") {
    std::atomic<int> mech_reads{0};
    auto provider=[&](const session::SessionId&,const wire::rpc::StartMechArguments& request,std::uint8_t previous) {
        ++mech_reads;CHECK(request.mech_instance_id=="20000000-0000-0000-0000-000000000001");
        CHECK(request.primary_weapon==256);CHECK(request.holo_taunt_type==-1);
        return std::optional{public_mech_result(previous)};
    };
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),ready_start_plan(),provider,{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan());
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);
    peer.send(f.owner.port(),ready_start_request(client,flow.next_packet++,1,true));
    const auto selected=links_output(peer,client,2000ms,1);REQUIRE(selected.bunches.size()==1);
    CHECK(selected.bunches.front().channel==9);CHECK(selected.bunches.front().sequence==104);
    CHECK(selected.bunches.front().payload_bits==551); // field5 uses7bits plus544-member payload.
    CHECK(mech_reads.load()==1);
    ready_ack(f,peer,client,flow,selected.packet_ids);
    const auto echo=links_output(peer,client,2000ms,1);ready_echo(echo,true,105);
    CHECK(f.owner.diagnostics().ready_rpc_received);CHECK_FALSE(f.owner.diagnostics().match_start_requested);
}

TEST_CASE("UDP Ready pending world stage three marks ready but defers validated ClientRestart until world action") {
    auto validate=public_spawn_validation_plan();validate.validate_rpc_field_id=249;
    validate.validated_rpc_field_id=250;validate.failed_rpc_field_id=251;validate.controller_exclusive_bound=294;
    auto admission=public_spawn_admission_plan();admission.stage_rpc_field_id=281;
    admission.client_restart_rpc_field_id=83;admission.controller_exclusive_bound=294;
    auto mech=[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
        return std::optional{public_mech_result(previous)};
    };
    auto validator=[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
        return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
    };
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),ready_start_plan(),mech,validate,validator,admission,
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan(),
        [](const session::SessionId&){return std::optional{loaded_ready_facts()};});
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);
    peer.send(f.owner.port(),ready_start_request(client,flow.next_packet++,1,false));
    const auto selected=links_output(peer,client,2000ms,1);REQUIRE(selected.bunches.size()==1);
    CHECK(selected.bunches.front().sequence==104);ready_ack(f,peer,client,flow,selected.packet_ids);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,2,"f9",8));
    const auto validated=links_output(peer,client,2000ms,1);REQUIRE(validated.bunches.size()==1);
    CHECK(validated.bunches.front().channel==7);CHECK(validated.bunches.front().sequence==45);
    CHECK(validated.bunches.front().payload_bits==8);CHECK(validated.bunches.front().payload==hex("fa"));
    ready_ack(f,peer,client,flow,validated.packet_ids);
    // Original normal stage3 path; no extra recovery ServerRestart RPC25 or Ready280.
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,3,"190f00000000",42));
    const auto waiting_echo=links_output(peer,client,2000ms,1);ready_echo(waiting_echo,true,105);
    CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
    CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    ready_ack(f,peer,client,flow,waiting_echo.packet_ids);
    const auto countdown=links_output(peer,client,2000ms,1);
    ready_counts(countdown,"30000000400c000000302b00000000",115);
    ready_ack(f,peer,client,flow,countdown.packet_ids);
    f.udp_now=110.0;f.monotonic=110;
    bool begun_seen=false,restart_seen=false;
    for (unsigned batch=0;batch<3 && !restart_seen;++batch) {
        const auto output=links_output(peer,client,2000ms,1);REQUIRE(output.bunches.size()==1);
        const auto& bunch=output.bunches.front();
        if (bunch.channel==10) {
            ready_begin(output,116);begun_seen=true;
        } else {
            CHECK(begun_seen);CHECK(bunch.channel==7);CHECK(bunch.sequence==46);
            CHECK(bunch.reliable);CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);CHECK_FALSE(bunch.control);
            CHECK(bunch.payload_bits==21);CHECK(bunch.payload==hex("532300")); // tag83+presence+Pawn8.
            restart_seen=true;
        }
        ready_ack(f,peer,client,flow,output.packet_ids);
    }
    REQUIRE(begun_seen);REQUIRE(restart_seen);
    CHECK(f.owner.diagnostics().match_start_requested);CHECK(f.owner.diagnostics().client_restart_sent);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,4,"190f00000000",42));
    CHECK(links_output(peer,client).bunches.empty());
}


TEST_CASE("UDP Ready false after active countdown cancels only after its real PRI ACK and never starts at the old deadline") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},
        ready_identity_plan(),public_bootstrap_plan(),ready_match_plan(),
        [](const session::SessionId&){return std::optional{loaded_ready_facts()};});
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    auto flow=ready_bootstrap(f,login,peer,client);REQUIRE(flow.initial_counts_seen);
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,1,"1803",10));
    const auto yes=links_output(peer,client,2000ms,1);ready_echo(yes,true,104);
    ready_ack(f,peer,client,flow,yes.packet_ids);
    const auto active=links_output(peer,client,2000ms,1);
    ready_counts(active,"30000000400c000000302b00000000",115);
    ready_ack(f,peer,client,flow,active.packet_ids);
    // Keep the fake world clock at100: no incidental intermediate counter frame
    // can alter the independently specified cancellation sequence below.
    peer.send(f.owner.port(),ready_request(client,flow.next_packet++,2,"1801",10));
    const auto no=links_output(peer,client,2000ms,1);ready_echo(no,false,105);
    CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    CHECK(links_output(peer,client).bunches.empty()); // false echo not ACKed yet.
    ready_ack(f,peer,client,flow,no.packet_ids);
    const auto cancelled=links_output(peer,client,2000ms,1);
    // 48|(1<<6),49|0,51|(uint32(-1)<<6): waitingReady1,waitingFor0,countdown-1.
    ready_counts(cancelled,"70000000400c00000030ffffffff03",116);
    ready_ack(f,peer,client,flow,cancelled.packet_ids);
    f.udp_now=120.0;f.monotonic=120;
    CHECK(links_output(peer,client).bunches.empty());
    CHECK_FALSE(f.owner.diagnostics().match_start_requested);
    CHECK_FALSE(f.owner.diagnostics().gri_match_begun_sent);
    CHECK_FALSE(f.owner.diagnostics().client_restart_sent);
    CHECK(f.service->authorize_udp(public_id));CHECK_FALSE(f.owner.diagnostics().closed);
}


namespace {
// Exact known fields from retained package metadata; this fixture does not
// assert that an observed native packet contained this coalesced stream.
Bytes active_index_coalesced_request(session::Context& client,control::Endian order,
    bool present,bool truncated,bool behind_unknown) {
    wire::BitWriter bits;
    const auto u32=[&](std::uint32_t value) {
        for (unsigned byte=0;byte<4;++byte) {
            const auto shift=order==control::Endian::little?byte*8:(3-byte)*8;
            bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(value>>shift)},8);
        }
    };
    bits.write_bounded(253,294);
    bits.write_bit(true);u32(37);
    for (const auto c:std::string_view("20000000-0000-0000-0000-000000000001"))
        bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(c)},8);
    bits.write_bits(std::array<std::uint8_t,1>{0},8);
    bits.write_bit(true);u32(256);bits.write_bit(true);u32(0xffffffffU);
    if (behind_unknown) bits.write_bounded(215,294);
    bits.write_bounded(214,294);bits.write_bit(present);
    if (present) {
        if (truncated) bits.write_bits(std::array<std::uint8_t,1>{0x04},8);
        else u32(0x01020304U);
    }
    if (!truncated) bits.write_bounded(249,294);
    wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=7;bunch.sequence=1;
    bunch.payload=bits.payload_bytes();bunch.payload_bits=bits.bit_count();
    wire::Packet packet;packet.packet_id=6;wire::PacketRecord record;record.bunch=std::move(bunch);
    packet.records.push_back(std::move(record));return client.encode(wire::encode_packet(packet,256));
}
udp::StartMechDispatchPlan active_index_start_plan(bool enabled=true) {
    auto plan=public_start_mech_plan();plan.controller_rpc_field_id=253;plan.controller_exclusive_bound=294;
    if (enabled) plan.active_item_index=udp::ActiveItemIndexRpcPlan{214,true,true};
    return plan;
}
udp::SpawnValidationDispatchPlan active_index_validation_plan() {
    auto plan=public_spawn_validation_plan();plan.validate_rpc_field_id=249;
    plan.validated_rpc_field_id=250;plan.failed_rpc_field_id=251;plan.controller_exclusive_bound=294;
    return plan;
}
ControllerOutput active_index_openings(LoginAuthorityFixture& f,const auth::AuthReply& login,
    Peer& peer,session::Context& client,control::Endian order) {
    if (order==control::Endian::little) return links_openings(f,login,peer,client);
    // Independent big-endian control archives for the same authenticated harness.
    authority_select(f,login);
    peer.send(f.owner.port(),authority_request(client,0,1,hex("1a00000007"),true));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("1b00000000")});peer.drain();
    peer.send(f.owner.port(),authority_request(client,1,2,hex("1c8d3f36c3")));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("1d")});peer.drain();
    peer.send(f.owner.port(),authority_request(client,2,3,hex("000000000800000014fedcba98")));
    REQUIRE(authority_messages(peer,client)==std::vector<Bytes>{hex("030000000b00000009313233344142434400")});peer.drain();
    Bytes response=hex("0500000025");
    const auto guid=login.result.at("UserId").get<std::string>();
    response.insert(response.end(),guid.begin(),guid.end());response.push_back(0);
    const auto tail=hex("00000000010203041234567880000000ffffffff");
    response.insert(response.end(),tail.begin(),tail.end());
    peer.send(f.owner.port(),authority_request(client,3,4,response));
    REQUIRE(publication_packets(peer,client).logical.size()==2);
    REQUIRE(wait_until([&]{return f.owner.diagnostics().world_published;}));
    peer.send(f.owner.port(),authority_request(client,4,5,
        hex("08010203041234567880000000ffffffff0000000209")));
    const auto out=controller_output(peer,client,2000ms,3);
    REQUIRE(out.bunches.size()==3);REQUIRE(out.packet_ids.size()==3);
    CHECK(out.bunches[0].channel==7);CHECK(out.bunches[1].channel==9);CHECK(out.bunches[2].channel==8);
    return out;
}
}
TEST_CASE("UDP ActiveItemIndex coalesced StartMech index Validate reaches validation only after selected PRI ACK") {
    for (unsigned mode=0;mode<3;++mode) {
        CAPTURE(mode);const auto order=mode==1?control::Endian::big:control::Endian::little;
        auto config=authority_skeleton();config.endian=order;
        std::atomic<int> checked{0};
        LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),config,
            public_pawn_plan(),public_pri_plan(),bootstrap_links(),active_index_start_plan(),
            [](const session::SessionId&,const wire::rpc::StartMechArguments& args,std::uint8_t previous) {
                CHECK(args.primary_weapon==256);CHECK(args.holo_taunt_type==-1);
                return std::optional{public_mech_result(previous)};
            },active_index_validation_plan(),[&](const session::SessionId&,
                const wire::rpc::StartMechArguments&,std::uint8_t setup) {
                CHECK(setup==42);++checked;return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            });
        const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
        const auto opened=active_index_openings(f,login,peer,client,order);
        peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
        std::vector<std::uint16_t> received_opens;
        const auto links=links_output(peer,client,2000ms,2,nullptr,&received_opens);
        REQUIRE(links.bunches.size()==2);
        CHECK(links.bunches[0].payload_bits==42);CHECK(links.bunches[0].payload==hex("0326a0400400"));
        CHECK(links.bunches[1].payload_bits==30);CHECK(links.bunches[1].payload==hex("79004f00"));
        peer.send(f.owner.port(),active_index_coalesced_request(client,order,mode!=2,false,false));
        const auto property=links_output(peer,client,2000ms,1,nullptr,&received_opens);
        start_mech_property_literal(property);CHECK(checked==0);CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent);
        auto ids=links.packet_ids;ids.insert(ids.end(),property.packet_ids.begin(),property.packet_ids.end());
        ids.insert(ids.end(),received_opens.begin(),received_opens.end());
        peer.send(f.owner.port(),links_ack(client,7,ids));
        const auto reply=links_output(peer,client,2000ms,1);
        CHECK(reply.bunches.size()==1);
        if (reply.bunches.size()==1) validation_literal(reply,0xfa,8);
        CHECK(checked==1);CHECK(f.owner.diagnostics().spawn_validation_requested);
        CHECK(f.owner.diagnostics().spawn_validation_sent);CHECK_FALSE(f.owner.diagnostics().closed);
    }
}
TEST_CASE("UDP ActiveItemIndex truncated known int closes without calling selection validation") {
    std::atomic<int> checked{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),active_index_start_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t p) {
            return std::optional{public_mech_result(p)};
        },active_index_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            ++checked;return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    const auto opened=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
    REQUIRE(links_output(peer,client,2000ms,2).bunches.size()==2);
    peer.send(f.owner.port(),active_index_coalesced_request(client,control::Endian::little,true,true,false));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));CHECK(checked==0);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_requested);CHECK_FALSE(f.owner.diagnostics().spawn_validation_sent);
}
TEST_CASE("UDP ActiveItemIndex behind unknown prefix never scans through it to Validate") {
    std::atomic<int> checked{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),active_index_start_plan(),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t p) {
            return std::optional{public_mech_result(p)};
        },active_index_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            ++checked;return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    const auto opened=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
    const auto links=links_output(peer,client,2000ms,2);REQUIRE(links.bunches.size()==2);
    peer.send(f.owner.port(),active_index_coalesced_request(client,control::Endian::little,true,false,true));
    const auto property=links_output(peer,client,2000ms,1);start_mech_property_literal(property);
    auto ids=links.packet_ids;ids.insert(ids.end(),property.packet_ids.begin(),property.packet_ids.end());
    peer.send(f.owner.port(),links_ack(client,7,ids));
    CHECK(links_output(peer,client,200ms).bunches.empty());CHECK(checked==0);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_requested);CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP ActiveItemIndex absent optional schema retains legacy unknown suffix behavior") {
    std::atomic<int> checked{0};
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),active_index_start_plan(false),
        [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t p) {
            return std::optional{public_mech_result(p)};
        },active_index_validation_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
            ++checked;return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
        });
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    const auto opened=links_openings(f,login,peer,client);
    peer.send(f.owner.port(),links_ack(client,5,links_opening_acks(opened)));
    const auto links=links_output(peer,client,2000ms,2);REQUIRE(links.bunches.size()==2);
    peer.send(f.owner.port(),active_index_coalesced_request(client,control::Endian::little,true,false,false));
    const auto property=links_output(peer,client,2000ms,1);start_mech_property_literal(property);
    auto ids=links.packet_ids;ids.insert(ids.end(),property.packet_ids.begin(),property.packet_ids.end());
    peer.send(f.owner.port(),links_ack(client,7,ids));
    CHECK(links_output(peer,client,200ms).bunches.empty());CHECK(checked==0);
    CHECK_FALSE(f.owner.diagnostics().spawn_validation_requested);CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP ActiveItemIndex preflight rejects ineligible out of range and every configured Controller collision") {
    for (unsigned invalid=0;invalid<13;++invalid) {
        CAPTURE(invalid);
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        auto start=active_index_start_plan();start.pri_exclusive_bound=87;
        switch(invalid) {
        case 0:start.active_item_index->rpc_eligible=false;break;
        case 1:start.active_item_index->argument_eligible=false;break;
        case 2:start.active_item_index->field_id=294;break;
        case 3:start.active_item_index->field_id=253;break;
        case 4:start.active_item_index->field_id=3;break;
        case 5:start.active_item_index->field_id=5;break;
        case 6:start.active_item_index->field_id=42;break;
        case 7:start.active_item_index->field_id=280;break;
        case 8:start.active_item_index->field_id=249;break;
        case 9:start.active_item_index->field_id=250;break;
        case 10:start.active_item_index->field_id=251;break;
        case 11:start.active_item_index->field_id=281;break;
        case 12:start.active_item_index->field_id=83;break;
        }
        udp::SpawnAdmissionDispatchPlan admission{281,83,294,{true,true,true,true,true,true}};
        error([&]{owner.start([](const session::SessionId&){return true;},
            [](const session::SessionId&,const control::posthello::RawString&){return true;},
            [](const session::SessionId&,const control::posthello::Login&){return std::optional{bootstrap_world()};},
            public_controller_plan(),public_pawn_plan(),public_pri_plan(),bootstrap_links(),start,
            [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t p) {
                return std::optional{public_mech_result(p)};
            },active_index_validation_plan(),[](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t) {
                return udp::SpawnSelectionValidationResult{{3,3,3,3,3},7};
            },admission,ready_identity_plan(),public_bootstrap_plan(),ready_match_plan());
        },udp::LocalUdpErrorCode::invalid_configuration);
        CHECK_FALSE(owner.running());
        CHECK_NOTHROW(owner.start([](const session::SessionId&){return true;}));owner.stop();
    }
}


namespace {
// SYNTHETIC caller metadata: distinct local3..6 in Root/netcount7, not native
// class IDs or CDO observation. Field widths follow independently retained schemas.
udp::InventoryBootstrapPlan inventory_plan() {
    const auto actor=[](std::uint16_t channel,std::uint32_t seed,std::int32_t local) {
        return udp::InitialPawnPlan{{channel,seed,0,false},
            {0x01020304U,0x12345678U,0x80000000U,0xffffffffU},local,{0,0,0},std::nullopt};
    };
    return {actor(10,111,3),actor(11,121,4),actor(12,131,5),actor(13,141,6),
        25,80,150,15,14,21,21,23,24,31,23,35,29,32,34,
        {true,true,true,true,true,true,true,true}};
}
udp::InitialActorLinksPlan inventory_base_links() {
    auto plan=public_links_plan();plan.pawn_exclusive_bound=150;return plan;
}
LoginAuthorityFixture::PublicationDecision inventory_publisher() {
    return [](LoginAuthorityFixture&,const session::SessionId&,const control::posthello::Login&) {
        auto world=public_world();world.packages[0].generation_net_counts={0,7};return std::optional{world};
    };
}
wire::mech::SMechSetupStruct inventory_materialized(std::uint8_t previous) {
    auto value=public_mech_result(previous);value.mech_type=1;value.mech_sub_type=15;
    value.mech_weapons.primary_idx=0;value.mech_weapons.secondary_idx=101;return value;
}
udp::LocalUdpOwner::StartMechProvider inventory_provider() {
    return [](const session::SessionId&,const wire::rpc::StartMechArguments&,std::uint8_t previous) {
        return std::optional{inventory_materialized(previous)};
    };
}
struct InventoryFixture : LoginAuthorityFixture {
    explicit InventoryFixture(std::optional<udp::InventoryBootstrapPlan> plan=inventory_plan(),
        udp::LocalUdpOwner::StartMechProvider provider=inventory_provider(),
        game::ConnectionConfig config=authority_skeleton())
        : LoginAuthorityFixture({},true,inventory_publisher(),public_controller_plan(),config,
            public_pawn_plan(),public_pri_plan(),inventory_base_links(),public_start_mech_plan(),
            std::move(provider),{},{},{},{},{},{},{},100.0,std::move(plan)) {}
};
struct InventoryFlow { std::uint16_t next_packet=0; std::uint16_t next_controller_sequence=1; };
ControllerOutput inventory_output(Peer& peer,session::Context& client,std::size_t expected,
    std::chrono::milliseconds budget=2000ms) {
    ControllerOutput out;const auto end=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<end) {
        const auto datagram=peer.receive(20ms);if (!datagram) continue;
        const auto packet=wire::decode_packet(client.decode(*datagram),256);
        for (const auto& record:packet.records) if (!record.is_ack) {
            out.bunches.push_back(record.bunch);out.packet_ids.push_back(packet.packet_id);
        }
        if (out.bunches.size()>=expected) break;
    }
    return out;
}
void inventory_ack(InventoryFixture& fixture,Peer& peer,session::Context& client,InventoryFlow& flow,
    const std::vector<std::uint16_t>& observed_ids) {
    REQUIRE_FALSE(observed_ids.empty());
    peer.send(fixture.owner.port(),links_ack(client,flow.next_packet++,observed_ids));
}
void inventory_login_join(InventoryFixture& f,Peer& peer,session::Context& client,InventoryFlow& flow) {
    const auto login=f.login();authority_select(f,login);
    const std::array archives{hex("1a07000000"),hex("1cc3363f8d"),hex("00080000001400000098badcfe")};
    const std::array responses{hex("1b00000000"),hex("1d"),hex("030b00000009000000313233344142434400")};
    for (std::size_t i=0;i<archives.size();++i) {
        peer.send(f.owner.port(),authority_request(client,flow.next_packet++,static_cast<std::uint16_t>(i+1),archives[i],i==0));
        const auto out=inventory_output(peer,client,1);REQUIRE(out.bunches.size()==1);
        CHECK(out.bunches[0].payload==responses[i]);inventory_ack(f,peer,client,flow,out.packet_ids);
    }
    peer.send(f.owner.port(),authority_request(client,flow.next_packet++,4,
        authority_login(login.result.at("UserId").get<std::string>())));
    const auto published=inventory_output(peer,client,2);REQUIRE(published.bunches.size()==2);
    REQUIRE(wait_until([&]{return f.owner.diagnostics().world_published;}));
    inventory_ack(f,peer,client,flow,published.packet_ids);
    peer.send(f.owner.port(),authority_request(client,flow.next_packet++,5,
        hex("08040302017856341200000080ffffffff0200000009")));
    const auto openings=inventory_output(peer,client,3);REQUIRE(openings.bunches.size()==3);
    CHECK(openings.bunches[0].channel==7);CHECK(openings.bunches[1].channel==9);CHECK(openings.bunches[2].channel==8);
    inventory_ack(f,peer,client,flow,openings.packet_ids);
    const auto links=inventory_output(peer,client,2);REQUIRE(links.bunches.size()==2);
    CHECK(links.bunches[0].channel==7);CHECK(links.bunches[1].channel==8);
    inventory_ack(f,peer,client,flow,links.packet_ids);
}
ControllerOutput inventory_selected(InventoryFixture& f,Peer& peer,session::Context& client,InventoryFlow& flow) {
    peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,flow.next_controller_sequence++,7,4,false,false,0,255));
    const auto output=inventory_output(peer,client,5);REQUIRE(output.bunches.size()==5);
    ControllerOutput openings;
    for (std::size_t i=0;i<output.bunches.size();++i) {
        const auto& bunch=output.bunches[i];
        if (bunch.open) { openings.bunches.push_back(bunch);openings.packet_ids.push_back(output.packet_ids[i]); }
        else { CHECK(bunch.channel==9);CHECK(bunch.payload_bits==547);
            inventory_ack(f,peer,client,flow,{output.packet_ids[i]}); }
    }
    REQUIRE(openings.bunches.size()==4);
    const std::array channels{10,11,12,13};const std::array sequences{112U,122U,132U,142U};
    const std::array literals{hex("060000004005"),hex("080000004005"),hex("0a0000004005"),hex("0c0000004005")};
    for (std::size_t i=0;i<4;++i) {
        const auto& bunch=openings.bunches[i];CHECK(bunch.channel==channels[i]);CHECK(bunch.sequence==sequences[i]);
        CHECK(bunch.payload==literals[i]);CHECK(bunch.payload_bits==43);
        CHECK(bunch.control);CHECK(bunch.open);CHECK(bunch.reliable);CHECK_FALSE(bunch.close);CHECK(bunch.channel_type==2);
    }
    return openings;
}
void inventory_given_literal(const ControllerOutput& out) {
    REQUIRE(out.bunches.size()==1);const auto& bunch=out.bunches[0];
    CHECK(bunch.channel==11);CHECK(bunch.sequence==125);CHECK(bunch.payload_bits==19);
    // tag23/b35(5bits), presence1, actorPawn8 =>reference17, Boolfalse.
    CHECK(bunch.payload==hex("770400"));CHECK(bunch.reliable);CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.control);CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);
}
Bytes inventory_manager_request(session::Context& client,InventoryFlow& flow,std::uint16_t sequence,
    std::string_view literal,std::size_t bits,std::uint16_t channel=10) {
    wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=channel;
    bunch.sequence=sequence;bunch.payload=hex(literal);bunch.payload_bits=bits;
    wire::Packet packet;packet.packet_id=flow.next_packet++;wire::PacketRecord record;record.bunch=std::move(bunch);
    packet.records.push_back(std::move(record));return client.encode(wire::encode_packet(packet,256));
}
void inventory_start_test_owner(udp::LocalUdpOwner& owner,const udp::InventoryBootstrapPlan& plan,unsigned missing=0) {
    auto publish=[](const session::SessionId&,const control::posthello::Login&) {return std::optional{public_world()};};
    owner.start([](const session::SessionId&){return true;},
        [](const session::SessionId&,const control::posthello::RawString&){return true;},
        missing==1?udp::LocalUdpOwner::PublicationProvider{}:udp::LocalUdpOwner::PublicationProvider{publish},
        missing==2?std::nullopt:std::optional{public_controller_plan()},
        missing==3?std::nullopt:std::optional{public_pawn_plan()},
        missing==4?std::nullopt:std::optional{public_pri_plan()},
        missing==5?std::nullopt:std::optional{inventory_base_links()},
        (missing==6 || missing==8)?std::nullopt:std::optional{public_start_mech_plan()},
        (missing==7 || missing==8)?udp::LocalUdpOwner::StartMechProvider{}:inventory_provider(),
        {},{},{},{},{},{},{},plan);
}
}

namespace {
void inventory_reference_literals(const ControllerOutput& out) {
    REQUIRE(out.bunches.size()==6);REQUIRE(out.packet_ids.size()==6);
    // FIVE destinations, SIX reliable batches: Dual scalar and array separate.
    // Manager14/15 bound24 take4 ID bits; Chain21 takes5. Other scalar
    // Inventory tags take5; Pawn25/80 bound150 take7. Actor references12.
    const std::array channels{10,11,12,13,8,11};
    const std::array sequences{113U,123U,133U,143U,74U,124U};
    const std::array bits{49U,51U,68U,68U,38U,50U};
    const std::array literals{hex("1f011e01f50200"),hex("2f025c04d40a00"),hex("2f025c04d40ae81700"),
        hex("2f025c04d40ae81700"),hex("990a805e00"),hex("1f20037ec00600")};
    for (std::size_t i=0;i<6;++i) {
        const auto& bunch=out.bunches[i];CHECK(bunch.channel==channels[i]);CHECK(bunch.sequence==sequences[i]);
        CHECK(bunch.payload_bits==bits[i]);CHECK(bunch.payload==literals[i]);
        CHECK(bunch.reliable);CHECK(bunch.channel_type==2);CHECK_FALSE(bunch.control);
        CHECK_FALSE(bunch.open);CHECK_FALSE(bunch.close);
    }
}
ControllerOutput inventory_references(InventoryFixture& f,Peer& peer,session::Context& client,InventoryFlow& flow) {
    const auto openings=inventory_selected(f,peer,client,flow);
    inventory_ack(f,peer,client,flow,openings.packet_ids);
    auto refs=inventory_output(peer,client,6);inventory_reference_literals(refs);return refs;
}
void inventory_activate(InventoryFixture& f,Peer& peer,session::Context& client,InventoryFlow& flow) {
    inventory_login_join(f,peer,client,flow);
    const auto refs=inventory_references(f,peer,client,flow);
    inventory_ack(f,peer,client,flow,refs.packet_ids);
    const auto given=inventory_output(peer,client,1);inventory_given_literal(given);
    inventory_ack(f,peer,client,flow,given.packet_ids);
}
}
TEST_CASE("UDP inventory waits for all four real opening and six reference ACKs before one GivenTo") {
    auto plan=inventory_plan();InventoryFixture f(plan);plan.dual_weapon.channel.channel=19; // caller copy immutable
    Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_login_join(f,peer,client,flow);
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty()); // No StartMech, no fabricated loadout.
    const auto before=f.owner.diagnostics();
    CHECK_FALSE(before.inventory_profile_accepted);CHECK_FALSE(before.inventory_openings_sent);
    CHECK_FALSE(before.inventory_openings_acked);CHECK_FALSE(before.inventory_links_sent);
    CHECK_FALSE(before.inventory_links_acked);CHECK_FALSE(before.inventory_given_to_sent);
    CHECK_FALSE(before.inventory_current_weapon_received);
    const auto openings=inventory_selected(f,peer,client,flow);
    inventory_ack(f,peer,client,flow,{openings.packet_ids[0],openings.packet_ids[1],openings.packet_ids[2]});
    CHECK(wait_until([&]{const auto d=f.owner.diagnostics();return d.inventory_profile_accepted && d.inventory_openings_sent;}));
    CHECK_FALSE(f.owner.diagnostics().inventory_openings_acked);
    CHECK_FALSE(f.owner.diagnostics().inventory_links_sent);
    CHECK_FALSE(f.owner.diagnostics().inventory_links_acked);
    CHECK_FALSE(f.owner.diagnostics().inventory_given_to_sent);
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    inventory_ack(f,peer,client,flow,{openings.packet_ids[0]}); // real duplicate, not an invented future ACK
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    inventory_ack(f,peer,client,flow,{openings.packet_ids[3]});
    const auto refs=inventory_output(peer,client,6);inventory_reference_literals(refs);
    CHECK(wait_until([&]{const auto d=f.owner.diagnostics();return d.inventory_openings_acked && d.inventory_links_sent;}));
    CHECK_FALSE(f.owner.diagnostics().inventory_links_acked);
    CHECK_FALSE(f.owner.diagnostics().inventory_given_to_sent);
    inventory_ack(f,peer,client,flow,{refs.packet_ids.begin(),refs.packet_ids.end()-1});
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty()); // Dual ARRAY record still pending.
    CHECK_FALSE(f.owner.diagnostics().inventory_links_acked);
    CHECK_FALSE(f.owner.diagnostics().inventory_given_to_sent);
    inventory_ack(f,peer,client,flow,{refs.packet_ids.back()});
    const auto given=inventory_output(peer,client,1);inventory_given_literal(given);
    inventory_ack(f,peer,client,flow,given.packet_ids);
    inventory_ack(f,peer,client,flow,refs.packet_ids);
    CHECK(wait_until([&]{const auto d=f.owner.diagnostics();return d.inventory_links_acked && d.inventory_given_to_sent;}));
    CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);
    f.udp_now=101.0;CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,flow.next_controller_sequence++,7,4,false,false,0,254));
    const auto revised=inventory_output(peer,client,1);REQUIRE(revised.bunches.size()==1);
    CHECK(revised.bunches[0].channel==9);CHECK_FALSE(revised.bunches[0].open);
    inventory_ack(f,peer,client,flow,revised.packet_ids);
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty()); // Same supported profile, no reopen/GivenTo replay.
    CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP inventory Manager consumes only known arguments without invented reply or suffix scanning") {
    InventoryFixture f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_activate(f,peer,client,flow);
    // tag23/b24 +presence1 +referenceDual11 =>0x5f7,18bits.
    peer.send(f.owner.port(),inventory_manager_request(client,flow,1,"f70500",18));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.owner.diagnostics().closed);
    peer.send(f.owner.port(),inventory_manager_request(client,flow,1,"f70500",18)); // reliable duplicate
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.owner.diagnostics().closed);
    struct Unsupported { const char* literal;std::size_t bits; };
    const Unsupported unsupported[]{{"17",6},{"770000",18},{"770600",18},{"b704000000",38}};
    std::uint16_t sequence=2;
    for (const auto& item:unsupported) {
        CAPTURE(item.literal);
        peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence++,item.literal,item.bits));
        CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.owner.diagnostics().closed);
    }
    // Unknown0/b24 then known23 with missing args: unknown suffix MUST stay opaque.
    peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence++,"e002",10));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.owner.diagnostics().closed);
    // Known valid18-bit prefix, then known23 truncated at the argument boundary.
    peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence,"f7055c",23));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP inventory truncated known Manager argument retires the exact lease") {
    InventoryFixture f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_activate(f,peer,client,flow);
    peer.send(f.owner.port(),inventory_manager_request(client,flow,1,"17",5)); // tag only, missing presence
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK_FALSE(f.service->authorize_udp(public_id));CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
}
TEST_CASE("UDP inventory rejects unsupported returned materialized profiles before publishing the PRI setup") {
    for (unsigned changed=0;changed<4;++changed) {
        CAPTURE(changed);
        InventoryFixture f(inventory_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,
            std::uint8_t previous) {
            auto value=inventory_materialized(previous);
            if (changed==0) value.mech_type=2;
            if (changed==1) value.mech_sub_type=16;
            if (changed==2) value.mech_weapons.primary_idx=1;
            if (changed==3) value.mech_weapons.secondary_idx=100;
            return std::optional{value};
        });
        Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
        inventory_login_join(f,peer,client,flow);
        peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,1,7,4,false,false,0,255));
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
        CHECK_FALSE(f.service->authorize_udp(public_id));
        const auto rejected=f.owner.diagnostics();
        CHECK_FALSE(rejected.inventory_profile_accepted);CHECK_FALSE(rejected.inventory_openings_sent);
        CHECK_FALSE(rejected.inventory_openings_acked);CHECK_FALSE(rejected.inventory_links_sent);
        CHECK_FALSE(rejected.inventory_links_acked);CHECK_FALSE(rejected.inventory_given_to_sent);
        CHECK_FALSE(rejected.inventory_current_weapon_received);
    }
}
TEST_CASE("UDP inventory stop or revocation after references cannot release GivenTo") {
    for (const bool stopping:{false,true}) {
        CAPTURE(stopping);InventoryFixture f;Peer peer;
        session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
        inventory_login_join(f,peer,client,flow);const auto refs=inventory_references(f,peer,client,flow);
        if (stopping) f.owner.stop();
        else {
            f.verifier.active=false;
            inventory_ack(f,peer,client,flow,refs.packet_ids);
        }
        CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
        CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
        CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK_FALSE(f.owner.diagnostics().inventory_links_acked);
        CHECK_FALSE(f.owner.diagnostics().inventory_given_to_sent);
        CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);
    }
}
TEST_CASE("UDP inventory stale provider lease retirement happens outside pins and never opens actors") {
    InventoryFixture* target=nullptr;
    InventoryFixture f(inventory_plan(),[&](const session::SessionId&,const wire::rpc::StartMechArguments&,
        std::uint8_t previous) {
        target->service.reset(); // Synchronously retires the lease; cannot deadlock on a held Use.
        return std::optional{inventory_materialized(previous)};
    });
    target=&f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_login_join(f,peer,client,flow);
    peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,1,7,4,false,false,0,255));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.service);
}
TEST_CASE("UDP inventory precomposes all four package prefixes before the first actor open") {
    auto plan=inventory_plan();plan.secondary_weapon.archetype_local_net_index=7; // Root owns0..6 only.
    InventoryFixture f(plan);Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_login_join(f,peer,client,flow);
    peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,1,7,4,false,false,0,255));
    CHECK(wait_until([&]{return f.owner.diagnostics().closed;}));
    const auto out=inventory_output(peer,client,5,100ms); // Drain any partial opens as well as the selected PRI.
    for (const auto& bunch:out.bunches) {CHECK_FALSE(bunch.open);CHECK(bunch.channel==9);}
    CHECK_FALSE(f.service->authorize_udp(public_id));
}
TEST_CASE("UDP inventory absent plan preserves the old selected PRI property only") {
    InventoryFixture f(std::nullopt);Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_login_join(f,peer,client,flow);
    peer.send(f.owner.port(),start_mech_packet(client,flow.next_packet++,1,7,4,false,false,0,255));
    const auto selected=inventory_output(peer,client,1);REQUIRE(selected.bunches.size()==1);
    CHECK(selected.bunches[0].channel==9);CHECK(selected.bunches[0].payload_bits==547);
    inventory_ack(f,peer,client,flow,selected.packet_ids);
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());CHECK_FALSE(f.owner.diagnostics().closed);
}
TEST_CASE("UDP inventory plan preflight rejects explicit premises collisions fields bounds and missing profile source") {
    for (unsigned bad=0;bad<30;++bad) {
        CAPTURE(bad);auto plan=inventory_plan();
        switch (bad) {
        case 0:plan.eligibility.primary_package_map=false;break;
        case 1:plan.eligibility.actor_prefixes_correct=false;break;
        case 2:plan.eligibility.archetypes_correct=false;break;
        case 3:plan.eligibility.actor_caches_correct=false;break;
        case 4:plan.eligibility.scalar_links_correct=false;break;
        case 5:plan.eligibility.hand_weap_array_correct=false;break;
        case 6:plan.eligibility.client_given_to_correct=false;break;
        case 7:plan.eligibility.server_set_current_weapon_correct=false;break;
        case 8:plan.manager.channel.channel=0;break;
        case 9:plan.dual_weapon.channel.channel=2048;break;
        case 10:plan.primary_weapon.channel.channel=7;break;
        case 11:plan.secondary_weapon.channel.channel=8;break;
        case 12:plan.manager.channel.channel=9;break;
        case 13:plan.secondary_weapon.channel.channel=plan.primary_weapon.channel.channel;break;
        case 14:plan.manager.archetype_local_net_index=-1;break;
        case 15:plan.dual_weapon.archetype_local_net_index=-1;break;
        case 16:plan.primary_weapon.archetype_local_net_index=-1;break;
        case 17:plan.secondary_weapon.archetype_local_net_index=-1;break;
        case 18:plan.pawn_exclusive_bound=149;break; // Same Pawn class cannot have conflicting bounds.
        case 19:plan.manager_exclusive_bound=0;break;
        case 20:plan.dual_exclusive_bound=0;break;
        case 21:plan.primary_exclusive_bound=0;break;
        case 22:plan.secondary_exclusive_bound=0;break;
        case 23:plan.pawn_inv_manager_field_id=150;break;
        case 24:plan.pawn_dual_weapon_field_id=25;break;
        case 25:plan.manager_server_set_current_weapon_field_id=21;break;
        case 26:plan.dual_client_given_to_field_id=31;break;
        case 27:plan.hand_dual_weapon_field_id=32;break;
        case 28:plan.owner_field_id=plan.instigator_field_id;break;
        case 29:plan.manager.position={std::numeric_limits<std::int32_t>::max(),0,0};break;
        }
        udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        CHECK_THROWS_AS(inventory_start_test_owner(owner,plan),udp::LocalUdpError);
        if (owner.running()) {owner.stop();continue;} // Current RED ignores the new plan; preserve every independent case.
        CHECK_NOTHROW(inventory_start_test_owner(owner,inventory_plan()));CHECK(owner.running());owner.stop();
    }
    for (unsigned missing=1;missing<=8;++missing) {
        CAPTURE(missing);udp::LocalUdpOwner owner(0,authority_skeleton(),[]{return 100.0;},5ms);
        CHECK_THROWS_AS(inventory_start_test_owner(owner,inventory_plan(),missing),udp::LocalUdpError);
        owner.stop();
    }
}

TEST_CASE("UDP inventory diagnostics distinguish unsupported Manager selections from the accepted current weapon") {
    InventoryFixture f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_activate(f,peer,client,flow);
    CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);
    // Null absent, explicit null, other actor, package reference, then opaque unknown prefix.
    struct Unsupported { const char* literal;std::size_t bits; };
    const Unsupported unsupported[]{{"17",6},{"770000",18},{"770600",18},{"b704000000",38},{"e002",10}};
    std::uint16_t sequence=1;
    for (const auto& item:unsupported) {
        CAPTURE(item.literal);
        peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence++,item.literal,item.bits));
        CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
        CHECK_FALSE(f.owner.diagnostics().closed);
        CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);
    }
    peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence,"f70500",18));
    CHECK(wait_until([&]{return f.owner.diagnostics().inventory_current_weapon_received;}));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence,"f70500",18)); // Reliable duplicate.
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    CHECK(f.owner.diagnostics().inventory_current_weapon_received);
    CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP actor content metadata counts delivered Manager headers without changing RPC or reliability") {
    InventoryFixture f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_activate(f,peer,client,flow);
    constexpr auto manager=static_cast<std::size_t>(udp::ActorDiagnosticSlot::manager);
    CHECK(f.owner.diagnostics().actor_bunches[manager].delivered_bunches==0);
    auto bad_mac=inventory_manager_request(client,flow,1,"f70500",18);bad_mac.back()^=1;
    peer.send(f.owner.port(),bad_mac);
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    CHECK(f.owner.diagnostics().actor_bunches[manager].delivered_bunches==0);
    CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);

    // Unknown tag0/b24, then an opaque suffix containing tag23 with no arguments.
    peer.send(f.owner.port(),inventory_manager_request(client,flow,1,"e002",10));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto unknown=f.owner.diagnostics();
    CHECK(unknown.actor_bunches[manager].delivered_bunches==1);
    CHECK(unknown.actor_bunches[manager].first_field_id==std::optional<std::uint32_t>{0});
    CHECK(unknown.actor_bunches[manager].last_payload_bits==10);
    CHECK(unknown.actor_bunches[manager].first_field_counts[0]==1);
    CHECK(unknown.actor_bunches[manager].first_field_counts[23]==0);
    CHECK(unknown.actor_bunches[manager].last_reliable);
    CHECK_FALSE(unknown.actor_bunches[manager].last_open);CHECK_FALSE(unknown.actor_bunches[manager].last_close);
    CHECK_FALSE(unknown.inventory_current_weapon_received);CHECK_FALSE(unknown.closed);
    peer.send(f.owner.port(),inventory_manager_request(client,flow,1,"e002",10)); // Fresh nonce, reliable duplicate.
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    CHECK(f.owner.diagnostics().actor_bunches[manager]==unknown.actor_bunches[manager]);

    // Independently retained literal: tag23/b24 +presence1 +actorDual11,18bits.
    const auto accepted=inventory_manager_request(client,flow,2,"f70500",18);
    peer.send(f.owner.port(),accepted);
    CHECK(wait_until([&]{return f.owner.diagnostics().inventory_current_weapon_received;}));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto delivered=f.owner.diagnostics();
    CHECK(delivered.actor_bunches[manager].delivered_bunches==2);
    CHECK(delivered.actor_bunches[manager].first_field_id==std::optional<std::uint32_t>{23});
    CHECK(delivered.actor_bunches[manager].last_payload_bits==18);
    CHECK(delivered.actor_bunches[manager].zero_payload_bunches==0);
    CHECK(delivered.actor_bunches[manager].header_truncated_bunches==0);
    CHECK(delivered.actor_bunches[manager].field_uninterpreted_bunches==0);
    CHECK(delivered.actor_bunches[manager].first_field_counts[0]==1);
    CHECK(delivered.actor_bunches[manager].first_field_counts[23]==1);
    peer.send(f.owner.port(),accepted); // Exact wrapper replay.
    peer.send(f.owner.port(),inventory_manager_request(client,flow,2,"f70500",18));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    CHECK(f.owner.diagnostics().actor_bunches[manager]==delivered.actor_bunches[manager]);
    peer.send(f.owner.port(),inventory_manager_request(client,flow,3,"e002",10));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto overwritten=f.owner.diagnostics().actor_bunches[manager];
    CHECK(overwritten.first_field_id==std::optional<std::uint32_t>{0});
    CHECK(overwritten.first_field_counts[0]==2);CHECK(overwritten.first_field_counts[23]==1);
    CHECK(overwritten.field_out_of_range_bunches==0);
    CHECK(f.owner.diagnostics().inventory_current_weapon_received);
    CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP actor content metadata uses fixed role bounds and distinguishes empty truncated and unreliable delivery") {
    InventoryFixture f;Peer peer;session::Context client(session::Role::client,public_id,public_key);InventoryFlow flow;
    inventory_activate(f,peer,client,flow);
    struct Role {udp::ActorDiagnosticSlot slot;std::uint16_t channel;};
    const Role roles[]{{udp::ActorDiagnosticSlot::controller,7},{udp::ActorDiagnosticSlot::pawn,8},
        {udp::ActorDiagnosticSlot::pri,9},{udp::ActorDiagnosticSlot::manager,10},
        {udp::ActorDiagnosticSlot::dual_weapon,11},{udp::ActorDiagnosticSlot::primary_weapon,12},
        {udp::ActorDiagnosticSlot::secondary_weapon,13}};
    for (const auto& role:roles) {
        CAPTURE(role.channel);const auto before=f.owner.diagnostics();
        // Numeric tag0 followed by opaque zero bits. No RPC name is inferred.
        const auto sequence=role.channel==7?flow.next_controller_sequence++:std::uint16_t{1};
        peer.send(f.owner.port(),inventory_manager_request(client,flow,sequence,"00",8,role.channel));
        CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
        const auto after=f.owner.diagnostics();const auto slot=static_cast<std::size_t>(role.slot);
        CHECK(after.actor_bunches[slot].delivered_bunches==before.actor_bunches[slot].delivered_bunches+1);
        CHECK(after.actor_bunches[slot].first_field_id==std::optional<std::uint32_t>{0});
        CHECK(after.actor_bunches[slot].first_field_counts[0]==before.actor_bunches[slot].first_field_counts[0]+1);
        CHECK(after.actor_bunches[slot].last_payload_bits==8);CHECK(after.actor_bunches[slot].last_reliable);
        for (std::size_t i=0;i<after.actor_bunches.size();++i)
            if (i!=slot) CHECK(after.actor_bunches[i]==before.actor_bunches[i]);
        CHECK_FALSE(after.closed);CHECK_FALSE(after.inventory_current_weapon_received);
    }
    constexpr auto manager=static_cast<std::size_t>(udp::ActorDiagnosticSlot::manager);
    peer.send(f.owner.port(),inventory_manager_request(client,flow,2,"",0));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto empty=f.owner.diagnostics().actor_bunches[manager];
    CHECK(empty.delivered_bunches==2);CHECK(empty.zero_payload_bunches==1);
    CHECK(empty.last_payload_bits==0);CHECK_FALSE(empty.first_field_id.has_value());
    peer.send(f.owner.port(),inventory_manager_request(client,flow,3,"01",1));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto truncated=f.owner.diagnostics().actor_bunches[manager];
    CHECK(truncated.delivered_bunches==3);CHECK(truncated.header_truncated_bunches==1);
    CHECK(truncated.last_payload_bits==1);CHECK_FALSE(truncated.first_field_id.has_value());
    // Ready existing Manager channel, plain unreliable has omitted sequence/type.
    wire::Bunch bunch;bunch.channel=10;bunch.payload=hex("00");bunch.payload_bits=8;
    wire::Packet packet;packet.packet_id=flow.next_packet++;wire::PacketRecord record;record.bunch=bunch;
    packet.records.push_back(record);peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
    CHECK(inventory_output(peer,client,1,100ms).bunches.empty());
    const auto unreliable=f.owner.diagnostics().actor_bunches[manager];
    CHECK(unreliable.delivered_bunches==4);CHECK_FALSE(unreliable.last_reliable);
    CHECK(unreliable.first_field_id==std::optional<std::uint32_t>{0});
    CHECK(unreliable.zero_payload_bunches==1);CHECK(unreliable.header_truncated_bunches==1);
    CHECK(unreliable.first_field_counts[0]==2);CHECK(unreliable.field_out_of_range_bunches==0);
    CHECK_FALSE(f.owner.diagnostics().closed);CHECK_FALSE(f.owner.diagnostics().inventory_current_weapon_received);

    // Synthetic future cache296 accepts numeric tag295, beyond diagnostic bins.
    // Stop before ACKing inventory openings: no changed reference golden needed.
    auto future_plan=inventory_plan();future_plan.manager_exclusive_bound=296;
    InventoryFixture future(future_plan);Peer future_peer;
    session::Context future_client(session::Role::client,public_id,public_key);InventoryFlow future_flow;
    inventory_login_join(future,future_peer,future_client,future_flow);
    inventory_selected(future,future_peer,future_client,future_flow);
    // SerializeInt(295,296): independently derived9bits 0x127 =>27,01.
    future_peer.send(future.owner.port(),inventory_manager_request(future_client,future_flow,1,"2701",9));
    CHECK(inventory_output(future_peer,future_client,1,100ms).bunches.empty());
    const auto overflow=future.owner.diagnostics().actor_bunches[manager];
    CHECK(overflow.delivered_bunches==1);CHECK(overflow.first_field_id==std::optional<std::uint32_t>{295});
    CHECK(overflow.field_out_of_range_bunches==1);
    CHECK(std::all_of(overflow.first_field_counts.begin(),overflow.first_field_counts.end(),
        [](std::uint64_t bin_count){return bin_count==0;}));
    CHECK_FALSE(future.owner.diagnostics().closed);CHECK_FALSE(future.owner.diagnostics().inventory_current_weapon_received);
}

TEST_CASE("UDP actor content metadata observes the configured GRI role without interpreting actor arguments") {
    LoginAuthorityFixture f({},true,bootstrap_publisher(),public_controller_plan(),{},
        public_pawn_plan(),public_pri_plan(),bootstrap_links(),{},{},{},{},{},{},public_bootstrap_plan());
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    bootstrap_openings(f,login,peer,client);
    constexpr auto gri=static_cast<std::size_t>(udp::ActorDiagnosticSlot::gri);
    CHECK(f.owner.diagnostics().actor_bunches[gri].delivered_bunches==0);
    wire::Bunch bunch;bunch.channel=10;bunch.channel_type=2;bunch.reliable=true;bunch.sequence=1;
    bunch.payload=hex("00");bunch.payload_bits=8;
    wire::Packet packet;packet.packet_id=5;wire::PacketRecord record;record.bunch=bunch;packet.records.push_back(record);
    peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
    CHECK(controller_output(peer,client,100ms).bunches.empty());
    const auto observed=f.owner.diagnostics().actor_bunches[gri];
    CHECK(observed.delivered_bunches==1);CHECK(observed.first_field_id==std::optional<std::uint32_t>{0});
    CHECK(observed.last_payload_bits==8);CHECK(observed.last_reliable);
    CHECK(observed.first_field_counts[0]==1);CHECK(observed.field_out_of_range_bunches==0);
    CHECK_FALSE(observed.last_open);CHECK_FALSE(observed.last_close);CHECK_FALSE(f.owner.diagnostics().closed);
}

TEST_CASE("UDP actor content metadata leaves a first field uninterpreted when no class bound is configured") {
    LoginAuthorityFixture f({},true,controller_publisher(),public_controller_plan());
    const auto login=f.login();Peer peer;session::Context client(session::Role::client,public_id,public_key);
    controller_publication(f,login,peer,client);peer.send(f.owner.port(),controller_join(client));
    REQUIRE(controller_output(peer,client,2000ms,1).bunches.size()==1);
    wire::Bunch bunch;bunch.channel=7;bunch.channel_type=2;bunch.reliable=true;bunch.sequence=1;
    bunch.payload=hex("17");bunch.payload_bits=8;
    wire::Packet packet;packet.packet_id=5;wire::PacketRecord record;record.bunch=bunch;packet.records.push_back(record);
    peer.send(f.owner.port(),client.encode(wire::encode_packet(packet,256)));
    CHECK(controller_output(peer,client,100ms).bunches.empty());
    const auto observed=f.owner.diagnostics().actor_bunches[static_cast<std::size_t>(udp::ActorDiagnosticSlot::controller)];
    CHECK(observed.delivered_bunches==1);CHECK(observed.field_uninterpreted_bunches==1);
    CHECK(observed.header_truncated_bunches==0);CHECK_FALSE(observed.first_field_id.has_value());
    CHECK(std::all_of(observed.first_field_counts.begin(),observed.first_field_counts.end(),
        [](std::uint64_t bin_count){return bin_count==0;}));
    CHECK(observed.field_out_of_range_bunches==0);
    CHECK_FALSE(f.owner.diagnostics().closed);
}
