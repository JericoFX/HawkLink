#pragma once
#include "hawken/game_wire.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace hawken::wire::reliable {
enum class TransportErrorCode {
    invalid_configuration, invalid_time, invalid_packet, unsupported_bunch,
    resource_limit, terminal_state, not_implemented
};
class TransportError : public std::runtime_error {
public:
    explicit TransportError(TransportErrorCode code);
    TransportErrorCode code() const noexcept;
private:
    TransportErrorCode code_;
};

// Our memory/work policies, not wire windows. Every limit is positive.
// Bunch/ACK/record/packet counts <=1024; combined queued payload <=1048576
// bytes. Payload accounting covers vector bytes, not allocator/total heap.
struct QueueLimits {
    std::size_t max_outgoing_bunches;
    std::size_t max_incoming_bunches;
    std::size_t max_ack_ids;
    std::size_t max_payload_bytes;
    std::size_t max_records_per_packet;
    std::size_t max_packets_per_step;
};
struct TransportConfig {
    // Full u32 bit patterns. No inferred CDO/default1, counter reset or epoch.
    std::uint32_t next_packet_id;
    std::uint32_t last_incoming_packet_id;
    std::uint32_t last_ack_packet_id;
    std::uint32_t outgoing_sequence;
    std::uint32_t incoming_sequence;
    bool open_confirmed;
    // Interior packet bytes INCLUDING marker/padding,2..65507 (our policy).
    // It excludes the48-byte session wrapper and is not a UDP MTU guarantee.
    std::uint32_t max_packet_bytes;
    QueueLimits limits;
    double incoming_timeout; // Caller-supplied positive finite threshold.
};
struct ActorChannelConfig {
    std::uint16_t channel;
    std::uint32_t outgoing_sequence;
    std::uint32_t incoming_sequence;
    bool open_confirmed;
};
struct ChannelSnapshot {
    std::uint16_t channel = 0;
    std::uint32_t outgoing_sequence = 0;
    std::uint32_t incoming_sequence = 0;
    std::optional<std::uint32_t> opening_packet_id;
    bool open_confirmed = false;
    bool closed = false;
    std::size_t outgoing_bunches = 0;
    std::size_t incoming_bunches = 0;
    std::size_t queued_payload_bytes = 0;
    std::optional<double> incoming_blocked_since;
    bool operator==(const ChannelSnapshot&) const = default;
};

struct Snapshot {
    std::uint32_t next_packet_id = 0;
    std::uint32_t last_incoming_packet_id = 0;
    std::uint32_t last_ack_packet_id = 0;
    std::uint32_t outgoing_sequence = 0;
    std::uint32_t incoming_sequence = 0;
    std::optional<std::uint32_t> opening_packet_id;
    bool open_confirmed = false;
    bool closed = false;
    std::size_t outgoing_bunches = 0;
    std::size_t incoming_bunches = 0;
    std::size_t current_ack_ids = 0;
    std::size_t prior_ack_ids = 0;
    std::size_t queued_payload_bytes = 0;
    std::optional<double> last_time;
    bool operator==(const Snapshot&) const = default;
};
struct TransportStep {
    // Newly packetized interiors. Caller encodes, session-wraps and transmits
    // them independently; no stored ciphertext or transport callback exists.
    std::vector<Packet> outgoing;
    // Complete bunches in reliable order; sequence is the raw10-bit value.
    // No logical opcode interpretation, authentication, admission or gameplay.
    std::vector<Bunch> delivered;
    bool closed = false;
    // Actor-channel close events, once per registered channel; not connection
    // admission or actor destruction. Control close still sets closed above.
    std::vector<std::uint16_t> closed_channels;
};

// Recovered nearest modular representative; ties at half-turn go BACKWARD.
// Return is a full u32 bit pattern. Comparisons use bit_cast<int32_t>; this
// reproduces signed32 rollover limits, not an unbounded logical epoch.
// raw>=0x4000/0x400 throws fixed invalid_packet, with no state to mutate.
std::uint32_t expand_packet_id(std::uint16_t raw, std::uint32_t reference);
std::uint32_t expand_sequence(std::uint16_t raw, std::uint32_t reference);

// One fresh channel0/type1 context with EMPTY queues and explicit seeds;
// optional explicitly registered reliable actor channels carry type2.
// Models the ACK-enabled native branch(Conn+188==0); other ACK modes pending.
// Exclusive owner; not a pending-connection restoration or session provider.
class ControlTransport {
public:
    explicit ControlTransport(const TransportConfig& config);
    ControlTransport(const ControlTransport&) = delete;
    ControlTransport& operator=(const ControlTransport&) = delete;
    Snapshot snapshot() const noexcept;

