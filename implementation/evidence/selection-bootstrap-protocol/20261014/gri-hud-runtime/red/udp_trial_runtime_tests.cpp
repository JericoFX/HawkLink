#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "hawken/udp_trial_runtime.hpp"
#include "hawken/packet_writer.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace hawken;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
constexpr std::string_view guid = "12345678-1234-5678-9012-123456789abc";
constexpr std::string_view ads = "/hawkenClientMatchmakingAdvertisements";
static_assert(!std::is_copy_constructible_v<udp::UdpTrialRuntime>);
static_assert(!std::is_move_constructible_v<udp::UdpTrialRuntime>);

// Only the external Steam boundary is fake. Account/grant/session/Context,
// sockets, steady/UTC clocks and production entropy are the real modules.
class Verifier final : public auth::SteamVerifier {
public:
    std::atomic<bool> valid{false};
    auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        valid = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {valid ? auth::SteamAuthStatus::authenticated : auth::SteamAuthStatus::invalid_ticket,
                valid ? 76561197960265729ULL : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        return valid && id == 76561197960265729ULL;
    }
    void end_session(std::uint64_t) override { valid = false; }
};
struct Fixture {
    test::TempDirectory temporary;
    store::LocalStore store{temporary.path() / "accounts.sqlite", "udp-trial-fixture"};
    Verifier verifier;
    auth::AuthService auth{store, verifier};
    auth::AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
udp::UdpTrialConfig config(std::uint16_t port = 0) {
    return {port, std::string(guid), "EU"};
}
std::string authorization(const auth::AuthReply& login) {
    return "Basic " + login.result.at("AccessGrant").get<std::string>();
}
std::string request(const auth::AuthReply& login) {
    const auto user = login.result.at("UserId").get<std::string>();
    return Json{{"OwnerGuid", user}, {"Region", "EU"}, {"GameVersion", "547429238"},
                {"Users", Json::array({user})}, {"RequestedServerGuid", std::string(guid)}}.dump();
}
auth::AuthReply provision(udp::UdpTrialRuntime& runtime, const auth::AuthReply& login) {
    REQUIRE(login.status == 200);
    const auto post = runtime.assignment().handle("POST", ads, authorization(login), request(login));
    REQUIRE(post.status == 200);
    REQUIRE(post.result.is_string());
    const auto poll = runtime.assignment().handle("GET",
        std::string(ads) + "/" + post.result.get<std::string>(), authorization(login));
    REQUIRE(poll.status == 200);
    CHECK(poll.result.at("Status") == 1);
    CHECK(poll.result.at("AssignedServerIp") == "127.0.0.1");
    CHECK(poll.result.at("AssignedServerPort") == runtime.port());
    CHECK(poll.result.at("AssignedServerGuid") == std::string(guid));
    CHECK(poll.result.at("SkillRatingAverage") == 0);
    CHECK(poll.result.at("SkillRatingSearchMin") == 0);
    const auto& created = poll.result.at("TimeCreated").get_ref<const std::string&>();
    REQUIRE(created.size() == 20);
    CHECK(created[10] == 'T');
    CHECK(created.back() == 'Z');
    return runtime.assignment().handle("GET", "/session", authorization(login));
}
template<class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("UDP trial fixture could not initialize.");
    }
    ~SocketRuntime() { WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
struct SocketRuntime {};
#endif
void close_socket(Socket value) noexcept {
#ifdef _WIN32
    closesocket(value);
#else
    ::close(value);
#endif
}
class Peer {
public:
    explicit Peer(std::uint16_t port = 0) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) throw std::runtime_error("UDP trial fixture could not open.");
        try {
#ifdef _WIN32
            const int exclusive = 1;
            if (setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), static_cast<int>(sizeof(exclusive))) != 0)
                throw std::runtime_error("UDP trial fixture could not configure.");
#endif
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) != 0)
                throw std::runtime_error("UDP trial fixture could not bind.");
#ifdef _WIN32
            int length = static_cast<int>(sizeof(address));
#else
            socklen_t length = sizeof(address);
#endif
            if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
                throw std::runtime_error("UDP trial fixture could not inspect.");
            port_ = ntohs(address.sin_port);
        } catch (...) { close_socket(socket_); socket_ = invalid_socket; throw; }
    }
    ~Peer() { if (socket_ != invalid_socket) close_socket(socket_); }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    std::uint16_t port() const noexcept { return port_; }
    void send(std::uint16_t port, const Bytes& data) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(port);
#ifdef _WIN32
        const auto count = sendto(socket_, reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&target),
            static_cast<int>(sizeof(target)));
#else
        const auto count = sendto(socket_, data.data(), data.size(), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
#endif
        if (count < 0 || static_cast<std::size_t>(count) != data.size())
            throw std::runtime_error("UDP trial fixture could not send.");
    }
    std::optional<Bytes> receive(std::chrono::milliseconds budget = 2000ms) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval timeout{static_cast<long>(budget.count() / 1000),
                        static_cast<long>((budget.count() % 1000) * 1000)};
#ifdef _WIN32
        const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
        const auto ready = select(socket_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
        if (ready < 0) throw std::runtime_error("UDP trial fixture could not wait.");
        if (ready == 0) return std::nullopt;
        std::array<std::uint8_t, 65507> data{};
#ifdef _WIN32
        const auto count = recv(socket_, reinterpret_cast<char*>(data.data()),
            static_cast<int>(data.size()), 0);
#else
        const auto count = recv(socket_, data.data(), data.size(), 0);
#endif
        if (count < 0) throw std::runtime_error("UDP trial fixture could not receive.");
        return Bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count));
    }
private:
    SocketRuntime sockets_;
    Socket socket_ = invalid_socket;
    std::uint16_t port_ = 0;
};

// The fixture consumes only its own freshly generated session in memory.
// Never compare/log the key, ID, grant, raw DTO or a captured native credential.
template<std::size_t N>
std::array<std::uint8_t, N> decode_base64url(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::array<std::uint8_t, N> output{};
    std::uint32_t pending = 0;
    unsigned bits = 0;
    std::size_t index = 0;
    for (const char c : text) {
        const auto digit = alphabet.find(c);
        if (digit == std::string_view::npos) throw std::runtime_error("Invalid fixture session encoding.");
        pending = (pending << 6) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (index >= N) throw std::runtime_error("Invalid fixture session encoding.");
            output[index++] = static_cast<std::uint8_t>(pending >> bits);
        }
    }
    if (index != N || (pending & ((1U << bits) - 1U)) != 0)
        throw std::runtime_error("Invalid fixture session encoding.");
    return output;
}
Bytes packet(session::Context& client, std::uint16_t pid, std::uint16_t sequence,
             Bytes logical, bool open = false) {
    wire::PacketRecord record;
    record.bunch.control = open;
    record.bunch.open = open;
    record.bunch.reliable = true;
    record.bunch.channel_type = 1;
    record.bunch.sequence = sequence;
    record.bunch.payload_bits = logical.size() * 8;
    record.bunch.payload = std::move(logical);
    wire::Packet value;
    value.packet_id = pid;
    value.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(value, 512));
}
std::vector<Bytes> messages(Peer& peer, session::Context& client,
                           std::chrono::milliseconds budget = 2000ms) {
    std::vector<Bytes> result;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    do {
        const auto data = peer.receive(20ms);
        if (!data) continue;
        CHECK(data->size() <= 560);
        const auto value = wire::decode_packet(client.decode(*data), 512);
        for (const auto& record : value.records) {
            if (record.is_ack) continue;
            CHECK_FALSE(record.bunch.open);
            CHECK_FALSE(record.bunch.close);
            CHECK(record.bunch.reliable);
            CHECK(record.bunch.channel_type == 1);
            result.push_back(record.bunch.payload);
        }
        if (!result.empty()) return result;
    } while (std::chrono::steady_clock::now() < deadline);
    return result;
}
void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void hello(udp::UdpTrialRuntime& runtime, Peer& peer, session::Context& client) {
    peer.send(runtime.port(), packet(client, 0, 1, {0x1a, 7, 0, 0, 0}, true));
    auto replies = messages(peer, client);
    REQUIRE(replies.size() == 1);
    const auto decoded = control::decode(replies.front(), control::Endian::little);
    const auto* challenge = std::get_if<control::Challenge1b>(&decoded);
    REQUIRE(challenge);
    Bytes response{0x1c};
    append_u32(response, control::response_crc(challenge->challenge));
    peer.send(runtime.port(), packet(client, 1, 2, std::move(response)));
    replies = messages(peer, client);
    REQUIRE(replies.size() == 1);
    CHECK(replies.front() == Bytes{0x1d});
    // Original file versions9188..154767; this explicitly exercises the
    // CONDITIONAL fresh native baseline, not the old synthetic7..11 fixture.
    peer.send(runtime.port(), packet(client, 2, 3,
        {0,0xe4,0x23,0,0,0x8f,0x5c,2,0,0,0,0,0}));
    replies = messages(peer, client);
    REQUIRE(replies.size() == 1);
    const auto value = control::posthello::decode_exact(replies.front(), control::Endian::little, {512,256});
    const auto* login_challenge = std::get_if<control::posthello::Challenge>(&value);
    REQUIRE(login_challenge);
    CHECK(login_challenge->version == 154767);
    CHECK(login_challenge->challenge.form == control::posthello::StringForm::narrow);
    REQUIRE(login_challenge->challenge.units.size() == 9);
    CHECK(login_challenge->challenge.units.back() == 0);
    CHECK(wait_until([&] { return runtime.diagnostics().hello_validated; }));
}
Bytes login_response(std::string_view user_guid) {
    if (user_guid.size() != 36) throw std::runtime_error("Invalid fixture response.");
    Bytes value{5,37,0,0,0};
    for (const char c : user_guid) value.push_back(static_cast<std::uint8_t>(c));
    value.push_back(0);
    // Empty URL plus PUBLIC zero ID16. ID16 is not authenticated by this seam.
    value.insert(value.end(), 20, 0);
    return value;
}
}

