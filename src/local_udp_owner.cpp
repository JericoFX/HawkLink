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
#include "hawken/spawn_validation_rpc_wire.hpp"
#include "hawken/controller_admission_rpc_wire.hpp"
#include "hawken/pawn_rpc_wire.hpp"
#include "hawken/initial_pawn.hpp"
#include "hawken/object_property_wire.hpp"
#include "hawken/unique_id_wire.hpp"
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
#include <vector>

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
std::array<wire::property::PropertyPayload,2> link_payloads(
    const InitialActorLinksPlan& plan,std::uint16_t controller,std::uint16_t pri,
    std::uint16_t pawn,std::size_t max_bits) {
    const std::array controller_fields{
        wire::property::ObjectProperty{plan.controller_pri_field_id,wire::object::ActorChannelReference{pri}},
        wire::property::ObjectProperty{plan.controller_pawn_field_id,wire::object::ActorChannelReference{pawn}}};
    const std::array pawn_fields{
        wire::property::ObjectProperty{plan.pawn_controller_field_id,wire::object::ActorChannelReference{controller}},
        wire::property::ObjectProperty{plan.pawn_pri_field_id,wire::object::ActorChannelReference{pri}}};
    return {wire::property::encode_object_properties(controller_fields,plan.controller_exclusive_bound,max_bits),
        wire::property::encode_object_properties(pawn_fields,plan.pawn_exclusive_bound,max_bits)};
}
struct Entry {
    session::SessionId id;
    std::unique_ptr<game::GameConnection> connection;
    // Registry mutex protects active/published/users; the ONE worker alone
    // accesses connection and peer while holding an in-flight reference.
    bool active=false,published=false;
    std::size_t users=0;
    std::optional<sockaddr_in> peer;
    bool links_sent=false; // Worker-only per-session emission state, not a diagnostic gate.
    std::optional<wire::identity::UniqueId> pri_identity;
    bool pri_identity_sent=false;
    std::optional<wire::rpc::StartMechArguments> pending_mech;
    std::uint8_t setup_id;
    std::optional<wire::rpc::StartMechArguments> accepted_mech;
    std::uint64_t mech_revision=0;
    bool validation_pending=false,validation_waits_for_mech=false,validation_responded=false;
    bool validation_succeeded=false;
    std::optional<std::uint64_t> admission_request_revision,admitted_revision;