    // Own bounded registration policy: indices1..2047, at most64 total
    // registrations INCLUDING closed tombstones; never reuse/reset an index.
    // Duplicate/invalid index -> invalid_configuration; cap -> resource_limit;
    // closed connection -> terminal_state. No packet, clock or control seed
    // mutation. Allocation/rejection preserves state. Seeds are explicit u32.
    void register_actor_channel(const ActorChannelConfig& config);
    // Channel0 exposes control state; unknown indices return nullopt. A pure
    // query, with no clock/PacketID/sequence mutation or implied actor lookup.
    std::optional<ChannelSnapshot> channel_snapshot(std::uint16_t channel) const;

    // send assigns sequence; caller must supply sequence0 and canonical
    // reliable control0/type1 or registered actor/type2 Bunch
    // (flagsPresent==open||close, ACK absent). Closed actor or queued outgoing
    // actor close (even ACKed behind a pending prefix) -> terminal_state.
    // Opening flags are explicit; this API does not infer the native+70 gate.
    TransportStep send(const Bunch& bunch, double now);
    // Packet has ALREADY been decoded/authenticated by caller. Validates all
    // records/encoded size first; a stale PacketID still processes records.
    // Unknown channels/wrong types or unreliable bunches are outside this API, not
    // proof that the original client never uses them. Full reliable seq is
    // expanded; stale/duplicate reliable never deliver, future seq queues,
    // and only consecutive heads drain. Incoming open confirms open.
    TransportStep receive(const Packet& packet, double now);
    // Our combined ordering: blocked incoming timeout FIRST, then ACK replay,
    // then native ControlChannel preopen retry(float(age)>1.0, pending<=8).
    // No universal retry after open, hardcoded Driver timeout or keepalive.
    TransportStep tick(double now);

    // Common contract (own policies except the recovered rules cited):
    // - now finite,>=0,nondecreasing; every rejection/allocation failure leaves
    //   state unchanged and returns no partial result. Errors never echo data.
    // - immediate flush, no merge/fragmentation; one bunch per packet; ACKs
    //   pack in order up to configured record/byte limits. Seeds advance per
    //   emitted packet. Bunch retransmits keep seq/payload, but associate with
    //   the NEW emitted packet ID. Old ACK cannot mark that entry; it may
    //   still confirm open by the separate opening_packet_id (native+74).
    // - exact ACK marks entries, gaps trigger LOCAL NAK resends, and only an
    //   ACKed queue prefix prunes. Control close becomes terminal on prune.
    // - prior ACK replay uses writer param3=0, NEVER reinserted in current;
    //   receive replays prior before current ACK, flush rotates current to
    //   prior. Native reserve32 is not an ACK/window limit. Input ACK-only
    //   packets still request ACK, as the selected native reader does.
    // - incoming control close delivers its payload, stops drain/remaining records,
    //   builds this packet's ACK and then clears queues at terminal commit.
    //   Timeout also clears queues. Later operations throw terminal_state.
    //   This strict terminal/transaction/packetization policy is ours; it is
    //   not admission nor a claim about native global tick/destructor order.
    // - all records preflight even if close would skip a later record. The
    //   existing packet codec's canonical limits remain our input policy.
    // - PacketIDs, ACKs, clock and quotas are connection-wide; legacy Snapshot
    //   sequences/open refer to control0, queue/byte counts are aggregate.
    //   Actor sequence/open/ordered queues/blocked time are per channel. Exact
    //   ACK/NAK spans all channels but prunes each channel's own ACKed prefix.
    // - actor close delivers then stops that channel's drain, clears only its
    //   queues, and emits its one-shot closed_channels entry. Other records/
    //   channels continue. Canonically valid incoming closed-channel records
    //   are ignored and still request the packet ACK; never reopen. An ACKed
    //   outgoing actor close closes only that channel. Control close remains
    //   connection terminal and clears all queues. No index reuse.
    // - any channel's blocked incoming timeout is our connection-terminal
    //   policy, checked before retry; preopen age retry is CONTROL ONLY.
    //   No unreliable/fragmentation or replication field bookkeeping claim.
private:
    struct PendingOutgoing {
        Bunch bunch;
        std::uint32_t sequence;
        std::uint32_t packet_id;
        double sent_at;
        bool acked;
    };
    struct PendingIncoming {
        Bunch bunch;
        std::uint32_t sequence;
    };
    struct ChannelState {
        ChannelSnapshot view;
        std::vector<PendingOutgoing> outgoing;
        std::vector<PendingIncoming> incoming;
    };
    struct State {
        Snapshot view;
        ChannelState control;
        std::vector<ChannelState> actors;
        std::vector<std::uint32_t> current_acks;
        std::vector<std::uint32_t> prior_acks;
    };
    TransportConfig config_;
    State state_;
};
}
