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

TEST_CASE("FTUX optional original menu choice never seeds account decisions or changes owned inventory") {
    for (const bool enabled : {true, false}) {
        CAPTURE(enabled);
        Fixture f;
        auto profile = balance();
        profile.ftux_test = hawken::inventory::FtuxTest{
            "40000000-0000-0000-0000-000000000001", "trial.ftux-1", 1, 20};
        profile.ftux_test->enable_new_user_dialog = enabled;
        InventoryService trial(f.store, f.auth, ids(), profile, offer_id);
        const auto login = f.login(); REQUIRE(login.status == 200);
        const auto account = f.store.find_account(fixture_id_text); REQUIRE(account);
        const auto before = f.store.wallet(account->ref);
        const auto catalogue = trial.get(catalogue_path, header(login)); REQUIRE(catalogue.status == 200);
        REQUIRE(catalogue.result.size() == 10);
        CHECK(catalogue.result.at(9) == (enabled
            ? nlohmann::json::parse(R"({"GameItemsGuid":"40000000-0000-0000-0000-000000000001","ItemName":"Globals","Version":"trial.ftux-1","invType":"41","FTUXMatchTime":"1","FTUXMatchGoal":"20","EnableNewUserDialog":"true"})")
            : nlohmann::json::parse(R"({"GameItemsGuid":"40000000-0000-0000-0000-000000000001","ItemName":"Globals","Version":"trial.ftux-1","invType":"41","FTUXMatchTime":"1","FTUXMatchGoal":"20","EnableNewUserDialog":"false"})")));
        CHECK_FALSE(f.store.starter_items(account->ref));
        CHECK(f.store.game_settings(account->ref).empty());
        const auto owned = trial.get(owned_path(login), header(login)); REQUIRE(owned.status == 200);
        check_owned(owned.result);
        CHECK(f.store.starter_items(account->ref) == persisted_expected());
        CHECK(f.store.wallet(account->ref) == before);
        CHECK(f.store.game_settings(account->ref).empty());
        const auto offers = trial.get(offers_path, header(login)); REQUIRE(offers.status == 200);
        REQUIRE(offers.result.size() == 1);
        CHECK(offers.result.at(0).at("GameItems").size() == 9);
    }
}

