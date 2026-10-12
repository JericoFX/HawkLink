#include "hawken/inventory_service.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using hawken::auth::AuthReply;
using hawken::auth::AuthService;
using hawken::inventory::InventoryService;
using hawken::inventory::StarterBalanceProfile;
using hawken::inventory::StarterCatalogueIds;
using hawken::store::LocalStore;
constexpr std::uint64_t fixture_id = 76561197960265729ULL;
constexpr const char* fixture_id_text = "76561197960265729";
constexpr const char* fixture_second_id_text = "76561197960265730";
constexpr const char* offer_id = "20000000-0000-0000-0000-000000000001";
constexpr std::string_view catalogue_path = "/game-client/game-items";
constexpr std::string_view offers_path = "/game-client/game-offers";
constexpr std::array<const char*, 9> type_ids{
    "10000000-0000-0000-0000-000000000001", "10000000-0000-0000-0000-000000000002",
    "10000000-0000-0000-0000-000000000003", "10000000-0000-0000-0000-000000000004",
    "10000000-0000-0000-0000-000000000005", "10000000-0000-0000-0000-000000000006",
    "10000000-0000-0000-0000-000000000007", "10000000-0000-0000-0000-000000000008",
    "10000000-0000-0000-0000-000000000009"};
// Literal oracle computed outside the C++ product with Python hashlib. Inputs:
// UserGuid = first128(SHA256("inventory-server/1")), then the documented ASCII
// namespace and slots. No product hash/ID generator computes expected values.
constexpr const char* expected_guid = "a54c90d7-d30b-8e6b-2417-3d744d78bd17";
constexpr std::array<const char*, 9> instance_ids{
    "445474ca-deb5-b989-8fa0-54b5d9630fa2", "acf28679-6b34-ed79-151e-26ec094cc9d0",
    "9465a78d-6495-da65-130d-99b71399c7db", "7a4a409e-2b04-6de1-8f82-21ad94bcb07c",
    "bd5551a6-35c1-7068-2a44-4367891ff83e", "198ae0cd-7e6f-6e9a-1f0f-1cb499a9b343",
    "29ec2edc-e014-2c6d-d994-56393ca7cf0f", "81880053-98d1-5323-1dcd-e86ea212841e",
    "ea800ec9-e850-9883-69a8-b2d3d2b86a14"};
constexpr std::array<const char*, 9> item_names{
    "Mech B - Rookie", "Weap Prim - Point-D Vulcan", "Weap Sec - TOW Rocket",
    "FRED UPPER", "FRED MIDDLE", "FRED ARMS", "FRED LOWER", "RD-TR1", "Ability - Coolant"};
