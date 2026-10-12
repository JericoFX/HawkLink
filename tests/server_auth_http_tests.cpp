#include "hawken/auth_service.hpp"
#include "hawken/server_http.hpp"
#include "hawken/sync_service.hpp"
#include "test_support.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace {
// Unit-test-only provider; no Steam API or factory is invoked by this listener.
class TestVerifier final : public hawken::auth::SteamVerifier {
public:
    std::atomic<bool> active{false};
    std::atomic<int> calls{0};
    std::atomic<int> claimed_calls{0};
    std::atomic<std::uint64_t> claimed_identity{0};
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t>) override {
        ++calls;
        active = true;
        return {hawken::auth::SteamAuthStatus::authenticated, 76561197960265729ULL};
    }
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket, std::uint64_t claim) override {
        ++claimed_calls;
        claimed_identity = claim;
        return verify(ticket);
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == 76561197960265729ULL;
    }
    void end_session(std::uint64_t) override { active = false; }
};
class LocalListener {
public:
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store;
    TestVerifier verifier;
    std::int64_t now = 1000;
    hawken::auth::AuthService auth{store, verifier, [this] { return now; }};
    std::unique_ptr<hawken::sync::SyncService> sync;
    explicit LocalListener(bool enable_sync = false, const std::filesystem::path& database_path = {})
        : store(database_path.empty() ? temporary.path() / "accounts.sqlite" : database_path, "http-fixture") {
        if (enable_sync) {
            sync = std::make_unique<hawken::sync::SyncService>(store, auth, 25000);
            hawken::server::configure_server_http(server_, &auth, sync.get());
        } else hawken::server::configure_server_http(server_, &auth);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Fixture could not bind loopback.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) throw std::runtime_error("Fixture listener did not start.");
    }
    ~LocalListener() { server_.stop(); }
    int port() const { return port_; }
private:
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};
struct Credentials { std::string path, token; };
Credentials login(httplib::Client& client) {
    const auto response = client.Post("/steam/login", R"({"SteamAuthTicket":"AA00BB01"})", "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto json = nlohmann::json::parse(response->body);
    REQUIRE(json.at("Status") == 200);
    REQUIRE(json.at("Result").at("NewUser") == false);
    return {"/users/" + json.at("Result").at("UserId").get<std::string>(),
        json.at("Result").at("AccessGrant").get<std::string>()};
}
httplib::Headers headers(const Credentials& credentials) {
    return {{"Authorization", "Basic " + credentials.token}};
}
}

TEST_CASE("Local verified facade serves the recovered login profile logout HTTP chain") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    CHECK(listener.verifier.calls == 1);
    const auto profile = client.Get(credentials.path, headers(credentials));
    REQUIRE(profile);
    REQUIRE(profile->status == 200);
    const auto json = nlohmann::json::parse(profile->body);
    CHECK(json.at("Result").at("Guid") == credentials.path.substr(7));
    CHECK(json.at("Result").at("Roles").empty());
    CHECK(profile->get_header_value("Connection") == "close");
    const auto logout = client.Put(credentials.path + "/accessGrant", headers(credentials),
        nlohmann::json{{"AccessGrant", credentials.token}}.dump(), "application/json");
    REQUIRE(logout);
    CHECK(logout->status == 200);
    CHECK_FALSE(listener.verifier.active);
    const auto revoked = client.Get(credentials.path, headers(credentials));
    REQUIRE(revoked);
    CHECK(revoked->status == 401);
}

TEST_CASE("HTTP login forwards one local claim with the original one field ticket body") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto response = client.Post("/steam/login", {{"X-Hawken-SteamID", "76561197960265729"}},
        R"({"SteamAuthTicket":"AA00BB01"})", "application/json");
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(listener.verifier.claimed_calls == 1);
    CHECK(listener.verifier.claimed_identity == 76561197960265729ULL);
    CHECK(listener.store.find_account("76561197960265729").has_value());
}

TEST_CASE("HTTP empty duplicate and malformed claims reject before touching a live grant") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const std::array<httplib::Headers, 5> invalid{{
        {{"X-Hawken-SteamID", ""}},
        {{"X-Hawken-SteamID", "76561197960265729"}, {"x-hawken-steamid", "76561197960265729"}},
        {{"X-Hawken-SteamID", "076561197960265729"}},
        {{"X-Hawken-SteamID", "18446744073709551616"}},
        {{"X-Hawken-SteamID", "1"}}
    }};
    for (const auto& claim_headers : invalid) {
        const auto response = client.Post("/steam/login", claim_headers,
            R"({"SteamAuthTicket":"AA00BB01"})", "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        CHECK(nlohmann::json::parse(response->body).at("Result").is_null());
        const auto profile = client.Get(credentials.path, headers(credentials));
        REQUIRE(profile);
        CHECK(profile->status == 200);
    }
    CHECK(listener.verifier.calls == 1);
    CHECK(listener.verifier.claimed_calls == 0);
}