namespace {
std::string broker_path(const AuthReply& login, std::string_view instance = instance_ids[0]) {
    return "/userGameItemsBroker/" + guid(login) + "/" + std::string(instance);
}
void saved_reply(const AuthReply& reply) {
    CHECK(reply.status == 200);
    CHECK(reply.result.is_null()); // ApiResult<void>, not an invented item DTO.
}
}
TEST_CASE("Broker save is exact raw two-ID PUT and never expands individual GET") {
    Fixture f; const auto login = f.login(); REQUIRE(login.status == 200);
    const auto path = broker_path(login);
    CHECK(f.inventory.supports_update_target(path));
    CHECK_FALSE(f.inventory.supports_target(path));
    failure(f.inventory.get(path, header(login)), 404);
    const auto uppercase = "/userGameItemsBroker/A54C90D7-d30b-8e6b-2417-3d744d78bd17/" + std::string(instance_ids[0]);
    for (const auto& alias : std::vector<std::string>{path+"/", path+"?x=1", path+"#x", path+"/extra",
        uppercase, "/userGameItemsBroker/"+guid(login)+"/%34"+std::string(instance_ids[0]).substr(1),
        "/userGameItemsBroker/"+guid(login), "/userGameItemsBroker/00000000-0000-0000-0000-000000000000/"+std::string(instance_ids[0]),
        "/userGameItemsBroker/"+guid(login)+"/00000000-0000-0000-0000-000000000000"}) {
        CAPTURE(alias); CHECK_FALSE(f.inventory.supports_update_target(alias));
        failure(f.inventory.put(alias, "", "{}"), 404);
    }
    const auto individual = "/userGameItems/"+guid(login)+"/"+instance_ids[0];
    CHECK_FALSE(f.inventory.supports_target(individual));
    failure(f.inventory.get(individual, header(login)),404);
}
TEST_CASE("Broker cannot grant items and updates only the already owned mech slot") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    failure(f.inventory.put(broker_path(login),header(login),R"({"Preset.Fixture":"v"})"),404);
    CHECK_FALSE(f.store.starter_items(account->ref));
    const auto before=f.inventory.get(owned_path(login),header(login)); REQUIRE(before.status==200);
    for(std::size_t slot=1;slot<9;++slot)
        failure(f.inventory.put(broker_path(login,instance_ids[slot]),header(login),R"({"Preset.Fixture":"v"})"),404);
    failure(f.inventory.put(broker_path(login,"30000000-0000-0000-0000-000000000099"),header(login),"{}"),404);
    CHECK(f.inventory.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
}
TEST_CASE("Broker merges Presets and replays void ACK without modifying root identity graph or balances") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login)); REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    fixture_sql(f.temporary.path()/"accounts.sqlite","UPDATE wallets SET hawken_points=73 WHERE account_id=1");
    const std::string first=R"({"Preset.Fixture":"received","Preset.ProgressionUnlockFlags":"opaque-not-entitlement","Preset.BonusXpMultiplier":"opaque-not-reward"})";
    saved_reply(f.inventory.put(broker_path(login),header(login),first));
    saved_reply(f.inventory.put(broker_path(login),header(login),first));
    saved_reply(f.inventory.put(broker_path(login),header(login),R"({"Preset.Fixture":"","Preset.Fixture2":"second-batch"})"));
    saved_reply(f.inventory.put(broker_path(login),header(login),"{}"));
    auto expected=before.result;
    expected.at(0)["Preset.Fixture"]=""; expected.at(0)["Preset.Fixture2"]="second-batch";
    expected.at(0)["Preset.ProgressionUnlockFlags"]="opaque-not-entitlement";
    expected.at(0)["Preset.BonusXpMultiplier"]="opaque-not-reward";
    const auto after=f.inventory.get(owned_path(login),header(login)); REQUIRE(after.status==200);
    CHECK(after.result==expected); // Complete comparison preserves every immutable row/field.
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==hawken::store::ItemAttributes{
        {"Preset.Fixture",""},{"Preset.Fixture2","second-batch"},
        {"Preset.ProgressionUnlockFlags","opaque-not-entitlement"},{"Preset.BonusXpMultiplier","opaque-not-reward"}});
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{73,0});
    CHECK(f.store.game_settings(account->ref).empty()); CHECK(f.store.visits(account->ref).empty());
}
TEST_CASE("Broker references bind exact existing starter roles while absent optional domains fail closed") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    const std::array<std::pair<const char*,std::size_t>,8> references{{
        {"Preset.PrimaryWeapon_InstanceId_0",1},{"Preset.SecondaryWeapon_InstanceId",2},
        {"Preset.Style.UpperPart_InstanceId",3},{"Preset.Style.MidPart_InstanceId",4},
        {"Preset.Style.ArmPart_InstanceId",5},{"Preset.Style.LowerPart_InstanceId",6},
        {"Preset.Style.DroneType_InstanceId",7},{"Preset.Ability_InstanceId",8}}};
    for(const auto& [key,slot]:references) {
        saved_reply(f.inventory.put(broker_path(login),header(login),nlohmann::json{{key,instance_ids[slot]}}.dump()));
        failure(f.inventory.put(broker_path(login),header(login),nlohmann::json{{key,instance_ids[0]}}.dump()),400);
        failure(f.inventory.put(broker_path(login),header(login),nlohmann::json{{key,"30000000-0000-0000-0000-000000000099"}}.dump()),400);
    }
    for(const auto key:{"Preset.Items1_InstanceId","Preset.Internals1_InstanceId","Preset.Style.CamoPattern_InstanceId",
                       "Preset.Style.CockpitDecoration0_InstanceId","Preset.Unknown_InstanceId_0"}) {
        failure(f.inventory.put(broker_path(login),header(login),nlohmann::json{{key,instance_ids[1]}}.dump()),400);
        saved_reply(f.inventory.put(broker_path(login),header(login),nlohmann::json{{key,""}}.dump()));
    }
    CHECK(f.store.starter_items(f.store.find_account(fixture_id_text)->ref)==persisted_expected());
}
TEST_CASE("Broker rejects whole invalid batches including duplicate nested and immutable fields before commit") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    for(const auto& value:std::vector<std::string>{"", "[]", "null", "1", "{}garbage",
        R"({"Preset.Fixture":1})",R"({"Preset.Fixture":true})",R"({"Preset.Fixture":null})",
        R"({"Preset.Fixture":[]})",R"({"Preset.Fixture":{}})",R"({"Preset.Fixture":"a","Preset.Fixture":"b"})",
        R"({"Preset.Fixture":"a","Preset.\u0046ixture":"b"})",R"({"Preset.Fixture":"a","ReadOnly_Parent":""})",
        R"({"GameItemsGuid":"private-type"})",R"({"UserGameItemGuid":"private-instance"})",
        R"({"ItemName":"changed"})",R"({"Version":"changed"})",R"({"preset.Fixture":"x"})",
        R"({"Preset.Fixture\u0000":"x"})",R"({"Preset.Fixture":"x\u0000"})",
        nlohmann::json{{"Preset.Fixture",std::string(16385,'v')}}.dump()}) {
        CAPTURE(value.size());failure(f.inventory.put(broker_path(login),header(login),value),400);
        CHECK(f.inventory.get(owned_path(login),header(login)).result==before.result);
    }
}
TEST_CASE("Broker accepts bounded SDK batches and refuses one byte or key over the local budgets") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    auto keys=nlohmann::json::object();for(int i=0;i<256;++i) keys["Preset.K"+std::to_string(i)]="v";
    saved_reply(f.inventory.put(broker_path(login),header(login),keys.dump()));
    keys["Preset.Excess"]="v";failure(f.inventory.put(broker_path(login),header(login),keys.dump()),400);
    auto large=nlohmann::json::object();for(int i=0;i<5;++i) large["Preset.Large"+std::to_string(i)]=std::string(12000,'x');
    const auto unpadded=large.dump(); REQUIRE(unpadded.size()<65536);
    // Spaces are JSON whitespace, so the wire budget tests do not evade value limits.
    const auto exact=unpadded+std::string(65536-unpadded.size(),' ');
    saved_reply(f.inventory.put(broker_path(login),header(login),exact));
    failure(f.inventory.put(broker_path(login),header(login),exact+" "),400);
    const auto after=f.inventory.get(owned_path(login),header(login));REQUIRE(after.status==200);
    CHECK(after.result.at(0).contains("Preset.Large0"));
    if(after.result.at(0).contains("Preset.Large0"))
        CHECK(after.result.at(0).at("Preset.Large0")==std::string(12000,'x'));
    CHECK_FALSE(after.result.at(0).contains("Preset.Excess"));
}
TEST_CASE("Broker rechecks grant account revocation expiry bans and clocks without changing items") {
    for(const auto mode:{0,1,2,3,4,5}) {
        CAPTURE(mode);Fixture f;const auto login=f.login();REQUIRE(login.status==200);
        REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
        const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
        auto authorization=header(login);int expected=401;
        if(mode==0)authorization="";
        if(mode==1)f.verifier.active=0;
        if(mode==2)f.now=1900;
        if(mode==3){f.store.set_ban(account->ref,{1000,std::nullopt,"private-ban-fixture"});expected=403;}
        if(mode==4)f.now=999;
        if(mode==5){f.clock_throws=true;expected=500;}
        failure(f.inventory.put(broker_path(login),authorization,R"({"Preset.Fixture":"x"})"),expected);
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
        CHECK(f.store.starter_items(account->ref)==persisted_expected());
        CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    }
}
TEST_CASE("Broker rejects foreign account paths items and equipment reference identities") {
    Fixture f;const auto first=f.login();REQUIRE(first.status==200);
    const auto old=f.inventory.get(owned_path(first),header(first));REQUIRE(old.status==200);
    const auto second=f.login(true);REQUIRE(second.status==200);
    const auto own=f.inventory.get(owned_path(second),header(second));REQUIRE(own.status==200);
    const auto own_id=own.result.at(0).at("UserGameItemGuid").get<std::string>();
    failure(f.inventory.put(broker_path(first),header(second),"{}"),401);
    failure(f.inventory.put(broker_path(second,instance_ids[0]),header(second),"{}"),404);
    failure(f.inventory.put(broker_path(second,own_id),header(second),
        nlohmann::json{{"Preset.PrimaryWeapon_InstanceId_0",instance_ids[1]}}.dump()),400);
    CHECK(f.inventory.get(owned_path(second),header(second)).result==own.result);
    CHECK(f.store.owned_item_attributes(f.store.find_account(fixture_id_text)->ref,instance_ids[0]).empty());
}
TEST_CASE("Broker Preset merge survives service store restart and does not revive an old grant") {
    hawken::test::TempDirectory temporary;const auto path=temporary.path()/"accounts.sqlite";
    std::string old_header; nlohmann::json expected;
    {
        LocalStore store(path,"inventory-server");FakeVerifier verifier;
        AuthService auth(store,verifier,[]{return std::int64_t{1000};});InventoryService inventory(store,auth,ids(),balance(),offer_id);
        const auto login=auth.login(R"({"SteamAuthTicket":"AABB0100"})");REQUIRE(login.status==200);
        const auto before=inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
        old_header=header(login);expected=before.result;expected.at(0)["Preset.RestartFixture"]="opaque-received";
        saved_reply(inventory.put(broker_path(login),old_header,R"({"Preset.RestartFixture":"opaque-received"})"));
    }
    LocalStore store(path,"inventory-server");FakeVerifier verifier;
    AuthService auth(store,verifier,[]{return std::int64_t{1001};});InventoryService inventory(store,auth,ids(),balance(),offer_id);
    const auto login=auth.login(R"({"SteamAuthTicket":"AABB0100"})");REQUIRE(login.status==200);
    failure(inventory.put(broker_path(login),old_header,"{}"),401);
    CHECK(inventory.get(owned_path(login),header(login)).result==expected);
    saved_reply(inventory.put(broker_path(login),header(login),R"({"Preset.RestartFixture":"opaque-received"})"));
    CHECK(store.starter_items(store.find_account(fixture_id_text)->ref)==persisted_expected());
    CHECK(store.wallet(store.find_account(fixture_id_text)->ref)==hawken::store::WalletBalances{});
}