TEST_CASE("UDP trial pure preflight rejects malformed identity without constructing a socket") {
    CHECK(udp::validate_trial_config(config()));
    CHECK(udp::validate_trial_config(config(65535)));
    for (const auto region : {"EU", "us-east_1", "region.2", "-"}) {
        auto candidate = config(); candidate.region = region;
        CHECK(udp::validate_trial_config(candidate));
    }
    for (const auto bad : {"", "00000000-0000-0000-0000-000000000000",
                           "12345678-1234-5678-9012-123456789abC", "12345678123456789012123456789abc"}) {
        auto candidate = config(); candidate.server_guid = bad;
        CHECK_FALSE(udp::validate_trial_config(candidate));
    }
    for (const auto bad : {"", "EU/US", "EU US", "EU?x=1", "\xC3\xB1"}) {
        auto candidate = config(); candidate.region = bad;
        CHECK_FALSE(udp::validate_trial_config(candidate));
    }
    auto candidate = config(); candidate.region.assign(65, 'a');
    CHECK_FALSE(udp::validate_trial_config(candidate));
    candidate.region = std::string("EU\0US", 5);
    CHECK_FALSE(udp::validate_trial_config(candidate));
}
TEST_CASE("UDP trial constructor validates identity before an occupied bind") {
    Fixture f; Peer occupied;
    auto bad = config(occupied.port()); bad.server_guid.clear();
    CHECK_THROWS_AS(udp::UdpTrialRuntime(f.auth, bad), std::invalid_argument);
    bad = config(occupied.port()); bad.region = "invalid/region";
    CHECK_THROWS_AS(udp::UdpTrialRuntime(f.auth, bad), std::invalid_argument);
    CHECK_FALSE(f.store.find_account("76561197960265729"));
}
TEST_CASE("UDP trial binds a real exclusive loopback port before publishing a session") {
    Fixture f; udp::UdpTrialRuntime runtime(f.auth, config());
    REQUIRE(runtime.running()); REQUIRE(runtime.port() != 0);
    CHECK_THROWS_AS(Peer(runtime.port()), std::runtime_error);
    CHECK(runtime.assignment().handle("GET", "/session", {}).status == 401);
    CHECK(runtime.diagnostics() == udp::LocalUdpDiagnostics{});
    const auto login = f.login(); REQUIRE(login.status == 200);
    CHECK(runtime.assignment().handle("GET", "/session", authorization(login)).status == 404);
    const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
    CHECK_FALSE(f.store.starter_items(account->ref));
}
TEST_CASE("UDP trial refuses a colliding destination without leaking configuration") {
    Fixture f; Peer occupied;
    CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth, config(occupied.port())),
        "UDP trial runtime could not start.", std::runtime_error);
    CHECK_FALSE(f.store.find_account("76561197960265729"));
}
TEST_CASE("UDP trial advertisement and session reuse one real Context with shared expiry epoch") {
    Fixture f; udp::UdpTrialRuntime runtime(f.auth, config());
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    CHECK(dto.result.at("AssignedServerPort") == runtime.port());
    CHECK(dto.result.at("SessionId").get_ref<const std::string&>().size() == 11);
    CHECK(dto.result.at("SessionKey").get_ref<const std::string&>().size() == 43);
    const auto duplicate = provision(runtime, login);
    // bool wrappers prevent doctest from printing freshly generated material.
    CHECK(bool(dto.result == duplicate.result));
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    CHECK(runtime.assignment().authorize_udp(id));
    Peer peer; session::Context client(session::Role::client, id, key);
    const auto initial = packet(client, 0, 1, {0x1a,7,0,0,0}, true);
    peer.send(runtime.port(), initial);
    REQUIRE(messages(peer, client).size() == 1);
    CHECK(wait_until([&] { return runtime.diagnostics().authenticated_datagrams == 1; }));
    peer.send(runtime.port(), initial); // Replay never resets the persistent server Context.
    std::this_thread::sleep_for(60ms);
    CHECK(runtime.diagnostics().authenticated_datagrams == 1);
    CHECK(bool(runtime.assignment().handle("GET", "/session", authorization(login)).result == dto.result));
}
TEST_CASE("UDP trial wires matching Login Response to authority but leaves admission pending") {
    Fixture f; udp::UdpTrialRuntime runtime(f.auth, config());
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet = f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client, id, key);
    hello(runtime, peer, client);
    peer.send(runtime.port(), packet(client, 3, 4,
        login_response(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&] { return runtime.diagnostics().login_response_validated; }));
    CHECK(runtime.diagnostics().login_received); CHECK_FALSE(runtime.diagnostics().closed);
    CHECK(runtime.running()); CHECK(runtime.assignment().authorize_udp(id));
    CHECK(messages(peer, client, 100ms).empty()); // ACK only; no fake Welcome/world.
    CHECK(f.store.wallet(account->ref) == wallet);
    CHECK_FALSE(f.store.starter_items(account->ref));
}
TEST_CASE("UDP trial revocation retires its session on idle without stopping the listener") {
    Fixture f; udp::UdpTrialRuntime runtime(f.auth, config());
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    f.verifier.valid = false;
    CHECK(wait_until([&] { return runtime.diagnostics().closed; }));
    CHECK(runtime.running()); CHECK_FALSE(runtime.assignment().authorize_udp(id));
    CHECK_FALSE(runtime.diagnostics().login_response_validated);
    CHECK(runtime.assignment().handle("GET", "/session", authorization(login)).status == 401);
}
TEST_CASE("UDP trial logout preserves the explicit one-successful-installation boundary") {
    Fixture f; udp::UdpTrialRuntime runtime(f.auth, config());
    const auto login = f.login(); REQUIRE(provision(runtime, login).status == 200);
    REQUIRE(f.auth.logout(login.result.at("UserId").get<std::string>(),
        authorization(login), Json{{"AccessGrant", login.result.at("AccessGrant")}}.dump()).status == 200);
    CHECK(wait_until([&] { return runtime.diagnostics().closed; }));
    const auto again = f.login(); REQUIRE(again.status == 200);
    const auto reply = runtime.assignment().select(authorization(again), guid);
    CHECK(reply.status == 503); CHECK(reply.result.is_null());
    // This temporary diagnostic limitation is not multiplayer Creator behavior.
}
TEST_CASE("UDP trial stop joins before releasing callbacks and frees its bound port") {
    Fixture f; auto runtime = std::make_unique<udp::UdpTrialRuntime>(f.auth, config());
    const auto login = f.login(); REQUIRE(provision(*runtime, login).status == 200);
    const auto port = runtime->port();
    const auto started = std::chrono::steady_clock::now();
    runtime->stop(); runtime->stop();
    CHECK(std::chrono::steady_clock::now() - started < 2000ms);
    CHECK_FALSE(runtime->running()); CHECK(runtime->diagnostics().closed);
    CHECK(runtime->assignment().handle("GET", "/session", authorization(login)).status == 503);
    Peer rebound(port);
    CHECK(rebound.port() == port);
    runtime.reset(); // Assignment/lease dies while Auth/Store remain alive.
    CHECK(f.auth.access(authorization(login)).status == 200);
}

namespace {
game::WorldPublication public_publication() {
    // Synthetic metadata only. This test does not select installed world assets.
    using control::posthello::StringForm;
    return {{{"FRoot", {0x01020304U, 0x12345678U, 0x80000000U, 0xffffffffU}, {0, 3}}},
        {{StringForm::narrow, std::u16string{u'M', u'a', u'p', 0}},
         {StringForm::narrow, std::u16string{u'G', u'a', u'm', u'e', 0}}}};
}
Bytes public_hex(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    if (text.size() % 2) throw std::logic_error("Invalid public fixture literal.");
    Bytes result;
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const auto high = digits.find(text[index]);
        const auto low = digits.find(text[index + 1]);
        if (high == std::string_view::npos || low == std::string_view::npos)
            throw std::logic_error("Invalid public fixture literal.");
        result.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return result;
}
std::vector<Bytes> publication_messages(Peer& peer, session::Context& client,
                                       std::size_t wanted = 2) {
    std::vector<Bytes> result;
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto data = peer.receive(20ms);
        if (!data) continue;
        CHECK(data->size() <= 560);
        const auto value = wire::decode_packet(client.decode(*data), 512);
        for (const auto& record : value.records) {
            if (record.is_ack) continue;
            CHECK(record.bunch.reliable);
            CHECK(record.bunch.channel == 0);
            CHECK(record.bunch.channel_type == 1);
            CHECK(record.bunch.sequence == static_cast<std::uint16_t>(4 + result.size()));
            CHECK_FALSE(record.bunch.open);
            CHECK_FALSE(record.bunch.close);
            result.push_back(record.bunch.payload);
        }
        if (result.size() >= wanted) break;
    }
    return result;
}
}

