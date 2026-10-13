#include "hawken/game_connection.hpp"
#include "hawken/package_map_ranges.hpp"
#include "hawken/packet_writer.hpp"
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace hawken::game {
ConnectionError::ConnectionError(ConnectionErrorCode code)
    : std::runtime_error("Game connection operation could not be completed."), code_(code) {}
ConnectionErrorCode ConnectionError::code() const noexcept { return code_; }

namespace {
[[noreturn]] void fail(ConnectionErrorCode code) { throw ConnectionError(code); }
void validate_config(const ConnectionConfig& config) {
    const auto& limits=config.limits;
    const auto count_ok=[](std::size_t n) { return n>0 && n<=1024; };
    // Own aggregate/wrapper policies. Lower constructors separately validate
    // the caller's explicit transport seeds, limits and handshake interval.
    if (config.transport.max_packet_bytes>session::max_payload_bytes ||
        limits.max_datagram_bytes>session::max_packet_bytes ||
        limits.max_datagram_bytes<static_cast<std::size_t>(config.transport.max_packet_bytes)+session::wrapper_bytes ||
        !count_ok(limits.max_logical_messages_per_step) ||
        !count_ok(limits.max_datagrams_per_step) || limits.max_output_bytes_per_step==0 ||
        limits.max_output_bytes_per_step>1048576 ||
        config.login.limits.max_archive_bytes==0 || config.login.limits.max_string_units==0 ||
        config.login.limits.max_string_units>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        config.login.driver_max_speed<1800)
        fail(ConnectionErrorCode::invalid_configuration);
}
std::size_t archive_size(std::uint8_t opcode) {
    switch (opcode) {
    case 0x1a: case 0x1b: case 0x1c: return 5;
    case 0x1d: return 1;
    case 0: return 13;
    case 2: return 9;
    default: fail(ConnectionErrorCode::invalid_archive);
    }
}
ConnectionErrorCode transport_error(wire::reliable::TransportErrorCode code) noexcept {
    using wire::reliable::TransportErrorCode;
    return code==TransportErrorCode::resource_limit ? ConnectionErrorCode::resource_limit
        : ConnectionErrorCode::invalid_packet;
}
ConnectionErrorCode login_error(control::login::ErrorCode code) noexcept {
    using control::login::ErrorCode;
    switch (code) {
    case ErrorCode::resource_limit: return ConnectionErrorCode::resource_limit;
    case ErrorCode::unexpected_message: return ConnectionErrorCode::unexpected_message;
    case ErrorCode::terminal_state: return ConnectionErrorCode::terminal_state;
    default: return ConnectionErrorCode::invalid_archive;
    }
}
bool same_folded_name(std::string_view left,std::string_view right) noexcept {
    if (left.size()!=right.size()) return false;
    const auto fold=[](char value) { return value>='A' && value<='Z' ? value+('a'-'A') : value; };
    for (std::size_t i=0;i<left.size();++i)
        if (fold(left[i])!=fold(right[i])) return false;
    return true;
}
ConnectionErrorCode posthello_error(control::posthello::ErrorCode code) noexcept {
    return code==control::posthello::ErrorCode::resource_limit
        ? ConnectionErrorCode::resource_limit : ConnectionErrorCode::invalid_archive;
}
}

struct GameConnection::Impl {
    // No retained ConnectionConfig/key copy. Context owns and erases its key;
    // neither it nor the reliable queues can be cloned/reset for rollback.
    session::Context context;
    wire::reliable::ControlTransport transport;
    control::ServerHandshake handshake;
    control::login::ServerLoginHandshake login;
    control::Endian endian;
    control::posthello::Limits login_limits;
    ConnectionLimits limits;
    ReplyFlags reply_flags;
    std::uint32_t max_packet_bytes;
    std::optional<double> last_time;
    bool closed=false;
    struct PublishedPackage {
        std::array<std::uint32_t,4> identity;
        std::uint32_t generation;
        bool acknowledged=false;
    };
    std::vector<PublishedPackage> published_packages;
    std::vector<wire::packages::ObjectRange> published_ranges;
    PublicationState publication=PublicationState::pending;
    std::size_t acknowledged_packages=0;

    explicit Impl(const ConnectionConfig& config)
        : context(session::Role::server,config.session_id,config.session_key),
          transport(config.transport),
          handshake(config.endian,config.challenge,config.server_min,config.server_max),
          login({config.endian,config.login.limits,config.login.challenge_token,config.login.driver_max_speed}),
          endian(config.endian), login_limits(config.login.limits),
          limits(config.limits), reply_flags(config.reply_flags),
          max_packet_bytes(config.transport.max_packet_bytes) {}

    void check_time(double now) const {
        if (closed) fail(ConnectionErrorCode::terminal_state);
        if (!std::isfinite(now) || now<0 || (last_time && now<*last_time))
            fail(ConnectionErrorCode::invalid_time);
    }
    void append_packets(ConnectionStep& result, const std::vector<wire::Packet>& packets,
                        std::size_t& output_bytes) {
        for (const auto& packet:packets) {
            if (result.datagrams.size()>=limits.max_datagrams_per_step)
                fail(ConnectionErrorCode::resource_limit);
            const auto interior=wire::encode_packet(packet,max_packet_bytes);
            // Encoder already bounds interior allocation; test all wrapper
            // budgets BEFORE Context reserves a send nonce or copies payload.
            const auto size=interior.size()+session::wrapper_bytes;
            if (size>limits.max_datagram_bytes ||
                size>limits.max_output_bytes_per_step-output_bytes)
                fail(ConnectionErrorCode::resource_limit);
            auto datagram=context.encode(interior);
            result.datagrams.push_back(std::move(datagram));
            output_bytes+=size;
        }
    }
    void send_reply(std::vector<std::uint8_t> archive,ConnectionStep& result,double now,
                    std::size_t& output_bytes) {
        if (archive.empty()) return;
        wire::Bunch reply;
        reply.open=reply_flags.open;
        reply.close=reply_flags.close;
        reply.control=reply.open || reply.close;
        reply.reliable=true;
        reply.channel_type=1;
        reply.payload_bits=archive.size()*8;
        reply.payload=std::move(archive);
        auto sent=transport.send(reply,now);
        append_packets(result,sent.outgoing,output_bytes);
        if (sent.closed) closed=true;
    }
    std::size_t posthello_size(std::span<const std::uint8_t> payload) const {
        // Bounded prefix decode discovers the physical length; its temporary
        // raw fields are destroyed before the exact state-machine decode.
        // No second parser or cross-bunch archive buffering is introduced.
        return control::posthello::decode_prefix(payload,endian,login_limits).consumed;
    }
    void receive_publication(std::span<const std::uint8_t> archive,ConnectionStep& result) {
        const auto message=control::posthello::decode_exact(archive,endian,login_limits);
        if (const auto* ack=std::get_if<control::posthello::PackageAck>(&message)) {
            for (auto& package:published_packages) {
                if (package.identity!=ack->identity) continue;
                if (package.generation!=ack->generation)
                    fail(ConnectionErrorCode::unexpected_message);
                if (!package.acknowledged) {
                    package.acknowledged=true;
                    ++acknowledged_packages;
                    if (acknowledged_packages==published_packages.size())
                        publication=PublicationState::awaiting_join;
                }
                return;
            }
            fail(ConnectionErrorCode::unexpected_message);
        }
        if (std::holds_alternative<control::posthello::Join>(message)) {
            if (publication!=PublicationState::awaiting_join)
                fail(ConnectionErrorCode::unexpected_message);
            publication=PublicationState::join_requested;
            result.join_requested=true;
            return;
        }
        if (std::holds_alternative<control::posthello::Failure>(message)) {
            closed=true;
            return;
        }
        fail(ConnectionErrorCode::unexpected_message);
    }
    ConnectionStep compose(wire::reliable::TransportStep step, double now) {
        ConnectionStep result;
        result.closed_actor_channels=std::move(step.closed_channels);
        std::size_t output_bytes=0;
        append_packets(result,step.outgoing,output_bytes);
        if (step.closed) {
            // Retain the transport's final ACK, but do not reply to any payload
            // delivered by its closing step. This is composition policy.
            closed=true;
        } else {
            std::size_t messages=0;
            for (const auto& bunch:step.delivered) {
                if (bunch.channel!=0) {
                    // Transport already validated a registered actor channel.
                    // Actor logical bits are opaque, not control opcodes.
                    if (messages>=limits.max_logical_messages_per_step)
                        fail(ConnectionErrorCode::resource_limit);
                    result.actor_bunches.push_back(bunch);
                    ++messages;
                    continue;
                }
                if (bunch.payload_bits%8!=0)
                    fail(ConnectionErrorCode::invalid_archive);
                const std::span<const std::uint8_t> payload(bunch.payload);
                std::size_t offset=0;
                while (offset<payload.size()) {
                    if (messages>=limits.max_logical_messages_per_step)
                        fail(ConnectionErrorCode::resource_limit);
                    const bool posthello=handshake.state()==control::HandshakeState::hello_validated;
                    const auto length=posthello ? posthello_size(payload.subspan(offset))
                        : archive_size(payload[offset]);
                    if (length==0 || length>payload.size()-offset)
                        fail(ConnectionErrorCode::invalid_archive);
                    // Only this prefix is passed to the exact archive codec.
                    // A partial archive never carries into another bunch.
                    if (posthello) {
                        if (publication!=PublicationState::pending) {
                            receive_publication(payload.subspan(offset,length),result);
                        } else {
                            auto logical=login.receive(payload.subspan(offset,length));
                            send_reply(std::move(logical.reply),result,now,output_bytes);
                            if (logical.candidate) result.login_candidate=std::move(logical.candidate);
                            if (logical.closed) closed=true;
                        }
                    } else {
                        auto logical=handshake.receive(payload.subspan(offset,length));
                        send_reply(std::move(logical.reply),result,now,output_bytes);
                        if (logical.hello) {
                            result.hello=logical.hello;
                            // Required next stage, with the SAME reliable
                            // sequence/ACK queues, session and send nonces.
                            auto next=login.begin(*logical.hello);
                            send_reply(std::move(next.reply),result,now,output_bytes);
                        }
                        if (logical.close) closed=true;
                    }
                    ++messages;
                    offset+=length;
                    if (closed) break;
                }
                if (closed) break;
            }
        }
        last_time=now;
        result.closed=closed;
        if (closed) {
            result.join_requested=false;
            result.actor_bunches.clear();
            result.closed_actor_channels.clear();
        }
        return result;
    }
};

GameConnection::GameConnection(const ConnectionConfig& config) {
    validate_config(config);
    try {
        impl_=std::make_unique<Impl>(config);
    } catch (const wire::reliable::TransportError&) {
        fail(ConnectionErrorCode::invalid_configuration);
    } catch (const control::HandshakeError&) {
        fail(ConnectionErrorCode::invalid_configuration);
    } catch (const control::login::Error&) {
        fail(ConnectionErrorCode::invalid_configuration);
    } catch (const std::bad_alloc&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        fail(ConnectionErrorCode::crypto_failure);
    }
}
GameConnection::~GameConnection() = default;
ConnectionSnapshot GameConnection::snapshot() const noexcept {
    return {impl_->closed ? control::HandshakeState::closed : impl_->handshake.state(),impl_->closed,
        impl_->login.snapshot(),impl_->publication,impl_->acknowledged_packages};
}
ConnectionStep GameConnection::receive(std::span<const std::uint8_t> datagram, double now) {
    impl_->check_time(now);
    if (datagram.size()<=session::wrapper_bytes || datagram.size()>impl_->limits.max_datagram_bytes)
        fail(ConnectionErrorCode::invalid_datagram);

    std::vector<std::uint8_t> interior;
    try {
        interior=impl_->context.decode(datagram);
    } catch (const session::SessionError& error) {
        using session::SessionErrorCode;
        switch (error.code()) {
        case SessionErrorCode::invalid_packet_size: fail(ConnectionErrorCode::invalid_datagram);
        case SessionErrorCode::session_mismatch:
        case SessionErrorCode::wrong_direction:
        case SessionErrorCode::authentication_failed: fail(ConnectionErrorCode::authentication_failed);
        case SessionErrorCode::replay: fail(ConnectionErrorCode::replay);
        default:
            impl_->closed=true;
            fail(ConnectionErrorCode::crypto_failure);
        }
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }

    // Decode consumed a valid receive nonce. Every later failure is terminal;
    // no partial result escapes and no lower-layer rollback is attempted.
    try {
        if (interior.size()>impl_->max_packet_bytes)
            fail(ConnectionErrorCode::invalid_packet);
        auto packet=wire::decode_packet(interior,impl_->max_packet_bytes);
        return impl_->compose(impl_->transport.receive(packet,now),now);
    } catch (const ConnectionError&) {
        impl_->closed=true;
        throw;
    } catch (const wire::reliable::TransportError& error) {
        impl_->closed=true;
        fail(transport_error(error.code()));
    } catch (const wire::WireError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_packet);
    } catch (const control::ControlError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_archive);
    } catch (const control::HandshakeError& error) {
        impl_->closed=true;
        fail(error.code()==control::HandshakeErrorCode::unexpected_message
            ? ConnectionErrorCode::unexpected_message : ConnectionErrorCode::invalid_archive);
    } catch (const control::posthello::Error& error) {
        impl_->closed=true;
        fail(posthello_error(error.code()));
    } catch (const control::login::Error& error) {
        impl_->closed=true;
        fail(login_error(error.code()));
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }
}
ConnectionStep GameConnection::tick(double now) {
    impl_->check_time(now);
    try {
        return impl_->compose(impl_->transport.tick(now),now);
    } catch (const ConnectionError&) {
        impl_->closed=true;
        throw;
    } catch (const wire::reliable::TransportError& error) {
        impl_->closed=true;
        fail(transport_error(error.code()));
    } catch (const wire::WireError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_packet);
    } catch (const control::posthello::Error& error) {
        impl_->closed=true;
        fail(posthello_error(error.code()));
    } catch (const control::login::Error& error) {
        impl_->closed=true;
        fail(login_error(error.code()));
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }
}
ConnectionStep GameConnection::reject_login(double now) {
    impl_->check_time(now);
    if (impl_->login.snapshot().state!=control::login::State::login_received ||
        impl_->publication!=PublicationState::pending)
        fail(ConnectionErrorCode::unexpected_message);
    try {
        ConnectionStep result;
        std::size_t output_bytes=0;
        // Operator denial text is constant, never taken from the candidate.
        // Include the transmitted NUL as a physical narrow FString unit.
        const control::posthello::Failure failure{{control::posthello::StringForm::narrow,
            std::u16string(u"Login rejected.",16)}};
        auto archive=control::posthello::encode(failure,impl_->endian,impl_->login_limits);
        impl_->send_reply(std::move(archive),result,now,output_bytes);
        // No tick/retry or lower-layer rollback. The caller sends this complete
        // flush before retirement. Keep login snapshot as historical state.
        impl_->last_time=now;
        impl_->closed=true;
        result.closed=true;
        return result;
    } catch (const ConnectionError&) {
        impl_->closed=true;
        throw;
    } catch (const control::posthello::Error& error) {
        impl_->closed=true;
        fail(posthello_error(error.code()));
    } catch (const wire::reliable::TransportError& error) {
        impl_->closed=true;
        fail(transport_error(error.code()));
    } catch (const wire::WireError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_packet);
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }
}
ConnectionStep GameConnection::publish_world(const WorldPublication& publication, double now) {
    impl_->check_time(now);
    if (impl_->login.snapshot().state!=control::login::State::login_received ||
        impl_->publication!=PublicationState::pending)
        fail(ConnectionErrorCode::unexpected_message);
    const auto root_count=publication.packages.size();
    const auto contained_count=publication.contained_packages.size();
    const auto package_limit=impl_->limits.max_logical_messages_per_step-1;
    if (root_count==0) fail(ConnectionErrorCode::invalid_archive);
    if (root_count>package_limit || contained_count>package_limit-root_count)
        fail(ConnectionErrorCode::resource_limit);
    const auto package_count=root_count+contained_count;
    const auto metadata_at=[&](std::size_t index)->const control::packages::RootMetadata& {
        return index<root_count ? publication.packages[index]
            : publication.contained_packages[index-root_count].package;
    };

    // All metadata validation, codecs and retained-state allocations happen
    // before any transport call. Rejected preflight never commits caller time.
    std::vector<std::vector<std::uint8_t>> archives;
    std::vector<Impl::PublishedPackage> packages;
    std::vector<wire::packages::PackageGenerations> generations;
    std::vector<wire::packages::ObjectRange> ranges;
    ConnectionStep result;
    try {
        archives.reserve(package_count+1);
        packages.reserve(package_count);
        generations.reserve(package_count);
        result.datagrams.reserve(package_count+1);
        for (std::size_t i=0;i<package_count;++i) {
            const auto& metadata=metadata_at(i);
            const auto package=i<root_count
                ? control::packages::make_root_package(metadata,impl_->login_limits)
                : control::packages::make_contained_package(
                    publication.contained_packages[i-root_count],impl_->login_limits);
            for (std::size_t j=0;j<i;++j) {
                const auto& previous=metadata_at(j);
                if (metadata.identity==previous.identity || same_folded_name(metadata.name,previous.name))
                    fail(ConnectionErrorCode::invalid_archive);
            }
            archives.push_back(control::posthello::encode(package,impl_->endian,impl_->login_limits));
            packages.push_back({metadata.identity,package.value1,false});
            // Exact generation-COUNT ACK policy means R=L for this publication.
            // Retain only computed coordinates, not the caller's metadata.
            generations.push_back({metadata.generation_net_counts,package.value1});
        }
        ranges=wire::packages::build_ranges(generations);
        archives.push_back(control::posthello::encode(publication.welcome,impl_->endian,impl_->login_limits));
    } catch (const ConnectionError&) {
        throw;
    } catch (const control::posthello::Error& error) {
        fail(posthello_error(error.code()));
    } catch (const wire::WireError&) {
        fail(ConnectionErrorCode::invalid_archive);
    } catch (const std::bad_alloc&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        fail(ConnectionErrorCode::crypto_failure);
    }

    // This move is non-allocating. From the first send onward, no rollback of
    // transport/session counters is possible: any failure closes permanently.
    impl_->published_packages=std::move(packages);
    impl_->published_ranges=std::move(ranges);
    impl_->publication=PublicationState::awaiting_package_acks;
    try {
        std::size_t output_bytes=0;
        for (auto& archive:archives) {
            impl_->send_reply(std::move(archive),result,now,output_bytes);
            if (impl_->closed) fail(ConnectionErrorCode::terminal_state);
        }
        impl_->last_time=now;
        return result;
    } catch (const ConnectionError&) {
        impl_->closed=true;
        throw;
    } catch (const wire::reliable::TransportError& error) {
        impl_->closed=true;
        fail(transport_error(error.code()));
    } catch (const wire::WireError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_packet);
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }
}

