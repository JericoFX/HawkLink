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
    return Json{{"OwnerGuid", user}, {"Region", "EU"}, {"GameVersion", "1.6.0.154767"},
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
                                              std::chrono::milliseconds budget=2000ms) {
    std::vector<wire::Bunch> result;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto data=peer.receive(20ms); if (!data) continue;
        const auto value=wire::decode_packet(client.decode(*data),512);
        for (const auto& record:value.records)
            if (!record.is_ack && record.bunch.channel!=0) result.push_back(record.bunch);
        if (!result.empty()) break;
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