TEST_CASE("UDP trial copies optional world plan and publishes on the persistent authorized session") {
    Fixture f;
    std::unique_ptr<udp::UdpTrialRuntime> runtime;
    {
        auto selected = config();
        selected.publication = public_publication();
        runtime = std::make_unique<udp::UdpTrialRuntime>(f.auth, selected);
        // Both mutation and destruction must leave the runtime's copy intact.
        selected.publication->packages.clear();
        selected.publication->welcome.level.units.clear();
        selected.publication.reset();
    }
    const auto login = f.login();
    const auto dto = provision(*runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet = f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client, id, key);
    hello(*runtime, peer, client);
    const auto previous_nonce = client.last_received();
    peer.send(runtime->port(), packet(client, 3, 4,
        login_response(login.result.at("UserId").get<std::string>())));
    // Independent logical literals, not generated by the publication encoder.
    const auto package = public_hex("07040302017856341200000080ffffffff0600000046526f6f7400"
        "0100000000000000000000000002000000010000000000");
    const auto welcome = public_hex("01040000004d6170000500000047616d6500");
    CHECK(publication_messages(peer, client) == std::vector<Bytes>{package, welcome});
    CHECK(client.last_received() > previous_nonce);
    CHECK(wait_until([&] { return runtime->diagnostics().world_published; }));
    CHECK(runtime->diagnostics().login_response_validated);
    CHECK_FALSE(runtime->diagnostics().closed);
    // ACK8's generation is COUNT2, distinct from last NetCount3. Join9 follows
    // in the same logical bunch; neither instruction creates actors or admits.
    peer.send(runtime->port(), packet(client, 4, 5,
        public_hex("08040302017856341200000080ffffffff0200000009")));
    CHECK(wait_until([&] { return runtime->diagnostics().join_requested; }));
    CHECK(runtime->assignment().authorize_udp(id));
    CHECK_FALSE(runtime->diagnostics().closed);
    CHECK(publication_messages(peer, client, 1).empty()); // ACK only, never actors.
    CHECK(f.store.wallet(account->ref) == wallet);
    CHECK_FALSE(f.store.starter_items(account->ref));
    runtime->stop();
    CHECK(runtime->diagnostics().world_published);
    CHECK(runtime->diagnostics().join_requested);
}
TEST_CASE("UDP trial without world plan keeps identity-only continuation") {
    Fixture f; const auto selected = config();
    CHECK_FALSE(selected.publication);
    udp::UdpTrialRuntime runtime(f.auth, selected);
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client, id, key);
    hello(runtime, peer, client);
    peer.send(runtime.port(), packet(client, 3, 4,
        login_response(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&] { return runtime.diagnostics().login_response_validated; }));
    CHECK(publication_messages(peer, client, 1).empty());
    CHECK_FALSE(runtime.diagnostics().world_published);
    CHECK_FALSE(runtime.diagnostics().join_requested);
    CHECK(runtime.assignment().authorize_udp(id));
    CHECK_FALSE(runtime.diagnostics().closed);
}
TEST_CASE("UDP trial invalid supplied publication retires without Welcome or Join") {
    Fixture f; auto selected = config(); selected.publication = public_publication();
    selected.publication->packages.front().identity = {};
    // Only identity/region are preflighted; no duplicate prebind plan validator.
    CHECK(udp::validate_trial_config(selected));
    udp::UdpTrialRuntime runtime(f.auth, selected);
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client, id, key);
    hello(runtime, peer, client);
    peer.send(runtime.port(), packet(client, 3, 4,
        login_response(login.result.at("UserId").get<std::string>())));
    CHECK(wait_until([&] { return runtime.diagnostics().closed; }));
    CHECK(publication_messages(peer, client, 1).empty());
    CHECK(runtime.diagnostics().login_received);
    CHECK_FALSE(runtime.diagnostics().world_published);
    CHECK_FALSE(runtime.diagnostics().join_requested);
    CHECK_FALSE(runtime.assignment().authorize_udp(id));
    CHECK(runtime.running());
}
TEST_CASE("UDP trial supplied publication cannot bypass Login Response validation") {
    Fixture f; auto selected = config(); selected.publication = public_publication();
    udp::UdpTrialRuntime runtime(f.auth, selected);
    const auto login = f.login(); const auto dto = provision(runtime, login);
    REQUIRE(dto.status == 200);
    const auto id = decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key = decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client, id, key);
    hello(runtime, peer, client);
    peer.send(runtime.port(), packet(client, 3, 4,
        login_response("abcdef01-2345-6789-abcd-ef0123456789")));
    CHECK(publication_messages(peer, client, 1) == std::vector<Bytes>{
        public_hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(wait_until([&] { return runtime.diagnostics().closed; }));
    CHECK_FALSE(runtime.diagnostics().login_response_validated);
    CHECK_FALSE(runtime.diagnostics().world_published);
    CHECK_FALSE(runtime.diagnostics().join_requested);
    CHECK_FALSE(runtime.assignment().authorize_udp(id));
}


namespace {
udp::InitialControllerPlan runtime_controller_plan() {
    // Public synthetic FRoot/local2 and explicit actor seeds, not native IDs.
    return {{7,41,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        2,{0,0,0},std::nullopt,0};
}
void runtime_publish(udp::UdpTrialRuntime& runtime,const auth::AuthReply& login,
                     Peer& peer,session::Context& client) {
    hello(runtime,peer,client);
    peer.send(runtime.port(),packet(client,3,4,
        login_response(login.result.at("UserId").get<std::string>())));
    const auto package=public_hex("07040302017856341200000080ffffffff0600000046526f6f7400"
        "0100000000000000000000000002000000010000000000");
    const auto welcome=public_hex("01040000004d6170000500000047616d6500");
    REQUIRE(publication_messages(peer,client)==std::vector<Bytes>{package,welcome});
    REQUIRE(wait_until([&]{return runtime.diagnostics().world_published;}));
}
std::vector<wire::Bunch> runtime_actor_messages(Peer& peer,session::Context& client,
                                              std::chrono::milliseconds budget=2000ms,std::size_t expected=1) {
    std::vector<wire::Bunch> result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto data=peer.receive(20ms); if (!data) continue;
        const auto value=wire::decode_packet(client.decode(*data),512);
        for (const auto& record:value.records)
            if (!record.is_ack && record.bunch.channel!=0) result.push_back(record.bunch);
        if (result.size()>=expected) break;
    }
    return result;
}
}
TEST_CASE("UDP trial retains immutable controller plan and emits exact actor after authorized Join") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected);
    selected.controller->channel.channel=8; selected.controller->channel.outgoing_sequence=900;
    selected.controller->archetype_package_guid={}; selected.controller->archetype_local_net_index=3;
    selected.controller->position={8,-8,0}; selected.controller->rotation=wire::actor::Rotation{256,0,65280};
    selected.controller->net_player_index=255; selected.controller.reset();
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    runtime_publish(runtime,login,peer,client);
    CHECK_FALSE(runtime.diagnostics().controller_sent);
    const auto previous_nonce=client.last_received();
    const auto join=packet(client,4,5,public_hex("08040302017856341200000080ffffffff0200000009"));
    peer.send(runtime.port(),join);
    const auto actor=runtime_actor_messages(peer,client); REQUIRE(actor.size()==1);
    // Independent PRIMARY ref/local2 + zero position + raw byte, exact51 bits.
    CHECK(actor.front().payload==public_hex("04000000400500")); CHECK(actor.front().payload_bits==51);
    CHECK(actor.front().channel==7); CHECK(actor.front().sequence==42); CHECK(actor.front().channel_type==2);
    CHECK(actor.front().control); CHECK(actor.front().open); CHECK(actor.front().reliable); CHECK_FALSE(actor.front().close);
    CHECK(client.last_received()>previous_nonce);
    CHECK(wait_until([&]{return runtime.diagnostics().controller_sent;}));
    CHECK(runtime.diagnostics().join_requested); CHECK_FALSE(runtime.diagnostics().closed);
    CHECK_FALSE(runtime.diagnostics().pawn_sent); // No implicit Pawn is configured.
    CHECK(runtime.assignment().authorize_udp(id));
    peer.send(runtime.port(),join); // Replay does not emit a second controller.
    CHECK(runtime_actor_messages(peer,client,200ms).empty());
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime.stop(); CHECK(runtime.diagnostics().controller_sent);
}
TEST_CASE("UDP trial absent controller preserves publication and ACK-only Join") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    CHECK_FALSE(selected.controller); udp::UdpTrialRuntime runtime(f.auth,selected);
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client,id,key);
    runtime_publish(runtime,login,peer,client);
    peer.send(runtime.port(),packet(client,4,5,public_hex("08040302017856341200000080ffffffff0200000009")));
    CHECK(wait_until([&]{return runtime.diagnostics().join_requested;}));
    CHECK(runtime_actor_messages(peer,client,200ms).empty());
    CHECK_FALSE(runtime.diagnostics().controller_sent); CHECK_FALSE(runtime.diagnostics().closed);
    CHECK(runtime.assignment().authorize_udp(id)); runtime.stop();
    CHECK_FALSE(runtime.diagnostics().controller_sent);
}
TEST_CASE("UDP trial controller configuration failures roll back before runtime publication") {
    Fixture f;
    for (unsigned invalid=0;invalid<5;++invalid) {
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan();
        switch (invalid) {
        case 0: selected.publication.reset(); break;
        case 1: selected.controller->channel.channel=0; break;
        case 2: selected.controller->archetype_local_net_index=-1; break;
        case 3: selected.controller->position[0]=1048576; break;
        case 4: selected.controller->rotation=wire::actor::Rotation{1,0,0}; break;
        }
        // Identity-only preflight remains pure; Owner owns plan validation at start.
        CHECK(udp::validate_trial_config(selected));
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        CHECK_FALSE(f.store.find_account("76561197960265729"));
    }
    auto valid=config(); valid.publication=public_publication(); valid.controller=runtime_controller_plan();
    udp::UdpTrialRuntime runtime(f.auth,valid); CHECK(runtime.running()); runtime.stop();
}

namespace {
udp::InitialPawnPlan runtime_pawn_plan() {
    return {{8,71,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        2,{8,-8,0},wire::actor::Rotation{256,0,65280}};
}
}
TEST_CASE("UDP trial copies Pawn plan and forwards literal emission after Controller on same session") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected);
    selected.pawn->channel.channel=9; selected.pawn->channel.outgoing_sequence=900;
    selected.pawn->archetype_package_guid={}; selected.pawn->archetype_local_net_index=3;
    selected.pawn->position={0,0,0}; selected.pawn->rotation.reset(); selected.pawn.reset();
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    runtime_publish(runtime,login,peer,client);
    CHECK_FALSE(runtime.diagnostics().pawn_sent);
    const auto previous=client.last_received();
    peer.send(runtime.port(),packet(client,4,5,public_hex("08040302017856341200000080ffffffff0200000009")));
    const auto actors=runtime_actor_messages(peer,client,2000ms,2); REQUIRE(actors.size()==2);
    CHECK(actors[0].channel==7); CHECK(actors[0].payload==public_hex("04000000400500"));
    CHECK(actors[0].payload_bits==51); CHECK(actors[0].sequence==42);
    CHECK(actors[1].channel==8); CHECK(actors[1].sequence==72);
    CHECK(actors[1].payload==public_hex("04000000032338c07f")); CHECK(actors[1].payload_bits==71);
    CHECK(actors[1].channel_type==2); CHECK(actors[1].reliable); CHECK(actors[1].control);
    CHECK(actors[1].open); CHECK_FALSE(actors[1].close); CHECK(client.last_received()>previous);
    CHECK(wait_until([&]{return runtime.diagnostics().pawn_sent;}));
    CHECK(runtime.diagnostics().controller_sent); CHECK_FALSE(runtime.diagnostics().closed);
    CHECK(runtime.assignment().authorize_udp(id));
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime.stop(); CHECK(runtime.diagnostics().pawn_sent); CHECK_FALSE(runtime.running());
}
TEST_CASE("UDP trial invalid Pawn configuration rolls back and preserves later valid bind") {
    Fixture f;
    for (unsigned invalid=0;invalid<8;++invalid) {
        CAPTURE(invalid);
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        switch (invalid) {
        case 0: selected.controller.reset(); break;
        case 1: selected.publication.reset(); break;
        case 2: selected.pawn->channel.channel=0; break;
        case 3: selected.pawn->channel.channel=2048; break;
        case 4: selected.pawn->channel.channel=7; break;
        case 5: selected.pawn->archetype_local_net_index=-1; break;
        case 6: selected.pawn->position[0]=1048576; break;
        case 7: selected.pawn->rotation=wire::actor::Rotation{1,0,0}; break;
        }
        CHECK(udp::validate_trial_config(selected)); // Identity preflight has no actor validation.
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        CHECK_FALSE(f.store.find_account("76561197960265729"));
    }
    auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.running()); runtime.stop();
}