TEST_CASE("Native empty sentinel is preserved only in the thirteen helper-produced Preset fields") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    // Original dcad70->db7c20, not a family-based entitlement rule. The cockpit
    // loop increments before formatting, hence indices 1..3 (not 0..2).
    const auto sentinels=nlohmann::json::parse(R"({
        "Preset.Items1_InstanceId":"empty","Preset.Items2_InstanceId":"empty","Preset.Items3_InstanceId":"empty",
        "Preset.Internals1_InstanceId":"empty","Preset.Internals2_InstanceId":"empty","Preset.Internals3_InstanceId":"empty",
        "Preset.Internals4_InstanceId":"empty","Preset.Internals5_InstanceId":"empty","Preset.Internals6_InstanceId":"empty",
        "Preset.Style.AnimTauntType_InstanceId":"empty","Preset.Style.CockpitDecoration1_InstanceId":"empty",
        "Preset.Style.CockpitDecoration2_InstanceId":"empty","Preset.Style.CockpitDecoration3_InstanceId":"empty"})");
    saved_reply(f.inventory.put(broker_path(login),header(login),sentinels.dump()));
    saved_reply(f.inventory.put(broker_path(login),header(login),sentinels.dump()));
    auto expected=before.result;
    for(const auto& [key,value]:sentinels.items())expected.at(0)[key]=value;
    CHECK(f.inventory.get(owned_path(login),header(login)).result==expected);
    const auto attributes=f.store.owned_item_attributes(account->ref,instance_ids[0]);
    CHECK(attributes.size()==13);
    for(const auto& entry:sentinels.items()) {
        const auto found=attributes.find(entry.key());REQUIRE(found!=attributes.end());
        CHECK(found->second=="empty"); // Remains opaque, not '' or a new UUID.
    }
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(f.store.game_settings(account->ref).empty());
}
TEST_CASE("Native empty sentinel does not authorize unknown indexed or direct equipment references") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    for(const auto* key:{"Preset.PrimaryWeapon_InstanceId_0","Preset.SecondaryWeapon_InstanceId","Preset.Ability_InstanceId",
        "Preset.Style.UpperPart_InstanceId","Preset.Style.MidPart_InstanceId","Preset.Style.ArmPart_InstanceId",
        "Preset.Style.LowerPart_InstanceId","Preset.Style.DroneType_InstanceId","Preset.Style.CamoPattern_InstanceId",
        "Preset.Unknown_InstanceId","Preset.Unknown_InstanceId_0","Preset.Items0_InstanceId","Preset.Items4_InstanceId",
        "Preset.Internals0_InstanceId","Preset.Internals7_InstanceId","Preset.Style.AnimTauntType_InstanceId_0",
        "Preset.Style.CockpitDecoration0_InstanceId","Preset.Style.CockpitDecoration4_InstanceId"}) {
        CAPTURE(key);
        failure(f.inventory.put(broker_path(login),header(login),
            nlohmann::json{{"Preset.Fixture","must-not-commit"},{key,"empty"}}.dump()),400);
    }
    for(const auto* value:{"EMPTY","Empty","empty "," empty"})
        failure(f.inventory.put(broker_path(login),header(login),
            nlohmann::json{{"Preset.Items1_InstanceId",value}}.dump()),400);
    CHECK(f.inventory.get(owned_path(login),header(login)).result==before.result);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}


TEST_CASE("Broker observer reports fixed rejection reasons exactly once without item mutation") {
    using hawken::inventory::PutReason;
    using hawken::inventory::PutObservation;
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login)); REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){ observed.push_back(observation); });
    auto too_many=nlohmann::json::object();
    for(int i=0;i<257;++i)too_many["Preset.K"+std::to_string(i)]="v";
    const std::vector<std::pair<std::string,PutReason>> cases{
        {"",PutReason::json_parse},{"null",PutReason::json_shape},
        {R"({"Preset.Fixture":{}})",PutReason::json_shape},
        {R"({"Preset.Fixture":[]})",PutReason::json_shape},
        {R"({"Preset.Fixture":"a","Preset.Fixture":"b"})",PutReason::duplicate_key},
        {too_many.dump(),PutReason::key_limit},
        {R"({"ReadOnly_Parent":""})",PutReason::preset_key},
        {R"({"Preset.Fixture":1})",PutReason::value_type},
        {nlohmann::json{{"Preset.Unknown_InstanceId",instance_ids[1]}}.dump(),PutReason::reference_unknown_key},
        {R"({"Preset.Ability_InstanceId":"empty"})",PutReason::reference_sentinel},
        {nlohmann::json{{"Preset.PrimaryWeapon_InstanceId_0",instance_ids[0]}}.dump(),PutReason::reference_instance},
        {nlohmann::json{{"Preset.Fixture",std::string(16385,'v')}}.dump(),PutReason::store_validation},
        {std::string(65537,' '),PutReason::body_limit}};
    for(const auto& [body,reason]:cases) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),body),400);
        REQUIRE(observed.size()==count+1); CHECK(observed.back().reason==reason);
        if(reason!=PutReason::reference_unknown_key) {
            CHECK(observed.back().reference_family==hawken::inventory::ReferenceFamily::none);
            CHECK(observed.back().value_form==hawken::inventory::ReferenceValueForm::none);
        }
        CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    }
    failure(diagnosed.put(broker_path(login)+"/",header(login),"{}"),404);
    REQUIRE(observed.size()==cases.size()+1); CHECK(observed.back().reason==PutReason::target);
    failure(diagnosed.put(broker_path(login),"","{}"),401);
    REQUIRE(observed.size()==cases.size()+2); CHECK(observed.back().reason==PutReason::authentication);
    failure(diagnosed.put(broker_path(login,instance_ids[1]),header(login),"{}"),404);
    REQUIRE(observed.size()==cases.size()+3); CHECK(observed.back().reason==PutReason::ownership);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(hawken::inventory::put_reason_name(PutReason::reference_unknown_key)=="reference_unknown_key");
    CHECK(hawken::inventory::put_reason_name(PutReason::store_validation)=="store_validation");
    CHECK(hawken::inventory::put_reason_name(static_cast<PutReason>(999))=="internal");
}
TEST_CASE("Broker success observer runs after SQL commit and outside the service mutex") {
    using hawken::inventory::PutReason;
    using hawken::inventory::PutObservation;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    InventoryService* self=nullptr; std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,[&](const PutObservation& observation) {
        observed.push_back(observation); CHECK(observation.reason==PutReason::success);
        CHECK(observation.reference_family==hawken::inventory::ReferenceFamily::none);
        CHECK(observation.value_form==hawken::inventory::ReferenceValueForm::none);
        const auto saved=f.store.owned_item_attributes(account->ref,instance_ids[0]);
        REQUIRE(saved.contains("Preset.ObserverFixture")); CHECK(saved.at("Preset.ObserverFixture")=="received");
        // Reentry must succeed: the PUT lock is already released, and GET sees commit.
        REQUIRE(self!=nullptr);
        const auto read=self->get(owned_path(login),header(login));REQUIRE(read.status==200);
        CHECK(read.result.at(0).at("Preset.ObserverFixture")=="received");
    });
    self=&diagnosed;
    saved_reply(diagnosed.put(broker_path(login),header(login),R"({"Preset.ObserverFixture":"received"})"));
    REQUIRE(observed.size()==1);CHECK(observed.front().reason==PutReason::success);
    saved_reply(diagnosed.put(broker_path(login),header(login),"{}"));
    REQUIRE(observed.size()==2);CHECK(observed.back().reason==PutReason::success);
}
TEST_CASE("Throwing Broker observer cannot change void success or fixed rejection") {
    using hawken::inventory::PutReason;
    using hawken::inventory::PutObservation;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,[&](const PutObservation& observation) {
        observed.push_back(observation); throw std::runtime_error("private-observer-fixture");
    });
    saved_reply(diagnosed.put(broker_path(login),header(login),R"({"Preset.ObserverFixture":"received"})"));
    REQUIRE(observed.size()==1);CHECK(observed.back().reason==PutReason::success);
    const auto before=diagnosed.get(owned_path(login),header(login));REQUIRE(before.status==200);
    failure(diagnosed.put(broker_path(login),header(login),R"({"Preset.Fixture":true})"),400,"private-observer-fixture");
    REQUIRE(observed.size()==2);CHECK(observed.back().reason==PutReason::value_type);
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
}