StarterCatalogueIds ids() {
    return {type_ids[0], type_ids[1], type_ids[2], type_ids[3], type_ids[4],
            type_ids[5], type_ids[6], type_ids[7], type_ids[8]};
}
StarterBalanceProfile balance() {
    // Synthetic explicit server policy, not reconstructed historical stats.
    return {"starter.test1", {2, 0.25f, 0.1f, 10000, 8, 0, 8, 10, 2, 10000, 90},
            {20, 0, 1.5f, 16000, 80, 0, 80, 1.5f, 20, 16000, 100}, {30, 1, 100}, {2500, 2500, 0, 80, 300, 100, 20}};
}
class FakeVerifier final : public hawken::auth::SteamVerifier {
public:
    std::uint64_t active = 0;
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && (ticket[2] == 1 || ticket[2] == 2) && ticket[3] == 0
            ? fixture_id + ticket[2] - 1 : 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket, active};
    }
    bool is_authenticated(std::uint64_t id) const override { return id != 0 && active == id; }
    void end_session(std::uint64_t id) override { if (active == id) active = 0; }
};
struct Fixture {
    hawken::test::TempDirectory temporary;
    LocalStore store;
    FakeVerifier verifier;
    std::int64_t now = 1000;
    bool clock_throws = false;
    AuthService auth;
    InventoryService inventory;
    explicit Fixture(std::string server_id = "inventory-server")
        : store(temporary.path() / "accounts.sqlite", std::move(server_id)),
          auth(store, verifier, [this] {
              if (clock_throws) throw std::runtime_error("private-clock-fixture");
              return now;
          }), inventory(store, auth, ids(), balance(), offer_id) {}
    AuthReply login(bool second = false) {
        return auth.login(second ? R"({"SteamAuthTicket":"AABB0200"})"
                                 : R"({"SteamAuthTicket":"AABB0100"})");
    }
};
std::string guid(const AuthReply& login) { return login.result.at("UserId").get<std::string>(); }
std::string header(const AuthReply& login) {
    return "Basic " + login.result.at("AccessGrant").get<std::string>();
}
std::string owned_path(const AuthReply& login) { return "/userGameItems/" + guid(login); }
std::array<std::string, 3> targets(const AuthReply& login) {
    return {std::string(catalogue_path), std::string(offers_path), owned_path(login)};
}
void failure(const AuthReply& reply, int status, std::string_view private_value = {}) {
    CHECK(reply.status == status);
    CHECK(reply.result.is_null());
    CHECK(reply.message.find(fixture_id_text) == std::string::npos);
    CHECK(reply.message.find("private-clock-fixture") == std::string::npos);
    CHECK(reply.message.find("accounts.sqlite") == std::string::npos);
    if (!private_value.empty()) CHECK(reply.message.find(private_value) == std::string::npos);
}
struct SqlCloser { void operator()(sqlite3* value) const { if (value) sqlite3_close(value); } };
struct StatementCloser { void operator()(sqlite3_stmt* value) const { sqlite3_finalize(value); } };
void fixture_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(path.string().c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, SqlCloser> database(raw);
    REQUIRE(opened == SQLITE_OK);
    sqlite3_stmt* raw_statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(), sql, -1, &raw_statement, nullptr);
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    REQUIRE(prepared == SQLITE_OK);
    REQUIRE(sqlite3_step(statement.get()) == SQLITE_DONE);
    // This helper edits only synthetic unit databases, never the local Steam DB.
}
hawken::store::StarterItems persisted_expected() {
    hawken::store::StarterItems rows;
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = {type_ids[i], instance_ids[i]};
    return rows;
}
void check_owned(const nlohmann::json& result) {
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 9);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& row = result.at(i);
        CHECK(row.at("GameItemsGuid") == type_ids[i]);
        CHECK(row.at("UserGameItemGuid") == instance_ids[i]);
        CHECK(row.at("ItemName") == item_names[i]);
        CHECK(row.at("Version") == "starter.test1");
        CHECK(row.at("ReadOnly_Parent") == (i == 0 ? "" : instance_ids[0]));
        CHECK(row.at("ReadOnly_GlobalItem") == "0");
        CHECK_FALSE(row.contains("AccessGrant"));
        CHECK_FALSE(row.contains("SteamId"));
        CHECK_FALSE(row.contains("invType"));
        CHECK_FALSE(row.contains("HeatPerShot"));
        CHECK_FALSE(row.contains("supportedMechClasses"));
        if (i != 0) {
            CHECK(row.at("ReadOnly_Children") == nlohmann::json::array());
            CHECK(row.size() == 7);
        }
    }
    const auto& mech = result.at(0);
    // The independently recovered graph has eight children; the mech is the parent.
    CHECK(mech.at("ReadOnly_Children") == nlohmann::json::array({
        "acf28679-6b34-ed79-151e-26ec094cc9d0", "9465a78d-6495-da65-130d-99b71399c7db",
        "7a4a409e-2b04-6de1-8f82-21ad94bcb07c", "bd5551a6-35c1-7068-2a44-4367891ff83e",
        "198ae0cd-7e6f-6e9a-1f0f-1cb499a9b343", "29ec2edc-e014-2c6d-d994-56393ca7cf0f",
        "81880053-98d1-5323-1dcd-e86ea212841e", "ea800ec9-e850-9883-69a8-b2d3d2b86a14"}));
    CHECK(mech.at("ReadOnly_Children").size() == 8);
    for (const auto& child : mech.at("ReadOnly_Children")) CHECK(child != instance_ids[0]);
    CHECK(mech.at("Preset.PrimaryWeapon_InstanceId_0") == instance_ids[1]);
    CHECK(mech.at("Preset.SecondaryWeapon_InstanceId") == instance_ids[2]);
    CHECK(mech.at("ReadOnly_DefaultUpperPart") == instance_ids[3]);
    CHECK(mech.at("ReadOnly_DefaultHealingDrone") == instance_ids[7]);
    CHECK(mech.at("Preset.Ability_InstanceId") == instance_ids[8]);
    CHECK_FALSE(mech.contains("CurrentPresetIndex"));
}
}