namespace {
udp::InitialPriPlan runtime_pri_plan() {
    // Public synthetic FRoot/local1. PRI has no Controller byte.
    return {{9,91,0,false},{0x01020304U,0x12345678U,0x80000000U,0xffffffffU},
        1,{0,0,0},std::nullopt};
}
}
TEST_CASE("UDP trial copies PRI plan and sends Controller then PRI then optional Pawn on same session") {
    bool with_pawn=false;
    SUBCASE("PRI without Pawn") {}
    SUBCASE("PRI precedes Pawn") { with_pawn=true; }
    Fixture f;
    std::unique_ptr<udp::UdpTrialRuntime> runtime;
    {
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan();
        if (with_pawn) selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan();
        runtime=std::make_unique<udp::UdpTrialRuntime>(f.auth,selected);
        auto changed=runtime_pri_plan(); changed.channel.channel=10;
        changed.channel.outgoing_sequence=900; changed.archetype_package_guid={};
        changed.archetype_local_net_index=3; changed.position={8,-8,0};
        changed.rotation=wire::actor::Rotation{256,0,65280};
        selected.pri=changed; selected.pri.reset();
    } // The runtime must also survive destruction of the caller's config.
    const auto login=f.login(); const auto dto=provision(*runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    runtime_publish(*runtime,login,peer,client);
    CHECK_FALSE(runtime->diagnostics().pri_sent);
    const auto previous=client.last_received();
    const auto join=packet(client,4,5,public_hex("08040302017856341200000080ffffffff0200000009"));
    peer.send(runtime->port(),join);
    const auto expected=with_pawn ? std::size_t{3} : std::size_t{2};
    const auto actors=runtime_actor_messages(peer,client,2000ms,expected);
    REQUIRE(actors.size()==expected);
    CHECK(actors[0].channel==7); CHECK(actors[0].sequence==42);
    CHECK(actors[0].payload==public_hex("04000000400500")); CHECK(actors[0].payload_bits==51);
    // PRIMARY package coordinate1 (32 bits), zero packed position (11 bits).
    // Literal derived independently; no byte alignment or Controller byte.
    CHECK(actors[1].channel==9); CHECK(actors[1].sequence==92);
    CHECK(actors[1].payload==public_hex("020000004005")); CHECK(actors[1].payload_bits==43);
    if (with_pawn) {
        CHECK(actors[2].channel==8); CHECK(actors[2].sequence==72);
        CHECK(actors[2].payload==public_hex("04000000032338c07f")); CHECK(actors[2].payload_bits==71);
    }
    for (const auto& actor:actors) {
        CHECK(actor.channel_type==2); CHECK(actor.reliable); CHECK(actor.control);
        CHECK(actor.open); CHECK_FALSE(actor.close);
    }
    CHECK(client.last_received()>previous);
    CHECK(wait_until([&]{return runtime->diagnostics().pri_sent;}));
    CHECK(runtime->diagnostics().controller_sent);
    CHECK(runtime->diagnostics().pawn_sent==with_pawn);
    CHECK(runtime->diagnostics().join_requested); CHECK_FALSE(runtime->diagnostics().closed);
    CHECK(runtime->assignment().authorize_udp(id));
    peer.send(runtime->port(),join);
    CHECK(runtime_actor_messages(peer,client,200ms).empty());
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime->stop(); CHECK(runtime->diagnostics().pri_sent); CHECK_FALSE(runtime->running());
}
TEST_CASE("UDP trial invalid PRI config releases bind and preserves later valid startup") {
    Fixture f;
    std::uint16_t port=0;
    { Peer reservation; port=reservation.port(); }
    for (unsigned invalid=0;invalid<9;++invalid) {
        CAPTURE(invalid);
        auto selected=config(port); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        auto pri=runtime_pri_plan();
        switch (invalid) {
        case 0: selected.controller.reset(); selected.pawn.reset(); break;
        case 1: selected.publication.reset(); break;
        case 2: pri.channel.channel=0; break;
        case 3: pri.channel.channel=2048; break;
        case 4: pri.channel.channel=7; break;
        case 5: pri.channel.channel=8; break;
        case 6: pri.archetype_local_net_index=-1; break;
        case 7: pri.position[0]=1048576; break;
        case 8: pri.rotation=wire::actor::Rotation{1,0,0}; break;
        }
        selected.pri=pri;
        CHECK(udp::validate_trial_config(selected)); // Owner owns actor-plan preflight.
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        CHECK_FALSE(f.store.find_account("76561197960265729"));
    }
    auto selected=config(port); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.running()); CHECK(runtime.port()==port);
    runtime.stop();
}
TEST_CASE("UDP trial unresolved PRI retires after Controller without sending Pawn") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    auto pri=runtime_pri_plan(); pri.archetype_local_net_index=3; // FRoot count3: one past end.
    selected.pri=pri; udp::UdpTrialRuntime runtime(f.auth,selected);
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    runtime_publish(runtime,login,peer,client);
    peer.send(runtime.port(),packet(client,4,5,public_hex("08040302017856341200000080ffffffff0200000009")));
    CHECK(wait_until([&]{return runtime.diagnostics().closed;}));
    const auto actors=runtime_actor_messages(peer,client,200ms,2); REQUIRE(actors.size()==1);
    CHECK(actors[0].channel==7); CHECK(actors[0].payload==public_hex("04000000400500"));
    CHECK(runtime.diagnostics().controller_sent); CHECK_FALSE(runtime.diagnostics().pri_sent);
    CHECK_FALSE(runtime.diagnostics().pawn_sent); CHECK_FALSE(runtime.assignment().authorize_udp(id));
    CHECK(runtime.running()); CHECK(f.store.wallet(account->ref)==wallet);
    CHECK_FALSE(f.store.starter_items(account->ref));
}
TEST_CASE("UDP trial PRI plan cannot bypass Login Response authorization") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); udp::UdpTrialRuntime runtime(f.auth,selected);
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    hello(runtime,peer,client);
    peer.send(runtime.port(),packet(client,3,4,login_response("abcdef01-2345-6789-abcd-ef0123456789")));
    CHECK(publication_messages(peer,client,1)==std::vector<Bytes>{
        public_hex("06100000004c6f67696e2072656a65637465642e00")});
    CHECK(wait_until([&]{return runtime.diagnostics().closed;}));
    CHECK(runtime_actor_messages(peer,client,200ms).empty());
    CHECK_FALSE(runtime.diagnostics().controller_sent); CHECK_FALSE(runtime.diagnostics().pri_sent);
    CHECK_FALSE(runtime.diagnostics().pawn_sent); CHECK_FALSE(runtime.assignment().authorize_udp(id));
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
}


namespace {
udp::InitialActorLinksPlan runtime_links_plan() {
    // SYNTHETIC effective field IDs/bounds. No native ID or eligibility claim.
    return {3,5,8,1,6,8};
}
struct RuntimeLinksPackets {
    std::vector<wire::Bunch> bunches;
    std::vector<std::uint16_t> packet_ids;
};
RuntimeLinksPackets runtime_links_packets(Peer& peer,session::Context& client,
    std::chrono::milliseconds budget,std::size_t expected,bool properties_only=false) {
    RuntimeLinksPackets result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto datagram=peer.receive(20ms); if (!datagram) continue;
        const auto value=wire::decode_packet(client.decode(*datagram),512);
        for (const auto& record:value.records) {
            if (record.is_ack || record.bunch.channel==0 ||
                (properties_only && record.bunch.open)) continue;
            result.bunches.push_back(record.bunch); result.packet_ids.push_back(value.packet_id);
        }
        if (result.bunches.size()>=expected) break;
    }
    return result;
}
Bytes runtime_links_ack(session::Context& client,std::uint16_t packet_id,
                        const std::vector<std::uint16_t>& ids) {
    wire::Packet value; value.packet_id=packet_id;
    for (const auto id:ids) {
        wire::PacketRecord record; record.is_ack=true; record.ack_packet_id=id;
        value.records.push_back(record);
    }
    return client.encode(wire::encode_packet(value,512));
}
RuntimeLinksPackets runtime_links_open(udp::UdpTrialRuntime& runtime,const auth::AuthReply& login,
                                     Peer& peer,session::Context& client) {
    runtime_publish(runtime,login,peer,client);
    peer.send(runtime.port(),packet(client,4,5,
        public_hex("08040302017856341200000080ffffffff0200000009")));
    auto opened=runtime_links_packets(peer,client,2000ms,3);
    REQUIRE(opened.bunches.size()==3); REQUIRE(opened.packet_ids.size()==3);
    CHECK(opened.bunches[0].channel==7); CHECK(opened.bunches[0].sequence==42);
    CHECK(opened.bunches[0].payload==public_hex("04000000400500"));
    CHECK(opened.bunches[1].channel==9); CHECK(opened.bunches[1].sequence==92);
    CHECK(opened.bunches[1].payload==public_hex("020000004005"));
    CHECK(opened.bunches[2].channel==8); CHECK(opened.bunches[2].sequence==72);
    CHECK(opened.bunches[2].payload==public_hex("04000000032338c07f"));
    for (const auto& bunch:opened.bunches) { CHECK(bunch.open); CHECK(bunch.reliable); }
    REQUIRE(wait_until([&]{return runtime.diagnostics().pawn_sent && runtime.diagnostics().pri_sent;}));
    return opened;
}
}
TEST_CASE("UDP trial copied actor links forward exact ACK gated literal batches on the same Context") {
    Fixture f; std::unique_ptr<udp::UdpTrialRuntime> runtime;
    {
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        runtime=std::make_unique<udp::UdpTrialRuntime>(f.auth,selected);
        selected.links->controller_exclusive_bound=0; selected.links->pawn_controller_field_id=100;
        selected.links.reset(); selected.controller.reset(); selected.pawn.reset(); selected.pri.reset();
    } // Caller config and plan are gone before authentication/publication/ACKs.
    const auto login=f.login(); const auto dto=provision(*runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    const auto opened=runtime_links_open(*runtime,login,peer,client);
    // PacketIDs derive from actual wire packets, including randomized-session replies.
    peer.send(runtime->port(),runtime_links_ack(client,5,{opened.packet_ids[0],opened.packet_ids[2]}));
    CHECK(runtime_links_packets(peer,client,200ms,2,true).bunches.empty());
    const auto previous=client.last_received();
    peer.send(runtime->port(),runtime_links_ack(client,6,{opened.packet_ids[1]}));
    const auto linked=runtime_links_packets(peer,client,2000ms,2,true);
    REQUIRE(linked.bunches.size()==2); REQUIRE(linked.packet_ids.size()==2);
    // Independent 2x15-bit property literals; no RPC presence or terminator.
    CHECK(linked.bunches[0].channel==7); CHECK(linked.bunches[0].sequence==43);
    CHECK(linked.bunches[0].payload==public_hex("9b804600")); CHECK(linked.bunches[0].payload_bits==30);
    CHECK(linked.bunches[1].channel==8); CHECK(linked.bunches[1].sequence==73);
    CHECK(linked.bunches[1].payload==public_hex("79004f00")); CHECK(linked.bunches[1].payload_bits==30);
    for (const auto& bunch:linked.bunches) {
        CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
        CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    }
    CHECK(linked.packet_ids[0]>opened.packet_ids.back());
    CHECK(linked.packet_ids[1]>linked.packet_ids[0]); CHECK(client.last_received()>previous);
    CHECK(runtime->assignment().authorize_udp(id)); CHECK_FALSE(runtime->diagnostics().closed);
    peer.send(runtime->port(),runtime_links_ack(client,7,opened.packet_ids));
    CHECK(runtime_links_packets(peer,client,200ms,2,true).bunches.empty());
    CHECK(runtime_links_packets(peer,client,200ms,2,true).bunches.empty());
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime->stop(); CHECK_FALSE(runtime->running()); CHECK_FALSE(runtime->assignment().authorize_udp(id));
}
TEST_CASE("UDP trial absent actor links preserves three actors and authorization without property batches") {
    Fixture f; auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); udp::UdpTrialRuntime runtime(f.auth,selected);
    const auto login=f.login(); const auto dto=provision(runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    Peer peer; session::Context client(session::Role::client,id,key);
    const auto opened=runtime_links_open(runtime,login,peer,client);
    peer.send(runtime.port(),runtime_links_ack(client,5,opened.packet_ids));
    CHECK(runtime_links_packets(peer,client,200ms,2,true).bunches.empty());
    CHECK(runtime.assignment().authorize_udp(id)); CHECK_FALSE(runtime.diagnostics().closed);
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime.stop(); CHECK_FALSE(runtime.assignment().authorize_udp(id));
}
TEST_CASE("UDP trial links prereqs and Owner scalar validation reject startup without consuming bind or authority") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto wallet=f.store.wallet(account->ref);
    std::uint16_t port=0; { Peer reservation; port=reservation.port(); }
    for (unsigned invalid=0;invalid<8;++invalid) {
        CAPTURE(invalid);
        auto selected=config(port); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        switch (invalid) {
        case 0: selected.publication.reset(); break;
        case 1: selected.controller.reset(); break;
        case 2: selected.pri.reset(); break;
        case 3: selected.pawn.reset(); break;
        case 4: selected.links->controller_exclusive_bound=0; break;
        case 5: selected.links->pawn_exclusive_bound=0; break;
        case 6: selected.links->controller_pawn_field_id=selected.links->controller_pri_field_id; break;
        case 7: selected.links->pawn_pri_field_id=selected.links->pawn_exclusive_bound; break;
        }
        CHECK(udp::validate_trial_config(selected)); // Identity-only API stays unchanged.
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    }
    auto selected=config(port); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.running()); CHECK(runtime.port()==port);
    CHECK(provision(runtime,login).status==200); // Same grant and first lease remain usable.
    CHECK(f.store.wallet(account->ref)==wallet); runtime.stop();
}