TEST_CASE("Broker first unknown reference reports exact native family without changing rejection") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    // Independent exact literals from dcad70/db7c20. These fixtures deliberately
    // use an incompatible primary instance; diagnostics do not authorize it.
    const std::vector<std::pair<const char*,ReferenceFamily>> cases{
        {"Preset.Style.CamoPattern_InstanceId",ReferenceFamily::camo},
        {"Preset.Style.CamoPatternMiddle_InstanceId",ReferenceFamily::camo},
        {"Preset.Style.CamoPatternLower_InstanceId",ReferenceFamily::camo},
        {"Preset.Style.CamoPatternArm_InstanceId",ReferenceFamily::camo},
        {"Preset.Style.BoosterPart_InstanceId",ReferenceFamily::booster},
        {"Preset.Style.ArmorPart_InstanceId",ReferenceFamily::armor},
        {"Preset.VisorStyle.PrimaryReticle_InstanceId",ReferenceFamily::visor},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId",ReferenceFamily::visor},
        {"Preset.VisorStyle.FriendHUDColor_InstanceId",ReferenceFamily::visor},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId",ReferenceFamily::visor},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId",ReferenceFamily::visor},
        {"Preset.Items1_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Items2_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Items3_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals1_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals2_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals3_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals4_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals5_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Internals6_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Style.AnimTauntType_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration1_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration2_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration3_InstanceId",ReferenceFamily::helper_optional},
        {"Preset.Style.CamoPattern_InstanceId_0",ReferenceFamily::other},
        {"Preset.Style.BoosterPart_InstanceId_1",ReferenceFamily::other},
        {"Preset.VisorStyle.Unknown_InstanceId",ReferenceFamily::other},
        {"Preset.Items4_InstanceId",ReferenceFamily::other},
        {"Preset.Style.CockpitDecoration0_InstanceId",ReferenceFamily::other}};
    for(const auto& [key,family]:cases) {
        const auto count=observed.size();
        const auto body=nlohmann::json{{"Preset.Fixture","must-not-commit"},{key,instance_ids[1]}}.dump();
        failure(diagnosed.put(broker_path(login),header(login),body),400,instance_ids[1]);
        REQUIRE(observed.size()==count+1);
        CHECK(observed.back().reason==PutReason::reference_unknown_key);
        CHECK(observed.back().reference_family==family);
        CHECK(observed.back().value_form==ReferenceValueForm::matches_owned);
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    }
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(f.store.game_settings(account->ref).empty());
}
TEST_CASE("Broker unknown reference value forms distinguish owned instances from type and zero UUIDs") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    auto check=[&](const char* value,ReferenceValueForm expected) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),
            nlohmann::json{{"Preset.Items1_InstanceId",value}}.dump()),400,value);
        REQUIRE(observed.size()==count+1);
        CHECK(observed.back().reason==PutReason::reference_unknown_key);
        CHECK(observed.back().reference_family==ReferenceFamily::helper_optional);
        CHECK(observed.back().value_form==expected);
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    };
    // All nine instances, including the mech, match before the generic UUID form.
    // A type identifier remains canonical_uuid; it is not an owned instance.
    for(const auto* value:instance_ids)check(value,ReferenceValueForm::matches_owned);
    for(const auto* value:type_ids)check(value,ReferenceValueForm::canonical_uuid);
    check("00000000-0000-0000-0000-000000000000",ReferenceValueForm::zero_uuid);
    check("abcdefab-cdef-abcd-efab-cdefabcdefab",ReferenceValueForm::canonical_uuid);
    for(const auto* value:{"ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB",
        "abcdefab-cdef-abcd-efab-cdefabcdefab ","00000000000000000000000000000000",
        "00000000_0000-0000-0000-000000000000","EMPTY","private-value-fixture"})
        check(value,ReferenceValueForm::other);
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Broker observation reports only first unknown reference and no categories for other outcomes") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService* self=nullptr;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation) {
            observed.push_back(observation);
            // Reentry on a rejection also proves the service lock is released.
            REQUIRE(self!=nullptr);
            CHECK(self->get(owned_path(login),header(login)).status==200);
        });
    self=&diagnosed;
    // nlohmann's existing object traversal is key-sorted, not raw request order.
    failure(diagnosed.put(broker_path(login),header(login),
        nlohmann::json{{"Preset.Style.CamoPattern_InstanceId",instance_ids[1]},
            {"Preset.Items1_InstanceId","00000000-0000-0000-0000-000000000000"}}.dump()),400);
    REQUIRE(observed.size()==1);
    CHECK(observed.back().reason==PutReason::reference_unknown_key);
    CHECK(observed.back().reference_family==ReferenceFamily::helper_optional);
    CHECK(observed.back().value_form==ReferenceValueForm::zero_uuid);
    const auto count=observed.size();
    const std::vector<std::pair<std::string,PutReason>> other_outcomes{
        {R"({"Preset.A":1,"Preset.Items1_InstanceId":"private-value-fixture"})",PutReason::value_type},
        {R"({"Preset.Ability_InstanceId":"empty"})",PutReason::reference_sentinel},
        {nlohmann::json{{"Preset.Ability_InstanceId",instance_ids[1]}}.dump(),PutReason::reference_instance},
        {R"({"Preset.Items1_InstanceId":""})",PutReason::success},
        {R"({"Preset.Items1_InstanceId":"empty"})",PutReason::success}};
    for(const auto& [body,reason]:other_outcomes) {
        const auto prior=observed.size();
        const auto reply=diagnosed.put(broker_path(login),header(login),body);
        if(reason==PutReason::success)saved_reply(reply);else failure(reply,400);
        REQUIRE(observed.size()==prior+1);
        CHECK(observed.back().reason==reason);
        CHECK(observed.back().reference_family==ReferenceFamily::none);
        CHECK(observed.back().value_form==ReferenceValueForm::none);
    }
    CHECK(observed.size()==count+other_outcomes.size());
    auto expected=before.result;expected.at(0)["Preset.Items1_InstanceId"]="empty";
    CHECK(diagnosed.get(owned_path(login),header(login)).result==expected);
    const hawken::store::ItemAttributes expected_attributes{{"Preset.Items1_InstanceId","empty"}};
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==expected_attributes);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(reference_family_name(ReferenceFamily::camo)=="camo");
    CHECK(reference_family_name(ReferenceFamily::booster)=="booster");
    CHECK(reference_family_name(ReferenceFamily::armor)=="armor");
    CHECK(reference_family_name(ReferenceFamily::visor)=="visor");
    CHECK(reference_family_name(ReferenceFamily::helper_optional)=="helper_optional");
    CHECK(reference_family_name(ReferenceFamily::other)=="other");
    CHECK(reference_family_name(static_cast<ReferenceFamily>(999))=="none");
    CHECK(reference_value_form_name(ReferenceValueForm::matches_owned)=="matches_owned");
    CHECK(reference_value_form_name(ReferenceValueForm::zero_uuid)=="zero_uuid");
    CHECK(reference_value_form_name(ReferenceValueForm::canonical_uuid)=="canonical_uuid");
    CHECK(reference_value_form_name(ReferenceValueForm::other)=="other");
    CHECK(reference_value_form_name(static_cast<ReferenceValueForm>(999))=="none");
}

