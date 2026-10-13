#pragma once
#include "hawken/control_handshake.hpp"
#include "hawken/object_reference_wire.hpp"
#include "hawken/package_publication.hpp"
#include "hawken/reliable_control_transport.hpp"
#include "hawken/server_login_handshake.hpp"
#include "hawken/session_wire.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace hawken::game {
struct ReplyFlags {
    bool open;
    bool close;
};
struct ConnectionLimits {
    // Own bounded work/output policies, not native defaults/windows/MTU.
    // Datagram cap<=65507 and >=transport.max_packet_bytes+48. Message and
    // output-datagram counts1..1024; output bytes1..1048576 (not total heap).
    std::size_t max_datagram_bytes;
    std::size_t max_logical_messages_per_step;
    std::size_t max_datagrams_per_step;
    std::size_t max_output_bytes_per_step;
};
struct LoginConfig {
    // Required post-Hello stage; explicit caller policy, no opt-in/default RNG.
    // Uses ConnectionConfig.endian. Token is independent of challenge1b and
    // session/Steam material. Positive codec limits and driver_max_speed>=1800.
    control::posthello::Limits limits;
    std::uint32_t challenge_token;
    std::int32_t driver_max_speed;
};
struct ConnectionConfig {
    // Caller provisions these through an authorized service. No Steam ticket
    // derivation, entropy/TTL generation or session distribution in this API.
    // Caller owns/scrubs its copy; this connection owns one persistent Context.
    session::SessionId session_id;
    session::SessionKey session_key;
    wire::reliable::TransportConfig transport;
    control::Endian endian;
    std::uint32_t challenge;
    std::int32_t server_min;
    std::int32_t server_max;
    ReplyFlags reply_flags; // Applied to each reply; no inferred local-open gate.
    ConnectionLimits limits;
    LoginConfig login;
};
enum class ConnectionErrorCode {
    invalid_configuration, invalid_time, invalid_datagram, authentication_failed,
    replay, invalid_packet, invalid_archive, unexpected_message, resource_limit,
    terminal_state, crypto_failure, not_implemented
};
class ConnectionError : public std::runtime_error {
public:
    explicit ConnectionError(ConnectionErrorCode code);
    ConnectionErrorCode code() const noexcept;
private:
    ConnectionErrorCode code_;
};
struct WorldPublication {
    std::vector<control::packages::RootMetadata> packages;
    control::posthello::Welcome welcome;
    // Explicit local-only contained descriptors, sent after roots and before
    // Welcome. Caller must establish the package/container association.
    std::vector<control::packages::ContainedMetadata> contained_packages = {};
};
enum class PublicationState { pending, awaiting_package_acks, awaiting_join, join_requested };
struct ConnectionSnapshot {
    control::HandshakeState handshake;
    bool closed;
    control::login::Snapshot login;
    PublicationState publication = PublicationState::pending;
    std::size_t acknowledged_packages = 0;
    bool operator==(const ConnectionSnapshot&) const = default;
};
struct ConnectionStep {
    // Complete newly authenticated/encrypted UDP payloads, with the48-byte
    // wrapper. No IP/UDP header, socket, endpoint, key/grant or admission result.
    std::vector<std::vector<std::uint8_t>> datagrams;
    std::optional<control::NegotiatedHello> hello;
    bool closed = false;
    // Raw caller-owned fields only; no Guid/Steam/URL authorization. Never
    // retained by this connection. Empty on ACK/tick, duplicate or any failure.
    std::optional<control::posthello::Login> login_candidate;
    // One-shot protocol event, not admission, actor creation or a hosted world.
    bool join_requested = false;
    // Opaque actor payloads; exact logical bits, no opcode/schema or
    // gameplay interpretation. Closing connection steps clear both vectors.
    std::vector<wire::Bunch> actor_bunches;
    std::vector<std::uint16_t> closed_actor_channels;
};

// Pure per-connection server composition. No sockets/SQL/Steam/HTTP/world.
// Noncopyable/nonmovable: no cloning/reset/reinstantiation to reuse nonces.
class GameConnection {
public:
    explicit GameConnection(const ConnectionConfig& config);
    ~GameConnection();
    GameConnection(const GameConnection&) = delete;
    GameConnection& operator=(const GameConnection&) = delete;
    GameConnection(GameConnection&&) = delete;
    GameConnection& operator=(GameConnection&&) = delete;
    ConnectionSnapshot snapshot() const noexcept;

    // Validates finite>=0 monotonic caller time and size BEFORE Context.decode.
    // Size/time/authentication/replay rejections preserve handshake/clock and
    // do not close. Authenticated nonce is consumed BEFORE inner parsing: no
    // rollback/reset promise for Context or ControlTransport. Any subsequent
    // packet/archive/order/quota/allocation/provider failure closes this owner
    // permanently, throws a fixed error, and returns NO partial datagrams.
    // Provider/allocation failures also close when their stage is uncertain.
    ConnectionStep receive(std::span<const std::uint8_t> datagram, double now);
    // Same supplied clock; incoming timeout, prior ACK replay, retry retain the
    // existing transport policies. Fatal composition/output failure closes.
    ConnectionStep tick(double now);

    // Own denial continuation, only while open and login_received. Existing
    // monotonic-time checks and invalid-state rejection precede the attempt
    // and preserve state. Emits constant narrow "Login rejected."+NUL as
    // Failure6 through the SAME transport/Context, then closed=true. No peer
    // text, authority decision, Welcome or tick/retry is generated here.
    // snapshot.login remains historical login_received; Connection.closed is
    // terminal. Caller sends the complete returned flush before retiring it;
    // no delivery/retransmission guarantee after retirement. Once encoding/
    // sending begins, any failure closes and throws without partial outputs.
    ConnectionStep reject_login(double now);

