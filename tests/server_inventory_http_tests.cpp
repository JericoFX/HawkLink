#include "hawken/inventory_service.hpp"
#include "hawken/server_http.hpp"
#include "hawken/sync_service.hpp"
#include "test_support.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
class TestVerifier final : public hawken::auth::SteamVerifier {
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
hawken::inventory::StarterCatalogueIds ids() {
    return {"10000000-0000-0000-0000-000000000001", "10000000-0000-0000-0000-000000000002",
            "10000000-0000-0000-0000-000000000003", "10000000-0000-0000-0000-000000000004",
            "10000000-0000-0000-0000-000000000005", "10000000-0000-0000-0000-000000000006",
            "10000000-0000-0000-0000-000000000007", "10000000-0000-0000-0000-000000000008",
            "10000000-0000-0000-0000-000000000009"};
}
hawken::inventory::StarterBalanceProfile balance() {
    return {"starter.http-test", {2, 0.25f, 0.1f, 10000, 8, 0, 8, 10, 2, 10000, 90},
            {20, 0, 1.5f, 16000, 80, 0, 80, 1.5f, 20, 16000, 100}, {30, 1, 100}, {2500, 2500, 0, 80, 300, 100, 20}};
}
class Listener {
public:
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store{temporary.path() / "accounts.sqlite", "http-inventory"};
    TestVerifier verifier;
    std::int64_t now = 1000;
    hawken::auth::AuthService auth{store, verifier, [this] { return now; }};
    hawken::sync::SyncService sync{store, auth, 25000};
    std::unique_ptr<hawken::inventory::InventoryService> inventory;
    explicit Listener(bool enable_inventory = true) {
        if (enable_inventory)
            inventory = std::make_unique<hawken::inventory::InventoryService>(store, auth, ids(), balance(),
                "20000000-0000-0000-0000-000000000001");
        hawken::server::configure_server_http(server_, &auth, &sync, inventory.get());
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
struct Credentials { std::string guid, grant; };
Credentials login(httplib::Client& client) {
    const auto response = client.Post("/steam/login", R"({"SteamAuthTicket":"AABB0100"})", "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    return {body.at("Result").at("UserId").get<std::string>(),
            body.at("Result").at("AccessGrant").get<std::string>()};
}
httplib::Headers headers(const Credentials& credentials) {
    return {{"Authorization", "Basic " + credentials.grant}};
}
std::array<std::string, 3> targets(const Credentials& credentials) {
    return {"/game-client/game-items", "/game-client/game-offers", "/userGameItems/" + credentials.guid};
}
nlohmann::json result(const httplib::Result& response, int status) {
    REQUIRE(response);
    REQUIRE(response->status == status);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body.at("Status") == status);
    CHECK(response->get_header_value("Connection") == "close");
    CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    return body.at("Result");
}
}

TEST_CASE("Inventory HTTP rejects missing grants before any account is created") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const Credentials unused{"01234567-89ab-cdef-0123-456789abcdef", ""};
    for (const auto& path : targets(unused)) CHECK(result(client.Get(path), 401).is_null());
    CHECK(listener.verifier.calls == 0);
    CHECK_FALSE(listener.store.find_account("76561197960265729"));
}

TEST_CASE("HTTP catalogue offers and persisted owned inventory share the same nine types") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto account = listener.store.find_account("76561197960265729");
    REQUIRE(account.has_value());
    const auto catalogue = result(client.Get("/game-client/game-items", headers(credentials)), 200);
    const auto offers = result(client.Get("/game-client/game-offers", headers(credentials)), 200);
    REQUIRE(catalogue.is_array());
    REQUIRE(catalogue.size() == 9);
    CHECK(catalogue.at(2).at("Proj.Speed") == "2500");
    CHECK(catalogue.at(2).at("Proj.MaxSpeed") == "2500");
    CHECK(catalogue.at(2).at("Proj.AccelRate") == "0");
    CHECK(catalogue.at(2).at("Proj.Damage") == "80");
    CHECK(catalogue.at(2).at("Proj.DamageRadius") == "300");
    CHECK(catalogue.at(2).at("Proj.InnerDamageRadius") == "100");
    CHECK(catalogue.at(2).at("Proj.MinDamage") == "20");
    CHECK_FALSE(catalogue.at(1).contains("Proj.Speed"));
    REQUIRE(offers.is_array());
    REQUIRE(offers.size() == 1);
    CHECK(offers.at(0).at("Fees") == nlohmann::json{{"HP", 0}});
    CHECK_FALSE(listener.store.starter_items(account->ref));
    const auto owned = result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200);
    REQUIRE(owned.is_array());
    REQUIRE(owned.size() == 9);
    const auto stored = listener.store.starter_items(account->ref);
    REQUIRE(stored.has_value());
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(owned.at(i).at("GameItemsGuid") == catalogue.at(i).at("GameItemsGuid"));
        CHECK(offers.at(0).at("GameItems").at(i).at("GameItemsGuid") == catalogue.at(i).at("GameItemsGuid"));
        CHECK(owned.at(i).at("UserGameItemGuid") == stored->at(i).instance_id);
        CHECK(owned.at(i).at("Version") == "starter.http-test");
    }
    CHECK(owned.at(0).at("Preset.PrimaryWeapon_InstanceId_0") == stored->at(1).instance_id);
    CHECK(listener.store.wallet(account->ref) == hawken::store::WalletBalances{});
    CHECK(result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200) == owned);
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Inventory HTTP rejects raw aliases methods and purchase routes without granting") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto paths = targets(credentials);
    for (const auto& path : paths) {
        for (const auto& alias : {path + "/", path + "?x=1"})
            CHECK(result(client.Get(alias, headers(credentials)), 404).is_null());
        const auto response = client.Post(path, headers(credentials), "{}", "application/json");
        CHECK(result(response, 405).is_null());
        CHECK(response->get_header_value("Allow") == "GET");
    }
    for (const auto& path : std::vector<std::string>{"/game-client/%67ame-items", "/Game-client/game-offers",
            "/userGameItems/%" + std::string("30") + credentials.guid.substr(1),
            "/game-client/game-offers/purchase"})
        CHECK(result(client.Get(path, headers(credentials)), 404).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Owned HTTP checks the path account and every inventory route checks revocation") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    CHECK(result(client.Get("/userGameItems/00000000-0000-0000-0000-000000000000", headers(credentials)), 401).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
    listener.verifier.active = false;
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 401).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
}

TEST_CASE("Logout revokes HTTP catalogue access while owned items stay persisted") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    REQUIRE(result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200).size() == 9);
    CHECK(result(client.Put("/users/" + credentials.guid + "/accessGrant", headers(credentials),
        nlohmann::json{{"AccessGrant", credentials.grant}}.dump(), "application/json"), 200).is_null());
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 401).is_null());
    REQUIRE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref).has_value());
}

TEST_CASE("Absent inventory facade preserves previous HTTP bootstrap auth and sync routes") {
    Listener listener(false);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 404).is_null());
    CHECK(result(client.Get("/gameCurrency/" + credentials.guid, headers(credentials)), 200) == 0);
    CHECK(result(client.Get("/users/" + credentials.guid, headers(credentials)), 200).at("Guid") == credentials.guid);
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
}
