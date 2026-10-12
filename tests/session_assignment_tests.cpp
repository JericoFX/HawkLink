#include "hawken/session_assignment.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using hawken::assignment::AssignmentPolicy;
using hawken::assignment::Destination;
using hawken::assignment::SessionAssignmentService;
using hawken::assignment::SessionProvision;
using hawken::assignment::UdpSessionLease;
using hawken::auth::AuthReply;
using Json = nlohmann::json;
constexpr std::uint64_t identity = 76561197960265729ULL;
constexpr std::string_view server_guid = "12345678-1234-5678-9012-123456789abc";
constexpr std::string_view foreign_guid = "abcdef01-2345-6789-abcd-ef0123456789";
constexpr std::string_view ad_guid = "00112233-4455-6677-8899-aabbccddeeff";
constexpr std::string_view ads = "/hawkenClientMatchmakingAdvertisements";
constexpr hawken::session::SessionId public_id{0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77};
constexpr hawken::session::SessionKey public_key{
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};

// Only Steam verification is replaced. Auth, SQLite and UDP Context are real.
// All ticket/identity/session bytes here are public isolated fixture material.
class FakeVerifier final : public hawken::auth::SteamVerifier {
public:
    bool active = false;
    std::uint64_t selected_identity = identity;
    mutable int checks = 0;
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket,
                active ? selected_identity : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        ++checks;
        return active && id == selected_identity;
    }
    void end_session(std::uint64_t id) override { if (id == selected_identity) active = false; }
};
struct Registry {
    int installs = 0;
    int retired = 0;
    int live = 0;
    int peak = 0;
    int mode = 0; // 1=null, 2=throw after private allocation, 3=inactive lease.
    bool active = true;
    std::vector<SessionProvision> provisions;
    hawken::session::Context* context = nullptr;
};
class Lease final : public UdpSessionLease {
public:
    Lease(Registry& registry, const SessionProvision& provision)
        : registry_(registry), context_(hawken::session::Role::server, provision.id, provision.key) {
        ++registry_.live;
        registry_.peak = std::max(registry_.peak, registry_.live);
        registry_.context = &context_;
    }
    ~Lease() override {
        --registry_.live;
        ++registry_.retired;
        if (registry_.context == &context_) registry_.context = nullptr;
    }
    bool active() const noexcept override { return registry_.active && registry_.mode != 3; }
private:
    Registry& registry_;
    hawken::session::Context context_;
};
struct Fixture {
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store;
    FakeVerifier verifier;
    std::int64_t auth_now = 1000;
    std::int64_t monotonic_now = 100;
    std::int64_t utc_now = 1704067200; // 2024-01-01T00:00:00Z, separate domain.
    bool monotonic_throws = false;
    bool utc_throws = false;
    bool bound = true;
    bool bound_throws = false;
    int entropy_calls = 0;
    int fail_entropy_call = 0;
    bool entropy_throws = false;
    int id_draws = 0;
    int ad_draws = 0;
    Registry registry;
    hawken::auth::AuthService auth{store, verifier, [this] { return auth_now; }};
    Destination destination{std::string(server_guid), "127.0.0.1", 27015, "EU", "1.6.0.154767"};
    AssignmentPolicy policy{30, 1200, 800};
    std::unique_ptr<SessionAssignmentService> service;
    explicit Fixture(std::string server_id = "assignment-server")
        : store(temporary.path() / "accounts.sqlite", std::move(server_id)) { construct(); }
    void construct() {
        service = std::make_unique<SessionAssignmentService>(auth, destination, policy,
            [this] {
                if (monotonic_throws) throw std::runtime_error("private-clock");
                return monotonic_now;
            }, [this] {
                if (utc_throws) throw std::runtime_error("private-utc");
                return utc_now;
            }, [this](std::span<std::uint8_t> bytes) {
                ++entropy_calls;
                if (entropy_calls == fail_entropy_call) {
                    if (entropy_throws) throw std::runtime_error("private-random");
                    return false;
                }
                if (bytes.size() == public_id.size()) {
                    std::copy(public_id.begin(), public_id.end(), bytes.begin());
                    bytes[0] = static_cast<std::uint8_t>(id_draws++);
                } else if (bytes.size() == public_key.size()) {
                    std::copy(public_key.begin(), public_key.end(), bytes.begin());
                } else if (bytes.size() == 16) {
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        bytes[i] = static_cast<std::uint8_t>(i * 17);
                    bytes[0] = static_cast<std::uint8_t>(ad_draws++);
                } else { return false; }
                return true;
            }, [this](const Destination& declared) {
                if (bound_throws) throw std::runtime_error("private-registry");
                return bound && declared.server_guid == destination.server_guid
                    && declared.ip == destination.ip && declared.port == destination.port;
            }, [this](const SessionProvision& provision) -> std::unique_ptr<UdpSessionLease> {
                ++registry.installs;
                registry.provisions.push_back(provision);
                if (registry.mode == 1) return nullptr;
                auto lease = std::make_unique<Lease>(registry, provision);
                if (registry.mode == 2) throw std::runtime_error("private-install");
                return lease;
            });
    }
    AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
std::string user_guid(const AuthReply& login) { return login.result.at("UserId").get<std::string>(); }
std::string header(const AuthReply& login) { return "Basic " + login.result.at("AccessGrant").get<std::string>(); }
std::string ad_target(std::string_view guid = ad_guid) { return std::string(ads) + "/" + std::string(guid); }
Json request(const AuthReply& login, bool requested = true) {
    Json result{{"OwnerGuid", user_guid(login)}, {"Region", "EU"},
                {"GameVersion", "1.6.0.154767"}, {"Users", Json::array({user_guid(login)})}};
    if (requested) result["RequestedServerGuid"] = server_guid;
    return result;
}
AuthReply post(Fixture& f, const AuthReply& login, bool requested = true) {
    return f.service->handle("POST", ads, header(login), request(login, requested).dump());
}
AuthReply session(Fixture& f, const AuthReply& login) {
    return f.service->handle("GET", "/session", header(login));
}
void failure(const AuthReply& reply, int status) {
    CHECK(reply.status == status);
    CHECK(reply.result.is_null());
    const std::string_view message = status == 400 ? "Invalid session assignment request."
        : status == 401 ? "Authentication required."
        : status == 403 ? "Access was rejected."
        : status == 404 ? "Resource not found."
        : status == 405 ? "Method not allowed."
        : status == 503 ? "Session assignment is not available."
        : "Request could not be processed.";
    CHECK(reply.message == message);
    CHECK(reply.message.find("private-") == std::string::npos);
    CHECK(reply.message.find("76561197960265729") == std::string::npos);
    CHECK(reply.message.find("accounts.sqlite") == std::string::npos);
    CHECK(reply.message.find("ABEiM0RVZnc") == std::string::npos);
}
void selected(Fixture& f, const AuthReply& login) {
    const auto reply = f.service->select(header(login), server_guid);
    REQUIRE(reply.status == 200);
    CHECK(reply.result.is_null());
}
Json expected_session(const AuthReply& login, std::string_view advertisement = {}) {
    // Independent literal wire oracle. Not generated by meteor::session_json.
    return { {"UserGuid", user_guid(login)}, {"SessionId", "ABEiM0RVZnc"},
        {"SessionKey", "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"},
        {"AssignedServerIp", "127.0.0.1"}, {"AssignedServerGuid", server_guid},
        {"ClientAdvertisementGuid", advertisement}, {"AssignedServerPort", 27015} };
}
}