    // Explicit caller-owned publication, only while open/login_received and
    // pending. Before sending: validate clock, nonempty package list bounded
    // by max_logical_messages_per_step-1 across roots and contained metadata,
    // unique ASCII folded
    // names and identities, and complete Welcome encoding. Rejected preflight
    // preserves state, clock, transport and Context send nonces. Empty/invalid
    // metadata or duplicates -> invalid_archive; exceeded quota -> resource_limit.
    // Send root Package7 in supplied order, then contained Package7 in supplied
    // order, then Welcome1 through the SAME transport
    // and Context. Once sending begins, any failure closes with no partial
    // result. Retain each identity, generation COUNT and precomputed object
    // range in roots-then-contained order; validate signed generation counts
    // and cumulative31-bit coordinate bounds before sending or committing time.
    // Own initial policy requires ACK8 generation exactly equal to the count;
    // unknown/mismatched ACK is terminal, equal duplicate ACK is idempotent.
    // Join9 only after every package ACK: one event/state transition. Early or
    // repeated Join, unexpected archives and malformed suffix are terminal;
    // no partial join event escapes. Any closing step clears its transient
    // join_requested event, even after Join; snapshot may retain history.
    // Failure6 closes without echo. Reliable
    // duplicates/ACK/tick never repeat events. reject_login rejects after this
    // stage. Caller must authorize publication and supply coherent assets;
    // this pure composition neither loads a world nor creates any actor.
    ConnectionStep publish_world(const WorldPublication& publication, double now);

    // Pure PRIMARY network-map coordinate, only after Join and while open.
    // Uses owned publication ranges with the existing exact generation-COUNT
    // ACK policy; caller metadata mutation cannot change them. Static0 is
    // valid, unlike actor-channel0/null. No actor transmission/resolution.
    // Closed -> terminal_state; pre-Join -> unexpected_message; unknown GUID,
    // negative local index or local>=negotiated count -> invalid_archive.
    // Errors preserve all state/time/nonces and contain no identity fields.
    wire::object::PackageExportReference resolve_package_reference(
        const std::array<std::uint32_t,4>& identity, std::int32_t local_net_index) const;

    // Pure registration, only open and after Join; no clock/nonce mutation.
    // Seeds/flags are explicit caller policy, never inferred native defaults.
    // Delegate bounded registration/tombstones to the SAME transport. Invalid
    // index/duplicate -> invalid_configuration, cap/allocation -> resource_limit;
    // pre-Join -> unexpected_message, closed connection -> terminal_state.
    // Rejections preserve all connection/transport state. No actor admission.
    void register_actor_channel(const wire::reliable::ActorChannelConfig& config);

    // Only open/after Join: reliable/type2 or plain unreliable/type0 actor.
    // Caller supplies sequence0. Unreliable needs a known opening PacketID,
    // omits sequence/type on wire, and cannot open/close or enter reliable queues.
    // Flags and exact payload_bits are supplied by caller, not ReplyFlags.
    // Time/state/shape/unknown-channel/quota/queued-close rejection preserves
    // state, clock and nonces via transactional transport.send. Closed actor
    // rejection -> terminal_state without closing the connection. Invalid
    // shape/unknown -> invalid_packet. After successful transport.send commits,
    // any output/encoding/provider failure is connection-terminal, no partial
    // result. Uses SAME Context/PacketIDs/ACKs; no object resolution/spawn.
    ConnectionStep send_actor(const wire::Bunch& bunch, double now);

    // Actor delivery is dispatched by registered channel before control framing;
    // exact nonbyte actor payloads are opaque events, one per shared message
    // quota unit. Reliable duplicates never repeat events; unreliable retains
    // native stale-PacketID delivery without changing reliable sequence. Actor close is a
    // channel event only; global/control close clears transient actor events.
    // No automatic actor registration, object interpretation or admission.
    // Prefix framing within each delivered CONTROL bunch:1a/1b/1c5bytes,1d1byte,
    // Hello0=13bytes,Upgrade2=9bytes. Control payload has whole logical bytes.
    // Every prefix in that bunch is consumed in order, across ordered bunches
    // in the same step; never carry a partial archive to another bunch.
    // After validated Hello, always emit Challenge3 via the same reliable
    // transport/Context, then frame post-Hello messages with decode_prefix and
    // pass each consumed exact archive to ServerLoginHandshake. Whole archives
    // within a bunch only: no cross-bunch partial strings or fragmentation.
    // Hello+NetSpeed4+Login5 may be coalesced. One shared incoming-message quota
    // covers both stages. A later malformed suffix/duplicate Login aborts the
    // entire step: no partial datagrams or candidate escape.
    // CRC failure closes without Ready; incompatible Hello emits Upgrade and
    // closes. A normal close stops further logical messages. Transport close
    // ends processing without generating replies from its delivered payload.
    // NetSpeed4 may repeat before Login5; Login5 need not follow NetSpeed.
    // Failure6 closes without echoing the peer's text. After login_received,
    // ACK/tick remain possible; pending publication rejects new archives. Once
    // explicitly published, only the PackageAck/Join/Failure flow above is
    // accepted; reliable duplicates remain suppressed and request ACK.
    // hello_validated/login_received are NOT admission, Welcome or gameplay.
    // Replies are canonical reliable channel0/type1; control=open||close.
    // Transport-generated ACK/retry packets precede logical reply packets in
    // this immediate-flush composition (own policy, not native callback order).
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