TEST_CASE("Broker visor literal captures only five exact keys with synthetic safe values") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    const std::array<std::pair<const char*,VisorKey>,5> keys{{
        {"Preset.VisorStyle.PrimaryReticle_InstanceId",VisorKey::primary_reticle},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId",VisorKey::secondary_reticle},
        {"Preset.VisorStyle.FriendHUDColor_InstanceId",VisorKey::friend_hud_color},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId",VisorKey::foe_hud_color},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId",VisorKey::neutral_hud_color}}};
    // Synthetic inventory literals, not a captured client request or identity.
    for(const auto& [key,expected_key]:keys)for(const std::string value:
        {"None","-1","ABCDEF0123456789ABCDEF0123456789"}) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),nlohmann::json{{key,value}}.dump()),400,value);
        REQUIRE(observed.size()==count+1);const auto& actual=observed.back();
        CHECK(actual.reason==PutReason::reference_unknown_key);
        CHECK(actual.reference_family==ReferenceFamily::visor);
        CHECK(actual.value_form==ReferenceValueForm::other);
        CHECK(actual.visor_key==expected_key);
        REQUIRE(actual.visor_literal_size<=64);
        CHECK(actual.visor_literal_size==value.size());
        CHECK(std::string(actual.visor_literal.data(),actual.visor_literal_size)==value);
        for(std::size_t i=actual.visor_literal_size;i<actual.visor_literal.size();++i)
            CHECK(actual.visor_literal[i]=='\0');
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    }
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Broker visor literal enforces safe alphabet and 63 64 65 boundaries") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    const std::vector<std::pair<std::string,bool>> cases{
        {"Az09_.:#-",true},{std::string(63,'x'),true},{std::string(64,'x'),true},
        {std::string(65,'x'),false},{"line\nbreak",false},{"space value",false},
        {"slash/value",false},{"plus+value",false},{"equal=value",false},
        {std::string("nul\0tail",8),false},{std::string("\xc3\xa9"),false},
        {"11111111111111111",false}};
    const std::array<char,65> empty_buffer{};
    for(const auto& [value,captured]:cases) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),
            nlohmann::json{{"Preset.VisorStyle.PrimaryReticle_InstanceId",value}}.dump()),400);
        REQUIRE(observed.size()==count+1);const auto& actual=observed.back();
        CHECK(actual.reason==PutReason::reference_unknown_key);
        CHECK(actual.visor_key==VisorKey::primary_reticle);
        REQUIRE(actual.visor_literal_size<=64);
        if(captured) {
            CHECK(actual.visor_literal_size==value.size());
            CHECK(std::string(actual.visor_literal.data(),actual.visor_literal_size)==value);
            for(std::size_t i=actual.visor_literal_size;i<actual.visor_literal.size();++i)
                CHECK(actual.visor_literal[i]=='\0');
        } else {
            CHECK(actual.visor_literal_size==0);CHECK(actual.visor_literal==empty_buffer);
        }
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    }
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Broker visor literal suppresses unrelated references and every owned match") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    const std::array<char,65> empty_buffer{};
    for(const auto* key:{"Preset.Unknown_InstanceId","Preset.VisorStyle.Unknown_InstanceId",
        "Preset.Style.CamoPattern_InstanceId","Preset.Style.BoosterPart_InstanceId",
        "Preset.Style.ArmorPart_InstanceId","Preset.Items1_InstanceId"}) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),nlohmann::json{{key,"None"}}.dump()),400);
        REQUIRE(observed.size()==count+1);const auto& actual=observed.back();
        CHECK(actual.reason==PutReason::reference_unknown_key);
        CHECK(actual.visor_key==VisorKey::none);
        CHECK(actual.visor_literal_size==0);CHECK(actual.visor_literal==empty_buffer);
    }
    for(const auto* value:instance_ids) {
        const auto count=observed.size();
        failure(diagnosed.put(broker_path(login),header(login),
            nlohmann::json{{"Preset.VisorStyle.SecondaryReticle_InstanceId",value}}.dump()),400,value);
        REQUIRE(observed.size()==count+1);const auto& actual=observed.back();
        CHECK(actual.reason==PutReason::reference_unknown_key);
        CHECK(actual.value_form==ReferenceValueForm::matches_owned);
        CHECK(actual.visor_key==VisorKey::secondary_reticle);
        CHECK(actual.visor_literal_size==0);CHECK(actual.visor_literal==empty_buffer);
    }
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Broker visor literal describes first validation rejection and no other outcome") {
    using namespace hawken::inventory;
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    std::vector<PutObservation> observed;
    InventoryService diagnosed(f.store,f.auth,ids(),balance(),offer_id,
        [&](const PutObservation& observation){observed.push_back(observation);});
    // Raw order is Primary first; existing DOM validation visits Foe first.
    failure(diagnosed.put(broker_path(login),header(login),
        R"({"Preset.VisorStyle.PrimaryReticle_InstanceId":"None","Preset.VisorStyle.FoeHUDColor_InstanceId":"-1"})"),400);
    REQUIRE(observed.size()==1);const auto& first=observed.front();
    CHECK(first.reason==PutReason::reference_unknown_key);
    CHECK(first.visor_key==VisorKey::foe_hud_color);
    REQUIRE(first.visor_literal_size<=64);
    CHECK(first.visor_literal_size==2);
    CHECK(std::string(first.visor_literal.data(),first.visor_literal_size)=="-1");
    for(std::size_t i=first.visor_literal_size;i<first.visor_literal.size();++i)
        CHECK(first.visor_literal[i]=='\0');
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    CHECK(diagnosed.get(owned_path(login),header(login)).result==before.result);
    const std::vector<std::pair<std::string,PutReason>> cases{
        {R"({"Preset.VisorStyle.PrimaryReticle_InstanceId":"empty"})",PutReason::reference_sentinel},
        {R"({"Preset.VisorStyle.PrimaryReticle_InstanceId":1})",PutReason::value_type},
        {nlohmann::json{{"Preset.Ability_InstanceId",instance_ids[1]}}.dump(),PutReason::reference_instance},
        {R"({"Preset.VisorStyle.PrimaryReticle_InstanceId":""})",PutReason::success}};
    const std::array<char,65> empty_buffer{};
    for(const auto& [body,reason]:cases) {
        const auto count=observed.size();const auto reply=diagnosed.put(broker_path(login),header(login),body);
        if(reason==PutReason::success)saved_reply(reply);else failure(reply,400);
        REQUIRE(observed.size()==count+1);const auto& actual=observed.back();
        CHECK(actual.reason==reason);CHECK(actual.visor_key==VisorKey::none);
        CHECK(actual.visor_literal_size==0);CHECK(actual.visor_literal==empty_buffer);
    }
    const hawken::store::ItemAttributes expected{{"Preset.VisorStyle.PrimaryReticle_InstanceId",""}};
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==expected);
    auto expected_read=before.result;expected_read.at(0)["Preset.VisorStyle.PrimaryReticle_InstanceId"]="";
    CHECK(diagnosed.get(owned_path(login),header(login)).result==expected_read);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}