namespace {
udp::StartMechDispatchPlan runtime_start_mech_plan() {
    return {4,8,5,8,{true,true,true,{true,true,true},true,true,true},0};
}
inventory::StarterCatalogueIds runtime_starter_types() {
    return {"10000000-0000-0000-0000-000000000001","10000000-0000-0000-0000-000000000002",
        "10000000-0000-0000-0000-000000000003","10000000-0000-0000-0000-000000000004",
        "10000000-0000-0000-0000-000000000005","10000000-0000-0000-0000-000000000006",
        "10000000-0000-0000-0000-000000000007","10000000-0000-0000-0000-000000000008",
        "10000000-0000-0000-0000-000000000009"};
}
store::StarterMechSnapshot runtime_seed_owned_starter(Fixture& fixture,const store::AccountRef& account) {
    const auto ids=runtime_starter_types();
    const std::array types{ids.mech,ids.primary,ids.secondary,ids.upper,ids.middle,
        ids.arms,ids.lower,ids.drone,ids.ability};
    store::StarterMechSnapshot snapshot;
    for (std::size_t i=0;i<types.size();++i)
        snapshot.items[i]={types[i],"20000000-0000-0000-0000-00000000000"+std::to_string(i+1)};
    snapshot.attributes={{"Preset.PrimaryWeapon_0","0"},
        {"Preset.PrimaryWeapon_InstanceId_0",snapshot.items[1].instance_id},
        {"Preset.SecondaryWeapon","1"},{"Preset.SecondaryWeapon_InstanceId",snapshot.items[2].instance_id},
        {"Preset.Style.UpperPart","10"},{"Preset.Style.UpperPart_InstanceId",snapshot.items[3].instance_id},
        {"Preset.Style.MidPart","10"},{"Preset.Style.MidPart_InstanceId",snapshot.items[4].instance_id},
        {"Preset.Style.ArmPart","10"},{"Preset.Style.ArmPart_InstanceId",snapshot.items[5].instance_id},
        {"Preset.Style.LowerPart","10"},{"Preset.Style.LowerPart_InstanceId",snapshot.items[6].instance_id},
        {"Preset.Style.DroneType","0"},{"Preset.Style.DroneType_InstanceId",snapshot.items[7].instance_id},
        {"Preset.Ability","3"},{"Preset.Ability_InstanceId",snapshot.items[8].instance_id}};
    // Test-only seeded nine owned rows + effective baseline, not a production grant path.
    REQUIRE(fixture.store.grant_starter_items(account,snapshot.items));
    REQUIRE(fixture.store.merge_owned_item_attributes(account,snapshot.items[0].instance_id,snapshot.attributes));
    return fixture.store.owned_starter_mech(account,snapshot.items[0].instance_id);
}
Bytes runtime_start_mech_packet(session::Context& client,std::uint16_t packet_id,
                               std::int32_t primary=256,std::int32_t holo=-1) {
    wire::BitWriter bits; bits.write_bounded(4,8);
    const auto u32=[&](std::uint32_t value) {
        const std::array<std::uint8_t,4> bytes{static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),
            static_cast<std::uint8_t>(value>>24)};
        bits.write_bits(bytes,32);
    };
    bits.write_bit(true); u32(37);
    for (const auto c:std::string_view("20000000-0000-0000-0000-000000000001"))
        bits.write_bits(std::array<std::uint8_t,1>{static_cast<std::uint8_t>(c)},8);
    bits.write_bits(std::array<std::uint8_t,1>{0},8);
    bits.write_bit(true); u32(static_cast<std::uint32_t>(primary));
    bits.write_bit(true); u32(static_cast<std::uint32_t>(holo));
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2; bunch.channel=7; bunch.sequence=1;
    bunch.payload_bits=bits.bit_count(); bunch.payload=bits.payload_bytes();
    wire::Packet value; value.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); value.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(value,512));
}
udp::UdpTrialConfig runtime_start_config() {
    auto selected=config(); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
    selected.start_mech=runtime_start_mech_plan();
    return selected;
}
}
TEST_CASE("UDP trial recognized start mech reads fresh owned scope and sends the independent effective preset") {
    for (const auto primary:{256,-256}) {
        Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
        REQUIRE(account); const auto owned=runtime_seed_owned_starter(f,account->ref);
        const auto wallet=f.store.wallet(account->ref); std::atomic<int> reads{0};
        session::SessionId id{}; udp::UdpTrialRuntime* active=nullptr;
        std::unique_ptr<udp::UdpTrialRuntime> runtime;
        {
            auto selected=runtime_start_config();
            selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
                [&](const store::AccountRef& scope,std::string_view instance) {
                    CHECK(scope==account->ref); CHECK(instance==owned.items[0].instance_id);
                    CHECK(active->assignment().account_for_udp(id)==std::optional{account->ref});
                    ++reads; return f.store.owned_starter_mech(scope,instance);
                }};
            runtime=std::make_unique<udp::UdpTrialRuntime>(f.auth,selected); active=runtime.get();
            selected.start_mech->controller_rpc_field_id=6;
            selected.start_mech_source->expected_catalogue.mech="not-a-type";
            selected.start_mech_source->read_effective={};
        } // Runtime owns copies, not caller config/source references.
        const auto dto=provision(*runtime,login);
        id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
        const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
        Peer peer; session::Context client(session::Role::client,id,key);
        const auto opened=runtime_links_open(*runtime,login,peer,client);
        peer.send(runtime->port(),runtime_start_mech_packet(client,5,primary,511));
        CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty()); CHECK(reads==0);
        peer.send(runtime->port(),runtime_links_ack(client,6,opened.packet_ids));
        const auto output=runtime_links_packets(peer,client,2000ms,3,true);
        REQUIRE(output.bunches.size()==3);
        CHECK(output.bunches[0].channel==7); CHECK(output.bunches[1].channel==8);
        const auto& preset=output.bunches[2];
        CHECK(preset.channel==9); CHECK(preset.sequence==93); CHECK(preset.reliable);
        CHECK_FALSE(preset.open); CHECK_FALSE(preset.close); CHECK(preset.payload_bits==547);
        // Independent retained67leaf baseline absolute offsets, shifted3 by id5.
        CHECK(preset.payload==public_hex(
            "05000000000000000000000000c0e1c900381808703010e06020c0c1400505050580ff82"
            "ffffffffffffffffff7fffffffffffff006500000340000000c0030000c0800000"));
        CHECK(reads==1); CHECK(runtime->assignment().authorize_udp(id));
        peer.send(runtime->port(),runtime_links_ack(client,7,output.packet_ids));
        CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty()); CHECK(reads==1);
        CHECK(f.store.wallet(account->ref)==wallet);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).attributes==owned.attributes);
        runtime->stop(); CHECK_FALSE(runtime->assignment().authorize_udp(id));
    }
}
TEST_CASE("UDP trial start mech source failure wrong root invalid role and revocation fail closed") {
    for (unsigned mode=0;mode<5;++mode) {
        CAPTURE(mode); Fixture f; const auto login=f.login();
        const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
        const auto owned=runtime_seed_owned_starter(f,account->ref); const auto wallet=f.store.wallet(account->ref);
        auto selected=runtime_start_config(); std::atomic<int> reads{0};
        selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
            [&](const store::AccountRef& scope,std::string_view instance) {
                ++reads;
                if (mode==0) throw std::runtime_error("synthetic snapshot fault");
                auto result=f.store.owned_starter_mech(scope,instance);
                if (mode==1) result.items[0].instance_id="30000000-0000-0000-0000-000000000001";
                if (mode==2) result.items[1].type_id=result.items[2].type_id;
                if (mode==3) f.verifier.valid=false;
                return result;
            }};
        udp::UdpTrialRuntime runtime(f.auth,selected); const auto dto=provision(runtime,login);
        const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
        const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
        Peer peer; session::Context client(session::Role::client,id,key);
        const auto opened=runtime_links_open(runtime,login,peer,client);
        peer.send(runtime.port(),runtime_links_ack(client,5,opened.packet_ids));
        REQUIRE(runtime_links_packets(peer,client,2000ms,2,true).bunches.size()==2);
        peer.send(runtime.port(),runtime_start_mech_packet(client,6,mode==4?257:256));
        CHECK(wait_until([&]{return runtime.diagnostics().closed;}));
        CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty());
        CHECK_FALSE(runtime.assignment().authorize_udp(id)); CHECK(reads<=1);
        CHECK(f.store.wallet(account->ref)==wallet);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).attributes==owned.attributes);
        runtime.stop();
    }
}
TEST_CASE("UDP trial start mech prerequisites reject before bind without consuming account lease or wallet") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto owned=runtime_seed_owned_starter(f,account->ref);
    const auto wallet=f.store.wallet(account->ref);
    std::uint16_t port=0; { Peer reservation; port=reservation.port(); }
    for (unsigned bad=0;bad<8;++bad) {
        auto selected=runtime_start_config(); selected.port=port;
        selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
            [&](const store::AccountRef& scope,std::string_view instance){return f.store.owned_starter_mech(scope,instance);}};
        switch(bad) {
        case 0: selected.publication.reset(); break;
        case 1: selected.controller.reset(); break;
        case 2: selected.pri.reset(); break;
        case 3: selected.pawn.reset(); break;
        case 4: selected.links.reset(); break;
        case 5: selected.start_mech_source.reset(); break;
        case 6: selected.start_mech_source->read_effective={}; break;
        case 7: selected.start_mech.reset(); break;
        }
        CAPTURE(bad);
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        CHECK(f.store.wallet(account->ref)==wallet);
    }
    auto selected=runtime_start_config(); selected.port=port;
    selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
        [&](const store::AccountRef& scope,std::string_view instance){return f.store.owned_starter_mech(scope,instance);}};
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.port()==port);
    CHECK(provision(runtime,login).status==200); CHECK(f.store.wallet(account->ref)==wallet);
    runtime.stop();
}


