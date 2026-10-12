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
#include "hawken/local_udp_owner.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace hawken::udp {
LocalUdpError::LocalUdpError(LocalUdpErrorCode code)
    : std::runtime_error("Local UDP operation could not be completed."), code_(code) {}
LocalUdpErrorCode LocalUdpError::code() const noexcept { return code_; }
namespace {
[[noreturn]] void fail(LocalUdpErrorCode code) { throw LocalUdpError(code); }
void erase(std::span<std::uint8_t> bytes) noexcept {
    volatile std::uint8_t* cursor=bytes.data();
    for (std::size_t i=0;i<bytes.size();++i) cursor[i]=0;
}
void validate(const game::ConnectionConfig& cfg,const LocalUdpOwner::Clock& clock,
              std::chrono::milliseconds interval) {
    const auto count=[](std::size_t n){return n>0 && n<=1024;};
    const auto zero=[](std::uint8_t value){return value==0;};
    const auto& t=cfg.transport; const auto& q=t.limits; const auto& l=cfg.limits;
    // Validate scalars only: constructing a probe GameConnection would create a
    // second Context. Full lower constructors still validate the real handoff.
    if (!clock || interval.count()<1 || interval.count()>1000 ||
        !std::all_of(cfg.session_id.begin(),cfg.session_id.end(),zero) ||
        !std::all_of(cfg.session_key.begin(),cfg.session_key.end(),zero) ||
        (cfg.endian!=control::Endian::little && cfg.endian!=control::Endian::big) ||
        cfg.server_min>cfg.server_max || t.max_packet_bytes<2 ||
        t.max_packet_bytes>session::max_payload_bytes ||
        !std::isfinite(t.incoming_timeout) || t.incoming_timeout<=0 ||
        !count(q.max_outgoing_bunches) || !count(q.max_incoming_bunches) ||
        !count(q.max_ack_ids) || !count(q.max_records_per_packet) ||
        !count(q.max_packets_per_step) || q.max_payload_bytes==0 || q.max_payload_bytes>1048576 ||
        l.max_datagram_bytes>session::max_packet_bytes ||
        l.max_datagram_bytes<static_cast<std::size_t>(t.max_packet_bytes)+session::wrapper_bytes ||
        !count(l.max_logical_messages_per_step) || !count(l.max_datagrams_per_step) ||
        l.max_output_bytes_per_step==0 || l.max_output_bytes_per_step>1048576 ||
        cfg.login.limits.max_archive_bytes==0 || cfg.login.limits.max_string_units==0 ||
        cfg.login.limits.max_string_units>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        cfg.login.driver_max_speed<1800)
        fail(LocalUdpErrorCode::invalid_configuration);
}
#ifdef _WIN32
using NativeSocket=SOCKET;
constexpr NativeSocket invalid_socket=INVALID_SOCKET;
int socket_error() noexcept { return WSAGetLastError(); }
bool transient(int code) noexcept { return code==WSAEWOULDBLOCK || code==WSAEINTR; }
#else
using NativeSocket=int;
constexpr NativeSocket invalid_socket=-1;
int socket_error() noexcept { return errno; }
bool transient(int code) noexcept { return code==EAGAIN || code==EWOULDBLOCK || code==EINTR; }
#endif
class BoundSocket {
public:
    explicit BoundSocket(std::uint16_t requested) {
        try {
#ifdef _WIN32
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2,2),&data)!=0) fail(LocalUdpErrorCode::bind_failed);
            initialized_=true;
#endif
            handle=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
            if (handle==invalid_socket) fail(LocalUdpErrorCode::bind_failed);
#ifdef _WIN32
            const int exclusive=1;
            if (setsockopt(handle,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),static_cast<int>(sizeof(exclusive)))!=0)
                fail(LocalUdpErrorCode::bind_failed);
            u_long nonblocking=1;
            if (ioctlsocket(handle,FIONBIO,&nonblocking)!=0) fail(LocalUdpErrorCode::bind_failed);
#else
            // No SO_REUSEADDR/SO_REUSEPORT: another socket cannot adopt this bind.
            const int flags=fcntl(handle,F_GETFL,0);
            if (flags<0 || fcntl(handle,F_SETFL,flags|O_NONBLOCK)<0) fail(LocalUdpErrorCode::bind_failed);
            if (handle>=FD_SETSIZE) fail(LocalUdpErrorCode::bind_failed);
