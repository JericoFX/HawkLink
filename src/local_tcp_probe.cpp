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
#include <atomic>
#include <stdexcept>
#include <thread>

namespace hawken::tcp {
namespace {
[[noreturn]] void startup_failed() {
    throw std::runtime_error("Local TCP probe could not be started.");
}
#ifdef _WIN32
using NativeSocket=SOCKET;
constexpr NativeSocket invalid_socket=INVALID_SOCKET;
void close_socket(NativeSocket socket) noexcept { closesocket(socket); }
int socket_error() noexcept { return WSAGetLastError(); }
bool interrupted(int error) noexcept { return error==WSAEINTR; }
bool transient_accept(int error) noexcept {
    return interrupted(error) || error==WSAEWOULDBLOCK || error==WSAECONNRESET;
}
#else
using NativeSocket=int;
constexpr NativeSocket invalid_socket=-1;
void close_socket(NativeSocket socket) noexcept { ::close(socket); }
int socket_error() noexcept { return errno; }
bool interrupted(int error) noexcept { return error==EINTR; }
bool transient_accept(int error) noexcept {
    return interrupted(error) || error==EAGAIN || error==EWOULDBLOCK || error==ECONNABORTED;
}
#endif
}

struct LocalTcpProbe::Impl {
    explicit Impl(std::uint16_t requested) {
        try {
#ifdef _WIN32
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2,2),&data)!=0) startup_failed();
            initialized=true;
#endif
            listener=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if (listener==invalid_socket) startup_failed();
#ifdef _WIN32
            const int exclusive=1;
            if (setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),static_cast<int>(sizeof(exclusive)))!=0)
                startup_failed();
            u_long nonblocking=1;
            if (ioctlsocket(listener,FIONBIO,&nonblocking)!=0) startup_failed();
#else
            // Permit TCP TIME_WAIT reuse, not concurrent listening sockets.
            // SO_REUSEPORT is deliberately never enabled.
            const int reuse=1;
            if (setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))!=0)
                startup_failed();
            const int flags=fcntl(listener,F_GETFL,0);
            if (flags<0 || fcntl(listener,F_SETFL,flags|O_NONBLOCK)<0 || listener>=FD_SETSIZE)
                startup_failed();
#endif
            sockaddr_in address{};
            address.sin_family=AF_INET;
            address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            address.sin_port=htons(requested);
            if (::bind(listener,reinterpret_cast<const sockaddr*>(&address),static_cast<int>(sizeof(address)))!=0 ||
                listen(listener,32)!=0) startup_failed();
#ifdef _WIN32
            int size=static_cast<int>(sizeof(address));
#else
            socklen_t size=sizeof(address);
#endif
            if (getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)!=0 ||
                size!=static_cast<decltype(size)>(sizeof(address)) || address.sin_family!=AF_INET ||
                address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || address.sin_port==0)
                startup_failed();
            bound_port=ntohs(address.sin_port);
            running=true;
            worker=std::thread([this] { run(); });
        } catch (...) {
            running=false;
            release();
            throw;
        }
    }
    ~Impl() { stop(); }

    void stop() noexcept {
        // Owner thread only. Keep the descriptor and WSA reference valid until
        // the worker leaves its bounded wait; no concurrent close/select race.
        stopping=true;
        if (worker.joinable()) worker.join();
        running=false;
        release();
    }
    void run() noexcept {
        while (!stopping.load()) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener,&readable);
            timeval timeout{0,20000};
#ifdef _WIN32
            const int ready=select(0,&readable,nullptr,nullptr,&timeout);
#else
            const int ready=select(listener+1,&readable,nullptr,nullptr,&timeout);
#endif
            if (ready<0) {
                if (interrupted(socket_error())) continue;
                break;
            }
            if (ready==0) continue;
            bool fatal=false;
            for (int accepted=0;accepted<32 && !stopping.load();++accepted) {
                const auto peer=accept(listener,nullptr,nullptr);
                if (peer==invalid_socket) {
                    fatal=!transient_accept(socket_error());
                    break;
                }
                // Native PortTester only establishes TCP. Never retain a peer,
                // read application bytes, send a reply or inspect its address.
                close_socket(peer);
            }
            if (fatal) break;
        }
        running=false;
    }
    void release() noexcept {
        if (listener!=invalid_socket) {
            close_socket(listener);
            listener=invalid_socket;
        }
#ifdef _WIN32
        if (initialized) { WSACleanup(); initialized=false; }
#endif
    }

    NativeSocket listener=invalid_socket;
    std::uint16_t bound_port=0;
    std::atomic<bool> running{false},stopping{false};
    std::thread worker;
#ifdef _WIN32
    bool initialized=false;
#endif
};

LocalTcpProbe::LocalTcpProbe(std::uint16_t requested_port) {
    try { impl_=std::make_unique<Impl>(requested_port); }
    catch (...) { startup_failed(); }
}
LocalTcpProbe::~LocalTcpProbe() = default;
void LocalTcpProbe::stop() noexcept { impl_->stop(); }
bool LocalTcpProbe::running() const noexcept { return impl_->running.load(); }
std::uint16_t LocalTcpProbe::port() const noexcept { return impl_->bound_port; }
}