void GameConnection::register_actor_channel(const wire::reliable::ActorChannelConfig& config) {
    if (impl_->closed) fail(ConnectionErrorCode::terminal_state);
    if (impl_->publication!=PublicationState::join_requested)
        fail(ConnectionErrorCode::unexpected_message);
    try {
        impl_->transport.register_actor_channel(config);
    } catch (const wire::reliable::TransportError& error) {
        using wire::reliable::TransportErrorCode;
        if (error.code()==TransportErrorCode::resource_limit)
            fail(ConnectionErrorCode::resource_limit);
        if (error.code()==TransportErrorCode::terminal_state)
            fail(ConnectionErrorCode::terminal_state);
        fail(ConnectionErrorCode::invalid_configuration);
    } catch (const std::bad_alloc&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        fail(ConnectionErrorCode::crypto_failure);
    }
}
ConnectionStep GameConnection::send_actor(const wire::Bunch& bunch, double now) {
    impl_->check_time(now);
    if (impl_->publication!=PublicationState::join_requested)
        fail(ConnectionErrorCode::unexpected_message);
    if (bunch.channel==0 || bunch.channel_type!=(bunch.reliable ? 2 : 0))
        fail(ConnectionErrorCode::invalid_packet);
    wire::reliable::TransportStep sent;
    // Lower send stages all changes. A rejection here preserves clock, queues
    // and Context nonces, including a closed/pending-close actor rejection.
    try {
        sent=impl_->transport.send(bunch,now);
    } catch (const wire::reliable::TransportError& error) {
        if (error.code()==wire::reliable::TransportErrorCode::terminal_state)
            fail(ConnectionErrorCode::terminal_state);
        fail(transport_error(error.code()));
    } catch (const std::bad_alloc&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        fail(ConnectionErrorCode::crypto_failure);
    }
    // Transport committed. Output failure cannot roll back transport state or
    // potentially consumed Context nonces; never return a partial flush.
    try {
        return impl_->compose(std::move(sent),now);
    } catch (const ConnectionError&) {
        impl_->closed=true;
        throw;
    } catch (const wire::WireError&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::invalid_packet);
    } catch (const std::bad_alloc&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (const std::length_error&) {
        impl_->closed=true;
        fail(ConnectionErrorCode::resource_limit);
    } catch (...) {
        impl_->closed=true;
        fail(ConnectionErrorCode::crypto_failure);
    }
}

wire::object::PackageExportReference GameConnection::resolve_package_reference(
    const std::array<std::uint32_t,4>& identity, std::int32_t local_net_index) const {
    if (impl_->closed) fail(ConnectionErrorCode::terminal_state);
    if (impl_->publication!=PublicationState::join_requested)
        fail(ConnectionErrorCode::unexpected_message);
    for (std::size_t slot=0;slot<impl_->published_packages.size();++slot) {
        if (impl_->published_packages[slot].identity!=identity) continue;
        try {
            return {wire::packages::encode_index(impl_->published_ranges,slot,local_net_index)};
        } catch (const wire::WireError&) {
            fail(ConnectionErrorCode::invalid_archive);
        }
    }
    fail(ConnectionErrorCode::invalid_archive);
}

}
