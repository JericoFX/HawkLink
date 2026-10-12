#include "hawken/server_http.hpp"
#include "hawken/session_assignment.hpp"
#include "test_support.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace {
using Json = nlohmann::json;
constexpr std::string_view server_guid = "12345678-1234-5678-9012-123456789abc";
constexpr std::string_view ad_guid = "00112233-4455-6677-8899-aabbccddeeff";
constexpr std::string_view ads = "/hawkenClientMatchmakingAdvertisements";
class Verifier final : public hawken::auth::SteamVerifier {
public:
    std::atomic<bool> active{false};
    std::atomic<int> calls{0};
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        ++calls;
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket,
                active ? 76561197960265729ULL : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == 76561197960265729ULL;
    }
    void end_session(std::uint64_t) override { active = false; }
};
// This fixture confirms only an in-memory owner, never a bound UDP GameServer.
class Lease final : public hawken::assignment::UdpSessionLease {
public:
    Lease(const hawken::assignment::SessionProvision& provision, std::atomic<int>& retired)
        : context(hawken::session::Role::server, provision.id, provision.key), retired_(retired) {}
    ~Lease() override { ++retired_; }
    bool active() const noexcept override { return true; }
    hawken::session::Context context;
private:
    std::atomic<int>& retired_;
};
class Listener {
public:
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store{temporary.path() / "accounts.sqlite", "assignment-http"};
    Verifier verifier;
    std::atomic<std::int64_t> now{1000}, monotonic{100};
    std::atomic<bool> bound{true};
    std::atomic<int> installs{0}, retired{0};
    hawken::auth::AuthService auth{store, verifier, [this] { return now.load(); }};
    hawken::assignment::SessionAssignmentService assignment{
        auth, {std::string(server_guid), "127.0.0.1", 27015, "EU", "1.6.0.154767"},
        {30,1200,800}, [this] { return monotonic.load(); }, [] { return 1704067200LL; },
        [](std::span<std::uint8_t> bytes) {
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(bytes.size() == 32 ? i : i * 17);
            return true;
        }, [this](const hawken::assignment::Destination&) { return bound.load(); },
        [this](const hawken::assignment::SessionProvision& provision) {
            ++installs; return std::make_unique<Lease>(provision, retired);
        }};
    explicit Listener(bool enabled = true) {
        hawken::server::configure_server_http(server_, &auth, nullptr, nullptr, nullptr,
                                              enabled ? &assignment : nullptr);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Fixture could not bind loopback.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) throw std::runtime_error("Fixture listener did not start.");
    }
    ~Listener() { server_.stop(); }
    int port() const { return port_; }
private:
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};
Json body(const httplib::Result& response, int status) {
    REQUIRE(response); REQUIRE(response->status == status);
    CHECK(response->get_header_value("Connection") == "close");
    CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    const auto value = Json::parse(response->body);
    CHECK(value.at("Status") == status); CHECK(value.at("Message").is_string());
    return value.at("Result");
}
Json login(httplib::Client& client) {
    return body(client.Post("/steam/login", R"({"SteamAuthTicket":"AABB0100"})",
                            "application/json"),200);
}
httplib::Headers headers(const Json& login) {
    return {{"Authorization", "Basic " + login.at("AccessGrant").get<std::string>()}};
}
std::string request(const Json& login) {
    const auto guid = login.at("UserId").get<std::string>();
    return Json{{"OwnerGuid",guid}, {"Users",Json::array({guid})}, {"Region","EU"},
                {"GameVersion","1.6.0.154767"}, {"RequestedServerGuid",server_guid}}.dump();
}
std::string poll() { return std::string(ads) + "/" + std::string(ad_guid); }
}

TEST_CASE("Assignment HTTP routes protect authentication without creating an owner") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    CHECK(body(client.Get("/session"),401).is_null());
    const auto public_owner = Json{{"UserId","11111111-1111-1111-1111-111111111111"}};
    CHECK(body(client.Post(std::string(ads),request(public_owner),"application/json"),401).is_null());
    CHECK(body(client.Get(poll()),401).is_null());
    CHECK(listener.installs == 0); CHECK(listener.verifier.calls == 0);
}
TEST_CASE("Assignment HTTP creates and polls one session with exact public wire values") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    CHECK(body(client.Get("/session",auth),404).is_null());
    CHECK(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200)
          == Json(ad_guid));
    const auto advertisement = body(client.Get(poll(),auth),200);
    CHECK(advertisement.at("Status") == 1);
    CHECK(advertisement.at("AssignedServerGuid") == std::string(server_guid));
    CHECK(advertisement.at("AssignedServerIp") == "127.0.0.1");
    CHECK(advertisement.at("AssignedServerPort") == 27015);
    const Json expected{{"SessionId","ABEiM0RVZnc"},
        {"SessionKey","AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"},
        {"UserGuid",credentials.at("UserId")}, {"ClientAdvertisementGuid",ad_guid},
        {"AssignedServerGuid",server_guid}, {"AssignedServerIp","127.0.0.1"},
        {"AssignedServerPort",27015}};
    CHECK(body(client.Get("/session",auth),200) == expected);
    CHECK(body(client.Get("/session",auth),200) == expected);
    CHECK(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200)
          == Json(ad_guid));
    CHECK(listener.installs == 1); CHECK(listener.verifier.calls == 1);
}
TEST_CASE("Assignment HTTP validates raw targets methods and complete request bodies") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    client.set_path_encode(false);
    for (const auto path : {"/session/", "/session?serverGuid=x", "/%73ession",
                           "/hawkenClientMatchmakingAdvertisements/"})
        CHECK(body(client.Post(path,auth,"{}","application/json"),404).is_null());
    const auto wrong = client.Get(std::string(ads),auth);
    CHECK(body(wrong,405).is_null()); CHECK(wrong->get_header_value("Allow") == "POST");
    const auto method = client.Post("/session",auth,"{}","application/json");
    CHECK(body(method,405).is_null()); CHECK(method->get_header_value("Allow") == "GET");
    CHECK(body(client.Post(std::string(ads),auth,std::string(4097,' '),"application/json"),400).is_null());
    httplib::Request get;
    get.method = "GET"; get.path = "/session"; get.headers = auth; get.body = "{}";
    CHECK(body(client.send(get),400).is_null());
    CHECK(listener.installs == 0);
}
TEST_CASE("Assignment HTTP respects binding expiry and Steam revocation") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    listener.bound = false;
    CHECK(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),503).is_null());
    listener.bound = true;
    REQUIRE(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200) == Json(ad_guid));
    listener.monotonic = 130;
    CHECK(body(client.Get("/session",auth),404).is_null());
    CHECK(body(client.Get(poll(),auth),404).is_null());
    listener.verifier.active = false;
    CHECK(body(client.Get("/session",auth),401).is_null());
}
TEST_CASE("Assignment HTTP optional facade keeps startup login and absent session routes") {
    Listener listener(false); httplib::Client client("127.0.0.1",listener.port());
    CHECK(body(client.Get("/version"),200).is_object());
    const auto credentials = login(client);
    CHECK(body(client.Get("/session",headers(credentials)),404).is_null());
    CHECK(listener.installs == 0); CHECK(listener.verifier.calls == 1);
}