TEST_CASE("Inventory constructor rejects invalid catalogue balance and offer configuration") {
    hawken::test::TempDirectory temporary;
    LocalStore store(temporary.path() / "accounts.sqlite", "inventory-server");
    FakeVerifier verifier;
    AuthService auth(store, verifier, [] { return std::int64_t{1000}; });
    auto bad_ids = ids();
    bad_ids.primary = bad_ids.mech;
    CHECK_THROWS_AS(InventoryService(store, auth, bad_ids, balance(), offer_id), std::invalid_argument);
    auto bad_balance = balance();
    bad_balance.primary.instant_hit_damage = 0;
    CHECK_THROWS_AS(InventoryService(store, auth, ids(), bad_balance, offer_id), std::invalid_argument);
    bad_balance = balance();
    bad_balance.version = "private invalid version";
    CHECK_THROWS_AS(InventoryService(store, auth, ids(), bad_balance, offer_id), std::invalid_argument);
    for (const auto invalid : {"", "PRIVATE-OFFER", "00000000-0000-0000-0000-000000000000", type_ids[0]}) {
        try {
            InventoryService service(store, auth, ids(), balance(), invalid);
            FAIL("Invalid offer configuration accepted");
        } catch (const std::invalid_argument& error) {
            CHECK(std::string(error.what()) == "Invalid inventory configuration.");
        }
    }
    CHECK_FALSE(store.find_account(fixture_id_text));
}

TEST_CASE("Inventory accepts only three exact raw target shapes") {
    Fixture f;
    for (const auto target : {catalogue_path, offers_path,
            std::string_view{"/userGameItems/01234567-89ab-cdef-0123-456789abcdef"}})
        CHECK(f.inventory.supports_target(target));
    const std::vector<std::string> rejected{"", "/", "/steam/login", "/gameItems/",
        "/gameItemsBatch", "/userGameItems/", "/game-client/game-items/", "/game-client/game-offers/",
        "/game-client/game-items?x=1", "/game-client/game-offers#x", "/game-client/%67ame-items",
        "/Game-client/game-items", "/userGameItems/0123456789abcdef0123456789abcdef",
        "/userGameItems/01234567-89AB-cdef-0123-456789abcdef",
        "/userGameItems/01234567-89ab-cdef-0123-456789abcdeg",
        "/userGameItems/01234567-89ab-cdef-0123-456789abcdef/",
        "/userGameItems/01234567-89ab-cdef-0123-456789abcdef?x=1",
        "/userGameItems/%30%31%32%33%34%35%36%37-89ab-cdef-0123-456789abcdef"};
    for (const auto& target : rejected) {
        CHECK_FALSE(f.inventory.supports_target(target));
        failure(f.inventory.get(target, "Basic private-path-fixture"), 404, "private-path-fixture");
    }
    auto nul = std::string(catalogue_path);
    nul.push_back('\0');
    CHECK_FALSE(f.inventory.supports_target(nul));
    failure(f.inventory.get(nul, ""), 404);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Inventory reads without a valid Steam grant cannot provision an account") {
    Fixture f;
    for (const auto target : {catalogue_path, offers_path,
            std::string_view{"/userGameItems/01234567-89ab-cdef-0123-456789abcdef"}})
        failure(f.inventory.get(target, ""), 401);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Catalogue supplies nine type records and does not grant owned items") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto reply = f.inventory.get(catalogue_path, header(login));
    REQUIRE(reply.status == 200);
    REQUIRE(reply.result.is_array());
    REQUIRE(reply.result.size() == 9);
    for (std::size_t i = 0; i < reply.result.size(); ++i) {
        CHECK(reply.result.at(i).at("GameItemsGuid") == type_ids[i]);
        CHECK(reply.result.at(i).at("ItemName") == item_names[i]);
        CHECK(reply.result.at(i).at("Version") == "starter.test1");
        CHECK_FALSE(reply.result.at(i).contains("UserGameItemGuid"));
    }
    CHECK(reply.result.at(0).at("invType") == "0");
    CHECK(reply.result.at(0).at("index") == "15");
    CHECK(reply.result.at(1).at("InstantHitDamage") == "8");
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK_FALSE(f.store.starter_items(account->ref));
    CHECK(f.store.wallet(account->ref) == hawken::store::WalletBalances{});
}