#endif
            sockaddr_in address{}; address.sin_family=AF_INET;
            address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(requested);
            if (::bind(handle,reinterpret_cast<const sockaddr*>(&address),static_cast<int>(sizeof(address)))!=0)
                fail(LocalUdpErrorCode::bind_failed);
            if (!local(address) || address.sin_family!=AF_INET ||
                address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || address.sin_port==0)
                fail(LocalUdpErrorCode::bind_failed);
            port=ntohs(address.sin_port);
        } catch (...) { close(); cleanup(); throw; }
    }
    ~BoundSocket() { close(); cleanup(); }
    BoundSocket(const BoundSocket&)=delete;
    BoundSocket& operator=(const BoundSocket&)=delete;
    bool local(sockaddr_in& address) const noexcept {
#ifdef _WIN32
        int size=static_cast<int>(sizeof(address));
#else
        socklen_t size=sizeof(address);
#endif
        return handle!=invalid_socket && getsockname(handle,reinterpret_cast<sockaddr*>(&address),&size)==0
            && size==static_cast<decltype(size)>(sizeof(address));
    }
    void close() noexcept {
        if (handle==invalid_socket) return;
#ifdef _WIN32
        closesocket(handle);
#else
        ::close(handle);
#endif
        handle=invalid_socket;
    }
    NativeSocket handle=invalid_socket;
    std::uint16_t port=0;
private:
    void cleanup() noexcept {
#ifdef _WIN32
        if (initialized_) { WSACleanup(); initialized_=false; }
#endif
    }
#ifdef _WIN32
    bool initialized_=false;
#endif
};
struct Entry {
    session::SessionId id;
    std::unique_ptr<game::GameConnection> connection;
    // Registry mutex protects active/published/users; the ONE worker alone
    // accesses connection and peer while holding an in-flight reference.
    bool active=false,published=false;
    std::size_t users=0;
    std::optional<sockaddr_in> peer;
    Entry(const session::SessionId& value,std::unique_ptr<game::GameConnection> conn)
        : id(value),connection(std::move(conn)) {}
    ~Entry() { erase(id); }
};
struct Registry {
    std::mutex mutex;
    std::condition_variable finished;
    std::shared_ptr<Entry> entry;
    bool accepting=false,installed_once=false;
    std::atomic<std::uint64_t> authenticated{0};
    std::atomic<bool> hello{false},closed{false},login_received{false},login_validated{false};
    std::atomic<bool> world_published{false},join_requested{false},controller_sent{false};

    void retire(const std::shared_ptr<Entry>& target) noexcept {
        if (!target) return;
        try {
            std::unique_lock lock(mutex);
            target->active=false;
            if (entry==target) entry.reset();
            if (target->published) closed=true;
            // Users acquired their ownership AFTER authorization. None can
            // re-enter Service/Auth while pinned; no worker join here.
            finished.wait(lock,[&]{return target->users==0;});
            target->connection.reset();
            erase(target->id);
        } catch (...) {
            // Retained shared ownership also keeps the Context alive through
            // any in-flight user if the OS synchronization primitive fails.
            closed=true;
        }
    }
    void retire_id(const session::SessionId& id) noexcept {
        std::shared_ptr<Entry> target;
        {
            std::lock_guard lock(mutex);
            if (entry && entry->id==id) target=entry;
        }
        retire(target);
    }
    void close() noexcept {
        std::shared_ptr<Entry> target;
        {
            std::lock_guard lock(mutex);
            accepting=false; target=entry;
        }
        closed=true; retire(target);
    }
};
class Lease final : public assignment::UdpSessionLease {
public:
    Lease(std::shared_ptr<Registry> registry,std::shared_ptr<Entry> entry)
        : registry_(std::move(registry)),entry_(std::move(entry)) {}
    ~Lease() override { registry_->retire(entry_); }
    bool active() const noexcept override {
        try {
            std::lock_guard lock(registry_->mutex);
            return registry_->accepting && entry_->active && registry_->entry==entry_;
        } catch (...) { return false; }
    }
private:
    std::shared_ptr<Registry> registry_;
    std::shared_ptr<Entry> entry_;
};
class Use {
public:
    Use(std::shared_ptr<Registry> registry,std::shared_ptr<Entry> entry)
        : registry_(std::move(registry)),entry_(std::move(entry)) {}
    Use(const Use&)=delete;
    Use& operator=(const Use&)=delete;
    Use(Use&&)=default;
    ~Use() {
        if (!entry_) return;
        std::lock_guard lock(registry_->mutex);
        --entry_->users; registry_->finished.notify_all();
    }
    Entry& entry() const noexcept { return *entry_; }
    std::shared_ptr<Entry> identity() const noexcept { return entry_; }
private:
    std::shared_ptr<Registry> registry_;
    std::shared_ptr<Entry> entry_;
};
struct KeyErase {
    game::ConnectionConfig& config;
    ~KeyErase() { erase(config.session_key); erase(config.session_id); }
};
}