namespace {
udp::SpawnValidationDispatchPlan runtime_spawn_validation_plan() {
    return {2,6,7,8,{true,true,true,true,true,true}};
}
Bytes runtime_spawn_validation_packet(session::Context& client,std::uint16_t packet_id) {
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2;
    bunch.channel=7; bunch.sequence=2; bunch.payload={0x02}; bunch.payload_bits=3;
    wire::Packet value; value.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); value.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(value,512));
}
RuntimeLinksPackets runtime_validation_selected(udp::UdpTrialRuntime& runtime,const auth::AuthReply& login,
    Peer& peer,session::Context& client) {
    const auto opened=runtime_links_open(runtime,login,peer,client);
    peer.send(runtime.port(),runtime_links_ack(client,5,opened.packet_ids));
    const auto links=runtime_links_packets(peer,client,2000ms,2,true); REQUIRE(links.bunches.size()==2);
    peer.send(runtime.port(),runtime_start_mech_packet(client,6,256,511));
    const auto property=runtime_links_packets(peer,client,2000ms,1,true); REQUIRE(property.bunches.size()==1);
    CHECK(property.bunches[0].channel==9); CHECK(property.bunches[0].sequence==93);
    CHECK(property.bunches[0].payload_bits==547);
    peer.send(runtime.port(),runtime_links_ack(client,7,links.packet_ids));
    return property;
}
void runtime_validation_literal(const RuntimeLinksPackets& output,std::uint8_t byte,std::size_t bits) {
    REQUIRE(output.bunches.size()==1); const auto& bunch=output.bunches[0];
    CHECK(bunch.channel==7); CHECK(bunch.sequence==44); CHECK(bunch.reliable);
    CHECK(bunch.channel_type==2); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    CHECK(bunch.payload_bits==bits); CHECK(bunch.payload==Bytes{byte});
}
}
TEST_CASE("UDP trial spawn validation reads exact owned profile and empty achievements only after request and PRI delivery") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto owned=runtime_seed_owned_starter(f,account->ref);
    const auto wallet=f.store.wallet(account->ref); std::atomic<int> reads{0},achievement_reads{0};
    auto selected=runtime_start_config(); udp::UdpTrialRuntime* active=nullptr; session::SessionId id{};
    selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
        [&](const store::AccountRef& scope,std::string_view instance) {
            CHECK(scope==account->ref); CHECK(instance==owned.items[0].instance_id);
            CHECK(active->assignment().account_for_udp(id)==std::optional{account->ref});
            ++reads; return f.store.owned_starter_mech(scope,instance);
        }};
    selected.spawn_validation=runtime_spawn_validation_plan();
    selected.spawn_validation_source=udp::SpawnValidationSource{
        [&](const store::AccountRef& scope) {
            CHECK(scope==account->ref); CHECK(active->assignment().account_for_udp(id)==std::optional{account->ref});
            ++achievement_reads; return f.store.account_achievement_ids(scope);
        },true};
    udp::UdpTrialRuntime runtime(f.auth,selected); active=&runtime;
    selected.spawn_validation->validated_rpc_field_id=1;
    selected.spawn_validation_source->permanent_starter_instances=false;
    selected.spawn_validation_source->read_achievement_ids={};
    const auto dto=provision(runtime,login); id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client,id,key);
    const auto property=runtime_validation_selected(runtime,login,peer,client);
    CHECK(reads==1); CHECK(achievement_reads==0);
    CHECK_FALSE(runtime.diagnostics().spawn_validation_requested);
    CHECK_FALSE(runtime.diagnostics().spawn_validation_sent); CHECK_FALSE(runtime.diagnostics().spawn_validation_failed);
    peer.send(runtime.port(),runtime_spawn_validation_packet(client,8));
    CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty());
    CHECK(reads==1); CHECK(achievement_reads==0); // No callback before exact selected-property ACK.
    CHECK(runtime.diagnostics().spawn_validation_requested);
    CHECK_FALSE(runtime.diagnostics().spawn_validation_sent); CHECK_FALSE(runtime.diagnostics().spawn_validation_failed);
    peer.send(runtime.port(),runtime_links_ack(client,9,property.packet_ids));
    const auto validated=runtime_links_packets(peer,client,2000ms,1,true);
    runtime_validation_literal(validated,0x06,3);
    CHECK(reads==2); CHECK(achievement_reads==1); CHECK(runtime.assignment().authorize_udp(id));
    CHECK(runtime.diagnostics().spawn_validation_sent); CHECK_FALSE(runtime.diagnostics().spawn_validation_failed);
    peer.send(runtime.port(),runtime_links_ack(client,10,validated.packet_ids));
    CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty());
    CHECK(f.store.wallet(account->ref)==wallet);
    CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
    CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).attributes==owned.attributes);
    runtime.stop();
}
TEST_CASE("UDP trial spawn validation does not turn failed loads foreign presets or unknown achievements into success") {
    for (unsigned mode=0;mode<5;++mode) {
        CAPTURE(mode); Fixture f; const auto login=f.login();
        const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
        const auto owned=runtime_seed_owned_starter(f,account->ref); const auto wallet=f.store.wallet(account->ref);
        std::atomic<int> reads{0}; auto selected=runtime_start_config();
        selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
            [&](const store::AccountRef& scope,std::string_view instance) {
                const auto count=++reads; auto value=f.store.owned_starter_mech(scope,instance);
                if (count>1 && mode==0) throw std::runtime_error("synthetic validation load failure");
                if (count>1 && mode==1) value.items[0].instance_id="30000000-0000-0000-0000-000000000001";
                if (count>1 && mode==2) value.attributes["Preset.PrimaryWeapon_InstanceId_0"]=value.items[2].instance_id;
                return value;
            }};
        selected.spawn_validation=runtime_spawn_validation_plan();
        selected.spawn_validation_source=udp::SpawnValidationSource{[&](const store::AccountRef& scope) {
            auto ids=f.store.account_achievement_ids(scope);
            if (mode==3) ids.push_back("30000000-0000-0000-0000-000000000001"); // Unsupported synthetic input, no award grant.
            if (mode==4) f.verifier.valid=false;
            return ids;
        },true};
        udp::UdpTrialRuntime runtime(f.auth,selected); const auto dto=provision(runtime,login);
        const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
        const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
        Peer peer; session::Context client(session::Role::client,id,key);
        const auto property=runtime_validation_selected(runtime,login,peer,client);
        peer.send(runtime.port(),runtime_links_ack(client,8,property.packet_ids));
        peer.send(runtime.port(),runtime_spawn_validation_packet(client,9));
        if (mode<4) runtime_validation_literal(runtime_links_packets(peer,client,2000ms,1,true),0x07,4);
        else {
            CHECK(wait_until([&]{return runtime.diagnostics().closed;}));
            CHECK(runtime_links_packets(peer,client,200ms,1,true).bunches.empty());
        }
        CHECK(runtime.diagnostics().spawn_validation_requested);
        CHECK_FALSE(runtime.diagnostics().spawn_validation_sent);
        CHECK(runtime.diagnostics().spawn_validation_failed==(mode<4));
        CHECK(f.store.wallet(account->ref)==wallet);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
        CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).attributes==owned.attributes);
        runtime.stop();
    }
}
TEST_CASE("UDP trial spawn validation source pair permanence and existing plans are required before bind") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto owned=runtime_seed_owned_starter(f,account->ref); const auto wallet=f.store.wallet(account->ref);
    for (unsigned invalid=0;invalid<10;++invalid) {
        CAPTURE(invalid); auto selected=runtime_start_config();
        selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
            [&](const store::AccountRef& scope,std::string_view instance){return f.store.owned_starter_mech(scope,instance);}};
        selected.spawn_validation=runtime_spawn_validation_plan();
        selected.spawn_validation_source=udp::SpawnValidationSource{
            [&](const store::AccountRef& scope){return f.store.account_achievement_ids(scope);},true};
        switch(invalid) {
        case 0: selected.spawn_validation.reset(); break;
        case 1: selected.spawn_validation_source.reset(); break;
        case 2: selected.spawn_validation_source->read_achievement_ids={}; break;
        case 3: selected.spawn_validation_source->permanent_starter_instances=false; break;
        case 4: selected.start_mech.reset(); selected.start_mech_source.reset(); break;
        case 5: selected.controller.reset(); break;
        case 6: selected.pri.reset(); break;
        case 7: selected.pawn.reset(); break;
        case 8: selected.links.reset(); break;
        case 9: selected.publication.reset(); break;
        }
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
    }
    CHECK(f.store.wallet(account->ref)==wallet);
    CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
}