TEST_CASE("One explicit free HP offer describes all nine catalogue types without a purchase operation") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto reply = f.inventory.get(offers_path, header(login));
    REQUIRE(reply.status == 200);
    const auto expected = nlohmann::json::parse(R"([
      {"GameOfferGuid":"20000000-0000-0000-0000-000000000001","Description":"Free starter equipment",
       "Fees":{"HP":0},"GameItems":[
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000001","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000002","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000003","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000004","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000005","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000006","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000007","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000008","Quantity":1,"GameItemsVersion":"starter.test1"},
        {"GameItemsGuid":"10000000-0000-0000-0000-000000000009","Quantity":1,"GameItemsVersion":"starter.test1"}]}
    ])");
    CHECK(reply.result == expected);
    CHECK(reply.result.at(0).at("Fees").at("HP").is_number_integer());
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK_FALSE(f.store.starter_items(account->ref));
    failure(f.inventory.get("/game-client/game-offers/purchase", header(login)), 404);
}

TEST_CASE("First owned read atomically persists the exact nine instance IDs and coherent graph") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    CHECK(guid(login) == expected_guid);
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK_FALSE(f.store.starter_items(account->ref));
    const auto reply = f.inventory.get(owned_path(login), header(login));
    REQUIRE(reply.status == 200);
    check_owned(reply.result);
    CHECK(f.store.starter_items(account->ref) == persisted_expected());
    CHECK(f.store.wallet(account->ref) == hawken::store::WalletBalances{});
    CHECK(f.store.visits(account->ref).empty());
}

TEST_CASE("Owned reads replay persisted IDs without changing wallets or duplicating a grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    fixture_sql(f.temporary.path() / "accounts.sqlite",
        "UPDATE wallets SET hawken_points=73,meteor_points=0 WHERE account_id=1");
    const auto first = f.inventory.get(owned_path(login), header(login));
    REQUIRE(first.status == 200);
    for (int i = 0; i < 3; ++i) {
        const auto next = f.inventory.get(owned_path(login), header(login));
        REQUIRE(next.status == 200);
        CHECK(next.result == first.result);
    }
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK(f.store.starter_items(account->ref) == persisted_expected());
    CHECK(f.store.wallet(account->ref) == hawken::store::WalletBalances{73, 0});
}

TEST_CASE("Server restart uses persisted owned IDs after Steam reauthentication") {
    hawken::test::TempDirectory temporary;
    const auto path = temporary.path() / "accounts.sqlite";
    nlohmann::json before;
    {
        LocalStore store(path, "inventory-server");
        FakeVerifier verifier;
        AuthService auth(store, verifier, [] { return std::int64_t{1000}; });
        InventoryService inventory(store, auth, ids(), balance(), offer_id);
        const auto login = auth.login(R"({"SteamAuthTicket":"AABB0100"})");
        REQUIRE(login.status == 200);
        const auto result = inventory.get(owned_path(login), header(login));
        REQUIRE(result.status == 200);
        before = result.result;
        fixture_sql(path, "UPDATE wallets SET hawken_points=29 WHERE account_id=1");
    }
    LocalStore store(path, "inventory-server");
    FakeVerifier verifier;
    AuthService auth(store, verifier, [] { return std::int64_t{1001}; });
    InventoryService inventory(store, auth, ids(), balance(), offer_id);
    const auto login = auth.login(R"({"SteamAuthTicket":"AABB0100"})");
    REQUIRE(login.status == 200);
    const auto result = inventory.get(owned_path(login), header(login));
    REQUIRE(result.status == 200);
    CHECK(result.result == before);
    check_owned(result.result);
    const auto account = store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK(store.wallet(account->ref) == hawken::store::WalletBalances{29, 0});
}

