#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "hawken/local_tcp_probe.hpp"
#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using hawken::tcp::LocalTcpProbe;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible_v<LocalTcpProbe>);
static_assert(!std::is_move_constructible_v<LocalTcpProbe>);
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2,2),&data)!=0) throw std::runtime_error("TCP fixture initialization failed.");
    }
    ~SocketRuntime() { WSACleanup(); }
};
void close_socket(Socket socket) noexcept { closesocket(socket); }
bool connecting_error() {
    const int error=WSAGetLastError();
    return error==WSAEWOULDBLOCK || error==WSAEINPROGRESS;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
struct SocketRuntime {};
void close_socket(Socket socket) noexcept { ::close(socket); }
bool connecting_error() { return errno==EINPROGRESS || errno==EWOULDBLOCK; }
#endif
void nonblocking(Socket socket) {
#ifdef _WIN32
    u_long enabled=1;
    if (ioctlsocket(socket,FIONBIO,&enabled)!=0) throw std::runtime_error("TCP fixture configuration failed.");
#else
    const int flags=fcntl(socket,F_GETFL,0);
    if (flags<0 || fcntl(socket,F_SETFL,flags|O_NONBLOCK)<0 || socket>=FD_SETSIZE)
        throw std::runtime_error("TCP fixture configuration failed.");
#endif
}
int wait_socket(Socket socket,bool writing) {
    fd_set ready,exception; FD_ZERO(&ready); FD_ZERO(&exception);
    FD_SET(socket,&ready); FD_SET(socket,&exception);
    timeval timeout{2,0};
#ifdef _WIN32
    return select(0,writing?nullptr:&ready,writing?&ready:nullptr,&exception,&timeout);
#else
    return select(socket+1,writing?nullptr:&ready,writing?&ready:nullptr,&exception,&timeout);
#endif
}
class ListenerFixture {
public:
    explicit ListenerFixture(std::uint16_t port=0,std::uint32_t address=INADDR_LOOPBACK) {
        socket_=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if (socket_==invalid_socket) throw std::runtime_error("TCP fixture socket failed.");
        try {
#ifdef _WIN32
            const int exclusive=1;
            if (setsockopt(socket_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),static_cast<int>(sizeof(exclusive)))!=0)
                throw std::runtime_error("TCP fixture exclusivity failed.");
#endif
            sockaddr_in endpoint{}; endpoint.sin_family=AF_INET;
            endpoint.sin_addr.s_addr=htonl(address); endpoint.sin_port=htons(port);
            if (::bind(socket_,reinterpret_cast<const sockaddr*>(&endpoint),static_cast<int>(sizeof(endpoint)))!=0 ||
                listen(socket_,8)!=0) throw std::runtime_error("TCP fixture bind failed.");
#ifdef _WIN32
            int size=static_cast<int>(sizeof(endpoint));
#else
            socklen_t size=sizeof(endpoint);
#endif
            if (getsockname(socket_,reinterpret_cast<sockaddr*>(&endpoint),&size)!=0)
                throw std::runtime_error("TCP fixture endpoint failed.");
            port_=ntohs(endpoint.sin_port);
        } catch (...) { close_socket(socket_); throw; }
    }
    ~ListenerFixture() { close_socket(socket_); }
    ListenerFixture(const ListenerFixture&)=delete;
    ListenerFixture& operator=(const ListenerFixture&)=delete;
    std::uint16_t port() const noexcept { return port_; }
    void send_public_response() const {
        nonblocking(socket_);
        if (wait_socket(socket_,false)<=0)
            throw std::runtime_error("TCP fixture accept was not ready.");
        const auto peer=accept(socket_,nullptr,nullptr);
        if (peer==invalid_socket) throw std::runtime_error("TCP fixture accept failed.");
        try {
            nonblocking(peer);
            if (wait_socket(peer,true)<=0)
                throw std::runtime_error("TCP fixture send was not ready.");
            constexpr char payload='P'; // One public synthetic application byte.
#ifdef _WIN32
            const auto sent=::send(peer,&payload,1,0);
#else
            const auto sent=::send(peer,&payload,1,MSG_NOSIGNAL);
#endif
            if (sent!=1) throw std::runtime_error("TCP fixture send failed.");
        } catch (...) { close_socket(peer); throw; }
        close_socket(peer);
    }
private:
    Socket socket_=invalid_socket;
    std::uint16_t port_=0;
};
struct CloseObservation {
    int select_result=0,select_error=0;
    bool recv_called=false;
    int recv_result=0,recv_error=0;
    int socket_error_query_result=0,socket_error_value=0,socket_error_query_error=0;
};
class Peer {
public:
    explicit Peer(std::uint16_t port) {
        socket_=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if (socket_==invalid_socket) throw std::runtime_error("TCP fixture peer failed.");
        try {
            nonblocking(socket_);
            sockaddr_in endpoint{}; endpoint.sin_family=AF_INET;
            endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK); endpoint.sin_port=htons(port);
            const int result=connect(socket_,reinterpret_cast<const sockaddr*>(&endpoint),static_cast<int>(sizeof(endpoint)));
            if (result!=0) {
                if (!connecting_error() || wait_socket(socket_,true)<=0)
                    throw std::runtime_error("TCP fixture connect failed.");
                int error=0;
#ifdef _WIN32
                int size=static_cast<int>(sizeof(error));
                if (getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)!=0 || error!=0)
#else
                socklen_t size=sizeof(error);
                if (getsockopt(socket_,SOL_SOCKET,SO_ERROR,&error,&size)!=0 || error!=0)
#endif
                    throw std::runtime_error("TCP fixture connect failed.");
            }
        } catch (...) { close_socket(socket_); throw; }
    }
    ~Peer() { close_socket(socket_); }
    Peer(const Peer&)=delete;
    Peer& operator=(const Peer&)=delete;
    // Public synthetic bytes only. Close may race this write; either outcome
    // is acceptable because the listener must neither read nor answer payloads.
    void send_public_payload() const {
        constexpr std::array<char,32> payload{'P','U','B','L','I','C','-','T','C','P','-','F','I','X','T','U','R','E'};
#ifdef _WIN32
        (void)::send(socket_,payload.data(),static_cast<int>(payload.size()),0);
#else
        (void)::send(socket_,payload.data(),payload.size(),MSG_NOSIGNAL);
#endif
    }
    bool closes_without_application_data(CloseObservation* observation=nullptr) const {
        CloseObservation result;
        result.select_result=wait_socket(socket_,false);
#ifdef _WIN32
        if (result.select_result<0) result.select_error=WSAGetLastError();
#else
        if (result.select_result<0) result.select_error=errno;
#endif
        if (result.select_result<=0) {
            if (observation) *observation=result;
            return false;
        }
        std::array<char,16> buffer{};
        result.recv_called=true;
#ifdef _WIN32
        const int size=recv(socket_,buffer.data(),static_cast<int>(buffer.size()),0);
        if (size==SOCKET_ERROR) result.recv_error=WSAGetLastError();
        // The measured payload-close case returns terminal 10053; this oracle
        // requires closure without application bytes, not specifically a FIN.
        const bool closed=size==0 || (size==SOCKET_ERROR &&
            (result.recv_error==WSAECONNRESET || result.recv_error==WSAECONNABORTED));
#else
        const auto size=recv(socket_,buffer.data(),buffer.size(),0);
        if (size<0) result.recv_error=errno;
        const bool closed=size==0 || (size<0 && result.recv_error==ECONNRESET);
#endif
        result.recv_result=static_cast<int>(size);
        if (observation) {
            // SO_ERROR can clear the pending error: query only after recv and
            // the unchanged predicate, never before either operation.
#ifdef _WIN32
            int length=static_cast<int>(sizeof(result.socket_error_value));
            result.socket_error_query_result=getsockopt(socket_,SOL_SOCKET,SO_ERROR,
                reinterpret_cast<char*>(&result.socket_error_value),&length);
            if (result.socket_error_query_result!=0) result.socket_error_query_error=WSAGetLastError();
#else
            socklen_t length=sizeof(result.socket_error_value);
            result.socket_error_query_result=getsockopt(socket_,SOL_SOCKET,SO_ERROR,
                &result.socket_error_value,&length);
            if (result.socket_error_query_result!=0) result.socket_error_query_error=errno;
#endif
            *observation=result;
        }
        return closed;
    }