namespace {
Bytes runtime_selection_stage_packet(session::Context& client,std::uint16_t packet_id) {
    // Stage tag1/8, eligible presence1, int32 stage3 LE: 3+1+32 bits.
    wire::Bunch bunch; bunch.reliable=true; bunch.channel_type=2; bunch.channel=7;
    bunch.sequence=3; bunch.payload={0x39,0,0,0,0}; bunch.payload_bits=36;
    wire::Packet packet; packet.packet_id=packet_id;
    wire::PacketRecord record; record.bunch=std::move(bunch); packet.records.push_back(std::move(record));
    return client.encode(wire::encode_packet(packet,512));
}
}
TEST_CASE("UDP trial admission copies explicit stage policy and forwards validated stage3 to the same Pawn context") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto owned=runtime_seed_owned_starter(f,account->ref);
    const auto wallet=f.store.wallet(account->ref);
    auto selected=runtime_start_config();
    selected.start_mech_source=udp::StartMechSource{runtime_starter_types(),
        [&](const store::AccountRef& scope,std::string_view instance){return f.store.owned_starter_mech(scope,instance);}};
    selected.spawn_validation=runtime_spawn_validation_plan();
    selected.spawn_validation_source=udp::SpawnValidationSource{
        [&](const store::AccountRef& scope){return f.store.account_achievement_ids(scope);},true};
    selected.spawn_admission=udp::SpawnAdmissionDispatchPlan{1,3,8,{true,true,true,true,true,true}};
    for (unsigned invalid=0;invalid<3;++invalid) {
        CAPTURE(invalid); auto bad=selected;
        if (invalid==0) { bad.spawn_validation.reset(); bad.spawn_validation_source.reset(); }
        if (invalid==1) bad.spawn_validation_source.reset();
        if (invalid==2) bad.spawn_admission->eligibility.stage_argument_eligible=false;
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,bad),
            "UDP trial runtime could not start.",std::runtime_error);
    }
    udp::UdpTrialRuntime runtime(f.auth,selected);
    selected.spawn_admission->client_restart_rpc_field_id=0;
    selected.spawn_admission->eligibility.pawn_reference_eligible=false;
    const auto dto=provision(runtime,login);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    Peer peer; session::Context client(session::Role::client,id,key);
    const auto property=runtime_validation_selected(runtime,login,peer,client);
    peer.send(runtime.port(),runtime_links_ack(client,8,property.packet_ids));
    peer.send(runtime.port(),runtime_spawn_validation_packet(client,9));
    const auto validated=runtime_links_packets(peer,client,2000ms,1,true);
    runtime_validation_literal(validated,0x06,3);
    peer.send(runtime.port(),runtime_links_ack(client,10,validated.packet_ids));
    CHECK_FALSE(runtime.diagnostics().stage3_requested); CHECK_FALSE(runtime.diagnostics().client_restart_sent);
    peer.send(runtime.port(),runtime_selection_stage_packet(client,11));
    const auto restarted=runtime_links_packets(peer,client,2000ms,1,true);
    REQUIRE(restarted.bunches.size()==1); const auto& bunch=restarted.bunches[0];
    CHECK(bunch.channel==7); CHECK(bunch.sequence==45); CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    CHECK(bunch.payload_bits==16); CHECK(bunch.payload==Bytes{0x1b,0x01});
    CHECK(runtime.diagnostics().stage3_requested); CHECK(runtime.diagnostics().client_restart_sent);
    CHECK(runtime.assignment().authorize_udp(id));
    CHECK(f.store.wallet(account->ref)==wallet);
    CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).items==owned.items);
    CHECK(f.store.owned_starter_mech(account->ref,owned.items[0].instance_id).attributes==owned.attributes);
    runtime.stop();
}


namespace {
udp::PriIdentityDispatchPlan runtime_pri_identity_plan() {
    // Synthetic caller field2/bound8, no native field-ID acceptance claim.
    return {2,8,control::Endian::little,{true,true,true,true,true}};
}
std::array<std::uint32_t,4> runtime_identity_expected_words(std::string_view server_user_guid) {
    // Independent primitive hexadecimal expectation from the trusted SERVER
    // Auth reply, which SessionAssignment installs in SessionProvision.user_guid.
    // Do not call the property encoder or the Owner's GUID conversion.
    REQUIRE(server_user_guid.size()==36);
    std::array<std::uint32_t,4> words{};
    std::size_t digits=0;
    for (std::size_t i=0;i<server_user_guid.size();++i) {
        const auto c=server_user_guid[i];
        if (i==8 || i==13 || i==18 || i==23) { REQUIRE(c=='-'); continue; }
        REQUIRE(((c>='0' && c<='9') || (c>='a' && c<='f')));
        const auto digit=static_cast<std::uint32_t>(c<='9' ? c-'0' : c-'a'+10);
        words[digits/8]=(words[digits/8]<<4)|digit;
        ++digits;
    }
    REQUIRE(digits==32);
    return words;
}
std::uint32_t runtime_identity_wire_word(const wire::Bunch& bunch,std::size_t bit) {
    // Read an unaligned little-endian u32 directly from the wire fixture.
    // The loop decodes observed bytes; it never constructs the expected payload.
    std::uint32_t result=0;
    for (unsigned i=0;i<32;++i) {
        const auto position=bit+i;
        result|=static_cast<std::uint32_t>((bunch.payload.at(position/8)>>(position%8))&1U)<<i;
    }
    return result;
}
}
TEST_CASE("UDP trial PRI identity copies explicit policy and forwards server account words before links") {
    Fixture f;
    std::unique_ptr<udp::UdpTrialRuntime> runtime;
    {
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        selected.pri_identity=runtime_pri_identity_plan();
        runtime=std::make_unique<udp::UdpTrialRuntime>(f.auth,selected);
        selected.pri_identity->field_id=999; selected.pri_identity->exclusive_bound=0;
        selected.pri_identity->eligibility.primary_package_map=false;
        selected.pri_identity.reset(); selected.links.reset();
    } // Caller policy lifetime has ended before the real authorized handoff.
    const auto login=f.login(); const auto dto=provision(*runtime,login);
    REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    const auto expected_words=runtime_identity_expected_words(login.result.at("UserId").get<std::string>());
    Peer peer; session::Context client(session::Role::client,id,key);
    const auto opened=runtime_links_open(*runtime,login,peer,client);
    peer.send(runtime->port(),runtime_links_ack(client,5,opened.packet_ids));
    const auto identity=runtime_links_packets(peer,client,2000ms,1,true);
    REQUIRE(identity.bunches.size()==1); REQUIRE(identity.packet_ids.size()==1);
    const auto& bunch=identity.bunches[0];
    REQUIRE(bunch.channel==9); REQUIRE(bunch.payload_bits==131);
    REQUIRE(bunch.payload.size()==17);
    CHECK(bunch.sequence==93); CHECK(bunch.reliable); CHECK(bunch.channel_type==2);
    CHECK_FALSE(bunch.control); CHECK_FALSE(bunch.open); CHECK_FALSE(bunch.close);
    CHECK((bunch.payload[0]&7U)==2U); // Literal 3-bit property tag, no presence.
    for (std::size_t i=0;i<4;++i)
        CHECK(runtime_identity_wire_word(bunch,3+i*32)==expected_words[i]);
    CHECK((bunch.payload.back()&0xf8U)==0U);
    REQUIRE(wait_until([&] { return runtime->diagnostics().pri_identity_sent; }));
    CHECK_FALSE(runtime->diagnostics().pri_identity_acked);
    CHECK_FALSE(runtime->diagnostics().actor_links_sent);
    // ACK IDs come from the actual property packet, not arithmetic/future IDs.
    // No artificial waiting/retransmission gap before this delivery ACK.
    peer.send(runtime->port(),runtime_links_ack(client,6,identity.packet_ids));
    const auto linked=runtime_links_packets(peer,client,2000ms,2,true);
    REQUIRE(linked.bunches.size()==2);
    CHECK(linked.bunches[0].channel==7); CHECK(linked.bunches[0].sequence==43);
    CHECK(linked.bunches[0].payload==public_hex("9b804600")); CHECK(linked.bunches[0].payload_bits==30);
    CHECK(linked.bunches[1].channel==8); CHECK(linked.bunches[1].sequence==73);
    CHECK(linked.bunches[1].payload==public_hex("79004f00")); CHECK(linked.bunches[1].payload_bits==30);
    REQUIRE(wait_until([&] { return runtime->diagnostics().pri_identity_acked && runtime->diagnostics().actor_links_sent; }));
    CHECK(runtime->assignment().authorize_udp(id)); CHECK_FALSE(runtime->diagnostics().closed);
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime->stop(); CHECK_FALSE(runtime->running()); CHECK_FALSE(runtime->assignment().authorize_udp(id));
}
TEST_CASE("UDP trial PRI identity preflight preserves bind rollback and authenticated authority") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto wallet=f.store.wallet(account->ref);
    std::uint16_t port=0; { Peer reservation; port=reservation.port(); }
    for (unsigned invalid=0;invalid<13;++invalid) {
        CAPTURE(invalid);
        auto selected=config(port); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        selected.pri_identity=runtime_pri_identity_plan();
        switch (invalid) {
        case 0: selected.publication.reset(); break;
        case 1: selected.controller.reset(); break;
        case 2: selected.pri.reset(); break;
        case 3: selected.pawn.reset(); break;
        case 4: selected.links.reset(); break;
        case 5: selected.pri_identity->exclusive_bound=0; break;
        case 6: selected.pri_identity->field_id=8; break;
        case 7: selected.pri_identity->integer_order=static_cast<control::Endian>(255); break;
        case 8: selected.pri_identity->eligibility.primary_package_map=false; break;
        case 9: selected.pri_identity->eligibility.pri_cache_correct=false; break;
        case 10: selected.pri_identity->eligibility.unique_id_property_eligible=false; break;
        case 11: selected.pri_identity->eligibility.unique_net_id_serializer_correct=false; break;
        case 12: selected.pri_identity->eligibility.integer_archive_order_correct=false; break;
        }
        CHECK(udp::validate_trial_config(selected)); // Remains identity-only.
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        { Peer rebound(port); CHECK(rebound.port()==port); }
        CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    }
    auto selected=config(port); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
    selected.pri_identity=runtime_pri_identity_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.running()); CHECK(runtime.port()==port);
    CHECK(provision(runtime,login).status==200); // No failed preflight consumed the first installation.
    CHECK(f.store.wallet(account->ref)==wallet); runtime.stop();
}