TEST_CASE("Owned inventory path binds its authenticated account before any starter grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    failure(f.inventory.get("/userGameItems/00000000-0000-0000-0000-000000000000",
                            header(login)), 401, header(login).substr(6));
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK_FALSE(f.store.starter_items(account->ref));
}

TEST_CASE("Every inventory route rejects malformed and rotated grants") {
    Fixture f;
    const auto first = f.login();
    REQUIRE(first.status == 200);
    const auto old_header = header(first);
    for (const auto& target : targets(first)) {
        for (const auto& value : std::vector<std::string>{"", "Basic wrong", "Bearer wrong",
                old_header.substr(6), "basic " + old_header.substr(6), old_header + " "})
            failure(f.inventory.get(target, value), 401, old_header.substr(6));
    }
    const auto second = f.login();
    REQUIRE(second.status == 200);
    for (const auto& target : targets(second)) {
        failure(f.inventory.get(target, old_header), 401, old_header.substr(6));
        CHECK(f.inventory.get(target, header(second)).status == 200);
    }
}

TEST_CASE("Inventory rechecks expiry Steam revocation and bans before granting items") {
    SUBCASE("expiry") {
        for (std::size_t i = 0; i < 3; ++i) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            f.now = 1900;
            failure(f.inventory.get(targets(login)[i], header(login)), 401);
            CHECK_FALSE(f.store.starter_items(f.store.find_account(fixture_id_text)->ref));
        }
    }
    SUBCASE("revocation") {
        for (std::size_t i = 0; i < 3; ++i) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            f.verifier.active = 0;
            failure(f.inventory.get(targets(login)[i], header(login)), 401);
            CHECK_FALSE(f.store.starter_items(f.store.find_account(fixture_id_text)->ref));
        }
    }
    SUBCASE("ban") {
        for (std::size_t i = 0; i < 3; ++i) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            const auto account = f.store.find_account(fixture_id_text);
            REQUIRE(account.has_value());
            f.store.set_ban(account->ref, {1000, std::nullopt, "private-ban-fixture"});
            failure(f.inventory.get(targets(login)[i], header(login)), 403, "private-ban-fixture");
            CHECK_FALSE(f.store.starter_items(account->ref));
        }
    }
}

TEST_CASE("Clock reversal and clock exceptions fail closed for all inventory routes") {
    SUBCASE("reversal") {
        for (std::size_t i = 0; i < 3; ++i) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            f.now = 999;
            failure(f.inventory.get(targets(login)[i], header(login)), 401);
        }
    }
    SUBCASE("exception") {
        for (std::size_t i = 0; i < 3; ++i) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            f.clock_throws = true;
            failure(f.inventory.get(targets(login)[i], header(login)), 500, "private-clock-fixture");
        }
    }
}

TEST_CASE("Existing persisted instances are returned even when they differ from generated starter IDs") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    auto stored = persisted_expected();
    for (std::size_t i = 0; i < stored.size(); ++i)
        stored[i].instance_id = "30000000-0000-0000-0000-00000000000" + std::to_string(i + 1);
    REQUIRE(f.store.grant_starter_items(account->ref, stored));
    const auto reply = f.inventory.get(owned_path(login), header(login));
    REQUIRE(reply.status == 200);
    REQUIRE(reply.result.size() == 9);
    for (std::size_t i = 0; i < stored.size(); ++i)
        CHECK(reply.result.at(i).at("UserGameItemGuid") == stored[i].instance_id);
    CHECK(reply.result.at(0).at("ReadOnly_Children") == nlohmann::json::array({
        stored[1].instance_id, stored[2].instance_id, stored[3].instance_id, stored[4].instance_id,
        stored[5].instance_id, stored[6].instance_id, stored[7].instance_id, stored[8].instance_id}));
    CHECK(reply.result.at(0).at("Preset.PrimaryWeapon_InstanceId_0") == stored[1].instance_id);
    CHECK(f.store.starter_items(account->ref) == stored);
}