// HTTP DELETE/ApiResult<void> is original; complete local session retirement
// and repeated404 are explicit policy, not recovered remote status behavior.
TEST_CASE("Assignment HTTP DELETE returns a void ACK only after owner retirement") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    REQUIRE(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200) == Json(ad_guid));
    const auto account = listener.store.find_account("76561197960265729"); REQUIRE(account);
    const auto balances = listener.store.wallet(account->ref);
    CHECK(body(client.Delete(poll(),auth),200).is_null());
    CHECK(listener.retired == 1); CHECK(listener.installs == 1);
    CHECK_FALSE(listener.assignment.authorize_udp({0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}));
    CHECK(body(client.Get(poll(),auth),404).is_null());
    CHECK(body(client.Get("/session",auth),404).is_null());
    CHECK(body(client.Delete(poll(),auth),404).is_null());
    CHECK(listener.auth.access(auth.begin()->second).status == 200);
    CHECK(listener.store.wallet(account->ref) == balances);
    CHECK_FALSE(listener.store.starter_items(account->ref));
    CHECK(listener.verifier.calls == 1);
}
TEST_CASE("Assignment HTTP DELETE invalid credentials and unknown IDs cannot cancel a valid session") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    REQUIRE(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200) == Json(ad_guid));
    const auto before = body(client.Get("/session",auth),200);
    CHECK(body(client.Delete(poll()),401).is_null());
    CHECK(body(client.Delete(poll(),httplib::Headers{{"Authorization","Basic foreign"}}),401).is_null());
    CHECK(body(client.Delete(std::string(ads)+"/abcdef01-2345-6789-abcd-ef0123456789",auth),404).is_null());
    CHECK(body(client.Get("/session",auth),200) == before);
    CHECK(listener.retired == 0); CHECK(listener.installs == 1);
    CHECK(listener.assignment.authorize_udp({0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}));
}
TEST_CASE("Assignment HTTP DELETE validates aliases bodies and methods without evicting") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto auth = headers(credentials);
    REQUIRE(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200) == Json(ad_guid));
    client.set_path_encode(false);
    for (const auto& path : {poll()+"/", poll()+"?x=1",
        std::string(ads)+"/00112233-4455-6677-8899-AABBCCDDEEFF",
        std::string(ads)+"/%30"+std::string(ad_guid).substr(1)})
        CHECK(body(client.Delete(path,auth),404).is_null());
    httplib::Request with_body;
    with_body.method = "DELETE"; with_body.path = poll();
    with_body.headers = auth; with_body.body = "{}";
    CHECK(body(client.send(with_body),400).is_null());
    const auto wrong = client.Put(poll(),auth,"","application/json");
    CHECK(body(wrong,405).is_null()); CHECK(wrong->get_header_value("Allow") == "GET, DELETE");
    CHECK(body(client.Delete("/session",auth),405).is_null());
    CHECK(body(client.Delete(std::string(ads),auth),405).is_null());
    CHECK(listener.retired == 0);
    CHECK(body(client.Get("/session",auth),200).is_object());
}
TEST_CASE("Assignment HTTP DELETE rechecks revocation ban and expiration before cancellation") {
    for (const int mode : {0,1,2,3}) {
        Listener listener; httplib::Client client("127.0.0.1",listener.port());
        const auto credentials = login(client); const auto auth = headers(credentials);
        REQUIRE(body(client.Post(std::string(ads),auth,request(credentials),"application/json"),200) == Json(ad_guid));
        if (mode == 0) listener.verifier.active = false;
        if (mode == 1) {
            const auto account = listener.store.find_account("76561197960265729"); REQUIRE(account);
            listener.store.set_ban(account->ref, {1000, std::nullopt, "fixture"});
        }
        if (mode == 2) listener.now = 1900;
        if (mode == 3) listener.monotonic = 130;
        CHECK(body(client.Delete(poll(),auth),mode == 1 ? 403 : mode == 3 ? 404 : 401).is_null());
        CHECK(listener.retired == 1); CHECK(listener.installs == 1);
        CHECK_FALSE(listener.assignment.authorize_udp({0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}));
    }
}