TEST_CASE("Broker observed visor defaults persist only on an authenticated owned mech") {
    Fixture f;const auto login=f.login();REQUIRE(login.status==200);
    const auto before=f.inventory.get(owned_path(login),header(login));REQUIRE(before.status==200);
    const auto account=f.store.find_account(fixture_id_text);REQUIRE(account);
    // The five exact field/value pairs come from native d9da50 defaults.
    // This test does not authorize arbitrary fake references or grant cosmetic items.
    const std::string body=R"({"Preset.VisorStyle.FriendHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID","Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2","Preset.VisorStyle.NeutralHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID3","Preset.VisorStyle.PrimaryReticle_InstanceId":"FAKE-PRIM-RETICLE-INSTANCE-GUID","Preset.VisorStyle.SecondaryReticle_InstanceId":"FAKE-SEC-RETICLE-INSTANCE-GUID"})";
    const auto path=broker_path(login);
    failure(f.inventory.put(path,"",body),401);
    failure(f.inventory.put("/userGameItemsBroker/30000000-0000-0000-0000-000000000098/"
        +std::string(instance_ids[0]),header(login),body),401);
    failure(f.inventory.put(broker_path(login,"30000000-0000-0000-0000-000000000099"),header(login),body),404);
    failure(f.inventory.put(broker_path(login,instance_ids[1]),header(login),body),404);
    for(const auto& rejected:std::vector<std::string>{
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2-X"})",
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID"})",
        R"({"Preset.VisorStyle.FriendHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2"})",
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId_0":"FAKE-COLOR-INSTANCE-GUID2"})",
        R"({"Preset.PrimaryWeapon_InstanceId_0":"FAKE-COLOR-INSTANCE-GUID2"})"})
        failure(f.inventory.put(path,header(login),rejected),400);
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    CHECK(f.inventory.get(owned_path(login),header(login)).result==before.result);
    saved_reply(f.inventory.put(path,header(login),body));
    const hawken::store::ItemAttributes expected_attributes{
        {"Preset.VisorStyle.FriendHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID"},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID2"},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID3"},
        {"Preset.VisorStyle.PrimaryReticle_InstanceId","FAKE-PRIM-RETICLE-INSTANCE-GUID"},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId","FAKE-SEC-RETICLE-INSTANCE-GUID"}};
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==expected_attributes);
    auto expected=before.result;
    for(const auto& [key,value]:expected_attributes)expected.at(0)[key]=value;
    CHECK(f.inventory.get(owned_path(login),header(login)).result==expected);
    saved_reply(f.inventory.put(path,header(login),body));
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==expected_attributes);
    CHECK(f.inventory.get(owned_path(login),header(login)).result==expected);
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    for(std::size_t slot=1;slot<instance_ids.size();++slot)
        CHECK(f.store.owned_item_attributes(account->ref,instance_ids[slot]).empty());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(f.store.game_settings(account->ref).empty());
}