TEST_CASE("assignment matches only exact session and legacy targets") {
    Fixture f;
    for (const auto& target : {std::string("/session"), std::string(ads), ad_target()})
        CHECK(f.service->supports_target(target));
    for (const auto& target : {"/session/", "/session?serverGuid=x", "/Session", "/%73ession",
        "/hawkenClientMatchmakingAdvertisements/", "/hawkenClientMatchmakingAdvertisements?x=1",
        "/hawkenClientMatchmakingAdvertisements/00112233-4455-6677-8899-AABBCCDDEEFF",
        "/hawkenClientMatchmakingAdvertisements/00000000-0000-0000-0000-000000000000",
        "/hawkenClientMatchmakingAdvertisements/00112233-4455-6677-8899-aabbccddeeff?x=1"}) {
        CHECK_FALSE(f.service->supports_target(target));
        failure(f.service->handle("GET", target, ""), 404);
    }
}
TEST_CASE("assignment rejects wrong methods and GET bodies without publishing") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    for (const auto method : {"POST", "PUT", "DELETE", "HEAD", "get"})
        failure(f.service->handle(method, "/session", header(login)), 405);
    failure(f.service->handle("GET", ads, header(login)), 405);
    failure(f.service->handle("POST", ad_target(), header(login), "{}"), 405);
    failure(f.service->handle("GET", "/session", header(login), "{}"), 400);
    failure(f.service->handle("GET", ad_target(), header(login), " "), 400);
    CHECK(f.entropy_calls == 0); CHECK(f.registry.installs == 0);
}
TEST_CASE("assignment requires auth and selection and never chooses from session query") {
    Fixture f;
    failure(f.service->select("", server_guid), 401);
    failure(f.service->handle("GET", "/session", ""), 401);
    const auto login = f.login(); REQUIRE(login.status == 200);
    failure(session(f, login), 404);
    failure(post(f, login, false), 404);
    failure(f.service->handle("GET", "/session?serverGuid=" + std::string(server_guid), header(login)), 404);
    failure(f.service->select(header(login), foreign_guid), 404);
    failure(f.service->select(header(login), "bad-guid"), 400);
    CHECK(f.registry.installs == 0);
}
TEST_CASE("assignment direct selection yields exact seven-field session without an advertisement") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    const auto reply = session(f, login);
    REQUIRE(reply.status == 200);
    CHECK(reply.result == expected_session(login));
    CHECK(reply.result.at("AssignedServerPort").is_number_integer());
    CHECK(reply.result.at("SessionId").get<std::string>().size() == 11);
    CHECK(reply.result.at("SessionKey").get<std::string>().size() == 43);
    CHECK(f.registry.installs == 1); CHECK(f.registry.live == 1);
    REQUIRE(f.registry.provisions.size() == 1);
    const auto& provision = f.registry.provisions.front();
    CHECK(provision.id == public_id); CHECK(provision.key == public_key);
    CHECK(provision.user_guid == user_guid(login));
    CHECK(provision.account == f.store.find_account("76561197960265729")->ref);
    CHECK(provision.expires_at_monotonic == 130);
    CHECK(f.service->authorize_udp(public_id));
}
TEST_CASE("assignment re-read and repeated selection preserve the real Context counters") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    REQUIRE(f.registry.context != nullptr);
    hawken::session::Context client(hawken::session::Role::client, public_id, public_key);
    constexpr std::array<std::uint8_t, 3> payload{1,2,3};
    const auto packet = client.encode(payload);
    CHECK(f.registry.context->decode(packet) == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    const auto response = f.registry.context->encode(payload);
    CHECK(client.decode(response) == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    const auto sent = f.registry.context->last_sent(), received = f.registry.context->last_received();
    const auto* original = f.registry.context;
    const auto first = session(f, login);
    for (int i = 0; i < 8; ++i) {
        selected(f, login);
        CHECK(session(f, login).result == first.result);
        CHECK(f.registry.context == original);
        CHECK(f.registry.context->last_sent() == sent);
        CHECK(f.registry.context->last_received() == received);
    }
    CHECK(f.entropy_calls == 2); CHECK(f.registry.installs == 1);
}
TEST_CASE("assignment legacy POST and poll are exact idempotent DTOs sharing the session") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    const auto created = post(f, login); REQUIRE(created.status == 200);
    CHECK(created.result == Json(ad_guid));
    Json expected{{"Guid", ad_guid}, {"TimeCreated", "2024-01-01T00:00:00Z"},
        {"OwnerGuid", user_guid(login)}, {"Region", "EU"}, {"GameVersion", "1.6.0.154767"},
        {"Users", Json::array({user_guid(login)})}, {"RequestedServerGuid", server_guid},
        {"Status", 1}, {"SkillRatingAverage", 1200}, {"SkillRatingSearchMin", 800},
        {"AssignedServerGuid", server_guid}, {"AssignedServerIp", "127.0.0.1"},
        {"AssignedServerPort", 27015}};
    const auto polled = f.service->handle("GET", ad_target(), header(login));
    REQUIRE(polled.status == 200);
    CHECK(polled.result == expected);
    CHECK(session(f, login).result == expected_session(login, ad_guid));
    for (int i = 0; i < 20; ++i) CHECK(post(f, login).result == created.result);
    CHECK(f.entropy_calls == 3); CHECK(f.registry.installs == 1);
    CHECK(f.registry.live == 1); CHECK(f.registry.peak == 1);
    CHECK(SessionAssignmentService::max_assignments == 1);
    CHECK(SessionAssignmentService::max_advertisements == 1);
}
TEST_CASE("assignment concurrent equivalent POSTs keep one handoff and advertisement") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    const auto authorization = header(login), body = request(login).dump();
    std::array<AuthReply, 8> replies;
    std::array<bool, 8> threw{};
    std::vector<std::thread> callers;
    for (std::size_t i = 0; i < replies.size(); ++i) {
        callers.emplace_back([&, i] {
            try { replies[i] = f.service->handle("POST", ads, authorization, body); }
            catch (...) { threw[i] = true; }
        });
    }
    for (auto& caller : callers) caller.join();
    // Assertions run only after all threads join, never inside callbacks.
    for (std::size_t i = 0; i < replies.size(); ++i) {
        CHECK_FALSE(threw[i]); CHECK(replies[i].status == 200);
        CHECK(replies[i].result == Json(ad_guid));
    }
    CHECK(f.registry.live == 1); CHECK(f.registry.peak == 1);
    CHECK(f.registry.installs == 1); CHECK(f.entropy_calls == 3);
}
TEST_CASE("assignment omitted requested GUID uses prior selection without recreating Context") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    const auto* original = f.registry.context;
    const auto created = post(f, login, false); REQUIRE(created.status == 200);
    const auto polled = f.service->handle("GET", ad_target(), header(login));
    REQUIRE(polled.status == 200);
    CHECK_FALSE(polled.result.contains("RequestedServerGuid"));
    CHECK(polled.result.at("AssignedServerGuid") == std::string(server_guid));
    CHECK(post(f, login, false).result == created.result);
    CHECK(f.registry.context == original); CHECK(f.registry.installs == 1);
}
TEST_CASE("assignment POST rejects malformed duplicate unknown and oversized fields") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    std::vector<std::string> invalid{"", "null", "[]", "{}", "{", std::string(4097, ' ')};
    for (const auto key : {"OwnerGuid", "Region", "GameVersion", "Users"}) {
        auto body = request(login); body.erase(key); invalid.push_back(body.dump());
        body = request(login); body[key] = 5; invalid.push_back(body.dump());
    }
    for (const auto key : {"PartyGuid", "GameType", "RequestedMaps", "Ready", "SkillRatingAverage"}) {
        auto body = request(login); body[key] = "unsupported"; invalid.push_back(body.dump());
    }
    for (const auto& users : {Json::array(), Json::array({user_guid(login), user_guid(login)}),
                            Json::array({foreign_guid}), Json::array({7})}) {
        auto body = request(login); body["Users"] = users; invalid.push_back(body.dump());
    }
    auto duplicate = request(login).dump();
    duplicate.pop_back(); duplicate += ",\"Region\":\"EU\"}"; invalid.push_back(duplicate);
    for (const auto& guid_value : {Json(""), Json("ABCDEF01-2345-6789-ABCD-EF0123456789"),
                                  Json(7), Json(nullptr)}) {
        auto body = request(login); body["RequestedServerGuid"] = guid_value;
        invalid.push_back(body.dump());
    }
    for (const auto& body : invalid) failure(f.service->handle("POST", ads, header(login), body), 400);
    CHECK(f.entropy_calls == 0); CHECK(f.registry.installs == 0);
}
TEST_CASE("assignment POST requires bound metadata and exact owner") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    for (const auto key : {"Region", "GameVersion", "RequestedServerGuid"}) {
        auto body = request(login); body[key] = key == std::string_view("RequestedServerGuid")
            ? std::string(foreign_guid) : "wrong";
        failure(f.service->handle("POST", ads, header(login), body.dump()), 400);
    }
    auto body = request(login); body["OwnerGuid"] = foreign_guid;
    body["Users"] = Json::array({foreign_guid});
    failure(f.service->handle("POST", ads, header(login), body.dump()), 401);
    CHECK(f.registry.installs == 0);
}
TEST_CASE("assignment rejects unbound destination and binding loss retires session") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    f.bound = false; failure(f.service->select(header(login), server_guid), 503);
    failure(post(f, login), 503);
    CHECK(f.registry.installs == 0);
    f.bound = true; selected(f, login);
    f.bound = false;
    CHECK_FALSE(f.service->authorize_udp(public_id));
    CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
    failure(session(f, login), 503);
}
TEST_CASE("assignment handoff null inactive and throw never publish partial state") {
    for (const int mode : {1,2,3}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        f.registry.mode = mode;
        failure(post(f, login), mode == 2 ? 500 : 503);
        CHECK(f.registry.live == 0);
        CHECK_FALSE(f.service->authorize_udp(public_id));
        failure(session(f, login), 404);
        failure(f.service->handle("GET", ad_target(), header(login)), 404);
        f.registry.mode = 0;
        const auto retry = post(f, login); REQUIRE(retry.status == 200);
        CHECK(f.registry.live == 1);
    }
}
TEST_CASE("assignment entropy failure or throw rolls back before any DTO") {
    for (const bool throws : {false,true}) {
        for (const int failed_draw : {1,2,3}) {
            Fixture f;
            const auto login = f.login(); REQUIRE(login.status == 200);
            f.fail_entropy_call = failed_draw; f.entropy_throws = throws;
            failure(post(f, login), 500);
            CHECK(f.registry.live == 0);
            failure(session(f, login), 404);
            f.fail_entropy_call = 0;
            REQUIRE(post(f, login).status == 200);
            CHECK(f.registry.live == 1);
        }
    }
}
TEST_CASE("assignment expiry is monotonic and exact while UTC changes only presentation") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    f.utc_now += 86400;
    f.monotonic_now = 129;
    CHECK(f.service->authorize_udp(public_id));
    CHECK(session(f, login).status == 200);
    f.monotonic_now = 130;
    CHECK_FALSE(f.service->authorize_udp(public_id));
    CHECK(f.registry.live == 0);
    failure(session(f, login), 404);
}
TEST_CASE("assignment clock failures and regression fail closed and redact errors") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    f.monotonic_throws = true; failure(post(f, login), 500);
    f.monotonic_throws = false; f.utc_throws = true; failure(post(f, login), 500);
    CHECK(f.registry.live == 0);
    f.utc_throws = false; f.utc_now = -1; failure(post(f, login), 500);
    f.utc_now = 253402300800LL; failure(post(f, login), 500);
    f.utc_now = 1704067200; selected(f, login);
    f.monotonic_now = 99;
    CHECK_FALSE(f.service->authorize_udp(public_id));
    CHECK(f.registry.live == 0);
    failure(session(f, login), 500);
}
TEST_CASE("assignment request byte ceiling permits exactly 4096 bytes") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    auto body = request(login).dump();
    REQUIRE(body.size() < SessionAssignmentService::max_request_bytes);
    body.append(SessionAssignmentService::max_request_bytes - body.size(), ' ');
    CHECK(f.service->handle("POST", ads, header(login), body).status == 200);
    body += ' ';
    failure(f.service->handle("POST", ads, header(login), body), 400);
    CHECK(f.registry.live == 1); CHECK(f.registry.installs == 1);
}
TEST_CASE("assignment configured TTL endpoints accept the explicit policy boundaries") {
    for (const std::int64_t ttl : {1LL, 900LL}) {
        Fixture f;
        f.service.reset(); f.policy.lifetime_seconds = ttl;
        f.destination.port = ttl == 1 ? 1 : 65535;
        f.construct();
        const auto login = f.login(); REQUIRE(login.status == 200);
        selected(f, login);
        CHECK(session(f, login).result.at("AssignedServerPort") == f.destination.port);
        f.monotonic_now = 100 + ttl - 1;
        CHECK(f.service->authorize_udp(public_id));
        ++f.monotonic_now;
        CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(f.registry.live == 0);
    }
}
TEST_CASE("assignment injected registry failures never expose credentials") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    f.bound_throws = true;
    failure(post(f, login), 500); CHECK(f.registry.live == 0);
    f.bound_throws = false; selected(f, login);
    f.monotonic_throws = true;
    failure(session(f, login), 500); CHECK(f.registry.live == 0);
}
TEST_CASE("assignment rechecks revocation ban and grant expiry for HTTP and UDP") {
    for (const int mode : {0,1,2}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        selected(f, login);
        if (mode == 0) f.verifier.active = false;
        if (mode == 1) {
            const auto account = f.store.find_account("76561197960265729"); REQUIRE(account.has_value());
            f.store.set_ban(account->ref, {1000, std::nullopt, "fixture"});
        }
        if (mode == 2) f.auth_now = 1900;
        // HTTP sees 403 on the ban's first check; auth then clears the grant.
        failure(session(f, login), mode == 1 ? 403 : 401);
        CHECK_FALSE(f.service->authorize_udp(public_id)); CHECK(f.registry.live == 0);
    }
}
TEST_CASE("assignment stored grant is invalidated by logout and cannot be revived") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    const auto logout = f.auth.logout(user_guid(login), header(login),
        Json{{"AccessGrant", login.result.at("AccessGrant")}}.dump());
    REQUIRE(logout.status == 200);
    CHECK_FALSE(f.service->authorize_udp(public_id)); CHECK(f.registry.live == 0);
    failure(session(f, login), 401);
}
TEST_CASE("assignment replacement grant or account retires old bounded state") {
    for (const bool another_account : {false,true}) {
        Fixture f;
        const auto first = f.login(); REQUIRE(first.status == 200);
        REQUIRE(post(f, first).status == 200);
        if (another_account) f.verifier.selected_identity = identity + 1;
        const auto second = f.login(); REQUIRE(second.status == 200);
        CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(f.registry.live == 0);
        failure(session(f, second), 404);
        failure(f.service->handle("GET", ad_target(), header(second)), 404);
        selected(f, second);
        CHECK(f.registry.live == 1); CHECK(f.registry.peak == 1);
        CHECK(f.registry.retired == 1); CHECK(f.registry.installs == 2);
        failure(session(f, first), 401);
        CHECK(session(f, second).status == 200);
    }
}
TEST_CASE("assignment foreign credentials and unknown IDs do not disturb an active session") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    REQUIRE(post(f, login).status == 200);
    failure(f.service->handle("GET", ad_target(foreign_guid), header(login)), 404);
    failure(f.service->handle("GET", ad_target(), "Basic foreign"), 401);
    auto different = public_id; ++different[7];
    CHECK_FALSE(f.service->authorize_udp(different));
    CHECK(f.service->authorize_udp(public_id)); CHECK(f.registry.live == 1);
    CHECK(session(f, login).status == 200);
}
TEST_CASE("assignment server scopes isolate account grants without mutating player progression") {
    Fixture first("assignment-A"), second("assignment-B");
    const auto a = first.login(), b = second.login();
    REQUIRE(a.status == 200); REQUIRE(b.status == 200);
    const auto account_a = first.store.find_account("76561197960265729");
    const auto account_b = second.store.find_account("76561197960265729");
    REQUIRE(account_a.has_value()); REQUIRE(account_b.has_value());
    CHECK_FALSE(account_a->ref == account_b->ref);
    const auto balance_a = first.store.wallet(account_a->ref);
    const auto balance_b = second.store.wallet(account_b->ref);
    selected(first, a); selected(second, b);
    failure(first.service->handle("GET", "/session", header(b)), 401);
    failure(second.service->handle("GET", "/session", header(a)), 401);
    CHECK(session(first, a).result.at("UserGuid") == user_guid(a));
    CHECK(session(second, b).result.at("UserGuid") == user_guid(b));
    CHECK(first.store.wallet(account_a->ref) == balance_a);
    CHECK(second.store.wallet(account_b->ref) == balance_b);
    CHECK_FALSE(first.store.starter_items(account_a->ref).has_value());
    CHECK_FALSE(second.store.starter_items(account_b->ref).has_value());
    CHECK(first.store.aliases(account_a->ref).empty());
    CHECK(first.store.visits(account_a->ref).empty());
    CHECK_FALSE(first.store.role(account_a->ref).has_value());
}
TEST_CASE("assignment inactive lease and service teardown retire the installed Context") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    f.registry.active = false;
    CHECK_FALSE(f.service->authorize_udp(public_id)); CHECK(f.registry.live == 0);
    f.registry.active = true;
    selected(f, login); CHECK(f.registry.live == 1);
    f.service.reset(); CHECK(f.registry.live == 0); CHECK(f.registry.retired == 2);
}
TEST_CASE("assignment validates explicit configuration and TTL without accepting client ready flags") {
    for (const int mode : {0,1,2,3,4,5,6,7,8}) {
        Fixture f;
        f.service.reset();
        switch (mode) {
        case 0: f.policy.lifetime_seconds = 0; break;
        case 1: f.policy.lifetime_seconds = 901; break;
        case 2: f.destination.port = 0; break;
        case 3: f.destination.port = 65536; break;
        case 4: f.destination.ip = "192.0.2.1"; break;
        case 5: f.destination.ip = "127.0.0.1:27015"; break;
        case 6: f.destination.server_guid = "00000000-0000-0000-0000-000000000000"; break;
        case 7: f.destination.region = "EU/NA"; break;
        case 8: f.destination.game_version = std::string(65, 'a'); break;
        default: break;
        }
        CHECK_THROWS_WITH_AS(f.construct(), "Invalid session assignment configuration.", std::invalid_argument);
        CHECK(f.entropy_calls == 0); CHECK(f.registry.installs == 0);
    }
}

