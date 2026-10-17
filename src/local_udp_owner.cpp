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
#include "hawken/inventory_rpc_wire.hpp"
#include "hawken/spawn_validation_rpc_wire.hpp"
#include "hawken/controller_admission_rpc_wire.hpp"
#include "hawken/pawn_rpc_wire.hpp"
#include "hawken/initial_pawn.hpp"
#include "hawken/object_property_wire.hpp"
#include "hawken/unique_id_wire.hpp"
#include "hawken/bool_property_wire.hpp"
#include "hawken/int_property_wire.hpp"
#include "hawken/movement_rpc.hpp"
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
std::array<wire::property::PropertyPayload,6> inventory_payloads(
    const InventoryBootstrapPlan& plan,std::uint16_t pawn,std::size_t max_bits) {
    using Property=wire::property::ObjectProperty;
    using Reference=wire::object::ActorChannelReference;
    const auto manager=plan.manager.channel.channel,dual=plan.dual_weapon.channel.channel;
    const std::array manager_fields{Property{plan.owner_field_id,Reference{pawn}},
        Property{plan.instigator_field_id,Reference{pawn}},Property{plan.manager_chain_field_id,Reference{dual}}};
    const std::array dual_fields{Property{plan.owner_field_id,Reference{pawn}},
        Property{plan.instigator_field_id,Reference{pawn}},Property{plan.inventory_manager_field_id,Reference{manager}}};
    const std::array hand_fields{Property{plan.owner_field_id,Reference{pawn}},
        Property{plan.instigator_field_id,Reference{pawn}},Property{plan.inventory_manager_field_id,Reference{manager}},
        Property{plan.hand_dual_weapon_field_id,Reference{dual}}};
    const std::array pawn_fields{Property{plan.pawn_inv_manager_field_id,Reference{manager}},
        Property{plan.pawn_dual_weapon_field_id,Reference{dual}}};
    const std::array hand_array{
        wire::property::ArrayObjectProperty{plan.dual_hand_weap_field_id,4,0,Reference{plan.primary_weapon.channel.channel}},
        wire::property::ArrayObjectProperty{plan.dual_hand_weap_field_id,4,1,Reference{plan.secondary_weapon.channel.channel}}};
    return {wire::property::encode_object_properties(manager_fields,plan.manager_exclusive_bound,max_bits),
        wire::property::encode_object_properties(dual_fields,plan.dual_exclusive_bound,max_bits),
        wire::property::encode_object_properties(hand_fields,plan.primary_exclusive_bound,max_bits),
        wire::property::encode_object_properties(hand_fields,plan.secondary_exclusive_bound,max_bits),
        wire::property::encode_object_properties(pawn_fields,plan.pawn_exclusive_bound,max_bits),
        wire::property::encode_object_array_properties(hand_array,plan.dual_exclusive_bound,max_bits)};
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
    bool inventory_profile_current=false,inventory_opened=false,inventory_links_sent=false,inventory_given=false;
    std::optional<std::uint16_t> current_inventory_weapon; // Own current Dual, not a replicated Pawn.Weapon tag.
    std::optional<game::PawnCombatState> combat_state;
    std::optional<bool> combat_permission_sent;
    std::optional<wire::identity::UniqueId> pri_identity;
    bool pri_identity_sent=false;
    bool gri_sent=false,game_class_sent=false,hud_sent=false;
    std::optional<wire::rpc::StartMechArguments> pending_mech;
    std::uint8_t setup_id;
    std::optional<wire::rpc::StartMechArguments> accepted_mech;
    std::uint64_t mech_revision=0;
    bool validation_pending=false,validation_waits_for_mech=false,validation_responded=false;
    bool validation_succeeded=false;
    bool ready_observed=false,ready_value=false;
    std::uint64_t ready_revision=0;
    std::optional<std::uint64_t> ready_sent_revision;
    std::optional<std::uint64_t> ready_acked_revision;
    std::optional<bool> ready_sent_value;
    std::optional<std::array<std::int32_t,3>> ready_counters_sent;
    bool ready_begun_sent=false;
    std::optional<std::uint64_t> admission_request_revision,admitted_revision;

    Entry(const session::SessionId& value,std::unique_ptr<game::GameConnection> conn,
          std::uint8_t initial_setup_id,std::optional<wire::identity::UniqueId> identity_value)
        : id(value),connection(std::move(conn)),pri_identity(std::move(identity_value)),setup_id(initial_setup_id) {}
    ~Entry() {
        if (pri_identity) pri_identity->words.fill(0);
        erase(id);
    }
};
struct AtomicActorBunchDiagnostics {
    std::atomic<std::uint64_t> delivered{0},zero_payload{0},header_truncated{0},field_uninterpreted{0};
    std::atomic<std::size_t> payload_bits{0};
    std::atomic<bool> reliable{false},open{false},close{false};
    static constexpr auto no_field=std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> first_field{no_field};
    std::array<std::atomic<std::uint64_t>,actor_field_histogram_capacity> first_fields{};
    std::atomic<std::uint64_t> field_out_of_range{0};