TEST_CASE("Item batch service is an exact authenticated read and empty requests never grant") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    const auto path="/userGameItemsBatch/"+guid(login);
    CHECK(f.inventory.supports_batch_target(path)); CHECK_FALSE(f.inventory.supports_target(path));
    for(const auto& alias:std::vector<std::string>{path+"/",path+"?x=1",path+"#x",path+"/extra",
        "/userGameItemsBatch/A54C90D7-d30b-8e6b-2417-3d744d78bd17",
        "/userGameItemsBatch/00000000-0000-0000-0000-000000000000",
        "/userGameItemsBatch/%61"+guid(login).substr(1)}) {
        CHECK_FALSE(f.inventory.supports_batch_target(alias)); failure(f.inventory.post_batch(alias,"","[]"),404);
    }
    for(const auto* body:{"null\n","[]\n"}) {
        const auto reply=f.inventory.post_batch(path,header(login),body);
        CHECK(reply.status==200); CHECK(reply.result==nlohmann::json::array());
    }
    failure(f.inventory.post_batch(path,header(login),nlohmann::json::array({instance_ids[0]}).dump()),404);
    CHECK_FALSE(f.store.starter_items(account->ref));
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(f.store.visits(account->ref).empty());
}
TEST_CASE("Item batch service normalizes requested IDs retains order and overlays saved Presets") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    saved_reply(f.inventory.put(broker_path(login),header(login),R"({"Preset.BatchFixture":"saved-not-seeded"})"));
    const auto path="/userGameItemsBatch/"+guid(login);
    const auto reply=f.inventory.post_batch(path,header(login),
        R"(["EA800EC9-E850-9883-69A8-B2D3D2B86A14","acf28679-6b34-ed79-151e-26ec094cc9d0","445474ca-deb5-b989-8fa0-54b5d9630fa2"])");
    REQUIRE(reply.status==200); REQUIRE(reply.result.size()==3);
    // Literal row oracle independent of the filtering implementation.
    CHECK(reply.result.at(0)==nlohmann::json{{"GameItemsGuid",type_ids[8]},{"UserGameItemGuid",instance_ids[8]},
        {"ItemName","Ability - Coolant"},{"Version","starter.test1"},{"ReadOnly_Parent",instance_ids[0]},
        {"ReadOnly_GlobalItem","0"},{"ReadOnly_Children",nlohmann::json::array()}});
    CHECK(reply.result.at(1).at("UserGameItemGuid")==instance_ids[1]);
    CHECK(reply.result.at(2).at("UserGameItemGuid")==instance_ids[0]);
    CHECK(reply.result.at(2).at("Preset.BatchFixture")=="saved-not-seeded");
    CHECK(reply.result.at(2).at("ReadOnly_Children").size()==8); // Returning mech does not enumerate children as rows.
    CHECK(f.inventory.post_batch(path,header(login),"null").result==nlohmann::json::array());
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0])==hawken::store::ItemAttributes{{"Preset.BatchFixture","saved-not-seeded"}});
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Item batch service rejects normalized duplicates invalid JSON unknown IDs and byte excess") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    const auto path="/userGameItemsBatch/"+guid(login); const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    for(const auto& body:std::vector<std::string>{"", "{", "{}", "true", "[1]", "[null]", "[{}]", "[[]]", "[\"\"]",
        R"(["00000000-0000-0000-0000-000000000000"])",R"(["445474ca-deb5-b989-8fa0-54b5d9630fa2/"])",
        R"(["445474ca-deb5-b989-8fa0-54b5d9630fa2","445474CA-DEB5-B989-8FA0-54B5D9630FA2"])",
        R"(["445474ca-deb5-b989-8fa0-54b5d9630fa2","445474c\u0061-deb5-b989-8fa0-54b5d9630fa2"])",
        R"(["445474ca-deb5-b989-8fa0-54b5d9630fa\u0000"])",R"(["445474ca-deb5-b989-8fa0-54b5d9630fa\u00e9"])",
        std::string("[]")+std::string(8191,' ')})
        failure(f.inventory.post_batch(path,header(login),body),400);
    failure(f.inventory.post_batch(path,header(login),R"(["445474ca-deb5-b989-8fa0-54b5d9630fa2","30000000-0000-0000-0000-000000000099"])") ,404);
    const auto boundary=f.inventory.post_batch(path,header(login),std::string("[]")+std::string(8190,' '));
    CHECK(boundary.status==200); CHECK(boundary.result==nlohmann::json::array());
    CHECK(f.store.starter_items(account->ref)==persisted_expected());
    CHECK(f.store.owned_item_attributes(account->ref,instance_ids[0]).empty());
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Item batch service revalidates auth lifecycle even for empty reads and isolates accounts") {
    for(const auto mode:{0,1,2,3,4,5}) {
        CAPTURE(mode); Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
        REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
        const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
        auto authorization=header(login); int expected=401;
        if(mode==0)authorization="";
        if(mode==1)f.verifier.active=0;
        if(mode==2)f.now=1900;
        if(mode==3){f.store.set_ban(account->ref,{1000,std::nullopt,"private-ban-fixture"});expected=403;}
        if(mode==4)f.now=999;
        if(mode==5){f.clock_throws=true;expected=500;}
        failure(f.inventory.post_batch("/userGameItemsBatch/"+guid(login),authorization,"null"),expected);
        CHECK(f.store.starter_items(account->ref)==persisted_expected());
        CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{});
    }
    Fixture f; const auto first=f.login(); REQUIRE(first.status==200);
    REQUIRE(f.inventory.get(owned_path(first),header(first)).status==200);
    const auto second=f.login(true); REQUIRE(second.status==200);
    const auto own=f.inventory.get(owned_path(second),header(second)); REQUIRE(own.status==200);
    const auto account=f.store.find_account(fixture_second_id_text); REQUIRE(account);
    const auto saved=f.store.starter_items(account->ref);
    failure(f.inventory.post_batch("/userGameItemsBatch/"+guid(first),header(second),"[]"),401);
    failure(f.inventory.post_batch("/userGameItemsBatch/"+guid(second),header(second),nlohmann::json::array({instance_ids[0]}).dump()),404);
    CHECK(f.store.starter_items(account->ref)==saved);
    CHECK(f.store.starter_items(f.store.find_account(fixture_id_text)->ref)==persisted_expected());
}
TEST_CASE("Item batch service fails closed on a persisted type inconsistent with its catalogue") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    REQUIRE(f.inventory.get(owned_path(login),header(login)).status==200);
    fixture_sql(f.temporary.path()/"accounts.sqlite",
        "UPDATE starter_items SET type_id='30000000-0000-0000-0000-000000000099' WHERE slot=8");
    failure(f.inventory.post_batch("/userGameItemsBatch/"+guid(login),header(login),nlohmann::json::array({instance_ids[0]}).dump()),500);
}

TEST_CASE("Owned starter mech service returns raw SQL snapshot without rendering defaults or notifying PUT") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
    const auto account=access.account->account;
    REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
    const hawken::store::ItemAttributes attributes{{"Preset.Label","saved"},{"Preset.PrimaryWeapon_0","0007"},{"Preset.Empty",""}};
    REQUIRE(f.store.merge_owned_item_attributes(account,instance_ids[0],attributes));
    unsigned observed=0;
    InventoryService service(f.store,f.auth,ids(),balance(),offer_id,[&](const auto&){++observed;});
    const auto path=f.temporary.path()/"accounts.sqlite"; const auto before=hawken::test::read_bytes(path);
    auto snapshot=service.owned_starter_mech(account,instance_ids[0]);
    CHECK(snapshot.items==persisted_expected()); CHECK(snapshot.attributes==attributes);
    CHECK_FALSE(snapshot.attributes.contains("Preset.Style.UpperPart"));
    snapshot.attributes["Preset.Label"]="changed";
    snapshot.items[0].type_id="changed";
    CHECK(service.owned_starter_mech(account,instance_ids[0]).attributes==attributes);
    CHECK(service.owned_starter_mech(account,instance_ids[0]).items==persisted_expected());
    CHECK(observed==0); CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
    CHECK(hawken::test::read_bytes(path)==before);
    LocalStore reopened(path,"inventory-server");
    InventoryService reopened_service(reopened,f.auth,ids(),balance(),offer_id);
    const auto persisted=reopened_service.owned_starter_mech(account,instance_ids[0]);
    CHECK(persisted.items==persisted_expected()); CHECK(persisted.attributes==attributes);
    CHECK(hawken::test::read_bytes(path)==before);
}

TEST_CASE("Owned starter mech service never grants or selects another account or equipment slot") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
    const auto account=access.account->account; auto expected=hawken::store::StoreErrorCode::not_found;
    auto selected=std::string(instance_ids[0]); auto scoped=account;
    SUBCASE("Absent starter grant") {}
    SUBCASE("Owned equipment is not the mech") {
        REQUIRE(f.store.grant_starter_items(account,persisted_expected())); selected=instance_ids[1];
    }
    SUBCASE("Unknown canonical instance") {
        REQUIRE(f.store.grant_starter_items(account,persisted_expected())); selected="30000000-0000-0000-0000-000000000099";
    }
    SUBCASE("Foreign account cannot select the existing mech") {
        REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
        scoped=f.store.get_or_create_account(fixture_second_id_text,1000).ref;
    }
    SUBCASE("Foreign server scope") {scoped.server_id="other-server";expected=hawken::store::StoreErrorCode::invalid_scope;}
    SUBCASE("Malformed instance") {selected="not-an-instance";expected=hawken::store::StoreErrorCode::invalid_input;}
    const auto grant_before=f.store.starter_items(account);
    const auto path=f.temporary.path()/"accounts.sqlite"; const auto before=hawken::test::read_bytes(path);
    bool caught=false;
    try {(void)f.inventory.owned_starter_mech(scoped,selected);}
    catch(const hawken::store::StoreError& error){caught=true;CHECK(error.code()==expected);}
    CHECK(caught); CHECK(hawken::test::read_bytes(path)==before);
    CHECK(f.store.starter_items(account)==grant_before);
    CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
}

