#include "hawken/reliable_control_transport.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace hawken::wire::reliable {
namespace {
[[noreturn]] void fail(TransportErrorCode code) { throw TransportError(code); }
std::int32_t signed_bits(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
bool later(std::uint32_t value, std::uint32_t reference) {
    return signed_bits(value) > signed_bits(reference);
}

void validate_config(const TransportConfig& c) {
    const auto count_ok=[](std::size_t n) { return n>0 && n<=1024; };
    if (c.max_packet_bytes<2 || c.max_packet_bytes>65507 ||
        !std::isfinite(c.incoming_timeout) || c.incoming_timeout<=0 ||
        !count_ok(c.limits.max_outgoing_bunches) || !count_ok(c.limits.max_incoming_bunches) ||
        !count_ok(c.limits.max_ack_ids) || !count_ok(c.limits.max_records_per_packet) ||
        !count_ok(c.limits.max_packets_per_step) || c.limits.max_payload_bytes==0 ||
        c.limits.max_payload_bytes>1048576)
        fail(TransportErrorCode::invalid_configuration);
}
void validate_time(const Snapshot& s, double now) {
    if (s.closed) fail(TransportErrorCode::terminal_state);
    if (!std::isfinite(now) || now<0 || (s.last_time && now<*s.last_time))
        fail(TransportErrorCode::invalid_time);
}
bool empty_bunch(const Bunch& b) {
    return !b.control && !b.open && !b.close && !b.reliable && b.channel==0 &&
        b.sequence==0 && b.channel_type==0 && b.payload_bits==0 && b.payload.empty();
}
std::size_t bounded_width(std::uint32_t value, std::uint32_t limit) {
    std::size_t bits=0;
    std::uint64_t prefix=0;
    for (std::uint64_t mask=1; prefix+mask<limit; mask<<=1U) {
        ++bits;
        if ((value&mask)!=0) prefix|=mask;
    }
    return bits;
}
// Allocation-free canonical/size preflight mirrors packet_writer. Scope
// rejection is our API boundary, not a claim about other native channels.
std::size_t bunch_bits(const Bunch& b, const TransportConfig& c) {
    if (b.channel>=0x800 || (b.reliable && b.sequence>=0x400) ||
        ((b.reliable || b.open) && b.channel_type>=8) ||
        b.control!=(b.open || b.close) || (!b.reliable && b.sequence!=0) ||
        (!(b.reliable || b.open) && b.channel_type!=0) ||
        b.payload_bits>=c.max_packet_bytes*8U)
        fail(TransportErrorCode::invalid_packet);
    const auto bytes=b.payload_bits/8+(b.payload_bits%8!=0 ? 1 : 0);
    if (b.payload.size()!=bytes || (b.payload_bits%8!=0 &&
        (b.payload.back()&(0xffU<<(b.payload_bits%8)))!=0))
        fail(TransportErrorCode::invalid_packet);
    if (!b.reliable || (b.channel==0 ? b.channel_type!=1 : b.channel_type!=2))
        fail(TransportErrorCode::unsupported_bunch);
    return 14+(b.control ? 2 : 0)+10+3+
        bounded_width(static_cast<std::uint32_t>(b.payload_bits),c.max_packet_bytes*8U)+b.payload_bits;
}
void validate_single_bunch(const Bunch& b, const TransportConfig& c) {
    if (bunch_bits(b,c)>static_cast<std::size_t>(c.max_packet_bytes)*8-15)
        fail(TransportErrorCode::invalid_packet);
}
void validate_packet(const Packet& p, const TransportConfig& c) {
    if (p.records.size()>c.limits.max_records_per_packet)
        fail(TransportErrorCode::resource_limit);
    if (p.packet_id>=0x4000) fail(TransportErrorCode::invalid_packet);
    const auto capacity=static_cast<std::size_t>(c.max_packet_bytes)*8-1;
    std::size_t total=14;
    for (const auto& r:p.records) {
        std::size_t bits;
        if (r.is_ack) {
            if (r.ack_packet_id>=0x4000 || !empty_bunch(r.bunch))
                fail(TransportErrorCode::invalid_packet);
            bits=15;
        } else {
            if (r.ack_packet_id!=0) fail(TransportErrorCode::invalid_packet);
            bits=bunch_bits(r.bunch,c);
        }
        if (bits>capacity-total) fail(TransportErrorCode::invalid_packet);
        total+=bits;
    }
}

template<class State>
auto& find_channel(State& state,std::uint16_t index) {
    if (index==0) return state.control;
    for (auto& channel:state.actors)
        if (channel.view.channel==index) return channel;
    fail(TransportErrorCode::unsupported_bunch);
}

// The staged State is bounded before copying. All outputs and allocations
// precede the noexcept move commit in the public methods.
template<class State>
class Operation {
public:
    Operation(const TransportConfig& config, State& state, double time)
        : c(config), s(state), now(time) {}
    TransportStep result;

    void send_new(const Bunch& input) {
        auto& channel=find_channel(s,input.channel);
        if (queue_count(true)>=c.limits.max_outgoing_bunches)
            fail(TransportErrorCode::resource_limit);
        require_payload(input.payload.size());
        require_output(1);
        const auto sequence=channel.view.outgoing_sequence+1U;
        auto b=input;
        b.sequence=static_cast<std::uint16_t>(sequence&0x3ffU);
        const auto id=emit_bunch(b);
        channel.outgoing.push_back({std::move(b),sequence,id,now,false});
        channel.view.outgoing_sequence=sequence;
        s.view.queued_payload_bytes+=input.payload.size();
        channel.view.queued_payload_bytes+=input.payload.size();
        if (input.open && !channel.view.opening_packet_id) channel.view.opening_packet_id=id;
    }

    void process_ack(std::uint16_t raw) {
        const auto id=expand_packet_id(raw,s.view.last_ack_packet_id);
        if (later(id,s.view.last_ack_packet_id)) {
            // Visit gaps in signed order, skipping IDs with no pending entry.
            // Re-scan CURRENT associations after each gap: a retry can move
            // an entry into a later gap. Each actual retry consumes a quota.
            auto cursor=s.view.last_ack_packet_id;
            for (;;) {
                std::optional<std::uint32_t> missing;
                for_channels([&](const auto& channel) {
                    for (const auto& entry:channel.outgoing)
                        if (!entry.acked && later(entry.packet_id,cursor) && later(id,entry.packet_id) &&
                            (!missing || later(*missing,entry.packet_id))) missing=entry.packet_id;
                });
                if (!missing) break;
                for_channels([&](auto& channel) {
                    for (auto& entry:channel.outgoing)
                        if (!entry.acked && entry.packet_id==*missing) retry(entry);
                });
                cursor=*missing;
            }
            s.view.last_ack_packet_id=id;
        }
        for_channels([&](auto& channel) {
            if (channel.view.closed || s.view.closed) return;
            for (auto& entry:channel.outgoing) {
                if (entry.packet_id==id) {
                    entry.acked=true;
                    if (entry.bunch.open) channel.view.open_confirmed=true;
                }
            }
            if (channel.view.opening_packet_id==id) channel.view.open_confirmed=true;
            // Each channel prunes only its own confirmed prefix.
            while (!channel.outgoing.empty() && channel.outgoing.front().acked) {
                const bool close=channel.outgoing.front().bunch.close;
                release_payload(channel,channel.outgoing.front().bunch.payload.size());
                channel.outgoing.erase(channel.outgoing.begin());
                if (close) { retire(channel); break; }
            }
        });
    }

    void process_bunch(const Bunch& b, std::uint32_t packet_id) {
        auto& channel=find_channel(s,b.channel);
        if (channel.view.closed) return;
        const auto seq=expand_sequence(b.sequence,channel.view.incoming_sequence);
        if (!later(seq,channel.view.incoming_sequence)) return;
        if (b.open) {
            channel.view.open_confirmed=true;
            channel.view.opening_packet_id=packet_id;
        }
        if (seq!=channel.view.incoming_sequence+1U) {
            auto pos=std::lower_bound(channel.incoming.begin(),channel.incoming.end(),seq,
                [](const auto& e,std::uint32_t value) { return signed_bits(e.sequence)<signed_bits(value); });
            if (pos!=channel.incoming.end() && pos->sequence==seq) return;
            if (queue_count(false)>=c.limits.max_incoming_bunches)
                fail(TransportErrorCode::resource_limit);
            require_payload(b.payload.size());
            const bool first=channel.incoming.empty();
            channel.incoming.insert(pos,{b,seq});
            s.view.queued_payload_bytes+=b.payload.size();
            channel.view.queued_payload_bytes+=b.payload.size();
            if (first) channel.view.incoming_blocked_since=now;
            return;
        }
        deliver(channel,b,seq);
        while (!channel.view.closed && !s.view.closed && !channel.incoming.empty() &&
               channel.incoming.front().sequence==channel.view.incoming_sequence+1U) {
            // Pop/account before delivery: actor close may clear the remaining
            // queues. Never keep a head reference across retire().
            auto head=std::move(channel.incoming.front());
            channel.incoming.erase(channel.incoming.begin());
            release_payload(channel,head.bunch.payload.size());
            deliver(channel,head.bunch,head.sequence);
            // Native refresh occurs after queued drain, not every immediate
            // expected delivery while some future entry remains blocked.
            if (!channel.view.closed) channel.view.incoming_blocked_since=now;
        }
        if (channel.incoming.empty()) channel.view.incoming_blocked_since.reset();
    }

    void acknowledge_packet(std::uint32_t id) {
        if (s.current_acks.size()>=c.limits.max_ack_ids)
            fail(TransportErrorCode::resource_limit);
        s.current_acks.push_back(id);
        emit_acks();
        // Only current ACKs rotate to prior. Replays are never reinserted.
        s.prior_acks=std::move(s.current_acks);
        s.current_acks.clear();
    }
    void replay_acks() {
        emit_acks();
        s.prior_acks.clear();
    }
    void tick() {
        for_channels([&](const auto& channel) {
            if (!channel.incoming.empty() && channel.view.incoming_blocked_since &&
                now-*channel.view.incoming_blocked_since>c.incoming_timeout)
                s.view.closed=true;
        });
        if (s.view.closed) return;
        replay_acks();
        auto& control=s.control;
        const auto pending=std::count_if(control.outgoing.begin(),control.outgoing.end(),
            [](const auto& e) { return !e.acked; });
        if (!control.view.open_confirmed && pending<=8) {
            for (auto& entry:control.outgoing) {
                const auto age=now-entry.sent_at;
                // Avoid out-of-range double->float conversion while retaining
                // the native strict float comparison near the1.0 boundary.
                if (!entry.acked && (age>static_cast<double>(std::numeric_limits<float>::max()) ||
                    static_cast<float>(age)>1.0F)) retry(entry);
            }
        }
    }
    void finish() {
        if (s.view.closed) {
            for_channels([&](auto& channel) {
                clear_channel(channel);
                channel.view.closed=true;
            });
            s.current_acks.clear(); s.prior_acks.clear();
        }
        s.view.outgoing_bunches=0;
        s.view.incoming_bunches=0;
        for_channels([&](auto& channel) {
            channel.view.outgoing_bunches=channel.outgoing.size();
            channel.view.incoming_bunches=channel.incoming.size();
            s.view.outgoing_bunches+=channel.outgoing.size();
            s.view.incoming_bunches+=channel.incoming.size();
        });
        s.view.outgoing_sequence=s.control.view.outgoing_sequence;
        s.view.incoming_sequence=s.control.view.incoming_sequence;
        s.view.opening_packet_id=s.control.view.opening_packet_id;
        s.view.open_confirmed=s.control.view.open_confirmed;
        s.view.current_ack_ids=s.current_acks.size();
        s.view.prior_ack_ids=s.prior_acks.size();
        s.view.last_time=now;
        result.closed=s.view.closed;
    }
private:
    const TransportConfig& c;
    State& s;
    double now;

    template<class F>
    void for_channels(F action) {
        action(s.control);
        for (auto& channel:s.actors) action(channel);
    }
    std::size_t queue_count(bool outgoing) const {
        auto count=outgoing ? s.control.outgoing.size() : s.control.incoming.size();
        for (const auto& channel:s.actors)
            count+=outgoing ? channel.outgoing.size() : channel.incoming.size();
        return count;
    }
    template<class Channel>
    void release_payload(Channel& channel,std::size_t bytes) {
        channel.view.queued_payload_bytes-=bytes;
        s.view.queued_payload_bytes-=bytes;
    }
    template<class Channel>
    void clear_channel(Channel& channel) {
        release_payload(channel,channel.view.queued_payload_bytes);
        channel.outgoing.clear(); channel.incoming.clear();
        channel.view.outgoing_bunches=0;
        channel.view.incoming_bunches=0;
        channel.view.incoming_blocked_since.reset();
    }
    template<class Channel>
    void retire(Channel& channel) {
        if (channel.view.closed) return;
        channel.view.closed=true;
        if (channel.view.channel==0) s.view.closed=true;
        else {
            result.closed_channels.push_back(channel.view.channel);
            clear_channel(channel);
        }
    }
    void require_payload(std::size_t bytes) const {
        if (bytes>c.limits.max_payload_bytes-s.view.queued_payload_bytes)
            fail(TransportErrorCode::resource_limit);
    }
    void require_output(std::size_t count) const {
        if (count>c.limits.max_packets_per_step-result.outgoing.size())
            fail(TransportErrorCode::resource_limit);
    }
    void emit(Packet&& p) {
        require_output(1);
        p.packet_id=static_cast<std::uint16_t>(s.view.next_packet_id&0x3fffU);
        result.outgoing.push_back(std::move(p));
        ++s.view.next_packet_id;
    }
    std::uint32_t emit_bunch(const Bunch& b) {
        require_output(1);
        validate_single_bunch(b,c);
        Packet p;
        p.records.push_back({false,0,b});
        const auto id=s.view.next_packet_id;
        emit(std::move(p));
        return id;
    }
    template<class Entry>
    void retry(Entry& entry) {
        const auto id=emit_bunch(entry.bunch);
        entry.packet_id=id;
        entry.sent_at=now;
    }
    template<class Channel>
    void deliver(Channel& channel,const Bunch& b,std::uint32_t sequence) {
        result.delivered.push_back(b);
        channel.view.incoming_sequence=sequence;
        if (b.close) retire(channel);
    }
    void emit_acks() {
        const auto count=s.prior_acks.size()+s.current_acks.size();
        if (count==0) return;
        //14-bit header+1 marker, then each ACK occupies15 bits.
        const auto per_packet=std::min(c.limits.max_records_per_packet,
            (static_cast<std::size_t>(c.max_packet_bytes)*8-15)/15);
        if (per_packet==0) fail(TransportErrorCode::invalid_packet);
        require_output(count/per_packet+(count%per_packet!=0 ? 1 : 0));
        Packet p;
        p.records.reserve(std::min(count,per_packet));
        const auto append=[&](std::uint32_t id) {
            PacketRecord r; r.is_ack=true;
            r.ack_packet_id=static_cast<std::uint16_t>(id&0x3fffU);
            p.records.push_back(std::move(r));
            if (p.records.size()==per_packet) {
                emit(std::move(p));
                p=Packet{};
            }
        };
        for (const auto id:s.prior_acks) append(id);
        for (const auto id:s.current_acks) append(id);
        if (!p.records.empty()) emit(std::move(p));
    }
};
}

TransportError::TransportError(TransportErrorCode code)
    : std::runtime_error("Reliable control transport operation could not be completed."), code_(code) {}
TransportErrorCode TransportError::code() const noexcept { return code_; }

std::uint32_t expand_packet_id(std::uint16_t raw, std::uint32_t reference) {
    if (raw>=0x4000) fail(TransportErrorCode::invalid_packet);
    return reference-0x2000U+((static_cast<std::uint32_t>(raw)-reference-0x2000U)&0x3fffU);
}
std::uint32_t expand_sequence(std::uint16_t raw, std::uint32_t reference) {
    if (raw>=0x400) fail(TransportErrorCode::invalid_packet);
    return reference-0x200U+((static_cast<std::uint32_t>(raw)-reference-0x200U)&0x3ffU);
}
ControlTransport::ControlTransport(const TransportConfig& config) : config_(config) {
    validate_config(config);
    state_.view.next_packet_id=config.next_packet_id;
    state_.view.last_incoming_packet_id=config.last_incoming_packet_id;
    state_.view.last_ack_packet_id=config.last_ack_packet_id;
    state_.view.outgoing_sequence=config.outgoing_sequence;
    state_.view.incoming_sequence=config.incoming_sequence;
    state_.view.open_confirmed=config.open_confirmed;
    state_.control.view.outgoing_sequence=config.outgoing_sequence;
    state_.control.view.incoming_sequence=config.incoming_sequence;
    state_.control.view.open_confirmed=config.open_confirmed;
}
Snapshot ControlTransport::snapshot() const noexcept { return state_.view; }
void ControlTransport::register_actor_channel(const ActorChannelConfig& config) {
    if (state_.view.closed) fail(TransportErrorCode::terminal_state);
    if (config.channel==0 || config.channel>=0x800)
        fail(TransportErrorCode::invalid_configuration);
    for (const auto& channel:state_.actors)
        if (channel.view.channel==config.channel) fail(TransportErrorCode::invalid_configuration);
    if (state_.actors.size()>=64) fail(TransportErrorCode::resource_limit);
    State staged=state_;
    ChannelState channel;
    channel.view.channel=config.channel;
    channel.view.outgoing_sequence=config.outgoing_sequence;
    channel.view.incoming_sequence=config.incoming_sequence;
    channel.view.open_confirmed=config.open_confirmed;
    staged.actors.push_back(std::move(channel));
    static_assert(std::is_nothrow_move_assignable_v<State>);
    state_=std::move(staged);
}
std::optional<ChannelSnapshot> ControlTransport::channel_snapshot(std::uint16_t channel) const {
    if (channel==0) return state_.control.view;
    for (const auto& actor:state_.actors)
        if (actor.view.channel==channel) return actor.view;
    return std::nullopt;
}
TransportStep ControlTransport::send(const Bunch& bunch, double now) {
    validate_time(state_.view,now);
    if (bunch.sequence!=0) fail(TransportErrorCode::invalid_packet);
    validate_single_bunch(bunch,config_);
    const auto& channel=find_channel(state_,bunch.channel);
    if (channel.view.closed || (bunch.channel!=0 &&
        std::any_of(channel.outgoing.begin(),channel.outgoing.end(),
            [](const auto& entry) { return entry.bunch.close; })))
        fail(TransportErrorCode::terminal_state);
    State staged=state_;
    Operation op(config_,staged,now);
    op.send_new(bunch);
    op.finish();
    static_assert(std::is_nothrow_move_assignable_v<State>);
    state_=std::move(staged);
    return std::move(op.result);
}
TransportStep ControlTransport::receive(const Packet& packet, double now) {
    validate_time(state_.view,now);
    validate_packet(packet,config_);
    for (const auto& record:packet.records)
        if (!record.is_ack) (void)find_channel(state_,record.bunch.channel);
    State staged=state_;
    Operation op(config_,staged,now);
    const auto id=expand_packet_id(packet.packet_id,staged.view.last_incoming_packet_id);
    if (later(id,staged.view.last_incoming_packet_id)) staged.view.last_incoming_packet_id=id;
    for (const auto& record:packet.records) {
        if (record.is_ack) op.process_ack(record.ack_packet_id);
        else op.process_bunch(record.bunch,id);
        if (staged.view.closed) break;
    }
    op.acknowledge_packet(id);
    op.finish();
    static_assert(std::is_nothrow_move_assignable_v<State>);
    state_=std::move(staged);
    return std::move(op.result);
}
TransportStep ControlTransport::tick(double now) {
    validate_time(state_.view,now);
    State staged=state_;
    Operation op(config_,staged,now);
    op.tick();
    op.finish();
    static_assert(std::is_nothrow_move_assignable_v<State>);
    state_=std::move(staged);
    return std::move(op.result);
}
}