private:
    Socket socket_=invalid_socket;
};
}

TEST_CASE("TCP probe permits the original five establishment attempts without application bytes") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    REQUIRE(probe.port()!=0);
    for (int attempt=0;attempt<5;++attempt) {
        Peer peer(probe.port());
        CHECK(peer.closes_without_application_data());
    }
    CHECK(probe.running());
}

TEST_CASE("TCP probe fails startup on an occupied IPv4 loopback port with fixed text") {
    [[maybe_unused]] SocketRuntime runtime;
    ListenerFixture occupied;
    try {
        LocalTcpProbe probe(occupied.port());
        FAIL_CHECK("Occupied TCP endpoint was accepted.");
    } catch (const std::runtime_error& error) {
        CHECK(std::string_view(error.what())=="Local TCP probe could not be started.");
    }
    // Failure must not damage the existing listener or the fixture's WSA ref.
    CHECK_NOTHROW(Peer(occupied.port()));
}

TEST_CASE("TCP probe binds only loopback and excludes a second listener on its address") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    REQUIRE(probe.port()!=0);
    CHECK_THROWS_AS(ListenerFixture(probe.port()),std::runtime_error);
    // A wildcard bind would also occupy this different address in 127/8.
    CHECK_NOTHROW(ListenerFixture(probe.port(),0x7f000002U));
}