TEST_CASE("Login consumes only valid bounded JSON and never authenticates invalid requests") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    for (const auto* body : {"{}", R"({"SteamAuthTicket":"aa"})",
            R"({"SteamAuthTicket":"AA","SteamAuthTicket":"BB"})"}) {
        const auto response = client.Post("/steam/login", body, "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        CHECK(nlohmann::json::parse(response->body).at("Result").is_null());
    }
    const auto oversized = client.Post("/steam/login", std::string(8193, ' '), "application/json");
    REQUIRE(oversized);
    CHECK(oversized->status == 413);
    CHECK(listener.verifier.calls == 0);
}

TEST_CASE("Authenticated HTTP routes still reject encoded query and slash aliases") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : std::vector<std::string>{credentials.path + "/", credentials.path + "?a=1",
            "/users/%" + std::string("30") + credentials.path.substr(8),
            "/users/" + credentials.path.substr(7) + "/accessGrant/", "/Users/" + credentials.path.substr(7)}) {
        const auto response = client.Get(path, headers(credentials));
        REQUIRE(response);
        CHECK(response->status == 404);
    }
    for (const auto* path : {"/steam/login/", "/steam/login?a=1", "/steam/%6cogin"}) {
        const auto response = client.Post(path, "{}", "application/json");
        REQUIRE(response);
        CHECK(response->status == 404);
    }
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Wrong HTTP methods fail before an authentication operation") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto wrong_login = client.Get("/steam/login");
    REQUIRE(wrong_login);
    CHECK(wrong_login->status == 405);
    CHECK(wrong_login->get_header_value("Allow") == "POST");
    CHECK(listener.verifier.calls == 0);
    const auto credentials = login(client);
    const auto wrong_profile = client.Post(credentials.path, "{}", "application/json");
    REQUIRE(wrong_profile);
    CHECK(wrong_profile->status == 405);
    CHECK(wrong_profile->get_header_value("Allow") == "GET");
    const auto wrong_logout = client.Get(credentials.path + "/accessGrant");
    REQUIRE(wrong_logout);
    CHECK(wrong_logout->status == 405);
    CHECK(wrong_logout->get_header_value("Allow") == "PUT");
}

TEST_CASE("Profile HTTP requires the recovered raw Basic header") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& value : std::vector<std::string>{"", "Bearer " + credentials.token,
            "Basic wrong", "Basic " + credentials.token + "="}) {
        const auto response = client.Get(credentials.path, httplib::Headers{{"Authorization", value}});
        REQUIRE(response);
        CHECK(response->status == 401);
    }
    const auto response = client.Get(credentials.path, headers(credentials));
    REQUIRE(response);
    CHECK(response->status == 200);
}

TEST_CASE("Native revocation invalidates subsequent HTTP profile requests") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    listener.verifier.active = false;
    const auto response = client.Get(credentials.path, headers(credentials));
    REQUIRE(response);
    CHECK(response->status == 401);
}

TEST_CASE("Startup replies remain available with an attached authentication facade") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    for (const auto* route : {"/status/services/", "/status/game_client/", "/version"}) {
        const auto response = client.Get(route);
        REQUIRE(response);
        CHECK(response->status == 200);
        CHECK(nlohmann::json::parse(response->body).at("Status") == 200);
    }
    CHECK(listener.verifier.calls == 0);
}

TEST_CASE("Optional sync HTTP returns the five recovered typed envelopes using the verified grant") {
    LocalListener listener(true);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto guid = credentials.path.substr(7);
    const std::array<std::pair<std::string, nlohmann::json>, 5> cases{{
        {"/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95",
            nlohmann::json{{"OverflowId", "32f2f08c-87f6-45a3-8440-a84242907a95"}, {"StatPerCurrency", 25000}}},
        {"/game-client/achievement-rewards", nlohmann::json::array()},
        {"/game-client/achievements/EU", nlohmann::json::array()},
        {"/gameCurrency/" + guid, 0}, {"/meteorCurrency/" + guid, 0}
    }};
    for (const auto& [target, expected] : cases) {
        const auto response = client.Get(target, headers(credentials));
        REQUIRE(response);
        REQUIRE(response->status == 200);
        const auto body = nlohmann::json::parse(response->body);
        CHECK(body.at("Status") == 200);
        CHECK(body.at("Message") == "");
        CHECK(body.at("Result") == expected);
        CHECK(response->get_header_value("Connection") == "close");
        CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    }
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Missing optional sync preserves the previous bootstrap and authentication route set") {
    LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : std::vector<std::string>{
        "/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95", "/game-client/achievement-rewards",
        "/game-client/achievements/", "/gameCurrency/" + credentials.path.substr(7),
        "/meteorCurrency/" + credentials.path.substr(7)}) {
        const auto response = client.Get(path, headers(credentials));
        REQUIRE(response);
        CHECK(response->status == 404);
    }
}