struct LocalUdpOwner::Impl {
    game::ConnectionConfig skeleton;
    Clock clock;
    std::chrono::milliseconds interval;
    std::shared_ptr<Registry> registry=std::make_shared<Registry>();
    BoundSocket socket;
    mutable std::mutex socket_mutex;
    std::mutex clock_mutex;
    std::mutex handoff_mutex;
    std::optional<double> last_clock;
    std::atomic<bool> running{false},stopping{false};
    bool started=false;
    Authorizer authorizer;
    LoginResponseValidator login_validator;
    PublicationProvider publication_provider;
    std::optional<InitialControllerPlan> controller_plan;
    std::thread worker;

    Impl(std::uint16_t port,const game::ConnectionConfig& cfg,Clock value,std::chrono::milliseconds poll)
        : skeleton(cfg),clock(std::move(value)),interval(poll),socket(port) {}
    ~Impl() { stop(); }
    bool bound(const assignment::Destination& destination) const noexcept {
        try {
            std::lock_guard lock(socket_mutex);
            sockaddr_in actual{};
            return running.load() && destination.ip=="127.0.0.1" && destination.port==socket.port &&
                socket.local(actual) && actual.sin_family==AF_INET &&
                actual.sin_addr.s_addr==htonl(INADDR_LOOPBACK) && ntohs(actual.sin_port)==socket.port;
        } catch (...) { return false; }
    }
    std::optional<double> time() noexcept {
        try {
            std::lock_guard lock(clock_mutex);
            const double now=clock();
            if (!std::isfinite(now) || now<0 || (last_clock && now<*last_clock)) return std::nullopt;
            last_clock=now; return now;
        } catch (...) { return std::nullopt; }
    }
    void fail_closed() noexcept {
        stopping=true; registry->close(); running=false;
    }
    bool authorize(const session::SessionId& id) noexcept {
        bool accepted=false;
        try { accepted=authorizer(id); } catch (...) {}
        if (!accepted) registry->retire_id(id);
        return accepted;
    }
    std::optional<Use> pin(const session::SessionId& id,const std::shared_ptr<Entry>& expected={}) {
        std::lock_guard lock(registry->mutex);
        if (!running.load() || !registry->accepting || !registry->entry ||
            !registry->entry->active || registry->entry->id!=id ||
            (expected && registry->entry!=expected)) return std::nullopt;
        ++registry->entry->users;
        return Use(registry,registry->entry);
    }
    bool send(const Entry& entry,const game::ConnectionStep& step) noexcept {
        if (stopping.load()) return true;
        if (!step.datagrams.empty() && !entry.peer) return false;
        for (const auto& bytes:step.datagrams) {
#ifdef _WIN32
            const auto count=sendto(socket.handle,reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),0,reinterpret_cast<const sockaddr*>(&*entry.peer),
                static_cast<int>(sizeof(sockaddr_in)));
#else
            const auto count=sendto(socket.handle,bytes.data(),bytes.size(),0,
                reinterpret_cast<const sockaddr*>(&*entry.peer),sizeof(sockaddr_in));
#endif
            // A failed UDP send cannot rewind an already-reserved nonce.
            if (count<0 || static_cast<std::size_t>(count)!=bytes.size()) return false;
        }
        return true;
    }
    void validate_response(const session::SessionId& id,const std::shared_ptr<Entry>& target,
                           const control::posthello::Login& candidate) {
        // The shared identity is not an in-flight pin. Retirement may finish
        // here, including synchronous lease destruction inside the validator.
        bool accepted=false;
        try { accepted=login_validator(id,candidate.response); } catch (...) {}
        std::optional<game::WorldPublication> publication;
        if (accepted && publication_provider) {
            try { publication=publication_provider(id,candidate); } catch (...) {}
            accepted=publication.has_value();
        }
        // No callback holds a Use or registry lock. Even a returned plan may
        // refer to a lease synchronously retired by either callback.
        if (!authorize(id)) return;
        const auto now=time();
        if (!now) { fail_closed(); return; }
        {
            auto use=pin(id,target);
            if (!use) return;
            auto& entry=use->entry();
            const auto snapshot=entry.connection->snapshot();
            if (snapshot.closed || snapshot.login.state!=control::login::State::login_received) return;
            {
                std::lock_guard lock(registry->mutex);
                if (!running.load() || !registry->accepting || registry->entry!=target || !entry.active) return;
                if (accepted) {
                    // Observation of a revalidated identity, not world admission
                    // or a guarantee against a subsequent revocation.
                    registry->login_validated=true;
                    if (!publication) return;
                }
            }
            try {
                if (accepted) {
                    const auto step=entry.connection->publish_world(*publication,*now);
                    if (!stopping.load() && send(entry,step) && !step.closed) {
                        registry->world_published=true;
                        return;
                    }
                } else {
                    const auto step=entry.connection->reject_login(*now);
                    send(entry,step);
                }
            } catch (...) {}
            // Denial or publication failure is terminal even if sending failed. Do not rewind
            // the existing Context's nonce, or retire while holding this Use.
        }
        registry->retire(target);
    }
    void receive(std::span<const std::uint8_t> bytes,const sockaddr_in& peer,double now) {
        session::SessionId id{};
        std::copy_n(bytes.begin(),id.size(),id.begin());
        // No registry access/pin before authorization. It may synchronously
        // destroy the lease through Assignment without waiting on this worker.
        if (!authorize(id)) { erase(id); return; }
        bool terminal=false;
        std::shared_ptr<Entry> target;
        std::optional<control::posthello::Login> candidate;
        {
            auto use=pin(id);
            if (!use) { erase(id); return; }
            auto& entry=use->entry();
            if (entry.peer && (entry.peer->sin_addr.s_addr!=peer.sin_addr.s_addr ||
                              entry.peer->sin_port!=peer.sin_port)) { erase(id); return; }
            try {
                auto step=entry.connection->receive(bytes,now);
                const auto count=registry->authenticated.load();
                if (count!=std::numeric_limits<std::uint64_t>::max()) registry->authenticated=count+1;
                if (step.hello) registry->hello=true;
                if (step.login_candidate) {
                    registry->login_received=true;
                    if (login_validator) {
                        target=use->identity();
                        candidate=std::move(*step.login_candidate);
                    }
                }
                if (!entry.peer) entry.peer=peer; // ONLY successful authenticated receive.
                const bool sent=send(entry,step);
                terminal=step.closed || !sent;
                if (sent && !step.closed && step.join_requested) {
                    registry->join_requested=true;
                    if (controller_plan) {
                        try {
                            if (stopping.load()) terminal=true;
                            else {
                                const auto& plan=*controller_plan;
                                const auto reference=entry.connection->resolve_package_reference(
                                    plan.archetype_package_guid,plan.archetype_local_net_index);
                                auto payload=wire::actor::encode_initial_controller(
                                    {reference,plan.position,plan.rotation,plan.net_player_index},
                                    static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                                entry.connection->register_actor_channel(plan.channel);
                                // Explicit first-controller policy, not inferred native flags.
                                wire::Bunch bunch;
                                bunch.control=true; bunch.open=true; bunch.reliable=true;
                                bunch.channel=plan.channel.channel; bunch.channel_type=2;
                                bunch.payload_bits=payload.payload_bits;
                                bunch.payload=std::move(payload.payload);
                                const auto actor=entry.connection->send_actor(bunch,now);
                                // send() can report no-op success during shutdown. Its
                                // monotonic stopping flag must also be checked AFTER send.
                                if (!actor.datagrams.empty() && !stopping.load() &&
                                    send(entry,actor) && !stopping.load() && !actor.closed)
                                    registry->controller_sent=true;
                                else terminal=true;
                            }
                        } catch (...) { terminal=true; }
                    }
                }
            } catch (const game::ConnectionError&) {
                terminal=entry.connection->snapshot().closed;
            } catch (...) { terminal=true; }
        } // Release in-flight ownership BEFORE retiring or any callback.
        if (terminal) registry->retire_id(id);
        else if (candidate) validate_response(id,target,*candidate);
        erase(id);
    }
    void idle(double now) {
        session::SessionId id{};
        {
            std::lock_guard lock(registry->mutex);
            if (!registry->entry || !registry->entry->active) return;
            id=registry->entry->id; // Snapshot ONLY; not Context/lease ownership.
        }
        if (!authorize(id)) { erase(id); return; }
        bool terminal=false;
        {
            auto use=pin(id);
            if (!use) { erase(id); return; }
            auto& entry=use->entry();
            try {
                const auto step=entry.connection->tick(now);
                const bool sent=send(entry,step);
                terminal=step.closed || !sent;
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire_id(id);
        erase(id);
    }
    void loop() noexcept {
        try {
            std::array<std::uint8_t,session::max_packet_bytes> bytes{};
            while (!stopping.load()) {
                fd_set readable; FD_ZERO(&readable); FD_SET(socket.handle,&readable);
                const auto ms=interval.count();
                timeval wait{static_cast<long>(ms/1000),static_cast<long>((ms%1000)*1000)};
#ifdef _WIN32
                const int ready=select(0,&readable,nullptr,nullptr,&wait);
#else
                const int ready=select(socket.handle+1,&readable,nullptr,nullptr,&wait);
#endif
                if (ready<0) {
                    if (transient(socket_error())) continue;
                    fail_closed(); return;
                }
                if (stopping.load()) break;
                const auto now=time();
                if (!now) { fail_closed(); return; }
                if (ready>0) {
                    sockaddr_in peer{};
#ifdef _WIN32
                    int peer_size=static_cast<int>(sizeof(peer));
                    const auto count=recvfrom(socket.handle,reinterpret_cast<char*>(bytes.data()),
                        static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),&peer_size);
                    if (count<0) {
                        const int error=socket_error();
                        if (error!=WSAEMSGSIZE && !transient(error)) { fail_closed(); return; }
                    } else if (peer_size==static_cast<int>(sizeof(peer)) && peer.sin_family==AF_INET &&
                        (ntohl(peer.sin_addr.s_addr)>>24)==127 && count>static_cast<int>(session::wrapper_bytes) &&
                        static_cast<std::size_t>(count)<=skeleton.limits.max_datagram_bytes) {
                        receive({bytes.data(),static_cast<std::size_t>(count)},peer,*now);
                    }
#else
                    iovec buffer{bytes.data(),bytes.size()};
                    msghdr message{}; message.msg_name=&peer; message.msg_namelen=sizeof(peer);
                    message.msg_iov=&buffer; message.msg_iovlen=1;
                    const auto count=recvmsg(socket.handle,&message,0);
                    if (count<0) {
                        if (!transient(socket_error())) { fail_closed(); return; }
                    } else if (!(message.msg_flags&MSG_TRUNC) && message.msg_namelen==sizeof(peer) &&
                        peer.sin_family==AF_INET && (ntohl(peer.sin_addr.s_addr)>>24)==127 &&
                        count>static_cast<ssize_t>(session::wrapper_bytes) &&
                        static_cast<std::size_t>(count)<=skeleton.limits.max_datagram_bytes) {
                        receive({bytes.data(),static_cast<std::size_t>(count)},peer,*now);
                    }
#endif
                    erase(bytes);
                }
                // Read time again: callback duration cannot make tick use a
                // timestamp older than a handoff's intervening clock sample.
                const auto tick_time=time();
                if (!tick_time) { fail_closed(); return; }
                if (!stopping.load()) idle(*tick_time);
            }
        } catch (...) { fail_closed(); }
    }
    void start(Authorizer callback,LoginResponseValidator validator,PublicationProvider publication,
               std::optional<InitialControllerPlan> controller) {
        if (!callback || started || stopping.load()) fail(LocalUdpErrorCode::invalid_lifecycle);
        if (publication && !validator) fail(LocalUdpErrorCode::invalid_configuration);
        if (controller) {
            const auto& plan=*controller;
            if (!validator || !publication || plan.channel.channel==0 ||
                plan.channel.channel>2047 || plan.archetype_local_net_index<0)
                fail(LocalUdpErrorCode::invalid_configuration);
            // Validate transforms locally; never construct a second Context or
            // resolve package coordinates before the actual publication/Join.
            try {
                wire::BitWriter writer;
                const auto cap=std::numeric_limits<std::size_t>::max();
                wire::actor::write_position(writer,plan.position,cap);
                if (plan.rotation) wire::actor::write_rotation(writer,*plan.rotation,cap);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        authorizer=std::move(callback); login_validator=std::move(validator);
        publication_provider=std::move(publication); controller_plan=std::move(controller); started=true;
        {
            std::lock_guard lock(registry->mutex); registry->accepting=true;
        }
        running=true;
        try { worker=std::thread([this]{loop();}); }
        catch (...) {
            fail_closed(); authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset();
            fail(LocalUdpErrorCode::invalid_lifecycle);
        }
    }
    void stop() noexcept {
        running=false; stopping=true; registry->close();
        if (worker.joinable()) worker.join();
        authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset();
        { std::lock_guard lock(clock_mutex); clock={}; }
        std::lock_guard lock(socket_mutex); socket.close();
    }
    std::unique_ptr<assignment::UdpSessionLease> handoff(const assignment::SessionProvision& provision) {
        // Serialize construction as well as publication: a competing handoff
        // must not construct a second tentative Context for this owner.
        std::lock_guard construction(handoff_mutex);
        if (!bound(provision.destination)) return nullptr;
        const auto now=time();
        if (!now) { fail_closed(); return nullptr; }
        if (static_cast<long double>(provision.expires_at_monotonic)<=static_cast<long double>(*now)) return nullptr;
        {
            std::lock_guard lock(registry->mutex);
            if (!registry->accepting || registry->entry || registry->installed_once) return nullptr;
        }
        try {
            auto config=skeleton;
            config.session_id=provision.id; config.session_key=provision.key;
            const KeyErase clear{config};
            auto entry=std::make_shared<Entry>(provision.id,std::make_unique<game::GameConnection>(config));
            // Allocate lease BEFORE publishing; any exception owns and destroys
            // the unexposed connection. No successful budget is consumed yet.
            auto lease=std::make_unique<Lease>(registry,entry);
            {
                std::lock_guard lock(registry->mutex);
                if (!running.load() || !registry->accepting || registry->entry || registry->installed_once)
                    return nullptr;
                // TODO: [shortcut] single provision per local owner lifetime;
                // upgrade before multi-session admission with reviewed fresh
                // unique ID/key rotation, NEVER reconstruction of an old Context.
                registry->entry=entry; entry->active=true; entry->published=true;
                registry->installed_once=true;
            }
            return lease;
        } catch (const game::ConnectionError& error) {
            fail(error.code()==game::ConnectionErrorCode::invalid_configuration
                ? LocalUdpErrorCode::invalid_configuration : LocalUdpErrorCode::invalid_lifecycle);
        } catch (...) { fail(LocalUdpErrorCode::invalid_lifecycle); }
    }
};
LocalUdpOwner::LocalUdpOwner(std::uint16_t port,const game::ConnectionConfig& skeleton,
                           Clock clock,std::chrono::milliseconds interval) {
    validate(skeleton,clock,interval);
    try { impl_=std::make_unique<Impl>(port,skeleton,std::move(clock),interval); }
    catch (const LocalUdpError&) { throw; }
    catch (...) { fail(LocalUdpErrorCode::invalid_lifecycle); }
}
LocalUdpOwner::~LocalUdpOwner() = default;
std::uint16_t LocalUdpOwner::port() const noexcept { return impl_->socket.port; }
bool LocalUdpOwner::running() const noexcept { return impl_->running.load(); }
LocalUdpDiagnostics LocalUdpOwner::diagnostics() const noexcept {
    return {impl_->registry->authenticated.load(),impl_->registry->hello.load(),impl_->registry->closed.load(),
        impl_->registry->login_received.load(),impl_->registry->login_validated.load(),
        impl_->registry->world_published.load(),impl_->registry->join_requested.load(),
        impl_->registry->controller_sent.load()};
}
bool LocalUdpOwner::destination_bound(const assignment::Destination& destination) const noexcept {
    return impl_->bound(destination);
}
void LocalUdpOwner::start(Authorizer authorize, LoginResponseValidator validate_response,
                          PublicationProvider publication,
                          std::optional<InitialControllerPlan> controller) {
    impl_->start(std::move(authorize),std::move(validate_response),std::move(publication),
        std::move(controller));
}
void LocalUdpOwner::stop() noexcept { impl_->stop(); }
std::unique_ptr<assignment::UdpSessionLease> LocalUdpOwner::handoff(
    const assignment::SessionProvision& provision) { return impl_->handoff(provision); }
}