TEST_CASE("TCP probe idle stop releases its endpoint and is idempotent") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    const auto port=probe.port();
    REQUIRE(port!=0);
    const auto begin=std::chrono::steady_clock::now();
    probe.stop();
    CHECK(std::chrono::steady_clock::now()-begin<3s);
    CHECK_FALSE(probe.running());
    CHECK(probe.port()==port);
    probe.stop();
    LocalTcpProbe rebound(port);
    CHECK(rebound.running());
    CHECK(rebound.port()==port);
}

TEST_CASE("TCP probe destructor releases an idle listener without client work") {
    [[maybe_unused]] SocketRuntime runtime;
    auto probe=std::make_unique<LocalTcpProbe>(0);
    REQUIRE(probe->running());
    const auto port=probe->port();
    REQUIRE(port!=0);
    const auto begin=std::chrono::steady_clock::now();
    probe.reset();
    CHECK(std::chrono::steady_clock::now()-begin<3s);
    LocalTcpProbe rebound(port);
    CHECK(rebound.running());
}

TEST_CASE("TCP probe promptly closes an idle accepted client and stop does not wait on it") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    REQUIRE(probe.port()!=0);
    Peer idle(probe.port());
    CHECK(idle.closes_without_application_data());
    const auto begin=std::chrono::steady_clock::now();
    probe.stop();
    CHECK(std::chrono::steady_clock::now()-begin<3s);
    CHECK_FALSE(probe.running());
}

TEST_CASE("TCP probe does not answer public payloads or block subsequent establishments") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    REQUIRE(probe.port()!=0);
    Peer sending(probe.port());
    sending.send_public_payload();
    CloseObservation observation;
    const bool closed=sending.closes_without_application_data(&observation);
    CAPTURE(observation.select_result);
    CAPTURE(observation.select_error);
    CAPTURE(observation.recv_called);
    CAPTURE(observation.recv_result);
    CAPTURE(observation.recv_error);
    CAPTURE(observation.socket_error_query_result);
    CAPTURE(observation.socket_error_value);
    CAPTURE(observation.socket_error_query_error);
    CHECK(closed);
    Peer following(probe.port());
    CHECK(following.closes_without_application_data());
    CHECK(probe.running());
}

TEST_CASE("TCP probe accepts concurrent local attempts without retaining clients") {
    [[maybe_unused]] SocketRuntime runtime;
    LocalTcpProbe probe(0);
    REQUIRE(probe.running());
    REQUIRE(probe.port()!=0);
    const auto port=probe.port();
    std::array<std::future<bool>,8> peers;
    for (auto& peer:peers) peer=std::async(std::launch::async,[port] {
        Peer connection(port);
        return connection.closes_without_application_data();
    });
    for (auto& peer:peers) CHECK(peer.get());
    CHECK(probe.running());
}

TEST_CASE("TCP closure oracle rejects a real listener application response") {
    [[maybe_unused]] SocketRuntime runtime;
    ListenerFixture listener;
    Peer peer(listener.port());
    listener.send_public_response();
    CloseObservation observation;
    const bool closed=peer.closes_without_application_data(&observation);
    CAPTURE(observation.select_result);
    CAPTURE(observation.select_error);
    CAPTURE(observation.recv_called);
    CAPTURE(observation.recv_result);
    CAPTURE(observation.recv_error);
    CAPTURE(observation.socket_error_query_result);
    CAPTURE(observation.socket_error_value);
    CAPTURE(observation.socket_error_query_error);
    REQUIRE(observation.recv_called);
    REQUIRE(observation.recv_result==1);
    CHECK_FALSE(closed);
}