TEST_CASE("Sync HTTP methods raw paths account binding and missing grants fail closed") {
    LocalListener listener(true);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto wallet = "/gameCurrency/" + credentials.path.substr(7);
    for (const auto& path : std::vector<std::string>{wallet + "/", wallet + "?x=1",
        "/game-client/achievements/%45U", "/game-client/achievements/EU/",
        "/game-client/achievement-rewards/", "/game-client/game-items"}) {
        const auto response = client.Get(path, headers(credentials));
        REQUIRE(response);
        CHECK(response->status == 404);
    }
    const auto missing = client.Get(wallet);
    REQUIRE(missing);
    CHECK(missing->status == 401);
    const auto wrong = client.Get("/gameCurrency/00000000-0000-0000-0000-000000000000", headers(credentials));
    REQUIRE(wrong);
    CHECK(wrong->status == 401);
    const auto method = client.Post(wallet, headers(credentials), "{}", "application/json");
    REQUIRE(method);
    CHECK(method->status == 405);
    CHECK(method->get_header_value("Allow") == "GET");
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Protected sync HTTP revalidates revocation for every initial-sync resource") {
    LocalListener listener(true);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const std::vector<std::string> paths{
        "/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95", "/game-client/achievement-rewards",
        "/game-client/achievements/", "/gameCurrency/" + credentials.path.substr(7),
        "/meteorCurrency/" + credentials.path.substr(7)};
    for (const auto& path : paths) {
        const auto response = client.Get(path, headers(credentials));
        REQUIRE(response);
        REQUIRE(response->status == 200);
    }
    listener.verifier.active = false;
    for (const auto& path : paths) {
        const auto response = client.Get(path, headers(credentials));
        REQUIRE(response);
        CHECK(response->status == 401);
        CHECK(nlohmann::json::parse(response->body).at("Result").is_null());
    }
}

TEST_CASE("HTTP backend restart preserves its SQL account and requires a new verified grant") {
    hawken::test::TempDirectory temporary;
    const auto database_path = temporary.path() / "accounts.sqlite";
    Credentials previous;
    hawken::store::Account original_account;
    // Synthetic opaque fixture data, not a client consent or tutorial-completion flag.
    const hawken::store::GameSettings settings{{"RestartFixture", "opaque-value"}};
    {
        LocalListener listener(false, database_path);
        httplib::Client client("127.0.0.1", listener.port());
        previous = login(client);
        CHECK(listener.verifier.calls == 1);
        const auto profile = client.Get(previous.path, headers(previous));
        REQUIRE(profile);
        REQUIRE(profile->status == 200);
        CHECK(nlohmann::json::parse(profile->body).at("Result").at("Guid") == previous.path.substr(7));
        const auto account = listener.store.find_account("76561197960265729");
        REQUIRE(account);
        original_account = *account;
        REQUIRE(listener.store.merge_game_settings(account->ref, settings));
    }
    {
        LocalListener replacement(false, database_path);
        httplib::Client client("127.0.0.1", replacement.port());
        const auto account = replacement.store.find_account("76561197960265729");
        REQUIRE(account);
        CHECK(account->ref == original_account.ref);
        CHECK(account->steam_id64 == original_account.steam_id64);
        CHECK(account->created_at == original_account.created_at);
        CHECK(replacement.store.game_settings(account->ref) == settings);
        const auto availability = client.Get("/status/services/");
        REQUIRE(availability);
        CHECK(availability->status == 200);
        CHECK(replacement.verifier.calls == 0);
        const auto stale_before_login = client.Get(previous.path, headers(previous));
        REQUIRE(stale_before_login);
        CHECK(stale_before_login->status == 401);
        const auto rejected = nlohmann::json::parse(stale_before_login->body);
        CHECK(rejected.at("Status") == 401);
        CHECK(rejected.at("Result").is_null());
        CHECK(replacement.verifier.calls == 0);

        const auto current = login(client);
        CHECK(replacement.verifier.calls == 1);
        CHECK(current.path == previous.path);
        CHECK(current.token != previous.token);
        const auto stale_after_login = client.Get(previous.path, headers(previous));
        REQUIRE(stale_after_login);
        CHECK(stale_after_login->status == 401);
        CHECK(nlohmann::json::parse(stale_after_login->body).at("Result").is_null());
        const auto profile = client.Get(current.path, headers(current));
        REQUIRE(profile);
        CHECK(profile->status == 200);
        const auto restored = nlohmann::json::parse(profile->body);
        CHECK(restored.at("Status") == 200);
        CHECK(restored.at("Result").at("Guid") == previous.path.substr(7));
        const auto unchanged = replacement.store.find_account("76561197960265729");
        REQUIRE(unchanged);
        CHECK(unchanged->ref == original_account.ref);
        CHECK(unchanged->created_at == original_account.created_at);
        CHECK(replacement.store.game_settings(unchanged->ref) == settings);
    }
}