TEST_CASE("Generated instance collision with a configured type fails before any partial grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    auto changed = ids();
    changed.mech = instance_ids[0];
    InventoryService collision(f.store, f.auth, changed, balance(), offer_id);
    failure(collision.get(owned_path(login), header(login)), 500, instance_ids[0]);
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    CHECK_FALSE(f.store.starter_items(account->ref));
    CHECK(f.store.wallet(account->ref) == hawken::store::WalletBalances{});
}

TEST_CASE("Changed type policy does not replace or regrant existing owned items") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    REQUIRE(f.inventory.get(owned_path(login), header(login)).status == 200);
    auto changed = ids();
    changed.primary = "10000000-0000-0000-0000-000000000099";
    InventoryService alternate(f.store, f.auth, changed, balance(), offer_id);
    failure(alternate.get(owned_path(login), header(login)), 500);
    CHECK(f.store.starter_items(f.store.find_account(fixture_id_text)->ref) == persisted_expected());
    CHECK(f.inventory.get(owned_path(login), header(login)).status == 200);
}

TEST_CASE("A recorded grant with a missing row is not silently repaired by owned reads") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    REQUIRE(f.inventory.get(owned_path(login), header(login)).status == 200);
    fixture_sql(f.temporary.path() / "accounts.sqlite",
                "DELETE FROM starter_items WHERE account_id=1 AND slot=8");
    failure(f.inventory.get(owned_path(login), header(login)), 500);
    CHECK_THROWS_AS(f.store.starter_items(f.store.find_account(fixture_id_text)->ref),
                    hawken::store::StoreError);
    // The independent store error after HTTP failure establishes no row repair.
}

TEST_CASE("Two account inventories share type definitions but never owned IDs or access") {
    Fixture f;
    const auto first = f.login();
    REQUIRE(first.status == 200);
    const auto first_items = f.inventory.get(owned_path(first), header(first));
    REQUIRE(first_items.status == 200);
    const auto second = f.login(true);
    REQUIRE(second.status == 200);
    CHECK(guid(second) == "28d639e0-22af-9e81-a8f3-bc84c48c55a1");
    failure(f.inventory.get(owned_path(first), header(second)), 401);
    failure(f.inventory.get(owned_path(second), header(first)), 401);
    const auto second_items = f.inventory.get(owned_path(second), header(second));
    REQUIRE(second_items.status == 200);
    REQUIRE(second_items.result.size() == 9);
    CHECK(second_items.result.at(0).at("UserGameItemGuid") == "ef47e206-cac6-8d37-ccd4-98032481e04c");
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(second_items.result.at(i).at("GameItemsGuid") == type_ids[i]);
        for (std::size_t j = 0; j < 9; ++j)
            CHECK(second_items.result.at(i).at("UserGameItemGuid")
                  != first_items.result.at(j).at("UserGameItemGuid"));
    }
    CHECK(f.store.starter_items(f.store.find_account(fixture_id_text)->ref) == persisted_expected());
    REQUIRE(f.store.starter_items(f.store.find_account(fixture_second_id_text)->ref).has_value());
}