TEST_CASE("Owned starter mech service validates every persisted type against its configured catalogue") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
    const auto account=access.account->account; REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
    fixture_sql(f.temporary.path()/"accounts.sqlite",
        "UPDATE starter_items SET type_id='30000000-0000-0000-0000-000000000099' WHERE slot=8");
    const auto before=hawken::test::read_bytes(f.temporary.path()/"accounts.sqlite");
    CHECK_THROWS_WITH(f.inventory.owned_starter_mech(account,instance_ids[0]),"Invalid owned inventory.");
    CHECK(hawken::test::read_bytes(f.temporary.path()/"accounts.sqlite")==before);
    CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
}

namespace {
hawken::store::ItemAttributes http_preset_attributes(const nlohmann::json& mech) {
    hawken::store::ItemAttributes attributes;
    for(const auto& [key,value]:mech.items())
        if(key.starts_with("Preset."))attributes.emplace(key,value.get<std::string>());
    return attributes;
}
}

TEST_CASE("Effective owned starter mech initial preset matches HTTP baseline while RAW remains empty") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
    const auto account=access.account->account; REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
    const auto path=f.temporary.path()/"accounts.sqlite"; const auto before=hawken::test::read_bytes(path);
    auto snapshot=f.inventory.effective_owned_starter_mech(account,instance_ids[0]);
    CHECK(snapshot.items==persisted_expected());
    const hawken::store::ItemAttributes literal_values{
        {"Preset.PrimaryWeapon_0","0"},{"Preset.SecondaryWeapon","1"},
        {"Preset.Style.UpperPart","10"},{"Preset.Style.MidPart","10"},
        {"Preset.Style.ArmPart","10"},{"Preset.Style.LowerPart","10"},{"Preset.Ability","3"},
        {"Preset.MechInternals.Internal1","-1"},{"Preset.MechInternals.Internal2","-1"},
        {"Preset.MechInternals.Internal3","-1"},{"Preset.MechInternals.Internal4","-1"},
        {"Preset.MechInternals.Internal5","-1"},{"Preset.MechInternals.Internal6","-1"}};
    for(const auto& [key,value]:literal_values) {
        CHECK(snapshot.attributes.contains(key));
        if(snapshot.attributes.contains(key))CHECK(snapshot.attributes.at(key)==value);
    }
    CHECK(snapshot.attributes.contains("Preset.PrimaryWeapon_InstanceId_0"));
    if(snapshot.attributes.contains("Preset.PrimaryWeapon_InstanceId_0"))
        CHECK(snapshot.attributes.at("Preset.PrimaryWeapon_InstanceId_0")==instance_ids[1]);
    const auto http=f.inventory.get(owned_path(login),header(login)); REQUIRE(http.status==200);
    CHECK(snapshot.attributes==http_preset_attributes(http.result.at(0)));
    CHECK_FALSE(snapshot.attributes.contains("ReadOnly_Children"));
    CHECK_FALSE(snapshot.attributes.contains("ItemName"));
    CHECK(f.inventory.owned_starter_mech(account,instance_ids[0]).attributes.empty());
    CHECK(f.store.owned_item_attributes(account,instance_ids[0]).empty());
    snapshot.attributes["Preset.Ability"]="changed";
    CHECK(f.inventory.owned_starter_mech(account,instance_ids[0]).attributes.empty());
    CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
    CHECK(hawken::test::read_bytes(path)==before);
}

TEST_CASE("Effective owned starter mech preserves saved empty values and absent baseline values like HTTP") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
    const auto account=access.account->account; REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
    const hawken::store::ItemAttributes saved{{"Preset.PrimaryWeapon_0","9"},{"Preset.Style.UpperPart",""},
        {"Preset.Label","custom"},{"Preset.Internals1_InstanceId","empty"}};
    REQUIRE(f.store.merge_owned_item_attributes(account,instance_ids[0],saved));
    unsigned observed=0;
    InventoryService service(f.store,f.auth,ids(),balance(),offer_id,[&](const auto&){++observed;});
    const auto path=f.temporary.path()/"accounts.sqlite"; const auto before=hawken::test::read_bytes(path);
    const auto snapshot=service.effective_owned_starter_mech(account,instance_ids[0]);
    CHECK(snapshot.items==persisted_expected());
    CHECK(snapshot.attributes.at("Preset.PrimaryWeapon_0")=="9");
    CHECK(snapshot.attributes.at("Preset.Style.UpperPart").empty());
    CHECK(snapshot.attributes.at("Preset.Internals1_InstanceId")=="empty");
    CHECK(snapshot.attributes.at("Preset.Label")=="custom");
    CHECK(snapshot.attributes.contains("Preset.SecondaryWeapon"));
    if(snapshot.attributes.contains("Preset.SecondaryWeapon"))CHECK(snapshot.attributes.at("Preset.SecondaryWeapon")=="1");
    const auto http=service.get(owned_path(login),header(login)); REQUIRE(http.status==200);
    CHECK(snapshot.attributes==http_preset_attributes(http.result.at(0)));
    CHECK(service.owned_starter_mech(account,instance_ids[0]).attributes==saved);
    CHECK(f.store.owned_item_attributes(account,instance_ids[0])==saved);
    CHECK(observed==0); CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
    CHECK(hawken::test::read_bytes(path)==before);
}

TEST_CASE("Effective owned starter mech rejects missing foreign equipment and catalogue mismatches without writes") {
    SUBCASE("Ownership and canonical selection") {
        Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
        const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
        const auto account=access.account->account; auto scoped=account; auto selected=std::string(instance_ids[0]);
        auto expected=hawken::store::StoreErrorCode::not_found;
        SUBCASE("Missing grant") {}
        SUBCASE("Another owned slot") {REQUIRE(f.store.grant_starter_items(account,persisted_expected()));selected=instance_ids[1];}
        SUBCASE("Foreign account") {
            REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
            scoped=f.store.get_or_create_account(fixture_second_id_text,1000).ref;
        }
        SUBCASE("Malformed instance") {selected="not-a-mech";expected=hawken::store::StoreErrorCode::invalid_input;}
        const auto grant_before=f.store.starter_items(account);
        const auto path=f.temporary.path()/"accounts.sqlite"; const auto before=hawken::test::read_bytes(path);
        bool caught=false;
        try{(void)f.inventory.effective_owned_starter_mech(scoped,selected);}
        catch(const hawken::store::StoreError& error){caught=true;CHECK(error.code()==expected);}
        CHECK(caught); CHECK(f.store.starter_items(account)==grant_before);
        CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
        CHECK(hawken::test::read_bytes(path)==before);
    }
    SUBCASE("Every configured type is required") {
        for(std::size_t slot=0;slot<9;++slot) {
            CAPTURE(slot); Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
            const auto access=f.auth.access(header(login)); REQUIRE(access.status==200); REQUIRE(access.account);
            const auto account=access.account->account; REQUIRE(f.store.grant_starter_items(account,persisted_expected()));
            const auto path=f.temporary.path()/"accounts.sqlite";
            const auto sql="UPDATE starter_items SET type_id='30000000-0000-0000-0000-000000000099' WHERE slot="+std::to_string(slot);
            fixture_sql(path,sql.c_str()); const auto before=hawken::test::read_bytes(path);
            CHECK_THROWS_WITH(f.inventory.effective_owned_starter_mech(account,instance_ids[0]),"Invalid owned inventory.");
            CHECK(f.store.wallet(account)==hawken::store::WalletBalances{});
            CHECK(hawken::test::read_bytes(path)==before);
        }
    }
}