namespace {
udp::SelectionBootstrapPlan runtime_selection_bootstrap_plan() {
    const std::array<std::uint32_t,4> package_guid={0x01020304U,0x12345678U,0x80000000U,0xffffffffU};
    // FRoot's synthetic three-object map: CDO local0, GameClass local1, HUDClass
    // local2. PackageExport0 is not an actor-channel0/null reference.
    udp::InitialPawnPlan gri{{10,111,0,false},package_guid,0,{0,0,0},std::nullopt};
    return {gri,package_guid,1,package_guid,2,31,73,42,294,
        {true,true,true,true,true,true,true,true,true,true,true,true}};
}
struct RuntimeBootstrapPackets {
    RuntimeLinksPackets properties;
    RuntimeLinksPackets openings;
};
RuntimeBootstrapPackets runtime_bootstrap_packets(Peer& peer,session::Context& client,
    std::chrono::milliseconds budget,std::size_t expected_properties,std::size_t expected_openings=0) {
    RuntimeBootstrapPackets output;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto datagram=peer.receive(20ms); if (!datagram) continue;
        const auto value=wire::decode_packet(client.decode(*datagram),512);
        for (const auto& record:value.records) {
            if (record.is_ack || record.bunch.channel==0) continue;
            auto& destination=record.bunch.open ? output.openings : output.properties;
            destination.bunches.push_back(record.bunch); destination.packet_ids.push_back(value.packet_id);
        }
        if (output.properties.bunches.size()>=expected_properties &&
            output.openings.bunches.size()>=expected_openings) break;
    }
    return output;
}
void runtime_bootstrap_append_acks(std::vector<std::uint16_t>& ids,const RuntimeLinksPackets& packets) {
    ids.insert(ids.end(),packets.packet_ids.begin(),packets.packet_ids.end());
}
}
TEST_CASE("UDP trial selection bootstrap copies policy and orders fourth actor GameClass and HUD by real ACKs") {
    Fixture f; std::unique_ptr<udp::UdpTrialRuntime> runtime;
    {
        auto selected=config(); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        selected.selection_bootstrap=runtime_selection_bootstrap_plan();
        runtime=std::make_unique<udp::UdpTrialRuntime>(f.auth,selected);
        selected.selection_bootstrap->gri.channel.channel=7;
        selected.selection_bootstrap->game_class_field_id=99;
        selected.selection_bootstrap->eligibility.hud_parameter_eligible=false;
        selected.selection_bootstrap.reset(); selected.links.reset();
    }
    Peer peer;
    peer.send(runtime->port(),Bytes(64,0)); // No installed authenticated Context.
    CHECK_FALSE(peer.receive(20ms));
    CHECK(runtime->diagnostics().authenticated_datagrams==0);
    CHECK_FALSE(runtime->diagnostics().gri_sent); CHECK_FALSE(runtime->diagnostics().game_class_sent);
    CHECK_FALSE(runtime->diagnostics().hud_sent);
    const auto login=f.login(); const auto dto=provision(*runtime,login); REQUIRE(dto.status==200);
    const auto id=decode_base64url<8>(dto.result.at("SessionId").get<std::string>());
    const auto key=decode_base64url<32>(dto.result.at("SessionKey").get<std::string>());
    const auto account=f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet=f.store.wallet(account->ref);
    session::Context client(session::Role::client,id,key);
    runtime_publish(*runtime,login,peer,client);
    peer.send(runtime->port(),packet(client,4,5,
        public_hex("08040302017856341200000080ffffffff0200000009")));
    const auto opened=runtime_bootstrap_packets(peer,client,2000ms,0,4);
    REQUIRE(opened.openings.bunches.size()==4); REQUIRE(opened.openings.packet_ids.size()==4);
    CHECK(opened.properties.bunches.empty());
    constexpr std::array<std::uint16_t,4> channels={7,9,8,10};
    constexpr std::array<std::uint16_t,4> sequences={42,92,72,112};
    for (std::size_t i=0;i<4;++i) {
        REQUIRE(opened.openings.bunches[i].channel==channels[i]);
        CHECK(opened.openings.bunches[i].sequence==sequences[i]);
        CHECK(opened.openings.bunches[i].open); CHECK(opened.openings.bunches[i].reliable);
    }
    CHECK(opened.openings.bunches[3].payload==public_hex("000000004005"));
    CHECK(opened.openings.bunches[3].payload_bits==43);
    CHECK_FALSE(runtime->diagnostics().game_class_sent); CHECK_FALSE(runtime->diagnostics().hud_sent);
    // GRI ACK is independent of the other three opening ACKs. Preserve all
    // opening copies actually observed while collecting the GameClass payload.
    peer.send(runtime->port(),runtime_links_ack(client,5,{opened.openings.packet_ids[3]}));
    const auto game_class=runtime_bootstrap_packets(peer,client,2000ms,1);
    REQUIRE(game_class.properties.bunches.size()==1); const auto& gri=game_class.properties.bunches[0];
    CHECK(gri.channel==10); CHECK(gri.sequence==113); CHECK(gri.reliable); CHECK(gri.channel_type==2);
    CHECK_FALSE(gri.open); CHECK_FALSE(gri.close);
    CHECK(gri.payload_bits==38); CHECK(gri.payload==public_hex("9f00000000"));
    CHECK_FALSE(runtime->diagnostics().hud_sent);
    std::vector<std::uint16_t> ready={opened.openings.packet_ids[0],opened.openings.packet_ids[1],opened.openings.packet_ids[2]};
    runtime_bootstrap_append_acks(ready,game_class.openings);
    runtime_bootstrap_append_acks(ready,game_class.properties);
    peer.send(runtime->port(),runtime_links_ack(client,6,ready));
    const auto linked=runtime_bootstrap_packets(peer,client,2000ms,2);
    REQUIRE(linked.properties.bunches.size()==2);
    CHECK(linked.properties.bunches[0].channel==7); CHECK(linked.properties.bunches[0].sequence==43);
    CHECK(linked.properties.bunches[0].payload==public_hex("9b804600"));
    CHECK(linked.properties.bunches[1].channel==8); CHECK(linked.properties.bunches[1].sequence==73);
    CHECK(linked.properties.bunches[1].payload==public_hex("79004f00"));
    CHECK_FALSE(runtime->diagnostics().hud_sent); // GameClass ACK does not ACK PC/Pawn data.
    ready.clear(); runtime_bootstrap_append_acks(ready,linked.openings);
    runtime_bootstrap_append_acks(ready,linked.properties);
    peer.send(runtime->port(),runtime_links_ack(client,7,ready));
    const auto hud=runtime_bootstrap_packets(peer,client,2000ms,1);
    REQUIRE(hud.properties.bunches.size()==1); const auto& controller=hud.properties.bunches[0];
    CHECK(controller.channel==7); CHECK(controller.sequence==44);
    CHECK(controller.reliable); CHECK(controller.channel_type==2);
    CHECK_FALSE(controller.open); CHECK_FALSE(controller.close);
    CHECK(controller.payload_bits==41); CHECK(controller.payload==public_hex("2a0900000000"));
    peer.send(runtime->port(),runtime_links_ack(client,8,hud.properties.packet_ids));
    REQUIRE(wait_until([&] { return runtime->diagnostics().game_class_acked && runtime->diagnostics().hud_acked; }));
    CHECK(runtime_bootstrap_packets(peer,client,200ms,1).properties.bunches.empty());
    CHECK(runtime->assignment().authorize_udp(id)); CHECK_FALSE(runtime->diagnostics().closed);
    CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    runtime->stop(); CHECK_FALSE(runtime->running()); CHECK_FALSE(runtime->assignment().authorize_udp(id));
}
TEST_CASE("UDP trial selection bootstrap validates prerequisites channel ranges and explicit schema before startup") {
    Fixture f; const auto login=f.login(); const auto account=f.store.find_account("76561197960265729");
    REQUIRE(account); const auto wallet=f.store.wallet(account->ref);
    std::uint16_t port=0; { Peer reservation; port=reservation.port(); }
    for (unsigned invalid=0;invalid<29;++invalid) {
        CAPTURE(invalid);
        auto selected=config(port); selected.publication=public_publication();
        selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
        selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
        selected.selection_bootstrap=runtime_selection_bootstrap_plan();
        auto& plan=*selected.selection_bootstrap;
        switch (invalid) {
        case 0: selected.publication.reset(); break;
        case 1: selected.controller.reset(); break;
        case 2: selected.pri.reset(); break;
        case 3: selected.pawn.reset(); break;
        case 4: selected.links.reset(); break;
        case 5: plan.gri.channel.channel=7; break;
        case 6: plan.gri.channel.channel=9; break;
        case 7: plan.gri.channel.channel=8; break;
        case 8: plan.gri.channel.channel=0; break;
        case 9: plan.gri.channel.channel=2048; break;
        case 10: plan.gri.archetype_local_net_index=-1; break;
        case 11: plan.game_class_local_net_index=-1; break;
        case 12: plan.hud_local_net_index=-1; break;
        case 13: plan.gri_exclusive_bound=0; break;
        case 14: plan.game_class_field_id=73; break;
        case 15: plan.controller_exclusive_bound=0; break;
        case 16: plan.client_set_hud_field_id=294; break;
        case 17: plan.eligibility.primary_package_map=false; break;
        case 18: plan.eligibility.common_gri_prefix_correct=false; break;
        case 19: plan.eligibility.gri_cdo_correct=false; break;
        case 20: plan.eligibility.gri_cache_correct=false; break;
        case 21: plan.eligibility.game_class_property_eligible=false; break;
        case 22: plan.eligibility.game_class_property_class_correct=false; break;
        case 23: plan.eligibility.game_class_meta_class_correct=false; break;
        case 24: plan.eligibility.controller_cache_correct=false; break;
        case 25: plan.eligibility.client_set_hud_rpc_eligible=false; break;
        case 26: plan.eligibility.hud_parameter_eligible=false; break;
        case 27: plan.eligibility.hud_parameter_class_correct=false; break;
        case 28: plan.eligibility.hud_meta_class_correct=false; break;
        }
        CHECK(udp::validate_trial_config(selected));
        CHECK_THROWS_WITH_AS(udp::UdpTrialRuntime(f.auth,selected),
            "UDP trial runtime could not start.",std::runtime_error);
        { Peer rebound(port); CHECK(rebound.port()==port); }
        CHECK(f.store.wallet(account->ref)==wallet); CHECK_FALSE(f.store.starter_items(account->ref));
    }
    auto selected=config(port); selected.publication=public_publication();
    selected.controller=runtime_controller_plan(); selected.pawn=runtime_pawn_plan();
    selected.pri=runtime_pri_plan(); selected.links=runtime_links_plan();
    selected.selection_bootstrap=runtime_selection_bootstrap_plan();
    udp::UdpTrialRuntime runtime(f.auth,selected); CHECK(runtime.running()); CHECK(runtime.port()==port);
    CHECK(provision(runtime,login).status==200); // Rollback preserves the first valid installation.
    CHECK(f.store.wallet(account->ref)==wallet); runtime.stop();
}