namespace {
using RawResponse = hawken::control::posthello::RawString;
using ResponseForm = hawken::control::posthello::StringForm;
RawResponse response_guid(std::string_view guid, ResponseForm form = ResponseForm::narrow,
                          bool terminated = false, bool uppercase = false) {
    RawResponse response{form, {}};
    for (const char byte : guid) {
        const char16_t unit = static_cast<char16_t>(static_cast<unsigned char>(byte));
        response.units += uppercase && unit >= u'a' && unit <= u'f'
            ? static_cast<char16_t>(unit - u'a' + u'A') : unit;
    }
    if (terminated) response.units += u'\0';
    return response;
}
static_assert(noexcept(std::declval<SessionAssignmentService&>().validate_login_response(
    public_id, std::declval<const RawResponse&>())));
}

TEST_CASE("Login Response matches its retained GUID across ASCII archive forms") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    for (const auto form : {ResponseForm::narrow, ResponseForm::wide})
        for (const bool terminated : {false, true})
            for (const bool uppercase : {false, true})
                CHECK(f.service->validate_login_response(public_id,
                    response_guid(user_guid(login), form, terminated, uppercase)));
    CHECK(f.registry.live == 1); CHECK(f.registry.installs == 1);
}
TEST_CASE("Login Response rejects malformed bounded claims without retiring valid state") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login);
    const auto good = response_guid(user_guid(login));
    std::vector<RawResponse> bad{{ResponseForm::zero, {}}, {ResponseForm::zero, good.units},
        {static_cast<ResponseForm>(99), good.units}, {ResponseForm::narrow, {}},
        {ResponseForm::wide, std::u16string(4096, u'a')}};
    auto add = [&](const std::u16string& units) { bad.push_back({ResponseForm::narrow, units}); };
    add(good.units.substr(0, 35)); add(good.units + u'x');
    auto nul_suffix = good.units; nul_suffix += u'\0'; nul_suffix += u'x'; add(nul_suffix);
    auto extra_null = good.units; extra_null += u'\0'; extra_null += u'\0'; add(extra_null);
    for (const auto unit : {u'\0', u' ', u'g', u'\u0100', u'\uffff'}) {
        auto changed = good.units; changed[0] = unit; add(changed);
    }
    auto hyphen = good.units; hyphen[8] = u'0'; add(hyphen);
    auto shifted = good.units; shifted[7] = u'-'; add(shifted);
    auto wide_unicode = good; wide_unicode.form = ResponseForm::wide; wide_unicode.units[0] = 0xd800;
    bad.push_back(wide_unicode);
    auto trailing = good; trailing.units += u' '; bad.push_back(trailing);
    for (const auto& claim : bad) {
        CHECK_FALSE(f.service->validate_login_response(public_id, claim));
        CHECK(f.registry.live == 1); CHECK(f.registry.retired == 0);
    }
    CHECK(f.service->validate_login_response(public_id, good));
}
TEST_CASE("Login Response does not select accounts or evict a session on foreign claims") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    const auto good = response_guid(user_guid(login));
    CHECK_FALSE(f.service->validate_login_response(public_id, good));
    CHECK(f.registry.installs == 0); CHECK(f.entropy_calls == 0);
    selected(f, login);
    auto unknown = public_id; ++unknown[7];
    CHECK_FALSE(f.service->validate_login_response(unknown, good));
    CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(foreign_guid)));
    CHECK(f.registry.live == 1); CHECK(f.registry.retired == 0);
    CHECK(f.service->validate_login_response(public_id, good));
    CHECK(session(f, login).result == expected_session(login));
}
TEST_CASE("Login Response revalidates ban logout and Steam revocation before claim") {
    for (const int mode : {0, 1, 2}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        selected(f, login);
        if (mode == 0) f.verifier.active = false;
        if (mode == 1) {
            const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
            f.store.set_ban(account->ref, {1000, std::nullopt, "fixture"});
        }
        if (mode == 2)
            REQUIRE(f.auth.logout(user_guid(login), header(login),
                Json{{"AccessGrant", login.result.at("AccessGrant")}}.dump()).status == 200);
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(user_guid(login))));
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
    }
}
TEST_CASE("Login Response enforces monotonic boundary regression and grant expiry") {
    for (const int mode : {0, 1, 2, 3}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        selected(f, login);
        f.monotonic_now = 129;
        CHECK(f.service->validate_login_response(public_id, response_guid(user_guid(login))));
        if (mode == 0) f.monotonic_now = 130;
        if (mode == 1) f.monotonic_now = 128;
        if (mode == 2) f.auth_now = 1900;
        if (mode == 3) f.monotonic_throws = true;
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(user_guid(login))));
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
    }
}
TEST_CASE("Login Response requires live bound destination and active installed lease") {
    for (const int mode : {0, 1, 2}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        selected(f, login);
        if (mode == 0) f.bound = false;
        if (mode == 1) f.registry.active = false;
        if (mode == 2) f.bound_throws = true;
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(user_guid(login))));
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
    }
}
TEST_CASE("Login Response cannot reuse a replaced account or grant binding") {
    for (const bool another_account : {false, true}) {
        Fixture f;
        const auto first = f.login(); REQUIRE(first.status == 200);
        selected(f, first);
        if (another_account) f.verifier.selected_identity = identity + 1;
        const auto second = f.login(); REQUIRE(second.status == 200);
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(user_guid(first))));
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(user_guid(second))));
        CHECK(f.registry.installs == 1); CHECK(f.entropy_calls == 2);
    }
}
TEST_CASE("Login Response checks preserve the same Context nonce counters and fixture wallet") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    selected(f, login); REQUIRE(f.registry.context);
    const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto wallet = f.store.wallet(account->ref);
    const auto guid = user_guid(login);
    auto* original = f.registry.context;
    hawken::session::Context client(hawken::session::Role::client, public_id, public_key);
    constexpr std::array<std::uint8_t, 2> payload{0x45, 0x67};
    CHECK(original->decode(client.encode(payload)) == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    CHECK(client.decode(original->encode(payload)) == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    const auto sent = original->last_sent(), received = original->last_received();
    for (int i = 0; i < 4; ++i) {
        CHECK(f.service->validate_login_response(public_id, response_guid(guid, ResponseForm::wide, true)));
        CHECK_FALSE(f.service->validate_login_response(public_id, response_guid(foreign_guid)));
        CHECK(f.registry.context == original);
        if (f.registry.context == original) {
            CHECK(original->last_sent() == sent); CHECK(original->last_received() == received);
        }
    }
    CHECK(f.entropy_calls == 2); CHECK(f.registry.installs == 1); CHECK(f.registry.retired == 0);
    CHECK(f.store.find_account("76561197960265729")->ref == account->ref);
    CHECK(f.store.wallet(account->ref) == wallet);
    CHECK_FALSE(f.store.starter_items(account->ref));
}

// DELETE's verb/void result are original SDK contracts. The status codes,
// retirement, repeat-404 and single-account rules below are new local policy.
TEST_CASE("assignment DELETE retires its own session before a void ACK") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    REQUIRE(post(f, login).status == 200);
    const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
    const auto balances = f.store.wallet(account->ref);
    const auto entropy = f.entropy_calls;
    REQUIRE(f.registry.live == 1);
    const auto cancelled = f.service->handle("DELETE", ad_target(), header(login));
    CHECK(cancelled.status == 200); CHECK(cancelled.result.is_null());
    CHECK(cancelled.message.empty());
    CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
    CHECK(f.registry.context == nullptr);
    CHECK_FALSE(f.service->authorize_udp(public_id));
    failure(f.service->handle("GET", ad_target(), header(login)), 404);
    failure(session(f, login), 404);
    failure(f.service->handle("DELETE", ad_target(), header(login)), 404);
    CHECK(f.entropy_calls == entropy); CHECK(f.registry.installs == 1);
    CHECK(f.auth.access(header(login)).status == 200);
    CHECK(f.store.find_account("76561197960265729")->ref == account->ref);
    CHECK(f.store.wallet(account->ref) == balances);
    CHECK_FALSE(f.store.starter_items(account->ref));
}
TEST_CASE("assignment DELETE unknown IDs and invalid auth preserve an active Context") {
    for (const bool advertisement : {false, true}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        if (advertisement) { REQUIRE(post(f, login).status == 200); }
        else selected(f, login);
        REQUIRE(f.registry.context != nullptr);
        auto* original = f.registry.context;
        hawken::session::Context client(hawken::session::Role::client, public_id, public_key);
        constexpr std::array<std::uint8_t, 1> payload{3};
        const auto packet = client.encode(payload);
        REQUIRE(original->decode(packet) == std::vector<std::uint8_t>{3});
        const auto reply = original->encode(payload);
        REQUIRE(client.decode(reply) == std::vector<std::uint8_t>{3});
        const auto sent = original->last_sent(), received = original->last_received();
        const auto before = session(f, login).result;
        const auto entropy = f.entropy_calls;
        failure(f.service->handle("DELETE", ad_target(foreign_guid), header(login)), 404);
        if (!advertisement)
            failure(f.service->handle("DELETE", ad_target(), header(login)), 404);
        for (const auto invalid : {"", "Basic foreign"})
            failure(f.service->handle("DELETE", ad_target(), invalid), 401);
        Fixture foreign("other-assignment-server");
        const auto other_login = foreign.login(); REQUIRE(other_login.status == 200);
        failure(f.service->handle("DELETE", ad_target(), header(other_login)), 401);
        CHECK(f.registry.context == original); CHECK(f.registry.live == 1);
        CHECK(f.registry.retired == 0); CHECK(f.registry.installs == 1);
        CHECK(original->last_sent() == sent); CHECK(original->last_received() == received);
        CHECK(session(f, login).result == before);
        CHECK(f.service->authorize_udp(public_id)); CHECK(f.entropy_calls == entropy);
    }
}
TEST_CASE("assignment DELETE rejects aliases bodies and unsupported methods without cancellation") {
    Fixture f;
    const auto login = f.login(); REQUIRE(login.status == 200);
    REQUIRE(post(f, login).status == 200);
    for (const auto& target : {ad_target()+"/", ad_target()+"?x=1",
        std::string(ads)+"/00112233-4455-6677-8899-AABBCCDDEEFF",
        std::string(ads)+"/00000000-0000-0000-0000-000000000000",
        std::string(ads)+"/%30"+std::string(ad_guid).substr(1)})
        failure(f.service->handle("DELETE", target, ""), 404);
    for (const auto body : {" ", "{}", "null"})
        failure(f.service->handle("DELETE", ad_target(), header(login), body), 400);
    for (const auto method : {"POST", "PUT", "PATCH", "HEAD", "delete"})
        failure(f.service->handle(method, ad_target(), header(login)), 405);
    failure(f.service->handle("DELETE", "/session", header(login)), 405);
    failure(f.service->handle("DELETE", ads, header(login)), 405);
    CHECK(f.registry.live == 1); CHECK(f.registry.retired == 0);
    CHECK(f.service->authorize_udp(public_id));
    CHECK(f.service->handle("DELETE", ad_target(), header(login)).status == 200);
}
TEST_CASE("assignment DELETE revalidates revoked banned logged-out and expired grants") {
    for (const int mode : {0, 1, 2, 3}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        REQUIRE(post(f, login).status == 200);
        if (mode == 0) f.verifier.active = false;
        if (mode == 1) {
            const auto account = f.store.find_account("76561197960265729"); REQUIRE(account);
            f.store.set_ban(account->ref, {1000, std::nullopt, "fixture"});
        }
        if (mode == 2) {
            REQUIRE(f.auth.logout(user_guid(login), header(login),
                Json{{"AccessGrant", login.result.at("AccessGrant")}}.dump()).status == 200);
        }
        if (mode == 3) f.auth_now = 1900;
        failure(f.service->handle("DELETE", ad_target(), header(login)), mode == 1 ? 403 : 401);
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
        CHECK(f.registry.context == nullptr); CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(f.registry.installs == 1); CHECK(f.entropy_calls == 3);
    }
}
TEST_CASE("assignment DELETE maintains expiry regression binding and active lease before ACK") {
    for (const int mode : {0, 1, 2, 3}) {
        Fixture f;
        const auto login = f.login(); REQUIRE(login.status == 200);
        REQUIRE(post(f, login).status == 200);
        if (mode == 0) f.monotonic_now = 130;
        if (mode == 1) f.monotonic_now = 99;
        if (mode == 2) f.bound = false;
        if (mode == 3) f.registry.active = false;
        failure(f.service->handle("DELETE", ad_target(), header(login)),
                mode == 0 ? 404 : mode == 1 ? 500 : 503);
        CHECK(f.registry.live == 0); CHECK(f.registry.retired == 1);
        CHECK(f.registry.context == nullptr); CHECK_FALSE(f.service->authorize_udp(public_id));
        CHECK(f.registry.installs == 1); CHECK(f.entropy_calls == 3);
    }
}