    static void increment(std::atomic<std::uint64_t>& counter) noexcept {
        // The single UDP worker writes; diagnostics readers only load.
        const auto value=counter.load(std::memory_order_relaxed);
        if (value!=std::numeric_limits<std::uint64_t>::max())
            counter.store(value+1,std::memory_order_relaxed);
    }
    void observe(const wire::Bunch& bunch,std::optional<std::uint32_t> bound) noexcept {
        payload_bits.store(bunch.payload_bits,std::memory_order_relaxed);
        reliable.store(bunch.reliable,std::memory_order_relaxed);
        open.store(bunch.open,std::memory_order_relaxed);
        close.store(bunch.close,std::memory_order_relaxed);
        first_field.store(no_field,std::memory_order_relaxed);
        if (bunch.payload_bits==0) increment(zero_payload);
        else if (!bound || bunch.open) increment(field_uninterpreted); // Opening has an actor prefix, not a field tag.
        else {
            try {
                wire::BitReader reader(bunch.payload,bunch.payload_bits);
                const auto field=reader.read_bounded(*bound);
                first_field.store(field,std::memory_order_relaxed);
                if (field<first_fields.size()) increment(first_fields[field]);
                else increment(field_out_of_range);
            } catch (const wire::WireError&) { increment(header_truncated); }
            catch (...) { increment(field_uninterpreted); } // Diagnostics must never change dispatch on failure.
        }
        increment(delivered);
    }
    ActorBunchDiagnostics snapshot() const noexcept {
        ActorBunchDiagnostics result;
        result.delivered_bunches=delivered.load(std::memory_order_relaxed);
        result.zero_payload_bunches=zero_payload.load(std::memory_order_relaxed);
        result.header_truncated_bunches=header_truncated.load(std::memory_order_relaxed);
        result.field_uninterpreted_bunches=field_uninterpreted.load(std::memory_order_relaxed);
        result.last_payload_bits=payload_bits.load(std::memory_order_relaxed);
        result.last_reliable=reliable.load(std::memory_order_relaxed);
        result.last_open=open.load(std::memory_order_relaxed);
        result.last_close=close.load(std::memory_order_relaxed);
        const auto field=first_field.load(std::memory_order_relaxed);
        if (field!=no_field) result.first_field_id=static_cast<std::uint32_t>(field);
        for (std::size_t i=0;i<result.first_field_counts.size();++i)
            result.first_field_counts[i]=first_fields[i].load(std::memory_order_relaxed);
        result.field_out_of_range_bunches=field_out_of_range.load(std::memory_order_relaxed);
        return result;
    }
};
struct AtomicMovementDiagnostics {
    std::atomic<std::uint64_t> decoded{0},malformed{0},jump{0};
    std::array<std::atomic<std::uint64_t>,3> kinds{};
    std::atomic<std::uint16_t> flags{256};
    void observe(const wire::Bunch& bunch,control::Endian endian) noexcept {
        if (bunch.open || !bunch.payload_bits) return;
        try {
            wire::BitReader reader(bunch.payload,bunch.payload_bits);
            auto tag_reader=reader;
            const auto tag=tag_reader.read_bounded(294);
            if (tag!=59 && tag!=60 && tag!=61) return;
            try {
                // Static original-schema interpretation only. This diagnostic
                // does not attest the live ClassNetCache/parameter map and
                // never changes dispatch, admission, state or a reply.
                constexpr wire::movement::Eligibility conditional{true,true,true,true};
                const auto value=wire::movement::read_rpc(reader,endian,conditional,bunch.payload_bits);
                AtomicActorBunchDiagnostics::increment(kinds[static_cast<std::size_t>(value.kind)]);
                if (value.current.jump) AtomicActorBunchDiagnostics::increment(jump);
                if (value.pending && value.pending->jump) AtomicActorBunchDiagnostics::increment(jump);
                flags.store(value.current.move_flags,std::memory_order_relaxed);
                AtomicActorBunchDiagnostics::increment(decoded);
            } catch (...) { AtomicActorBunchDiagnostics::increment(malformed); }
        } catch (...) { /* First-tag truncation is counted by actor metadata. */ }
    }
    MovementBunchDiagnostics snapshot() const noexcept {
        MovementBunchDiagnostics result;
        result.decoded_requests=decoded.load(std::memory_order_relaxed);
        result.malformed_requests=malformed.load(std::memory_order_relaxed);
        result.jump_requests=jump.load(std::memory_order_relaxed);
        for (std::size_t i=0;i<kinds.size();++i) result.kinds[i]=kinds[i].load(std::memory_order_relaxed);
        const auto last=flags.load(std::memory_order_relaxed);
        if (last<256) result.last_move_flags=static_cast<std::uint8_t>(last);
        return result;
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
    std::atomic<bool> gri_sent{false},gri_open_acked{false},game_class_sent{false},game_class_acked{false};
    std::atomic<bool> hud_sent{false},hud_acked{false};
    std::atomic<bool> ready_rpc_received{false},pri_ready_sent{false},pri_ready_acked{false};
    std::atomic<bool> match_start_requested{false},gri_match_begun_sent{false},gri_match_begun_acked{false};
    std::atomic<bool> inventory_profile_accepted{false},inventory_openings_sent{false},inventory_openings_acked{false};
    std::atomic<bool> inventory_links_sent{false},inventory_links_acked{false},inventory_given_to_sent{false};
    std::atomic<bool> inventory_current_weapon_received{false};
    std::array<AtomicActorBunchDiagnostics,static_cast<std::size_t>(ActorDiagnosticSlot::count)> actor_bunches{};
    AtomicMovementDiagnostics movement;

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
            target->inventory_profile_current=false;target->current_inventory_weapon.reset();
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
    std::optional<SelectionBootstrapPlan> bootstrap_plan;
    std::optional<ReadyMatchPlan> ready_plan;
    ReadyFactsProvider ready_facts_provider;
    std::optional<InventoryBootstrapPlan> inventory_plan;
    std::optional<game::PendingMatch> pending_match;
    double world_clock_base=0;
    bool world_start_committed=false; // Worker-only own action, not a native match flag.
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
    bool send_bootstrap(Entry& entry,double now) noexcept {
        if (!bootstrap_plan) return true;
        if (stopping.load() || entry.connection->snapshot().closed) return false;
        if (!entry.gri_sent) return true;
        try {
            const auto& plan=*bootstrap_plan;
            const auto gri=entry.connection->channel_snapshot(plan.gri.channel.channel);
            if (!gri || !gri->opening_packet_id || !gri->outgoing_open_acked || gri->closed)
                return true;
            registry->gri_open_acked=true;
            const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
            if (!entry.game_class_sent) {
                // ClassProperty uses the verified ObjectProperty PRIMARY reference
                // serializer. Resolve the UClass, not its CDO, after publication.
                const auto reference=entry.connection->resolve_package_reference(
                    plan.game_class_package_guid,plan.game_class_local_net_index);
                const std::array fields{wire::property::ObjectProperty{plan.game_class_field_id,reference}};
                auto payload=wire::property::encode_object_properties(fields,plan.gri_exclusive_bound,cap);
                wire::Bunch bunch;
                bunch.reliable=true; bunch.channel_type=2; bunch.channel=plan.gri.channel.channel;
                bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
                const auto step=entry.connection->send_actor(bunch,now);
                if (step.datagrams.empty() || step.closed || stopping.load() ||
                    !send(entry,step) || stopping.load()) return false;
                entry.game_class_sent=true; registry->game_class_sent=true;
                return true; // The new reliable property has not been ACKed yet.
            }
            if (gri->outgoing_bunches!=0) return true;
            registry->game_class_acked=true;
            if (!entry.links_sent) return true;
            const auto controller=entry.connection->channel_snapshot(controller_plan->channel.channel);
            const auto pawn=entry.connection->channel_snapshot(pawn_plan->channel.channel);
            if (!controller || !pawn || controller->closed || pawn->closed ||
                !controller->opening_packet_id || !pawn->opening_packet_id ||
                !controller->outgoing_open_acked || !pawn->outgoing_open_acked)
                return true;
            if (entry.hud_sent) {
                if (controller->outgoing_bunches==0) registry->hud_acked=true;
                return true;
            }
            // links_sent records sends only. Both reliable queues must actually
            // drain before this RPC; GameClass is independently gated from UID.
            if (controller->outgoing_bunches!=0 || pawn->outgoing_bunches!=0) return true;
            const auto reference=entry.connection->resolve_package_reference(
                plan.hud_package_guid,plan.hud_local_net_index);
            auto payload=wire::rpc::encode_rpc({plan.client_set_hud_field_id,reference},
                plan.controller_exclusive_bound,true,cap);
            wire::Bunch bunch;
            bunch.reliable=true; bunch.channel_type=2; bunch.channel=controller_plan->channel.channel;
            bunch.payload_bits=payload.logical_bits; bunch.payload=std::move(payload.payload);
            const auto step=entry.connection->send_actor(bunch,now);
            if (step.datagrams.empty() || step.closed || stopping.load() ||
                !send(entry,step) || stopping.load()) return false;
            entry.hud_sent=true; registry->hud_sent=true;
            return true;
        } catch (...) { return false; } // The same pinned caller retires after Use.
    }
    bool send_combat_permission(Entry& entry,double now) {
        if (!inventory_plan->combat || !entry.inventory_given) return true;
        const auto& plan=*inventory_plan;
        const bool admitted=entry.inventory_profile_current && entry.accepted_mech && !entry.pending_mech &&
            entry.validation_succeeded && entry.validation_responded &&
            entry.admitted_revision==entry.mech_revision && (!ready_plan || world_start_committed);
        const bool permitted=admitted && entry.combat_state &&
            game::evaluate_pawn_combat(*entry.combat_state,entry.mech_revision,false).server_can_fire;
        // No permission claim before admission. A previously granted permission
        // is revoked even while a new profile/selection is being materialized.
        if ((!entry.combat_permission_sent && !permitted) || entry.combat_permission_sent==permitted) return true;
        const std::array fields{wire::property::BoolProperty{plan.combat->server_can_fire_field_id,permitted}};
        auto payload=wire::property::encode_bool_properties(fields,plan.dual_exclusive_bound,
            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
        wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=plan.dual_weapon.channel.channel;
        bunch.payload_bits=payload.logical_bits;bunch.payload=std::move(payload.payload);
        const auto step=entry.connection->send_actor(bunch,now);
        if (step.datagrams.empty() || step.closed || stopping.load() || !send(entry,step) || stopping.load()) return false;
        entry.combat_permission_sent=permitted;
        return true;
    }
    bool send_inventory(Entry& entry,double now) {
        if (!inventory_plan) return true;
        if (stopping.load() || entry.connection->snapshot().closed) return false;
        const auto& plan=*inventory_plan;
        const std::array actors{&plan.manager,&plan.dual_weapon,&plan.primary_weapon,&plan.secondary_weapon};
        if (entry.inventory_opened) for (const auto* actor:actors) {
            const auto state=entry.connection->channel_snapshot(actor->channel.channel);
            if (!state || state->closed) return false;
        }
        if (!send_combat_permission(entry,now)) return false;
        if (!entry.inventory_profile_current || !entry.accepted_mech || entry.pending_mech || !entry.links_sent)
            return true;
        const std::array base_channels{controller_plan->channel.channel,pri_plan->channel.channel,pawn_plan->channel.channel};
        for (const auto channel:base_channels) {
            const auto state=entry.connection->channel_snapshot(channel);
            if (state && state->closed) return false;
            if (!state || !state->opening_packet_id || !state->outgoing_open_acked) return true;
        }
        const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
        if (!entry.inventory_opened) {
            // Resolve and encode ALL four prefixes before any registration/send.
            std::array<wire::Bunch,4> openings;
            for (std::size_t i=0;i<actors.size();++i) {
                const auto& actor=*actors[i];
                const auto reference=entry.connection->resolve_package_reference(actor.archetype_package_guid,actor.archetype_local_net_index);
                auto payload=wire::actor::encode_initial_pawn({reference,actor.position,actor.rotation},cap);
                auto& bunch=openings[i];bunch.control=true;bunch.open=true;bunch.reliable=true;
                bunch.channel_type=2;bunch.channel=actor.channel.channel;
                bunch.payload_bits=payload.payload_bits;bunch.payload=std::move(payload.payload);
            }
            for (const auto* actor:actors) entry.connection->register_actor_channel(actor->channel);
            for (const auto& bunch:openings) {
                if (stopping.load()) return false;
                const auto step=entry.connection->send_actor(bunch,now);
                if (step.datagrams.empty() || step.closed || !send(entry,step) || stopping.load()) return false;
            }
            entry.inventory_opened=true;
            registry->inventory_openings_sent=true;
            return true;
        }
        for (const auto* actor:actors) {
            const auto state=entry.connection->channel_snapshot(actor->channel.channel);
            if (state && state->closed) return false;
            if (!state || !state->opening_packet_id || !state->outgoing_open_acked) return true;
        }
        registry->inventory_openings_acked=true;
        if (!entry.inventory_links_sent) {
            // Five destination channels, six batches; Dual array is separate.
            auto payloads=inventory_payloads(plan,pawn_plan->channel.channel,cap);
            const std::array destinations{plan.manager.channel.channel,plan.dual_weapon.channel.channel,
                plan.primary_weapon.channel.channel,plan.secondary_weapon.channel.channel,
                pawn_plan->channel.channel,plan.dual_weapon.channel.channel};
            for (std::size_t i=0;i<payloads.size();++i) {
                if (stopping.load()) return false;
                wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=destinations[i];
                bunch.payload_bits=payloads[i].logical_bits;bunch.payload=std::move(payloads[i].payload);
                const auto step=entry.connection->send_actor(bunch,now);
                if (step.datagrams.empty() || step.closed || !send(entry,step) || stopping.load()) return false;
            }
            entry.inventory_links_sent=true;
            registry->inventory_links_sent=true;
            return true;
        }
        const std::array queues{plan.manager.channel.channel,plan.dual_weapon.channel.channel,
            plan.primary_weapon.channel.channel,plan.secondary_weapon.channel.channel,pawn_plan->channel.channel};
        for (const auto channel:queues) {
            const auto state=entry.connection->channel_snapshot(channel);
            if (!state || state->closed) return false;
            if (!state->opening_packet_id || !state->outgoing_open_acked || state->outgoing_bunches!=0) return true;
        }
        registry->inventory_links_acked=true;
        if (!entry.inventory_given) {
            auto payload=wire::rpc::encode_client_given_to(plan.dual_client_given_to_field_id,
                plan.dual_exclusive_bound,pawn_plan->channel.channel,false,cap);
            wire::Bunch bunch;bunch.reliable=true;bunch.channel_type=2;bunch.channel=plan.dual_weapon.channel.channel;
            bunch.payload_bits=payload.logical_bits;bunch.payload=std::move(payload.payload);
            const auto step=entry.connection->send_actor(bunch,now);
            if (step.datagrams.empty() || step.closed || !send(entry,step) || stopping.load()) return false;
            entry.inventory_given=true;
            registry->inventory_given_to_sent=true;
        }
        return send_combat_permission(entry,now);
    }
    void apply_inventory(const session::SessionId& id,const std::shared_ptr<Entry>& target) {
        if (!inventory_plan || !target || stopping.load() || !authorize(id)) return;
        const auto now=time();if (!now) {fail_closed();return;}
        bool terminal=false;
        {
            auto use=pin(id,target);if (!use) return;
            {
                std::lock_guard lock(registry->mutex);
                if (stopping.load() || !running.load() || !registry->accepting || registry->entry!=target || !use->entry().active)
                    return;
            }
            try {terminal=!send_inventory(use->entry(),*now);} catch (...) {terminal=true;}
        }
        if (terminal) registry->retire(target); // Never retire while holding Use.
    }
    void observe_actor_bunches(const std::vector<wire::Bunch>& bunches) noexcept {
        std::optional<std::uint32_t> controller_bound,pri_bound,pawn_bound;
        if (links_plan) {
            controller_bound=links_plan->controller_exclusive_bound;
            pawn_bound=links_plan->pawn_exclusive_bound;
        } else if (inventory_plan) pawn_bound=inventory_plan->pawn_exclusive_bound;
        if (!controller_bound && start_mech_plan) controller_bound=start_mech_plan->controller_exclusive_bound;
        if (!controller_bound && validation_plan) controller_bound=validation_plan->controller_exclusive_bound;
        if (!controller_bound && admission_plan) controller_bound=admission_plan->controller_exclusive_bound;
        if (!controller_bound && bootstrap_plan) controller_bound=bootstrap_plan->controller_exclusive_bound;
        if (!controller_bound && ready_plan) controller_bound=ready_plan->controller_exclusive_bound;
        if (pri_identity_plan) pri_bound=pri_identity_plan->exclusive_bound;
        else if (start_mech_plan) pri_bound=start_mech_plan->pri_exclusive_bound;
        else if (ready_plan) pri_bound=ready_plan->pri_exclusive_bound;
        const auto observe=[&](ActorDiagnosticSlot role,std::uint16_t channel,std::optional<std::uint32_t> bound) {
            auto& metadata=registry->actor_bunches[static_cast<std::size_t>(role)];
            for (const auto& bunch:bunches)
                if (bunch.channel==channel) {
                    metadata.observe(bunch,bound);
                    if (role==ActorDiagnosticSlot::controller && bound && *bound==294)
                        registry->movement.observe(bunch,skeleton.endian);
                }
        };
        if (controller_plan) observe(ActorDiagnosticSlot::controller,controller_plan->channel.channel,controller_bound);
        if (pawn_plan) observe(ActorDiagnosticSlot::pawn,pawn_plan->channel.channel,pawn_bound);
        if (pri_plan) observe(ActorDiagnosticSlot::pri,pri_plan->channel.channel,pri_bound);
        if (bootstrap_plan) observe(ActorDiagnosticSlot::gri,bootstrap_plan->gri.channel.channel,bootstrap_plan->gri_exclusive_bound);
        if (inventory_plan) {
            const auto& plan=*inventory_plan;
            observe(ActorDiagnosticSlot::manager,plan.manager.channel.channel,plan.manager_exclusive_bound);
            observe(ActorDiagnosticSlot::dual_weapon,plan.dual_weapon.channel.channel,plan.dual_exclusive_bound);
            observe(ActorDiagnosticSlot::primary_weapon,plan.primary_weapon.channel.channel,plan.primary_exclusive_bound);
            observe(ActorDiagnosticSlot::secondary_weapon,plan.secondary_weapon.channel.channel,plan.secondary_exclusive_bound);
        }
    }
    bool collect_inventory_manager(Entry& entry,const std::vector<wire::Bunch>& bunches) {
        if (!inventory_plan) return true;
        const auto& plan=*inventory_plan;
        for (const auto& bunch:bunches) {
            if (bunch.channel!=plan.manager.channel.channel || bunch.payload_bits==0) continue;
            const auto state=entry.connection->channel_snapshot(bunch.channel);
            if (!state || state->closed) return false;
            wire::BitReader reader(bunch.payload,bunch.payload_bits);
            while (reader.position()<bunch.payload_bits) {
                std::uint32_t field;
                try {field=reader.read_bounded(plan.manager_exclusive_bound);} catch (const wire::WireError&) {break;}
                if (field!=plan.manager_server_set_current_weapon_field_id) break; // Opaque unknown suffix.
                const auto decoded=wire::rpc::read_server_set_current_weapon_arguments(reader,bunch.payload_bits-reader.position());
                const auto* desired=std::get_if<wire::object::ActorChannelReference>(&decoded.desired_weapon);
                if (desired && desired->channel==plan.dual_weapon.channel.channel && entry.inventory_given &&
                    entry.inventory_profile_current && entry.accepted_mech && !entry.pending_mech) {
                    entry.current_inventory_weapon=desired->channel;
                    registry->inventory_current_weapon_received=true;
                }
                // Native client already applied its local switch; no invented reply.
                // Null/package/other actor selections are unsupported no-ops here.
            }
        }
        return true;
    }
    bool readiness_ready(const Entry& entry) const {
        if (!ready_plan || !bootstrap_plan || !entry.links_sent || !entry.pri_identity_sent ||
            !entry.game_class_sent || !entry.hud_sent || entry.connection->snapshot().closed ||
            entry.connection->snapshot().publication!=game::PublicationState::join_requested) return false;
        const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
            pawn_plan->channel.channel,bootstrap_plan->gri.channel.channel};
        for (const auto channel:channels) {
            const auto state=entry.connection->channel_snapshot(channel);
            if (!state || state->closed || !state->opening_packet_id || !state->outgoing_open_acked ||
                state->outgoing_bunches!=0) return false;
        }
        return true;
    }
    bool send_ready_payload(Entry& entry,std::uint16_t channel,std::vector<std::uint8_t> payload,
                            std::size_t bits,double now) {
        wire::Bunch bunch;
        bunch.reliable=true; bunch.channel_type=2; bunch.channel=channel;
        bunch.payload=std::move(payload); bunch.payload_bits=bits;
        const auto step=entry.connection->send_actor(bunch,now);
        return !step.datagrams.empty() && !step.closed && !stopping.load() &&
            send(entry,step) && !stopping.load();
    }
    bool send_ready_counters(Entry& entry,const std::array<std::int32_t,3>& values,bool begun,double now) {
        const bool changed=!entry.ready_counters_sent || *entry.ready_counters_sent!=values;
        if (!changed && !begun) return true;
        const auto& plan=*ready_plan;
        const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
        // Compose the entire changed counter tuple and optional begin bit before
        // reserving a packet/nonce. Never align the two property payloads.
        wire::BitWriter payload;
        if (changed) {
            const std::array fields{wire::property::IntProperty{plan.gri_waiting_to_ready_field_id,values[0]},
                wire::property::IntProperty{plan.gri_waiting_for_field_id,values[1]},
                wire::property::IntProperty{plan.gri_countdown_field_id,values[2]}};
            const auto ints=wire::property::encode_int_properties(fields,plan.gri_exclusive_bound,
                plan.integer_order,cap);
            payload.write_bits(ints.payload,ints.logical_bits);
        }
        if (begun) {
            const std::array field{wire::property::BoolProperty{plan.gri_match_begun_field_id,true}};
            const auto flag=wire::property::encode_bool_properties(field,plan.gri_exclusive_bound,cap);
            if (payload.bit_count()>cap || flag.logical_bits>cap-payload.bit_count())
                fail(LocalUdpErrorCode::invalid_configuration);
            payload.write_bits(flag.payload,flag.logical_bits);
        }
        if (!send_ready_payload(entry,bootstrap_plan->gri.channel.channel,payload.payload_bytes(),
            payload.bit_count(),now)) return false;
        if (changed) entry.ready_counters_sent=values;
        if (begun) {
            entry.ready_begun_sent=true; registry->gri_match_begun_sent=true;
        }
        return true;
    }
    void apply_readiness(const session::SessionId& id,const std::shared_ptr<Entry>& target) {
        if (!ready_plan || !target || stopping.load()) return;
        if (!authorize(id)) return;
        const auto before=time();
        if (!before) { fail_closed(); return; }
        bool terminal=false,load_facts=false;
        std::uint64_t revision=0;
        {
            auto use=pin(id,target);
            if (!use) return;
            auto& entry=use->entry();
            {
                std::lock_guard lock(registry->mutex);
                if (!running.load() || stopping.load() || !registry->accepting ||
                    registry->entry!=target || !entry.active) return;
            }
            try {
                terminal=entry.connection->snapshot().closed;
                const std::array channels{controller_plan->channel.channel,pri_plan->channel.channel,
                    pawn_plan->channel.channel,bootstrap_plan->gri.channel.channel};
                for (const auto channel:channels) {
                    const auto state=entry.connection->channel_snapshot(channel);
                    if (state && state->closed) terminal=true;
                }
                const auto pri=entry.connection->channel_snapshot(pri_plan->channel.channel);
                const auto gri=entry.connection->channel_snapshot(bootstrap_plan->gri.channel.channel);
                if (entry.ready_begun_sent && gri && !gri->closed && gri->outgoing_bunches==0)
                    registry->gri_match_begun_acked=true;
                if (entry.ready_sent_revision && *entry.ready_sent_revision==entry.ready_revision &&
                    pri && !pri->closed && pri->outgoing_bunches==0) {
                    entry.ready_acked_revision=entry.ready_revision;
                    registry->pri_ready_acked=true;
                }
                if (!terminal && readiness_ready(entry)) {
                    if (!entry.ready_counters_sent) {
                        const auto state=pending_match->snapshot();
                        terminal=!send_ready_counters(entry,{state.waiting_to_ready,state.waiting_for,
                            state.countdown_remaining},false,*before);
                    } else if (entry.ready_observed && (!entry.ready_sent_value ||
                        *entry.ready_sent_value!=entry.ready_value)) {
                        const auto& plan=*ready_plan;
                        const std::array fields{wire::property::BoolProperty{plan.pri_ready_field_id,entry.ready_value},
                            wire::property::BoolProperty{plan.pri_waiting_field_id,plan.role_waiting_player}};
                        auto payload=wire::property::encode_bool_properties(fields,plan.pri_exclusive_bound,
                            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                        terminal=!send_ready_payload(entry,pri_plan->channel.channel,std::move(payload.payload),
                            payload.logical_bits,*before);
                        if (!terminal) {
                            entry.ready_sent_value=entry.ready_value;
                            entry.ready_sent_revision=entry.ready_revision;
                            entry.ready_acked_revision.reset(); registry->pri_ready_sent=true;
                        }
                    } else if (!world_start_committed && ready_facts_provider && entry.ready_observed &&
                        entry.ready_acked_revision==entry.ready_revision) {
                        revision=entry.ready_revision; load_facts=true;
                    }
                }
            } catch (...) { terminal=true; }
        } // Provider/SQL/Auth must never run under an in-flight Use or registry lock.
        if (terminal) { registry->retire(target); return; }
        if (!load_facts) return;
        std::optional<ReadyAccountFacts> facts;
        try { facts=ready_facts_provider(id); }
        catch (...) { registry->retire(target); return; }
        if (!authorize(id)) return;
        const auto after=time();
        if (!after) { fail_closed(); return; }
        {
            auto use=pin(id,target);
            if (!use) return;
            auto& entry=use->entry();
            {
                std::lock_guard lock(registry->mutex);
                if (!running.load() || stopping.load() || !registry->accepting ||
                    registry->entry!=target || !entry.active) return;
            }
            if (world_start_committed || entry.ready_revision!=revision ||
                entry.ready_acked_revision!=revision || !readiness_ready(entry)) return;
            try {
                // This is the configured one joined-entry local roster. No native
                // NET isPlayer inference, fabricated travellers or mech revision.
                const bool synced=facts && facts->owned_collection_loaded && facts->unranked_stats_available &&
                    facts->item_stats_loaded && facts->achievements_loaded;
                const auto& plan=*ready_plan;
                const std::array roster{game::MatchPlayerState{plan.role_is_player,plan.role_waiting_player,
                    entry.ready_value,synced,facts && facts->purchasing,facts && facts->saving}};
                const double relative=*after-world_clock_base;
                if (!std::isfinite(relative) || relative<0 || relative>std::numeric_limits<float>::max())
                    fail(LocalUdpErrorCode::invalid_lifecycle);
                const game::PendingMatchInputs inputs{static_cast<float>(relative),plan.role_is_player ? 1 : 0,
                    0,roster,plan.world_tracking_created,plan.world_pending_quit,true,false};
                const auto evaluated=pending_match->evaluate(inputs);
                const auto state=pending_match->snapshot();
                terminal=!send_ready_counters(entry,{state.waiting_to_ready,state.waiting_for,
                    state.countdown_remaining},evaluated.start_requested,*after);
                if (!terminal && evaluated.start_requested) {
                    world_start_committed=true; registry->match_start_requested=true;
                }
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire(target);
    }
    void record_ready(Entry& entry,bool value) {
        registry->ready_rpc_received=true;
        if (!entry.ready_observed || entry.ready_value!=value) {
            if (entry.ready_revision==std::numeric_limits<std::uint64_t>::max())
                fail(LocalUdpErrorCode::invalid_lifecycle);
            entry.ready_observed=true; entry.ready_value=value; ++entry.ready_revision;
            entry.ready_acked_revision.reset();
            // Latest value can return to the tuple already on the wire before
            // the intermediate value was sent. Its actual queue ACK still gates.
            if (entry.ready_sent_value && *entry.ready_sent_value==value)
                entry.ready_sent_revision=entry.ready_revision;
        }
    }
    bool collect_start_mech(Entry& entry,const std::vector<wire::Bunch>& bunches) {
        if (!start_mech_plan && !ready_plan) return true;
        const auto bound=ready_plan ? ready_plan->controller_exclusive_bound
                                    : start_mech_plan->controller_exclusive_bound;
        const bool active_item_index=start_mech_plan && start_mech_plan->active_item_index.has_value();
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
                try { field=reader.read_bounded(bound); }
                catch (const wire::WireError&) { break; }
                if (ready_plan && field==ready_plan->ready_rpc_field_id) {
                    const auto decoded=wire::rpc::read_ready_arguments(reader,1);
                    record_ready(entry,decoded.ready);
                } else if (validation_plan && field==validation_plan->validate_rpc_field_id) {
                    request_validation();
                    if (!admission_plan && !ready_plan && !active_item_index) break;
                } else if (admission_plan && field==admission_plan->stage_rpc_field_id) {
                    const auto decoded=wire::rpc::read_selection_stage_arguments(reader,skeleton.endian,
                        bunch.payload_bits-reader.position());
                    if (decoded.stage==3) {
                        registry->stage3_requested=true;
                        // Native PlayerWaiting stage3 marks ready while the game
                        // waits. The selected-data admission remains pending.
                        if (ready_plan && !world_start_committed) record_ready(entry,true);
                        if (entry.validation_succeeded && entry.validation_responded && entry.accepted_mech &&
                            !entry.pending_mech && entry.admitted_revision!=entry.mech_revision)
                            entry.admission_request_revision=entry.mech_revision;
                    } else entry.admission_request_revision.reset();
                } else if (active_item_index && field==start_mech_plan->active_item_index->field_id) {
                    // Same exact eligible IntProperty/flags80/ArrayDim1 wire
                    // schema as SelectionStage; preserve its bounded cursor.
                    // This compatibility path does not execute active-item state.
                    static_cast<void>(wire::rpc::read_selection_stage_arguments(reader,skeleton.endian,
                        bunch.payload_bits-reader.position()));
                } else if (start_mech_plan && field==start_mech_plan->controller_rpc_field_id) {
                    auto decoded=wire::rpc::read_start_mech_arguments(reader,skeleton.endian,
                        bunch.payload_bits-reader.position());
                    if (entry.pending_mech) {
                        const auto& prior=*entry.pending_mech;
                        if (prior.mech_instance_id!=decoded.value.mech_instance_id ||
                            prior.primary_weapon!=decoded.value.primary_weapon ||
                            prior.holo_taunt_type!=decoded.value.holo_taunt_type) return false;
                    } else entry.pending_mech=std::move(decoded.value);
                    if (inventory_plan) entry.inventory_profile_current=false;
                    // A newly requested preset cannot borrow the old validation.
                    entry.validation_succeeded=false;
                    entry.admission_request_revision.reset();
                    if (!admission_plan && !ready_plan && !active_item_index) {
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
                if (inventory_plan && value && (value->mech_type!=1 || value->mech_sub_type!=15 ||
                    value->mech_weapons.primary_idx!=0 || value->mech_weapons.secondary_idx!=101)) terminal=true;
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
                        if (inventory_plan) {
                            entry.inventory_profile_current=true;
                            registry->inventory_profile_accepted=true;
                        }
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
            if (ready_plan && !world_start_committed) return;
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
                    if (inventory_plan && inventory_plan->combat) {
                        auto state=inventory_plan->combat->initial_state;
                        state.spawn_revision=entry.mech_revision;
                        state.alive=true;state.spawn_active=true;
                        entry.combat_state=state;
                    }
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
                observe_actor_bunches(step.actor_bunches);
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
                                    if (!terminal && bootstrap_plan) {
                                        const auto& gri=bootstrap_plan->gri;
                                        const auto gri_reference=entry.connection->resolve_package_reference(
                                            gri.archetype_package_guid,gri.archetype_local_net_index);
                                        // GRI shares the verified null-GetPC actor prefix,
                                        // with caller-supplied CDO/rotation eligibility.
                                        auto gri_payload=wire::actor::encode_initial_pawn(
                                            {gri_reference,gri.position,gri.rotation},
                                            static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8);
                                        entry.connection->register_actor_channel(gri.channel);
                                        wire::Bunch gri_bunch;
                                        gri_bunch.control=true; gri_bunch.open=true; gri_bunch.reliable=true;
                                        gri_bunch.channel=gri.channel.channel; gri_bunch.channel_type=2;
                                        gri_bunch.payload_bits=gri_payload.payload_bits;
                                        gri_bunch.payload=std::move(gri_payload.payload);
                                        const auto gri_step=entry.connection->send_actor(gri_bunch,now);
                                        if (!gri_step.datagrams.empty() && !stopping.load() &&
                                            send(entry,gri_step) && !stopping.load() && !gri_step.closed) {
                                            entry.gri_sent=true; registry->gri_sent=true;
                                        } else terminal=true;
                                    }
                                }
                                else terminal=true;
                            }
                        } catch (...) { terminal=true; }
                    }
                }
                if (!terminal && !send_links(entry,now)) terminal=true;
                if (!terminal && !send_bootstrap(entry,now)) terminal=true;
                if (!terminal && !collect_start_mech(entry,step.actor_bunches)) terminal=true;
                if (!terminal && !collect_inventory_manager(entry,step.actor_bunches)) terminal=true;
                if (!terminal && !take_start_mech(*use,mech)) terminal=true;
            } catch (const game::ConnectionError&) {
                terminal=entry.connection->snapshot().closed;
            } catch (...) { terminal=true; }
        } // Release in-flight ownership BEFORE retiring or any callback.
        if (terminal) registry->retire_id(id);
        else if (candidate) validate_response(id,target,*candidate);
        else {
            if (mech) apply_start_mech(id,*mech);
            apply_inventory(id,target);
            apply_validation(id,target);
            apply_readiness(id,target);
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
                if (!terminal && !send_bootstrap(entry,now)) terminal=true;
                if (!terminal && !take_start_mech(*use,mech)) terminal=true;
            } catch (...) { terminal=true; }
        }
        if (terminal) registry->retire_id(id);
        else {
            if (mech) apply_start_mech(id,*mech);
            apply_inventory(id,target);
            apply_validation(id,target);
            apply_readiness(id,target);
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
               std::optional<PriIdentityDispatchPlan> pri_identity,
               std::optional<SelectionBootstrapPlan> selection_bootstrap,
               std::optional<ReadyMatchPlan> ready_match,ReadyFactsProvider ready_facts,
               std::optional<InventoryBootstrapPlan> inventory_bootstrap) {
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
        if (selection_bootstrap) {
            const auto& plan=*selection_bootstrap;
            const auto& e=plan.eligibility;
            if (!validator || !publication || !controller || !pri || !pawn || !links ||
                !e.primary_package_map || !e.common_gri_prefix_correct || !e.gri_cdo_correct ||
                !e.gri_cache_correct || !e.game_class_property_eligible ||
                !e.game_class_property_class_correct || !e.game_class_meta_class_correct ||
                !e.controller_cache_correct || !e.client_set_hud_rpc_eligible ||
                !e.hud_parameter_eligible || !e.hud_parameter_class_correct || !e.hud_meta_class_correct ||
                plan.gri.channel.channel==0 || plan.gri.channel.channel>2047 ||
                plan.gri.channel.channel==controller->channel.channel ||
                plan.gri.channel.channel==pri->channel.channel || plan.gri.channel.channel==pawn->channel.channel ||
                plan.gri.archetype_local_net_index<0 || plan.game_class_local_net_index<0 || plan.hud_local_net_index<0 ||
                plan.gri_exclusive_bound==0 || plan.game_class_field_id>=plan.gri_exclusive_bound ||
                plan.controller_exclusive_bound!=links->controller_exclusive_bound ||
                plan.client_set_hud_field_id>=plan.controller_exclusive_bound ||
                plan.client_set_hud_field_id==links->controller_pri_field_id ||
                plan.client_set_hud_field_id==links->controller_pawn_field_id)
                fail(LocalUdpErrorCode::invalid_configuration);
            if (start_mech && (plan.controller_exclusive_bound!=start_mech->controller_exclusive_bound ||
                plan.client_set_hud_field_id==start_mech->controller_rpc_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_validation && (plan.controller_exclusive_bound!=spawn_validation->controller_exclusive_bound ||
                plan.client_set_hud_field_id==spawn_validation->validate_rpc_field_id ||
                plan.client_set_hud_field_id==spawn_validation->validated_rpc_field_id ||
                plan.client_set_hud_field_id==spawn_validation->failed_rpc_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_admission && (plan.controller_exclusive_bound!=spawn_admission->controller_exclusive_bound ||
                plan.client_set_hud_field_id==spawn_admission->stage_rpc_field_id ||
                plan.client_set_hud_field_id==spawn_admission->client_restart_rpc_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            try {
                const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                static_cast<void>(wire::actor::encode_initial_pawn(
                    {{0},plan.gri.position,plan.gri.rotation},cap));
                const std::array fields{wire::property::ObjectProperty{plan.game_class_field_id,
                    wire::object::PackageExportReference{0}}};
                static_cast<void>(wire::property::encode_object_properties(fields,plan.gri_exclusive_bound,cap));
                static_cast<void>(wire::rpc::encode_rpc({plan.client_set_hud_field_id,
                    wire::object::PackageExportReference{0}},plan.controller_exclusive_bound,true,cap));
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (ready_facts && !ready_match) fail(LocalUdpErrorCode::invalid_configuration);
        if (ready_match) {
            const auto& plan=*ready_match;
            const auto& e=plan.eligibility;
            if (!publication || !validator || !controller || !pawn || !pri || !links ||
                !pri_identity || !selection_bootstrap ||
                !e.primary_package_map || !e.controller_cache_correct || !e.pri_cache_correct ||
                !e.gri_cache_correct || !e.ready_rpc_eligible || !e.ready_argument_bool_net_correct ||
                !e.pri_ready_property_eligible || !e.pri_waiting_property_eligible ||
                !e.gri_waiting_to_ready_property_eligible || !e.gri_waiting_for_property_eligible ||
                !e.gri_countdown_property_eligible || !e.gri_match_begun_property_eligible ||
                !e.integer_archive_order_correct || plan.initial_state.pending_start_time!=0.0F ||
                (plan.integer_order!=control::Endian::little && plan.integer_order!=control::Endian::big))
                fail(LocalUdpErrorCode::invalid_configuration);
            const auto collision=[&](std::uint32_t id) { return plan.ready_rpc_field_id==id; };
            if (plan.controller_exclusive_bound==0 || plan.pri_exclusive_bound==0 ||
                plan.gri_exclusive_bound==0 || plan.ready_rpc_field_id>=plan.controller_exclusive_bound ||
                plan.controller_exclusive_bound!=links->controller_exclusive_bound ||
                plan.controller_exclusive_bound!=selection_bootstrap->controller_exclusive_bound ||
                plan.pri_exclusive_bound!=pri_identity->exclusive_bound ||
                plan.gri_exclusive_bound!=selection_bootstrap->gri_exclusive_bound ||
                collision(links->controller_pri_field_id) || collision(links->controller_pawn_field_id) ||
                collision(selection_bootstrap->client_set_hud_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (start_mech && (plan.controller_exclusive_bound!=start_mech->controller_exclusive_bound ||
                plan.pri_exclusive_bound!=start_mech->pri_exclusive_bound || collision(start_mech->controller_rpc_field_id)))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_validation && (plan.controller_exclusive_bound!=spawn_validation->controller_exclusive_bound ||
                collision(spawn_validation->validate_rpc_field_id) || collision(spawn_validation->validated_rpc_field_id) ||
                collision(spawn_validation->failed_rpc_field_id))) fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_admission && (plan.controller_exclusive_bound!=spawn_admission->controller_exclusive_bound ||
                collision(spawn_admission->stage_rpc_field_id) || collision(spawn_admission->client_restart_rpc_field_id)))
                fail(LocalUdpErrorCode::invalid_configuration);
            const std::array pri_fields{plan.pri_ready_field_id,plan.pri_waiting_field_id};
            for (const auto id:pri_fields)
                if (id>=plan.pri_exclusive_bound || id==pri_identity->field_id ||
                    (start_mech && id==start_mech->pri_mech_setup_field_id))
                    fail(LocalUdpErrorCode::invalid_configuration);
            if (pri_fields[0]==pri_fields[1]) fail(LocalUdpErrorCode::invalid_configuration);
            const std::array gri_fields{plan.gri_waiting_to_ready_field_id,plan.gri_waiting_for_field_id,
                plan.gri_countdown_field_id,plan.gri_match_begun_field_id};
            for (std::size_t i=0;i<gri_fields.size();++i) {
                if (gri_fields[i]>=plan.gri_exclusive_bound || gri_fields[i]==selection_bootstrap->game_class_field_id)
                    fail(LocalUdpErrorCode::invalid_configuration);
                for (std::size_t j=0;j<i;++j)
                    if (gri_fields[i]==gri_fields[j]) fail(LocalUdpErrorCode::invalid_configuration);
            }
            try {
                static_cast<void>(game::PendingMatch(plan.rules,plan.initial_state));
                const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                const std::array echo{wire::property::BoolProperty{plan.pri_ready_field_id,true},
                    wire::property::BoolProperty{plan.pri_waiting_field_id,plan.role_waiting_player}};
                static_cast<void>(wire::property::encode_bool_properties(echo,plan.pri_exclusive_bound,cap));
                const std::array counters{wire::property::IntProperty{gri_fields[0],0},
                    wire::property::IntProperty{gri_fields[1],0},wire::property::IntProperty{gri_fields[2],-1}};
                const auto ints=wire::property::encode_int_properties(counters,plan.gri_exclusive_bound,plan.integer_order,cap);
                const std::array begun{wire::property::BoolProperty{plan.gri_match_begun_field_id,true}};
                const auto flag=wire::property::encode_bool_properties(begun,plan.gri_exclusive_bound,cap);
                if (ints.logical_bits>cap || flag.logical_bits>cap-ints.logical_bits)
                    fail(LocalUdpErrorCode::invalid_configuration);
            } catch (...) { fail(LocalUdpErrorCode::invalid_configuration); }
        }
        if (start_mech && start_mech->active_item_index) {
            const auto& plan=*start_mech->active_item_index;
            const auto collision=[&](std::uint32_t id) { return plan.field_id==id; };
            if (!plan.rpc_eligible || !plan.argument_eligible ||
                plan.field_id>=start_mech->controller_exclusive_bound ||
                collision(start_mech->controller_rpc_field_id) ||
                collision(links->controller_pri_field_id) || collision(links->controller_pawn_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_validation && (collision(spawn_validation->validate_rpc_field_id) ||
                collision(spawn_validation->validated_rpc_field_id) || collision(spawn_validation->failed_rpc_field_id)))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (spawn_admission && (collision(spawn_admission->stage_rpc_field_id) ||
                collision(spawn_admission->client_restart_rpc_field_id)))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (selection_bootstrap && collision(selection_bootstrap->client_set_hud_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
            if (ready_match && collision(ready_match->ready_rpc_field_id))
                fail(LocalUdpErrorCode::invalid_configuration);
        }
        if (inventory_bootstrap) {
            const auto& plan=*inventory_bootstrap;const auto& eligible=plan.eligibility;
            if (!validator || !publication || !controller || !pri || !pawn || !links || !start_mech || !provider ||
                plan.pawn_exclusive_bound!=links->pawn_exclusive_bound ||
                !eligible.primary_package_map || !eligible.actor_prefixes_correct || !eligible.archetypes_correct ||
                !eligible.actor_caches_correct || !eligible.scalar_links_correct || !eligible.hand_weap_array_correct ||
                !eligible.client_given_to_correct || !eligible.server_set_current_weapon_correct)
                fail(LocalUdpErrorCode::invalid_configuration);
            const auto fields=[](const auto& ids,std::uint32_t bound) {
                if (bound==0) fail(LocalUdpErrorCode::invalid_configuration);
                for (std::size_t i=0;i<ids.size();++i) {
                    if (ids[i]>=bound) fail(LocalUdpErrorCode::invalid_configuration);
                    for (std::size_t j=0;j<i;++j) if (ids[i]==ids[j]) fail(LocalUdpErrorCode::invalid_configuration);
                }
            };
            fields(std::array{plan.pawn_inv_manager_field_id,plan.pawn_dual_weapon_field_id,
                links->pawn_controller_field_id,links->pawn_pri_field_id},plan.pawn_exclusive_bound);
            fields(std::array{plan.owner_field_id,plan.instigator_field_id,plan.manager_chain_field_id,
                plan.manager_server_set_current_weapon_field_id},plan.manager_exclusive_bound);
            fields(std::array{plan.owner_field_id,plan.instigator_field_id,plan.inventory_manager_field_id,
                plan.dual_hand_weap_field_id,plan.dual_client_given_to_field_id},plan.dual_exclusive_bound);
            if (plan.combat) {
                const auto& combat=*plan.combat;const auto& seed=combat.initial_state;
                if (!spawn_admission || !combat.server_can_fire_property_correct ||
                    seed.spawn_revision!=0 || seed.alive || seed.spawn_active || seed.move_state>=23 ||
                    !std::isfinite(seed.forward_boost_fire_cooldown) || !std::isfinite(seed.emp_time_remaining) ||
                    !std::isfinite(seed.heavy_mode_time) || !std::isfinite(seed.heavy_mode_start_time) ||
                    !std::isfinite(seed.heavy_mode_transform_time_multiplier))
                    fail(LocalUdpErrorCode::invalid_configuration);
                fields(std::array{plan.owner_field_id,plan.instigator_field_id,plan.inventory_manager_field_id,
                    plan.dual_hand_weap_field_id,plan.dual_client_given_to_field_id,combat.server_can_fire_field_id},
                    plan.dual_exclusive_bound);
            }
            for (const auto bound:{plan.primary_exclusive_bound,plan.secondary_exclusive_bound})
                fields(std::array{plan.owner_field_id,plan.instigator_field_id,plan.inventory_manager_field_id,
                    plan.hand_dual_weapon_field_id},bound);
            const std::array actors{&plan.manager,&plan.dual_weapon,&plan.primary_weapon,&plan.secondary_weapon};
            try {
                const auto cap=static_cast<std::size_t>(skeleton.transport.max_packet_bytes)*8;
                for (std::size_t i=0;i<actors.size();++i) {
                    const auto& actor=*actors[i];const auto channel=actor.channel.channel;
                    if (channel==0 || channel>2047 || channel==controller->channel.channel ||
                        channel==pawn->channel.channel || channel==pri->channel.channel ||
                        (selection_bootstrap && channel==selection_bootstrap->gri.channel.channel) ||
                        actor.archetype_local_net_index<0 || actor.rotation)
                        fail(LocalUdpErrorCode::invalid_configuration);
                    for (std::size_t j=0;j<i;++j) if (channel==actors[j]->channel.channel)
                        fail(LocalUdpErrorCode::invalid_configuration);
                    static_cast<void>(wire::actor::encode_initial_pawn(
                        {wire::object::PackageExportReference{0},actor.position,actor.rotation},cap));
                }
                static_cast<void>(inventory_payloads(plan,pawn->channel.channel,cap));
                static_cast<void>(wire::rpc::encode_client_given_to(plan.dual_client_given_to_field_id,
                    plan.dual_exclusive_bound,pawn->channel.channel,false,cap));
                if (plan.combat) {
                    const std::array field{wire::property::BoolProperty{plan.combat->server_can_fire_field_id,true}};
                    static_cast<void>(wire::property::encode_bool_properties(field,plan.dual_exclusive_bound,cap));
                }
            } catch (...) {fail(LocalUdpErrorCode::invalid_configuration);}
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
        bootstrap_plan=std::move(selection_bootstrap);
        if (ready_match) {
            const auto epoch=time();
            if (!epoch) fail(LocalUdpErrorCode::invalid_configuration);
            world_clock_base=*epoch;
            pending_match.emplace(ready_match->rules,ready_match->initial_state);
        }
        ready_plan=std::move(ready_match); ready_facts_provider=std::move(ready_facts);
        inventory_plan=std::move(inventory_bootstrap);
        started=true;
        {
            std::lock_guard lock(registry->mutex); registry->accepting=true;
        }
        running=true;
        try { worker=std::thread([this]{loop();}); }
        catch (...) {
            fail_closed(); authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset(); pawn_plan.reset(); pri_plan.reset(); links_plan.reset(); start_mech_plan.reset(); mech_provider={}; validation_plan.reset(); spawn_validator={}; admission_plan.reset(); pri_identity_plan.reset(); bootstrap_plan.reset(); ready_plan.reset(); ready_facts_provider={}; pending_match.reset(); inventory_plan.reset();
            fail(LocalUdpErrorCode::invalid_lifecycle);
        }
    }
    void stop() noexcept {
        running=false; stopping=true; registry->close();
        if (worker.joinable()) worker.join();
        authorizer={}; login_validator={}; publication_provider={}; controller_plan.reset(); pawn_plan.reset(); pri_plan.reset(); links_plan.reset(); start_mech_plan.reset(); mech_provider={}; validation_plan.reset(); spawn_validator={}; admission_plan.reset(); pri_identity_plan.reset(); bootstrap_plan.reset(); ready_plan.reset(); ready_facts_provider={}; pending_match.reset(); inventory_plan.reset();
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
    LocalUdpDiagnostics result{impl_->registry->authenticated.load(),impl_->registry->hello.load(),impl_->registry->closed.load(),
        impl_->registry->login_received.load(),impl_->registry->login_validated.load(),
        impl_->registry->world_published.load(),impl_->registry->join_requested.load(),
        impl_->registry->controller_sent.load(),impl_->registry->pawn_sent.load(),impl_->registry->pri_sent.load(),
        impl_->registry->spawn_validation_requested.load(),impl_->registry->spawn_validation_sent.load(),
        impl_->registry->spawn_validation_failed.load(),impl_->registry->stage3_requested.load(),
        impl_->registry->client_restart_sent.load(),impl_->registry->controller_open_acked.load(),
        impl_->registry->pri_open_acked.load(),impl_->registry->pawn_open_acked.load(),
        impl_->registry->actor_links_sent.load(),impl_->registry->pri_identity_sent.load(),
        impl_->registry->pri_identity_acked.load(),impl_->registry->gri_sent.load(),
        impl_->registry->gri_open_acked.load(),impl_->registry->game_class_sent.load(),
        impl_->registry->game_class_acked.load(),impl_->registry->hud_sent.load(),impl_->registry->hud_acked.load(),
        impl_->registry->ready_rpc_received.load(),impl_->registry->pri_ready_sent.load(),impl_->registry->pri_ready_acked.load(),
        impl_->registry->match_start_requested.load(),impl_->registry->gri_match_begun_sent.load(),
        impl_->registry->gri_match_begun_acked.load(),impl_->registry->inventory_profile_accepted.load(),
        impl_->registry->inventory_openings_sent.load(),impl_->registry->inventory_openings_acked.load(),
        impl_->registry->inventory_links_sent.load(),impl_->registry->inventory_links_acked.load(),
        impl_->registry->inventory_given_to_sent.load(),impl_->registry->inventory_current_weapon_received.load()};
    for (std::size_t i=0;i<result.actor_bunches.size();++i)
        result.actor_bunches[i]=impl_->registry->actor_bunches[i].snapshot();
    result.movement=impl_->registry->movement.snapshot();
    return result;
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
                          std::optional<PriIdentityDispatchPlan> pri_identity,
                          std::optional<SelectionBootstrapPlan> selection_bootstrap,
                          std::optional<ReadyMatchPlan> ready_match,ReadyFactsProvider ready_facts,
               std::optional<InventoryBootstrapPlan> inventory_bootstrap) {
    impl_->start(std::move(authorize),std::move(validate_response),std::move(publication),
        std::move(controller),std::move(pawn),std::move(pri),std::move(links),
        std::move(start_mech),std::move(mech_provider),
        std::move(spawn_validation),std::move(selection_validator),std::move(spawn_admission),
        std::move(pri_identity),std::move(selection_bootstrap),std::move(ready_match),std::move(ready_facts),
        std::move(inventory_bootstrap));
}
void LocalUdpOwner::stop() noexcept { impl_->stop(); }
std::unique_ptr<assignment::UdpSessionLease> LocalUdpOwner::handoff(
    const assignment::SessionProvision& provision) { return impl_->handoff(provision); }
}