TEST_CASE("Server databases isolate account identity persisted instances and grants") {
    Fixture a, b("other-server");
    const auto first = a.login(), second = b.login();
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(guid(second) == "9442931b-c611-dcc2-b0f5-7f640081a68f");
    const auto a_items = a.inventory.get(owned_path(first), header(first));
    const auto b_items = b.inventory.get(owned_path(second), header(second));
    REQUIRE(a_items.status == 200);
    REQUIRE(b_items.status == 200);
    CHECK(b_items.result.at(0).at("UserGameItemGuid") == "5a8f1dbc-6724-402c-b16f-d04da248ea37");
    for (const auto& target : targets(first)) failure(a.inventory.get(target, header(second)), 401);
    for (const auto& target : targets(second)) failure(b.inventory.get(target, header(first)), 401);
    hawken::test::TempDirectory other;
    LocalStore wrong_store(other.path() / "private-other.sqlite", "wrong-server");
    InventoryService mismatched(wrong_store, a.auth, ids(), balance(), offer_id);
    failure(mismatched.get(owned_path(first), header(first)), 500, "private-other.sqlite");
    CHECK_FALSE(wrong_store.find_account(fixture_id_text));
}

TEST_CASE("Concurrent first owned reads publish the same complete persisted grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto path = owned_path(login), authorization = header(login);
    std::array<AuthReply, 4> replies;
    std::array<std::exception_ptr, 4> errors;
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] {
            try { replies[i] = f.inventory.get(path, authorization); }
            catch (...) { errors[i] = std::current_exception(); }
        });
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) CHECK(error == nullptr);
    for (const auto& reply : replies) {
        REQUIRE(reply.status == 200);
        CHECK(reply.result == replies[0].result);
    }
    check_owned(replies[0].result);
    CHECK(f.store.starter_items(f.store.find_account(fixture_id_text)->ref) == persisted_expected());
}

TEST_CASE("Optional FTUX Globals is catalogue configuration never an owned starter or wallet grant") {
    Fixture f;
    auto profile = balance();
    constexpr const char* globals_id = "40000000-0000-0000-0000-000000000001";
    profile.ftux_test = hawken::inventory::FtuxTest{globals_id, "trial.ftux-1", 1, 20};
    InventoryService trial(f.store, f.auth, ids(), profile, offer_id);
    const auto login = f.login(); REQUIRE(login.status == 200);
    const auto account = f.store.find_account(fixture_id_text); REQUIRE(account);
    const auto before = f.store.wallet(account->ref);
    const auto catalogue = trial.get(catalogue_path, header(login));
    REQUIRE(catalogue.status == 200); REQUIRE(catalogue.result.is_array());
    CHECK(catalogue.result.size() == 10);
    if (catalogue.result.size() == 10)
        CHECK(catalogue.result.at(9) == nlohmann::json{{"GameItemsGuid", globals_id},
            {"ItemName", "Globals"}, {"Version", "trial.ftux-1"}, {"invType", "41"},
            {"FTUXMatchTime", "1"}, {"FTUXMatchGoal", "20"}});
    CHECK_FALSE(f.store.starter_items(account->ref)); // Catalogue read alone grants nothing.
    const auto owned = trial.get(owned_path(login), header(login)); REQUIRE(owned.status == 200);
    check_owned(owned.result);
    CHECK(f.store.starter_items(account->ref) == persisted_expected());
    CHECK(f.store.wallet(account->ref) == before);
    const auto offers = trial.get(offers_path, header(login)); REQUIRE(offers.status == 200);
    REQUIRE(offers.result.size() == 1); CHECK(offers.result.at(0).at("GameItems").size() == 9);
    for (const auto& row : offers.result.at(0).at("GameItems")) CHECK(row.at("GameItemsGuid") != globals_id);
    // Inspect only this private fixture DB, not the local runtime account database.
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2((f.temporary.path() / "accounts.sqlite").string().c_str(),
        &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, SqlCloser> database(raw); REQUIRE(opened == SQLITE_OK);
    sqlite3_stmt* raw_statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(),
        "SELECT count(*),sum(type_id=?) FROM starter_items WHERE account_id=?", -1, &raw_statement, nullptr);
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement); REQUIRE(prepared == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement.get(), 1, globals_id, -1, SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement.get(), 2, account->ref.local_id) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(statement.get(), 0) == 9);
    CHECK(sqlite3_column_int64(statement.get(), 1) == 0);
    CHECK(sqlite3_step(statement.get()) == SQLITE_DONE);
}