    Entry(const session::SessionId& value,std::unique_ptr<game::GameConnection> conn,
          std::uint8_t initial_setup_id,std::optional<wire::identity::UniqueId> identity_value)
        : id(value),connection(std::move(conn)),pri_identity(std::move(identity_value)),setup_id(initial_setup_id) {}
    ~Entry() {
        if (pri_identity) pri_identity->words.fill(0);
        erase(id);
    }
};
struct Registry {
    std::mutex mutex;
    std::condition_variable finished;
    std::shared_ptr<Entry> entry;
    bool accepting=false,installed_once=false;
    std::atomic<std::uint64_t> authenticated{0};
    std::atomic<bool> hello{false},closed{false},login_received{false},login_validated{false};
    std::atomic<bool> world_published{false},join_requested{false},controller_sent{false},pawn_sent{false},pri_sent{false};
    std::atomic<bool> spawn_validation_requested{false},spawn_validation_sent{false},spawn_validation_failed{false};
    std::atomic<bool> stage3_requested{false},client_restart_sent{false};
    std::atomic<bool> controller_open_acked{false},pri_open_acked{false},pawn_open_acked{false};
    std::atomic<bool> actor_links_sent{false},pri_identity_sent{false},pri_identity_acked{false};

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
            target->pending_mech.reset();
            target->accepted_mech.reset();
            if (target->pri_identity) target->pri_identity->words.fill(0);
            target->pri_identity.reset();
            target->validation_pending=false;
            target->admission_request_revision.reset();
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
    std::optional<InitialPawnPlan> pawn_plan;
    std::optional<InitialPriPlan> pri_plan;
    std::optional<InitialActorLinksPlan> links_plan;
    std::optional<StartMechDispatchPlan> start_mech_plan;
    StartMechProvider mech_provider;
    std::optional<SpawnValidationDispatchPlan> validation_plan;
    SpawnSelectionValidator spawn_validator;
    std::optional<SpawnAdmissionDispatchPlan> admission_plan;
    std::optional<PriIdentityDispatchPlan> pri_identity_plan;
    bool provision_identity_enabled=false; // Immutable after start, also read by handoff.
    std::uint8_t initial_setup_id=0; // Immutable after start publishes running, also read by handoff.
    struct MechWork {
        std::shared_ptr<Entry> target;
        wire::rpc::StartMechArguments request;
        std::uint8_t previous_setup_id;
    };
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
    bool send_identity(Entry& entry,double now) {
        if (!pri_identity_plan) return true;
        if (stopping.load()) return false;
        const auto state=entry.connection->channel_snapshot(pri_plan->channel.channel);
        if (!state || !state->opening_packet_id || !state->outgoing_open_acked || state->closed)
            return true;
        if (entry.pri_identity_sent) {
            if (state->outgoing_bunches==0) registry->pri_identity_acked=true;
            return true;
        }
        if (!entry.pri_identity) return false;
        const auto& plan=*pri_identity_plan;
        auto payload=wire::identity::encode_unique_id_property(*entry.pri_identity,
            plan.field_id,plan.exclusive_bound,plan.integer_order,
            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
        wire::Bunch bunch;
        bunch.reliable=true; bunch.channel_type=2; bunch.channel=pri_plan->channel.channel;
        bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
        const auto step=entry.connection->send_actor(bunch,now);
        if (step.datagrams.empty() || step.closed || stopping.load() ||
            !send(entry,step) || stopping.load()) return false;
        entry.pri_identity_sent=true; registry->pri_identity_sent=true;
        return true;
    }
    bool send_links(Entry& entry,double now) noexcept {
        if (stopping.load()) return false;
        try {
            const auto observe=[&](const auto& plan,std::atomic<bool>& flag) {
                if (!plan) return;
                const auto state=entry.connection->channel_snapshot(plan->channel.channel);
                if (state && state->opening_packet_id && state->outgoing_open_acked && !state->closed)
                    flag=true;
            };
            observe(controller_plan,registry->controller_open_acked);
            observe(pri_plan,registry->pri_open_acked);
            observe(pawn_plan,registry->pawn_open_acked);
            if (!send_identity(entry,now)) return false;
            if (!links_plan || entry.links_sent) return true;
            if (pri_identity_plan) {
                const auto state=entry.connection->channel_snapshot(pri_plan->channel.channel);
                if (!entry.pri_identity_sent || !state || state->outgoing_bunches!=0)
                    return true;
            }
            const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
                pawn_plan->channel.channel};
            for (const auto channel:channels) {
                const auto state=entry.connection->channel_snapshot(channel);
                if (!state || !state->opening_packet_id || !state->outgoing_open_acked || state->closed)
                    return true; // Pending/closed channels never release this plan.
            }
            // Compose BOTH batches before reserving the first packet/nonce.
            auto payloads=link_payloads(*links_plan,channels[0],channels[1],channels[2],
                static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
            const std::array destinations{channels[0],channels[2]};
            for (std::size_t i=0;i<payloads.size();++i) {
                if (stopping.load()) return false;
                wire::Bunch bunch;
                bunch.reliable=true; bunch.channel_type=2; bunch.channel=destinations[i];
                bunch.payload_bits=payloads[i].logical_bits; bunch.payload=std::move(payloads[i].payload);
                const auto step=entry.connection->send_actor(bunch,now);
                if (step.datagrams.empty() || step.closed || stopping.load() ||
                    !send(entry,step) || stopping.load()) return false;
            }
            entry.links_sent=true; registry->actor_links_sent=true;
            return true;
        } catch (...) { return false; } // Caller retires only AFTER releasing Use.
    }
    bool collect_start_mech(Entry& entry,const std::vector<wire::Bunch>& bunches) {
        if (!start_mech_plan) return true;
        const auto request_validation=[&] {
            registry->spawn_validation_requested=true;
            if (!entry.validation_responded || entry.pending_mech) {
                entry.validation_pending=true;
                entry.validation_waits_for_mech=entry.pending_mech.has_value();
            }
        };
        for (const auto& bunch:bunches) {
            if (bunch.channel!=controller_plan->channel.channel || bunch.payload_bits==0) continue;
            const auto state=entry.connection->channel_snapshot(bunch.channel);
            if (!state || state->closed) return false;
            wire::BitReader reader(bunch.payload,bunch.payload_bits);
            // Known schemas advance by their exact argument boundary. An unknown
            // or truncated tag ends parsing; never search an opaque suffix.
            while (reader.position()<bunch.payload_bits) {
                std::uint32_t field;
                try { field=reader.read_bounded(start_mech_plan->controller_exclusive_bound); }
                catch (const wire::WireError&) { break; }
                if (validation_plan && field==validation_plan->validate_rpc_field_id) {
                    request_validation();
                    if (!admission_plan) break;
                } else if (admission_plan && field==admission_plan->stage_rpc_field_id) {
                    const auto decoded=wire::rpc::read_selection_stage_arguments(reader,skeleton.endian,
                        bunch.payload_bits-reader.position());
                    if (decoded.stage==3) {
                        registry->stage3_requested=true;
                        if (entry.validation_succeeded && entry.validation_responded && entry.accepted_mech &&
                            !entry.pending_mech && entry.admitted_revision!=entry.mech_revision)
                            entry.admission_request_revision=entry.mech_revision;
                    } else entry.admission_request_revision.reset();
                } else if (field==start_mech_plan->controller_rpc_field_id) {
                    auto decoded=wire::rpc::read_start_mech_arguments(reader,skeleton.endian,
                        bunch.payload_bits-reader.position());
                    if (entry.pending_mech) {
                        const auto& prior=*entry.pending_mech;
                        if (prior.mech_instance_id!=decoded.value.mech_instance_id ||
                            prior.primary_weapon!=decoded.value.primary_weapon ||
                            prior.holo_taunt_type!=decoded.value.holo_taunt_type) return false;
                    } else entry.pending_mech=std::move(decoded.value);
                    // A newly requested preset cannot borrow the old validation.
                    entry.validation_succeeded=false;
                    entry.admission_request_revision.reset();
                    if (!admission_plan) {
                        if (validation_plan && reader.position()<bunch.payload_bits) {
                            try {
                                if (reader.read_bounded(validation_plan->controller_exclusive_bound)==
                                    validation_plan->validate_rpc_field_id) request_validation();
                            } catch (const wire::WireError&) {}
                        }
                        break;
                    }
                } else break;
            }
        }
        return true;
    }
    bool take_start_mech(Use& use,std::optional<MechWork>& work) {
        auto& entry=use.entry();
        if (!start_mech_plan) return true;
        if (stopping.load() || entry.connection->snapshot().closed) return false;
        bool ready=entry.links_sent;
        const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
            pawn_plan->channel.channel};
        for (const auto channel:channels) {
            const auto state=entry.connection->channel_snapshot(channel);
            if (state && state->closed) return false;
            if (!state || !state->opening_packet_id || !state->outgoing_open_acked) ready=false;
        }
        if (ready && entry.pending_mech) {
            work=MechWork{use.identity(),std::move(*entry.pending_mech),entry.setup_id};
            entry.pending_mech.reset();
        }
        return true;
    }
    void apply_start_mech(const session::SessionId& id,const MechWork& work) {
        // Shared Entry identity is not an in-flight Use. Provider/SQL/Auth may
        // synchronously retire it; never read its connection or erased ID here.
        if (stopping.load()) { registry->retire(work.target); return; }
        std::optional<wire::mech::SMechSetupStruct> value;
        try { value=mech_provider(id,work.request,work.previous_setup_id); } catch (...) {}
        if (!authorize(id)) return;
        const auto now=time();
        if (!now) { fail_closed(); return; }
        bool terminal=false;
        {
            auto use=pin(id,work.target);
            if (!use) return; // Callback result for a retired/replaced Entry is stale.
            auto& entry=use->entry();
            {
                std::lock_guard lock(registry->mutex);
                if (!running.load() || stopping.load() || !registry->accepting ||
                    registry->entry!=work.target || !entry.active) return;
            }
            try {
                terminal=!value || entry.connection->snapshot().closed || !entry.links_sent;
                const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
                    pawn_plan->channel.channel};
                for (const auto channel:channels) {
                    const auto state=entry.connection->channel_snapshot(channel);
                    if (!state || state->closed || !state->opening_packet_id || !state->outgoing_open_acked)
                        terminal=true;
                }
                if (!terminal) {
                    auto payload=wire::mech::encode_mech_setup_property(*value,
                        start_mech_plan->pri_mech_setup_field_id,start_mech_plan->pri_exclusive_bound,
                        skeleton.endian,static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                    wire::Bunch bunch;
                    bunch.reliable=true; bunch.channel_type=2; bunch.channel=pri_plan->channel.channel;
                    bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
                    const auto step=entry.connection->send_actor(bunch,*now);
                    if (step.datagrams.empty() || step.closed || stopping.load() ||
                        !send(entry,step) || stopping.load()) terminal=true;
                    else {
                        entry.setup_id=value->setup_id;
                        entry.accepted_mech=work.request;
                        ++entry.mech_revision;
                        entry.validation_responded=false;
                        entry.validation_succeeded=false;
                        entry.admission_request_revision.reset();
                        entry.admitted_revision.reset();
                        if (!entry.validation_waits_for_mech) entry.validation_pending=false;
                        entry.validation_waits_for_mech=false;
                    }
                }
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire(work.target); // Release Use before lease retirement.
    }
    bool validation_ready(const Entry& entry) const {
        if (!entry.links_sent || entry.connection->snapshot().closed) return false;
        const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
            pawn_plan->channel.channel};
        for (const auto channel:channels) {
            const auto state=entry.connection->channel_snapshot(channel);
            if (!state || state->closed || !state->opening_packet_id || !state->outgoing_open_acked)
                return false;
            // Delivery proof is independent of the subsequent SQL validation.
            if (channel==pri_plan->channel.channel && state->outgoing_bunches!=0) return false;
        }
        return true;
    }
    void apply_validation(const session::SessionId& id,const std::shared_ptr<Entry>& target) {
        if (!validation_plan || !target || stopping.load()) return;
        if (!authorize(id)) return;
        std::optional<wire::rpc::StartMechArguments> request;
        std::uint8_t setup_id=0;
        std::uint64_t revision=0;
        {
            auto use=pin(id,target);
            if (!use) return;
            const auto& entry=use->entry();
            if (!entry.validation_pending || entry.validation_responded || entry.pending_mech ||
                entry.validation_waits_for_mech || !validation_ready(entry)) return;
            request=entry.accepted_mech; setup_id=entry.setup_id; revision=entry.mech_revision;
        }
        // No Use/registry/socket lock survives into caller SQL or authorization.
        SpawnSelectionValidationResult result{{2,2,2,2,2},0};
        if (request) {
            try { result=spawn_validator(id,*request,setup_id); }
            catch (...) { registry->retire(target); return; }
        }
        if (!authorize(id)) return;
        const auto now=time();
        if (!now) { fail_closed(); return; }
        bool terminal=false;
        {
            auto use=pin(id,target);
            if (!use) return;
            auto& entry=use->entry();
            {
                std::lock_guard lock(registry->mutex);
                if (stopping.load() || !running.load() || !registry->accepting ||
                    registry->entry!=target || !entry.active) return;
            }
            if (entry.mech_revision!=revision || entry.setup_id!=setup_id || entry.pending_mech ||
                !entry.validation_pending || entry.validation_responded || !validation_ready(entry)) return;
            const auto status=game::aggregate_spawn_validation(result.checks);
            if (status==game::SpawnValidationStatus::in_process) return;
            try {
                const bool succeeded=status==game::SpawnValidationStatus::succeeded;
                if (!succeeded && result.failure_reason>8) terminal=true;
                else {
                    const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                    auto payload=succeeded ? wire::rpc::encode_spawn_validated(
                        validation_plan->validated_rpc_field_id,validation_plan->controller_exclusive_bound,cap)
                        : wire::rpc::encode_spawn_failed(result.failure_reason,
                            validation_plan->failed_rpc_field_id,validation_plan->controller_exclusive_bound,cap);
                    wire::Bunch bunch;
                    bunch.reliable=true; bunch.channel_type=2; bunch.channel=controller_plan->channel.channel;
                    bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
                    const auto step=entry.connection->send_actor(bunch,*now);
                    if (step.datagrams.empty() || step.closed || stopping.load() ||
                        !send(entry,step) || stopping.load()) terminal=true;
                    else {
                        entry.validation_pending=false; entry.validation_responded=true;
                        entry.validation_succeeded=succeeded;
                        if (succeeded) registry->spawn_validation_sent=true;
                        else registry->spawn_validation_failed=true;
                    }
                }
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire(target);
    }
    void apply_admission(const session::SessionId& id,const std::shared_ptr<Entry>& target) {
        if (!admission_plan || !target || stopping.load()) return;
        if (!authorize(id)) return;
        const auto now=time();
        if (!now) { fail_closed(); return; }
        bool terminal=false;
        {
            auto use=pin(id,target);
            if (!use) return;
            auto& entry=use->entry();
            {
                std::lock_guard lock(registry->mutex);
                if (stopping.load() || !running.load() || !registry->accepting ||
                    registry->entry!=target || !entry.active) return;
            }
            if (!entry.admission_request_revision || *entry.admission_request_revision!=entry.mech_revision ||
                entry.admitted_revision==entry.mech_revision || !entry.validation_succeeded ||
                !entry.validation_responded || !entry.accepted_mech || entry.pending_mech ||
                !validation_ready(entry)) return;
            try {
                auto payload=wire::rpc::encode_rpc({admission_plan->client_restart_rpc_field_id,
                    wire::object::ActorChannelReference{pawn_plan->channel.channel}},
                    admission_plan->controller_exclusive_bound,true,
                    static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                wire::Bunch bunch;
                bunch.reliable=true; bunch.channel_type=2; bunch.channel=controller_plan->channel.channel;
                bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
                const auto step=entry.connection->send_actor(bunch,*now);
                if (step.datagrams.empty() || step.closed || stopping.load() ||
                    !send(entry,step) || stopping.load()) terminal=true;
                else {
                    entry.admitted_revision=entry.mech_revision;
                    entry.admission_request_revision.reset();
                    registry->client_restart_sent=true;
                }
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire(target);
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
        std::optional<MechWork> mech;
        {
            auto use=pin(id);
            if (!use) { erase(id); return; }
            auto& entry=use->entry();
            target=use->identity();
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
                                    send(entry,actor) && !stopping.load() && !actor.closed) {
                                    registry->controller_sent=true;
                                    if (pri_plan) {
                                        const auto& pri=*pri_plan;
                                        const auto pri_reference=entry.connection->resolve_package_reference(
                                            pri.archetype_package_guid,pri.archetype_local_net_index);
                                        // The verified PRI branch shares Pawn's dynamic actor
                                        // prefix: supplied rotation, no Controller index byte.
                                        auto pri_payload=wire::actor::encode_initial_pawn(
                                            {pri_reference,pri.position,pri.rotation},
                                            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                                        entry.connection->register_actor_channel(pri.channel);
                                        wire::Bunch pri_bunch;
                                        pri_bunch.control=true; pri_bunch.open=true; pri_bunch.reliable=true;
                                        pri_bunch.channel=pri.channel.channel; pri_bunch.channel_type=2;
                                        pri_bunch.payload_bits=pri_payload.payload_bits;
                                        pri_bunch.payload=std::move(pri_payload.payload);
                                        const auto pri_step=entry.connection->send_actor(pri_bunch,now);
                                        if (!pri_step.datagrams.empty() && !stopping.load() &&
                                            send(entry,pri_step) && !stopping.load() && !pri_step.closed)
                                            registry->pri_sent=true;
                                        else terminal=true;
                                    }
                                    if (!terminal && pawn_plan) {
                                        const auto& pawn=*pawn_plan;
                                        const auto pawn_reference=entry.connection->resolve_package_reference(
                                            pawn.archetype_package_guid,pawn.archetype_local_net_index);
                                        auto pawn_payload=wire::actor::encode_initial_pawn(
                                            {pawn_reference,pawn.position,pawn.rotation},
                                            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                                        entry.connection->register_actor_channel(pawn.channel);
                                        // Explicit first-Pawn policy on its own actor channel.
                                        wire::Bunch pawn_bunch;
                                        pawn_bunch.control=true; pawn_bunch.open=true; pawn_bunch.reliable=true;
                                        pawn_bunch.channel=pawn.channel.channel; pawn_bunch.channel_type=2;
                                        pawn_bunch.payload_bits=pawn_payload.payload_bits;
                                        pawn_bunch.payload=std::move(pawn_payload.payload);
                                        const auto pawn_step=entry.connection->send_actor(pawn_bunch,now);
                                        if (!pawn_step.datagrams.empty() && !stopping.load() &&
                                            send(entry,pawn_step) && !stopping.load() && !pawn_step.closed)
                                            registry->pawn_sent=true;
                                        else terminal=true;
                                    }
                                }
                                else terminal=true;
                            }
                        } catch (...) { terminal=true; }
                    }
                }
                if (!terminal && !send_links(entry,now)) terminal=true;
                if (!terminal && !collect_start_mech(entry,step.actor_bunches)) terminal=true;
                if (!terminal && !take_start_mech(*use,mech)) terminal=true;
            } catch (const game::ConnectionError&) {
                terminal=entry.connection->snapshot().closed;
            } catch (...) { terminal=true; }
        } // Release in-flight ownership BEFORE retiring or any callback.
        if (terminal) registry->retire_id(id);
        else if (candidate) validate_response(id,target,*candidate);
        else {
            if (mech) apply_start_mech(id,*mech);
            apply_validation(id,target);
            apply_admission(id,target);
        }
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
        std::optional<MechWork> mech;
        std::shared_ptr<Entry> target;
        {
            auto use=pin(id);
            if (!use) { erase(id); return; }
            auto& entry=use->entry();
            target=use->identity();
            try {
                const auto step=entry.connection->tick(now);
                const bool sent=send(entry,step);
                terminal=step.closed || !sent;
                if (!terminal && !send_links(entry,now)) terminal=true;
                if (!terminal && !take_start_mech(*use,mech)) terminal=true;
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire_id(id);
        else {
            if (mech) apply_start_mech(id,*mech);
            apply_validation(id,target);
            apply_admission(id,target);
        }
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
               std::optional<InitialControllerPlan> controller,
               std::optional<InitialPawnPlan> pawn,std::optional<InitialPriPlan> pri,
               std::optional<InitialActorLinksPlan> links,
               std::optional<StartMechDispatchPlan> start_mech,StartMechProvider provider,
               std::optional<SpawnValidationDispatchPlan> spawn_validation,
               SpawnSelectionValidator selection_validator,
               std::optional<SpawnAdmissionDispatchPlan> spawn_admission,
               std::optional<PriIdentityDispatchPlan> pri_identity) {
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
        if (pawn) {
            const auto& plan=*pawn;
            if (!validator || !publication || !controller || plan.channel.channel==0 ||
                plan.channel.channel>2047 || plan.channel.channel==controller->channel.channel ||
                plan.archetype_local_net_index<0)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                wire::BitWriter writer;
                const auto cap=std::numeric_limits<std::size_t>::max();
                wire::actor::write_position(writer,plan.position,cap);
                if (plan.rotation) wire::actor::write_rotation(writer,*plan.rotation,cap);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (pri) {
            const auto& plan=*pri;
            if (!validator || !publication || !controller || plan.channel.channel==0 ||
                plan.channel.channel>2047 || plan.channel.channel==controller->channel.channel ||
                (pawn && plan.channel.channel==pawn->channel.channel) || plan.archetype_local_net_index<0)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                wire::BitWriter writer;
                const auto cap=std::numeric_limits<std::size_t>::max();
                wire::actor::write_position(writer,plan.position,cap);
                if (plan.rotation) wire::actor::write_rotation(writer,*plan.rotation,cap);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (links) {
            const auto& plan=*links;
            if (!validator || !publication || !controller || !pri || !pawn ||
                plan.controller_exclusive_bound==0 || plan.pawn_exclusive_bound==0 ||
                plan.controller_pri_field_id>=plan.controller_exclusive_bound ||
                plan.controller_pawn_field_id>=plan.controller_exclusive_bound ||
                plan.pawn_controller_field_id>=plan.pawn_exclusive_bound ||
                plan.pawn_pri_field_id>=plan.pawn_exclusive_bound ||
                plan.controller_pri_field_id==plan.controller_pawn_field_id ||
                plan.pawn_controller_field_id==plan.pawn_pri_field_id)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                (void)link_payloads(plan,controller->channel.channel,pri->channel.channel,
                    pawn->channel.channel,static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (pri_identity) {
            const auto& plan=*pri_identity;
            const auto& evidence=plan.eligibility;
            if (!validator || !publication || !pri || !links ||
                !evidence.primary_package_map || !evidence.pri_cache_correct ||
                !evidence.unique_id_property_eligible || !evidence.unique_net_id_serializer_correct ||
                !evidence.integer_archive_order_correct)
                fail(LocalUdpErrorCode::invalid_configuration);
            if (start_mech && (plan.exclusive_bound!=start_mech->pri_exclusive_bound ||
                plan.field_id==start_mech->pri_mech_setup_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                static_cast<void>(wire::identity::encode_unique_id_property({},plan.field_id,plan.exclusive_bound,
                    plan.integer_order,static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8));
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (static_cast<bool>(start_mech)!=static_cast<bool>(provider))
            fail(LocalUdpErrorCode::invalid_configuration);
        if (start_mech) {
            const auto& plan=*start_mech;
            const auto& evidence=plan.eligibility;
            if (!validator || !publication || !controller || !pri || !pawn || !links ||
                !evidence.primary_package_map || !evidence.controller_cache_correct ||
                !evidence.rpc_eligible || !evidence.pri_cache_correct ||
                !evidence.pri_property_eligible || !evidence.all_members_eligible ||
                !std::all_of(evidence.parameters_eligible.begin(),evidence.parameters_eligible.end(),
                    [](bool eligible){return eligible;}) ||
                plan.controller_exclusive_bound==0 || plan.pri_exclusive_bound==0 ||
                plan.controller_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.pri_mech_setup_field_id>=plan.pri_exclusive_bound ||
                plan.controller_exclusive_bound!=links->controller_exclusive_bound)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                wire::BitWriter tag;
                tag.write_bounded(plan.pri_mech_setup_field_id,plan.pri_exclusive_bound);
                // Fixed all-members human/null schema length; only its tag is
                // composed here. No default completed preset or Context is made.
                const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                if (cap<544 || tag.bit_count()>cap-544)
                    fail(LocalUdpErrorCode::invalid_configuration);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (static_cast<bool>(spawn_validation)!=static_cast<bool>(selection_validator))
            fail(LocalUdpErrorCode::invalid_configuration);
        if (spawn_validation) {
            const auto& plan=*spawn_validation;
            const auto& evidence=plan.eligibility;
            if (!start_mech || !provider || !evidence.primary_package_map ||
                !evidence.controller_cache_correct || !evidence.validate_rpc_eligible ||
                !evidence.validated_rpc_eligible || !evidence.failed_rpc_eligible ||
                !evidence.failure_reason_eligible || plan.controller_exclusive_bound==0 ||
                plan.controller_exclusive_bound!=start_mech->controller_exclusive_bound ||
                plan.validate_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.validated_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.failed_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.validate_rpc_field_id==plan.validated_rpc_field_id ||
                plan.validate_rpc_field_id==plan.failed_rpc_field_id ||
                plan.validated_rpc_field_id==plan.failed_rpc_field_id ||
                plan.validate_rpc_field_id==start_mech->controller_rpc_field_id ||
                plan.validated_rpc_field_id==start_mech->controller_rpc_field_id ||
                plan.failed_rpc_field_id==start_mech->controller_rpc_field_id)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                (void)wire::rpc::encode_spawn_validated(plan.validated_rpc_field_id,plan.controller_exclusive_bound,cap);
                (void)wire::rpc::encode_spawn_failed(8,plan.failed_rpc_field_id,plan.controller_exclusive_bound,cap);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (spawn_admission) {
            const auto& plan=*spawn_admission;
            const auto& evidence=plan.eligibility;
            if (!spawn_validation || !selection_validator || !start_mech || !provider ||
                !evidence.primary_package_map || !evidence.controller_cache_correct ||
                !evidence.stage_rpc_eligible || !evidence.stage_argument_eligible ||
                !evidence.client_restart_rpc_eligible || !evidence.pawn_reference_eligible ||
                plan.controller_exclusive_bound==0 ||
                plan.controller_exclusive_bound!=spawn_validation->controller_exclusive_bound ||
                plan.stage_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.client_restart_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.stage_rpc_field_id==plan.client_restart_rpc_field_id ||
                plan.stage_rpc_field_id==start_mech->controller_rpc_field_id ||
                plan.stage_rpc_field_id==spawn_validation->validate_rpc_field_id ||
                plan.stage_rpc_field_id==spawn_validation->validated_rpc_field_id ||
                plan.stage_rpc_field_id==spawn_validation->failed_rpc_field_id ||
                plan.client_restart_rpc_field_id==start_mech->controller_rpc_field_id ||
                plan.client_restart_rpc_field_id==spawn_validation->validate_rpc_field_id ||
                plan.client_restart_rpc_field_id==spawn_validation->validated_rpc_field_id ||
                plan.client_restart_rpc_field_id==spawn_validation->failed_rpc_field_id)
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                (void)wire::rpc::encode_rpc({plan.client_restart_rpc_field_id,
                    wire::object::ActorChannelReference{pawn->channel.channel}},plan.controller_exclusive_bound,true,
                    static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        authorizer=std::move(callback); login_validator=std::move(validator);
        publication_provider=std::move(publication); controller_plan=std::move(controller);
        pawn_plan=std::move(pawn); pri_plan=std::move(pri); links_plan=std::move(links);
        if (start_mech) initial_setup_id=start_mech->initial_setup_id;
        start_mech_plan=std::move(start_mech); mech_provider=std::move(provider);
        validation_plan=std::move(spawn_validation); spawn_validator=std::move(selection_validator);
        admission_plan=std::move(spawn_admission);
        provision_identity_enabled=pri_identity.has_value();
        pri_identity_plan=std::move(pri_identity);
        started=true;
        {
            std::lock_guard lock(registry->mutex); registry->accepting=true;
        }
        running=true;
        try { worker=std::thread([this]{loop();}); }
        catch (...) {
            fail_closed(); authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset(); pawn_plan.reset(); pri_plan.reset(); links_plan.reset(); start_mech_plan.reset(); mech_provider={}; validation_plan.reset(); spawn_validator={}; admission_plan.reset(); pri_identity_plan.reset();
            fail(LocalUdpErrorCode::invalid_lifecycle);
        }
    }
    void stop() noexcept {
        running=false; stopping=true; registry->close();
        if (worker.joinable()) worker.join();
        authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset(); pawn_plan.reset(); pri_plan.reset(); links_plan.reset(); start_mech_plan.reset(); mech_provider={}; validation_plan.reset(); spawn_validator={}; admission_plan.reset(); pri_identity_plan.reset();
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
            std::optional<wire::identity::UniqueId> identity_value;
            if (provision_identity_enabled) {
                // Private, configured provision only: four successive native
                // eight-hex words. No client Login identity or generic UUID parser.
                const auto& guid=provision.user_guid;
                if (guid.size()!=36) return nullptr;
                wire::identity::UniqueId value;
                std::size_t digit=0;
                bool nonzero=false;
                for (std::size_t i=0;i<guid.size();++i) {
                    const auto c=guid[i];
                    if (i==8 || i==13 || i==18 || i==23) {
                        if (c!='-') return nullptr;
                        continue;
                    }
                    std::uint32_t nibble;
                    if (c>='0' && c<='9') nibble=static_cast<std::uint32_t>(c-'0');
                    else if (c>='a' && c<='f') nibble=static_cast<std::uint32_t>(c-'a'+10);
                    else return nullptr;
                    value.words[digit/8]=(value.words[digit/8]<<4)|nibble;
                    nonzero=nonzero || nibble!=0; ++digit;
                }
                if (!nonzero) return nullptr;
                identity_value=value;
            }
            auto config=skeleton;
            config.session_id=provision.id; config.session_key=provision.key;
            const KeyErase clear{config};
            auto entry=std::make_shared<Entry>(provision.id,std::make_unique<game::GameConnection>(config),
                initial_setup_id,std::move(identity_value));
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
        impl_->registry->controller_sent.load(),impl_->registry->pawn_sent.load(),impl_->registry->pri_sent.load(),
        impl_->registry->spawn_validation_requested.load(),impl_->registry->spawn_validation_sent.load(),
        impl_->registry->spawn_validation_failed.load(),impl_->registry->stage3_requested.load(),
        impl_->registry->client_restart_sent.load(),impl_->registry->controller_open_acked.load(),
        impl_->registry->pri_open_acked.load(),impl_->registry->pawn_open_acked.load(),
        impl_->registry->actor_links_sent.load(),impl_->registry->pri_identity_sent.load(),
        impl_->registry->pri_identity_acked.load()};
}
bool LocalUdpOwner::destination_bound(const assignment::Destination& destination) const noexcept {
    return impl_->bound(destination);
}
void LocalUdpOwner::start(Authorizer authorize, LoginResponseValidator validate_response,
                          PublicationProvider publication,
                          std::optional<InitialControllerPlan> controller,
                          std::optional<InitialPawnPlan> pawn,std::optional<InitialPriPlan> pri,
                          std::optional<InitialActorLinksPlan> links,
                          std::optional<StartMechDispatchPlan> start_mech,StartMechProvider mech_provider,
                          std::optional<SpawnValidationDispatchPlan> spawn_validation,
                          SpawnSelectionValidator selection_validator,
                          std::optional<SpawnAdmissionDispatchPlan> spawn_admission,
                          std::optional<PriIdentityDispatchPlan> pri_identity) {
    impl_->start(std::move(authorize),std::move(validate_response),std::move(publication),
        std::move(controller),std::move(pawn),std::move(pri),std::move(links),
        std::move(start_mech),std::move(mech_provider),
        std::move(spawn_validation),std::move(selection_validator),std::move(spawn_admission),
        std::move(pri_identity));
}
void LocalUdpOwner::stop() noexcept { impl_->stop(); }
std::unique_ptr<assignment::UdpSessionLease> LocalUdpOwner::handoff(
    const assignment::SessionProvision& provision) { return impl_->handoff(provision); }
}
