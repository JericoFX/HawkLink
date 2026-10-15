#include <doctest/doctest.h>
#include <hawken/local_store.hpp>
#include <sqlite3.h>
#include "test_support.hpp"
#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <thread>

namespace {
using namespace hawken::store;

class FixtureDb {
public:
    explicit FixtureDb(const std::filesystem::path& path) {
        const auto utf8 = path.u8string();
        const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        const auto rc = sqlite3_open_v2(name.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error("Cannot open starter fixture");
        }
    }
    ~FixtureDb() { sqlite3_close_v2(db_); }
    FixtureDb(const FixtureDb&) = delete;
    FixtureDb& operator=(const FixtureDb&) = delete;
    void exec(const char* sql) const {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot prepare starter fixture");
    }
    std::int64_t scalar(const char* sql) const {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot inspect starter fixture");
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER)
            throw std::runtime_error("Missing starter fixture result");
        return sqlite3_column_int64(stmt.get(), 0);
    }
private:
    sqlite3* db_ = nullptr;
};

template<class Operation>
void expect_error(StoreErrorCode code, Operation operation) {
    bool caught = false;
    try { operation(); }
    catch (const StoreError& error) {
        caught = true;
        CHECK(error.code() == code);
        CHECK(std::string(error.what()) == "Local store operation failed.");
    }
    CHECK(caught);
}

StarterItems fixture_items(char instance_prefix = '2') {
    StarterItems items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].type_id = "10000000-0000-0000-0000-00000000000" + std::to_string(i + 1);
        items[i].instance_id = "20000000-0000-0000-0000-00000000000" + std::to_string(i + 1);
        items[i].instance_id[0] = instance_prefix;
    }
    return items;
}
}

TEST_CASE("Schema six creates empty starter tables without silently granting items") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    REQUIRE(store.schema_version() == 6);
    const auto account = store.get_or_create_account("1", 0);
    CHECK_FALSE(store.starter_items(account.ref).has_value());
    CHECK(store.wallet(account.ref) == WalletBalances{0, 0});
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 0);
    CHECK(db.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

TEST_CASE("Starter grant persists all nine slots and identical replay is idempotent") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    const auto items = fixture_items();
    Account account;
    {
        LocalStore store(path, "server-a");
        account = store.get_or_create_account("1", 0);
        CHECK(store.grant_starter_items(account.ref, items));
        CHECK(store.starter_items(account.ref) == std::optional<StarterItems>(items));
        CHECK_FALSE(store.grant_starter_items(account.ref, items));
        CHECK(store.wallet(account.ref) == WalletBalances{0, 0});
        const FixtureDb db(path);
        CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 1);
        CHECK(db.scalar("SELECT count(*) FROM starter_items") == 9);
    }
    LocalStore reopened(path, "server-a");
    CHECK(reopened.starter_items(account.ref) == std::optional<StarterItems>(items));
    CHECK_FALSE(reopened.grant_starter_items(account.ref, items));
}

TEST_CASE("A changed starter grant conflicts without overwriting the original entitlement") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const auto items = fixture_items();
    REQUIRE(store.grant_starter_items(account.ref, items));
    auto changed = fixture_items('3');
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(account.ref, changed); });
    changed = items;
    changed[2].type_id = "40000000-0000-0000-0000-000000000003";
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(account.ref, changed); });
    CHECK(store.starter_items(account.ref) == std::optional<StarterItems>(items));
}

TEST_CASE("Starter ownership and account scope remain isolated with shared catalogue type IDs") {
    hawken::test::TempDirectory temp;
    LocalStore first(temp.path() / "a.db", "server-a");
    LocalStore second(temp.path() / "b.db", "server-b");
    const auto a = first.get_or_create_account("1", 0);
    const auto other = first.get_or_create_account("2", 0);
    const auto b = second.get_or_create_account("1", 0);
    CHECK(first.grant_starter_items(a.ref, fixture_items()));
    CHECK(first.grant_starter_items(other.ref, fixture_items('3')));
    CHECK(second.grant_starter_items(b.ref, fixture_items()));
    CHECK(first.starter_items(a.ref) == std::optional<StarterItems>(fixture_items()));
    CHECK(first.starter_items(other.ref) == std::optional<StarterItems>(fixture_items('3')));
    CHECK(second.starter_items(b.ref) == std::optional<StarterItems>(fixture_items()));
    expect_error(StoreErrorCode::invalid_scope, [&] { first.starter_items(b.ref); });
    expect_error(StoreErrorCode::invalid_scope, [&] { second.grant_starter_items(a.ref, fixture_items('4')); });
    expect_error(StoreErrorCode::not_found, [&] { first.starter_items({"server-a", 999}); });
    expect_error(StoreErrorCode::not_found, [&] { first.grant_starter_items({"server-a", 999}, fixture_items('4')); });
    expect_error(StoreErrorCode::invalid_input, [&] { first.starter_items({"server-a", 0}); });
    expect_error(StoreErrorCode::invalid_input, [&] { first.grant_starter_items({"server-a", -1}, fixture_items('4')); });
}

TEST_CASE("Every submitted starter ID must be canonical nonzero lower-case hex") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const std::array<std::string, 8> bad_ids{
        "", "00000000-0000-0000-0000-000000000000", "100000000000-0000-0000-000000000001",
        "A0000000-0000-0000-0000-000000000001", "10000000-0000-0000-0000-00000000000g",
        "10000000-0000-0000-0000-000000000001 ",
        std::string("10000000-0000-0000-0000-00000000000\0", 36),
        "10000000-0000-0000-0000-00000000000\n"};
    for (std::size_t slot = 0; slot < 9; ++slot) {
        for (const auto& bad : bad_ids) {
            auto items = fixture_items();
            items[slot].type_id = bad;
            expect_error(StoreErrorCode::invalid_input, [&] { store.grant_starter_items(account.ref, items); });
            items = fixture_items();
            items[slot].instance_id = bad;
            expect_error(StoreErrorCode::invalid_input, [&] { store.grant_starter_items(account.ref, items); });
        }
    }
    CHECK_FALSE(store.starter_items(account.ref).has_value());
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 0);
}

TEST_CASE("Duplicate and cross-domain starter IDs are rejected before writing any row") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    for (const auto kind : {0, 1, 2}) {
        auto items = fixture_items();
        if (kind == 0) items[8].type_id = items[0].type_id;
        if (kind == 1) items[8].instance_id = items[0].instance_id;
        if (kind == 2) items[8].instance_id = items[0].type_id;
        expect_error(StoreErrorCode::invalid_input, [&] { store.grant_starter_items(account.ref, items); });
    }
    CHECK_FALSE(store.starter_items(account.ref).has_value());
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 0);
}

TEST_CASE("Instance IDs and their domains cannot collide with grants of another account") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto a = store.get_or_create_account("1", 0);
    const auto b = store.get_or_create_account("2", 0);
    REQUIRE(store.grant_starter_items(a.ref, fixture_items()));
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(b.ref, fixture_items()); });
    auto collision = fixture_items('3');
    collision[8].instance_id = fixture_items()[0].type_id;
    collision[0].type_id = "40000000-0000-0000-0000-000000000001";
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(b.ref, collision); });
    collision = fixture_items('3');
    collision[8].type_id = fixture_items()[0].instance_id;
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(b.ref, collision); });
    CHECK_FALSE(store.starter_items(b.ref).has_value());
    CHECK(store.starter_items(a.ref) == std::optional<StarterItems>(fixture_items()));
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 1);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 9);
}

TEST_CASE("Missing rows in a recorded grant never silently recreate or downgrade ownership") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto a = store.get_or_create_account("1", 0);
    REQUIRE(store.grant_starter_items(a.ref, fixture_items()));
    SUBCASE("one slot missing") { const FixtureDb db(path); db.exec("DELETE FROM starter_items WHERE slot=5"); }
    SUBCASE("all slots missing") { const FixtureDb db(path); db.exec("DELETE FROM starter_items"); }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { store.starter_items(a.ref); });
    expect_error(StoreErrorCode::invalid_schema, [&] { store.grant_starter_items(a.ref, fixture_items('3')); });
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Starter reads validate values even if a trusted DB owner bypassed checks") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto a = store.get_or_create_account("1", 0);
    REQUIRE(store.grant_starter_items(a.ref, fixture_items()));
    const FixtureDb db(path);
    db.exec("PRAGMA ignore_check_constraints=ON");
    SUBCASE("malformed ID") { db.exec("UPDATE starter_items SET type_id='bad' WHERE slot=3"); }
    SUBCASE("cross-domain ID") { db.exec("UPDATE starter_items SET type_id='20000000-0000-0000-0000-000000000001' WHERE slot=3"); }
    SUBCASE("duplicate type") { db.exec("UPDATE starter_items SET type_id='10000000-0000-0000-0000-000000000001' WHERE slot=3"); }
    SUBCASE("invalid slot") { db.exec("UPDATE starter_items SET slot=9 WHERE slot=3"); }
    expect_error(StoreErrorCode::invalid_schema, [&] { store.starter_items(a.ref); });
}

TEST_CASE("An orphan starter row is not treated as a fresh ungranted account") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto a = store.get_or_create_account("1", 0);
    REQUIRE(store.grant_starter_items(a.ref, fixture_items()));
    { const FixtureDb db(path); db.exec("PRAGMA foreign_keys=OFF; DELETE FROM starter_grants"); }
    expect_error(StoreErrorCode::invalid_schema, [&] { store.starter_items(a.ref); });
    expect_error(StoreErrorCode::invalid_schema, [&] { store.grant_starter_items(a.ref, fixture_items('3')); });
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 9);
}

TEST_CASE("Starter grant rolls back its marker when a persisted value conflicts") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto a = store.get_or_create_account("1", 0);
    const auto b = store.get_or_create_account("2", 0);
    REQUIRE(store.grant_starter_items(a.ref, fixture_items()));
    auto conflicting = fixture_items('3');
    conflicting[8].instance_id = fixture_items()[0].instance_id;
    expect_error(StoreErrorCode::conflict, [&] { store.grant_starter_items(b.ref, conflicting); });
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants WHERE account_id=2") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items WHERE account_id=2") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 9);
}

TEST_CASE("Concurrent identical trusted grants publish one complete bundle") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto a = store.get_or_create_account("1", 0);
    std::array<bool, 2> created{};
    std::array<std::exception_ptr, 2> failures;
    std::array<std::jthread, 2> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::jthread([&, i] {
            try { created[i] = store.grant_starter_items(a.ref, fixture_items()); }
            catch (...) { failures[i] = std::current_exception(); }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(failures[0] == nullptr);
    REQUIRE(failures[1] == nullptr);
    CHECK(created[0] != created[1]);
    CHECK(store.starter_items(a.ref) == std::optional<StarterItems>(fixture_items()));
}

TEST_CASE("Frozen schema two migrates to five retaining accounts and nonzero earned balances") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "v2.db";
    const auto fixture = std::filesystem::path(HAWKEN_PROJECT_ROOT) / "tests/fixtures/local-store-v2.sqlite";
    const auto original = hawken::test::read_bytes(fixture);
    REQUIRE(std::filesystem::copy_file(fixture, path));
    LocalStore store(path, "server-a");
    REQUIRE(store.schema_version() == 6);
    REQUIRE(store.find_account("1").has_value());
    CHECK(store.find_account("1")->ref.local_id == 17);
    CHECK(store.wallet({"server-a", 17}) == WalletBalances{200, 0});
    CHECK(store.wallet({"server-a", 23}) == WalletBalances{INT32_MAX, 50});
    CHECK(store.role({"server-a", 17}) == std::optional<Role>(Role::analyst));
    CHECK(store.visits({"server-a", 17}).size() == 1);
    CHECK_FALSE(store.starter_items({"server-a", 17}).has_value());
    CHECK_FALSE(store.starter_items({"server-a", 23}).has_value());
    const FixtureDb db(path);
    CHECK(db.scalar("PRAGMA user_version") == 6);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants") == 0);
    CHECK(db.scalar("SELECT count(*) FROM starter_items") == 0);
    CHECK(db.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0);
    CHECK(hawken::test::read_bytes(fixture) == original);
}

TEST_CASE("Invalid schema two fails before creating starter tables without altering DB bytes") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "v2.db";
    REQUIRE(std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT) / "tests/fixtures/local-store-v2.sqlite", path));
    SUBCASE("foreign scope") {
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_scope, [&] { LocalStore rejected(path, "server-b"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
    SUBCASE("unexpected table") {
        { const FixtureDb db(path); db.exec("CREATE TABLE starter_grants(account_id INTEGER)"); }
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
    SUBCASE("foreign key damage") {
        { const FixtureDb db(path); db.exec("PRAGMA foreign_keys=OFF; DELETE FROM accounts WHERE id=17"); }
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
}

TEST_CASE("A schema two write lock prevents any partial starter migration") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "v2.db";
    REQUIRE(std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT) / "tests/fixtures/local-store-v2.sqlite", path));
    {
        const FixtureDb lock(path);
        lock.exec("BEGIN EXCLUSIVE");
        expect_error(StoreErrorCode::busy, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(lock.scalar("PRAGMA user_version") == 2);
        CHECK(lock.scalar("SELECT count(*) FROM sqlite_schema WHERE name IN ('starter_grants','starter_items')") == 0);
        lock.exec("ROLLBACK");
    }
    LocalStore retry(path, "server-a");
    CHECK(retry.schema_version() == 6);
    CHECK(retry.wallet({"server-a", 23}) == WalletBalances{INT32_MAX, 50});
}

TEST_CASE("Owned attributes persist and merge without replacing entitlement or wallet") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"attributes.db";
    const auto items=fixture_items(); Account account;
    const ItemAttributes first{{"Preset.GarageOrder","2"},{"Preset.Label","Original \xc3\xa9"}};
    const ItemAttributes expected{{"Preset.GarageOrder","3"},{"Preset.Label","Original \xc3\xa9"},
        {"Preset.Empty",""},{"Preset.Opaque","line\n\tend"}};
    {
        LocalStore store(path,"server-a"); account=store.get_or_create_account("1",0);
        REQUIRE(store.grant_starter_items(account.ref,items));
        { const FixtureDb db(path); db.exec("UPDATE wallets SET hawken_points=73,meteor_points=11 WHERE account_id=1"); }
        CHECK(store.owned_item_attributes(account.ref,items[0].instance_id).empty());
        CHECK_FALSE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,{}));
        CHECK(store.merge_owned_item_attributes(account.ref,items[0].instance_id,first));
        CHECK_FALSE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,first));
        CHECK(store.merge_owned_item_attributes(account.ref,items[0].instance_id,
            {{"Preset.GarageOrder","3"},{"Preset.Empty",""},{"Preset.Opaque","line\n\tend"}}));
        CHECK(store.owned_item_attributes(account.ref,items[0].instance_id)==expected);
        CHECK_FALSE(store.grant_starter_items(account.ref,items));
        CHECK(store.starter_items(account.ref)==std::optional<StarterItems>(items));
        CHECK(store.wallet(account.ref)==WalletBalances{73,11});
        CHECK(store.game_settings(account.ref).empty());
        for (std::size_t i=1;i<items.size();++i)
            CHECK(store.owned_item_attributes(account.ref,items[i].instance_id).empty());
        const FixtureDb db(path);
        CHECK(db.scalar("SELECT count(*) FROM starter_grants")==1);
        CHECK(db.scalar("SELECT count(*) FROM starter_items")==9);
        CHECK(db.scalar("SELECT count(*) FROM owned_item_attributes")==4);
    }
    LocalStore reopened(path,"server-a");
    CHECK(reopened.owned_item_attributes(account.ref,items[0].instance_id)==expected);
    CHECK_FALSE(reopened.merge_owned_item_attributes(account.ref,items[0].instance_id,expected));
    CHECK(reopened.starter_items(account.ref)==std::optional<StarterItems>(items));
    CHECK(reopened.wallet(account.ref)==WalletBalances{73,11});
}

TEST_CASE("Attributes belong to one instance rather than a shared catalogue type") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"attributes.db","server-a");
    const auto a=store.get_or_create_account("1",0),b=store.get_or_create_account("2",0);
    const auto a_items=fixture_items(),b_items=fixture_items('3');
    REQUIRE(store.grant_starter_items(a.ref,a_items)); REQUIRE(store.grant_starter_items(b.ref,b_items));
    CHECK(store.merge_owned_item_attributes(a.ref,a_items[0].instance_id,{{"Preset.GarageOrder","2"}}));
    CHECK(store.merge_owned_item_attributes(a.ref,a_items[1].instance_id,{{"Preset.Label","weapon"}}));
    CHECK(store.merge_owned_item_attributes(b.ref,b_items[0].instance_id,{{"Preset.GarageOrder","7"}}));
    CHECK(store.owned_item_attributes(a.ref,a_items[0].instance_id)==ItemAttributes{{"Preset.GarageOrder","2"}});
    CHECK(store.owned_item_attributes(a.ref,a_items[1].instance_id)==ItemAttributes{{"Preset.Label","weapon"}});
    CHECK(store.owned_item_attributes(b.ref,b_items[0].instance_id)==ItemAttributes{{"Preset.GarageOrder","7"}});
    CHECK(store.starter_items(a.ref)==std::optional<StarterItems>(a_items));
    CHECK(store.starter_items(b.ref)==std::optional<StarterItems>(b_items));
}

TEST_CASE("Owned attributes reject foreign instances account scopes and catalogue IDs") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a"),other(temp.path()/"b.db","server-b");
    const auto a=store.get_or_create_account("1",0),b=store.get_or_create_account("2",0);
    const auto foreign=other.get_or_create_account("1",0);
    const auto items=fixture_items(),other_items=fixture_items('3');
    REQUIRE(store.grant_starter_items(a.ref,items)); REQUIRE(store.grant_starter_items(b.ref,other_items));
    for (const auto& id:{other_items[0].instance_id,items[0].type_id,std::string("40000000-0000-0000-0000-000000000001")}) {
        expect_error(StoreErrorCode::not_found,[&]{store.owned_item_attributes(a.ref,id);});
        expect_error(StoreErrorCode::not_found,[&]{store.merge_owned_item_attributes(a.ref,id,{});});
    }
    expect_error(StoreErrorCode::invalid_scope,[&]{store.owned_item_attributes(foreign.ref,items[0].instance_id);});
    expect_error(StoreErrorCode::invalid_scope,[&]{store.merge_owned_item_attributes(foreign.ref,items[0].instance_id,{});});
    expect_error(StoreErrorCode::not_found,[&]{store.merge_owned_item_attributes({"server-a",999},items[0].instance_id,{});});
    expect_error(StoreErrorCode::invalid_input,[&]{store.owned_item_attributes({"server-a",0},items[0].instance_id);});
    CHECK(store.starter_items(a.ref)==std::optional<StarterItems>(items));
    CHECK(store.starter_items(b.ref)==std::optional<StarterItems>(other_items));
    CHECK(store.wallet(a.ref)==WalletBalances{});
}

TEST_CASE("Malformed instance IDs never select or create attributes") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    for (const auto& id:std::array<std::string,5>{"","PRIVATE-ID","00000000-0000-0000-0000-000000000000",
        "A0000000-0000-0000-0000-000000000001",items[0].instance_id+" "}) {
        expect_error(StoreErrorCode::invalid_input,[&]{store.owned_item_attributes(a.ref,id);});
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_owned_item_attributes(a.ref,id,{{"Preset.X","1"}});});
    }
    CHECK(store.starter_items(a.ref)==std::optional<StarterItems>(items));
}

TEST_CASE("Attribute operations require a complete grant and never repair inventory") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"a.db"; LocalStore store(path,"server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    auto code=StoreErrorCode::not_found;
    SUBCASE("No existing grant") {}
    SUBCASE("Recorded grant lacks another slot") {
        REQUIRE(store.grant_starter_items(a.ref,items));
        { const FixtureDb db(path); db.exec("DELETE FROM starter_items WHERE slot=8"); }
        code=StoreErrorCode::invalid_schema;
    }
    expect_error(code,[&]{store.owned_item_attributes(a.ref,items[0].instance_id);});
    expect_error(code,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,{});});
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_items")== (code==StoreErrorCode::not_found ? 0 : 8));
    CHECK(store.wallet(a.ref)==WalletBalances{});
}

TEST_CASE("Owned attributes allow only bounded Preset UTF8 strings and reject immutable keys atomically") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    const ItemAttributes initial{{"Preset.Label","before"}};
    REQUIRE(store.merge_owned_item_attributes(a.ref,items[0].instance_id,initial));
    for (const ItemAttributes& update:std::vector<ItemAttributes>{
        {{"Preset.Label","changed"},{"GameItemsGuid",items[1].type_id}},
        {{"UserGameItemGuid",items[1].instance_id}},{{"ReadOnly_Children","private"}},
        {{"ItemName","private"}},{{"Version","private"}},{{"Currency.HP","999"}},
        {{"Unlocked","1"}},{{"preset.Label","bad"}},{{"","bad"}},
        {{"Preset."+std::string(250,'k'),"bad"}},{{"Preset.Value",std::string(16385,'v')}},
        {{std::string("Preset.k\0x",10),"bad"}},{{"Preset.Value",std::string("v\0x",3)}},
        {{"Preset.\nkey","bad"}},{{std::string("Preset.\xc2\x85"),"bad"}},
        {{"Preset.Value",std::string("\xc0\xaf")}},{{std::string("Preset.\xed\xa0\x80"),"bad"}}}) {
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,update);});
        CHECK(store.owned_item_attributes(a.ref,items[0].instance_id)==initial);
    }
    const ItemAttributes boundary{{"Preset."+std::string(249,'k'),std::string(16384,'v')},
        {"Preset.Empty",""},{"Preset.Controls","line\n\tend"},{"Preset.Unicode","\xc3\xa9"}};
    CHECK(store.merge_owned_item_attributes(a.ref,items[0].instance_id,boundary));
    CHECK(store.owned_item_attributes(a.ref,items[0].instance_id).at("Preset.Controls")=="line\n\tend");
    CHECK(store.starter_items(a.ref)==std::optional<StarterItems>(items));
}

TEST_CASE("Owned attribute update count is bounded before any partial write") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    REQUIRE(store.merge_owned_item_attributes(a.ref,items[0].instance_id,{{"Preset.Label","before"}}));
    ItemAttributes update; for(int i=0;i<257;++i) update.emplace("Preset.k"+std::to_string(i),"v");
    expect_error(StoreErrorCode::invalid_input,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,update);});
    CHECK(store.owned_item_attributes(a.ref,items[0].instance_id)==ItemAttributes{{"Preset.Label","before"}});
}

TEST_CASE("Per instance attribute key and byte capacities reject whole merges") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    SUBCASE("1024 keys") {
        for(int batch=0;batch<4;++batch) {
            ItemAttributes update;
            for(int i=0;i<256;++i) update.emplace("Preset.k"+std::to_string(batch*256+i),"");
            REQUIRE(store.merge_owned_item_attributes(a.ref,items[0].instance_id,update));
        }
        REQUIRE(store.owned_item_attributes(a.ref,items[0].instance_id).size()==1024);
        const auto before=store.owned_item_attributes(a.ref,items[0].instance_id);
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,
            {{"Preset.k0","changed"},{"Preset.extra","v"}});});
        CHECK(store.owned_item_attributes(a.ref,items[0].instance_id)==before);
        CHECK(store.merge_owned_item_attributes(a.ref,items[1].instance_id,{{"Preset.extra","other instance"}}));
    }
    SUBCASE("262144 bytes") {
        ItemAttributes update; for(int i=0;i<16;++i) update.emplace("Preset.k"+std::to_string(i),std::string(16360,'v'));
        REQUIRE(store.merge_owned_item_attributes(a.ref,items[0].instance_id,update));
        const auto before=store.owned_item_attributes(a.ref,items[0].instance_id);
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,
            {{"Preset.k0",std::string(16384,'q')},{"Preset.extra",std::string(1024,'v')}});});
        CHECK(store.owned_item_attributes(a.ref,items[0].instance_id)==before);
    }
}

TEST_CASE("Concurrent attribute merges preserve independent keys") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    std::array<std::exception_ptr,2> failures;
    std::jthread one([&]{try {store.merge_owned_item_attributes(a.ref,items[0].instance_id,{{"Preset.A","');DROP TABLE accounts;--"}});}
        catch(...) {failures[0]=std::current_exception();}});
    std::jthread two([&]{try {store.merge_owned_item_attributes(a.ref,items[0].instance_id,{{"Preset.B","second"}});}
        catch(...) {failures[1]=std::current_exception();}});
    one.join(); two.join(); REQUIRE_FALSE(failures[0]); REQUIRE_FALSE(failures[1]);
    CHECK(store.owned_item_attributes(a.ref,items[0].instance_id)==ItemAttributes{
        {"Preset.A","');DROP TABLE accounts;--"},{"Preset.B","second"}});
    CHECK(store.find_account("1")==std::optional<Account>(a));
}

TEST_CASE("Stored attribute corruption fails closed without overwriting bad rows") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"a.db"; LocalStore store(path,"server-a");
    REQUIRE(store.schema_version()==6);
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    REQUIRE(store.merge_owned_item_attributes(a.ref,items[0].instance_id,{{"Preset.A","before"}}));
    SUBCASE("Forbidden key") {
        const FixtureDb db(path); db.exec("PRAGMA ignore_check_constraints=ON; UPDATE owned_item_attributes SET key='ReadOnly_Children'");
    }
    SUBCASE("Invalid value UTF8") {
        const FixtureDb db(path); db.exec("UPDATE owned_item_attributes SET value=CAST(x'c0af' AS TEXT)");
    }
    const auto before=hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema,[&]{store.owned_item_attributes(a.ref,items[0].instance_id);});
    expect_error(StoreErrorCode::invalid_schema,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,{{"Preset.A","new"}});});
    CHECK(hawken::test::read_bytes(path)==before);
}

TEST_CASE("Owned attribute schema damage is rejected before reading or writing rows") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"a.db"; LocalStore store(path,"server-a");
    REQUIRE(store.schema_version()==6);
    const auto a=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(a.ref,items));
    SUBCASE("Missing child table") { const FixtureDb db(path); db.exec("DROP TABLE owned_item_attributes"); }
    SUBCASE("Unknown child column") { const FixtureDb db(path); db.exec("ALTER TABLE owned_item_attributes ADD COLUMN unexpected TEXT"); }
    const auto before=hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema,[&]{store.owned_item_attributes(a.ref,items[0].instance_id);});
    expect_error(StoreErrorCode::invalid_schema,[&]{store.merge_owned_item_attributes(a.ref,items[0].instance_id,{});});
    CHECK(hawken::test::read_bytes(path)==before);
}

namespace {
void frozen_four(const std::filesystem::path& path) {
    // Independent historical schema4 fixture, not generated by current LocalStore.
    if (!std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT)/"tests/fixtures/local-store-v2.sqlite",path))
        throw std::runtime_error("Cannot copy historical starter fixture");
    const FixtureDb db(path);
    db.exec(R"(CREATE TABLE starter_grants (
    account_id INTEGER PRIMARY KEY NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT
) STRICT)");
    db.exec(R"(CREATE TABLE starter_items (
    account_id INTEGER NOT NULL REFERENCES starter_grants(account_id) ON DELETE RESTRICT,
    slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 8),
    type_id TEXT NOT NULL CHECK(
        length(CAST(type_id AS BLOB)) = 36 AND instr(CAST(type_id AS BLOB), x'00') = 0
        AND substr(type_id, 9, 1) = '-' AND substr(type_id, 14, 1) = '-'
        AND substr(type_id, 19, 1) = '-' AND substr(type_id, 24, 1) = '-'
        AND length(replace(type_id, '-', '')) = 32
        AND replace(type_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND type_id != '00000000-0000-0000-0000-000000000000'),
    instance_id TEXT NOT NULL UNIQUE CHECK(
        length(CAST(instance_id AS BLOB)) = 36 AND instr(CAST(instance_id AS BLOB), x'00') = 0
        AND substr(instance_id, 9, 1) = '-' AND substr(instance_id, 14, 1) = '-'
        AND substr(instance_id, 19, 1) = '-' AND substr(instance_id, 24, 1) = '-'
        AND length(replace(instance_id, '-', '')) = 32
        AND replace(instance_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND instance_id != '00000000-0000-0000-0000-000000000000'),
    PRIMARY KEY(account_id, slot)
) STRICT)");
    db.exec(R"(CREATE TABLE game_settings (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    key TEXT NOT NULL CHECK(
        length(CAST(key AS BLOB)) BETWEEN 1 AND 256
        AND instr(CAST(key AS BLOB), x'00') = 0),
    value TEXT NOT NULL CHECK(
        length(CAST(value AS BLOB)) <= 16384
        AND instr(CAST(value AS BLOB), x'00') = 0),
    PRIMARY KEY(account_id, key)
) STRICT)");
    db.exec("INSERT INTO starter_grants VALUES(17)");
    const auto items=fixture_items();
    for (std::size_t slot=0;slot<items.size();++slot) {
        const auto sql="INSERT INTO starter_items VALUES(17,"+std::to_string(slot)+",'"+
            items[slot].type_id+"','"+items[slot].instance_id+"')";
        db.exec(sql.c_str());
    }
    db.exec("INSERT INTO game_settings VALUES(17,'UIFlags.FTUXCompletedFlag','4'); PRAGMA user_version=4");
}
}

TEST_CASE("Schema four migrates to six preserving accounts grants balances and opaque settings") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"v4.db"; frozen_four(path);
    const auto items=fixture_items();
    LocalStore store(path,"server-a"); REQUIRE(store.schema_version()==6);
    const auto account=store.find_account("1"); REQUIRE(account);
    CHECK(account->ref==AccountRef{"server-a",17}); CHECK(account->created_at==10);
    CHECK(store.starter_items(account->ref)==std::optional<StarterItems>(items));
    CHECK(store.wallet(account->ref)==WalletBalances{200,0});
    CHECK(store.wallet({"server-a",23})==WalletBalances{INT32_MAX,50});
    CHECK(store.role(account->ref)==std::optional<Role>(Role::analyst));
    CHECK(store.visits(account->ref).size()==1);
    CHECK(store.game_settings(account->ref)==GameSettings{{"UIFlags.FTUXCompletedFlag","4"}});
    CHECK(store.owned_item_attributes(account->ref,items[0].instance_id).empty());
    const FixtureDb db(path);
    CHECK(db.scalar("PRAGMA user_version")==6);
    CHECK(db.scalar("SELECT count(*) FROM owned_item_attributes")==0);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants")==1);
    CHECK(db.scalar("SELECT count(*) FROM starter_items")==9);
    CHECK(db.scalar("SELECT count(*) FROM pragma_foreign_key_check")==0);
}

TEST_CASE("Invalid historical schema or scope cannot partially add item attributes") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"v4.db"; frozen_four(path);
    auto code=StoreErrorCode::invalid_schema; auto server="server-a";
    SUBCASE("Scope mismatch") { code=StoreErrorCode::invalid_scope; server="server-b"; }
    SUBCASE("Unexpected trigger") {
        const FixtureDb db(path); db.exec("CREATE TRIGGER unexpected AFTER INSERT ON starter_items BEGIN DELETE FROM wallets; END");
    }
    SUBCASE("Damaged foreign key") {
        const FixtureDb db(path); db.exec("PRAGMA foreign_keys=OFF; UPDATE starter_items SET account_id=999 WHERE slot=8");
    }
    const auto before=hawken::test::read_bytes(path);
    expect_error(code,[&]{LocalStore rejected(path,server);});
    CHECK(hawken::test::read_bytes(path)==before);
    const FixtureDb db(path); CHECK(db.scalar("PRAGMA user_version")==4);
    CHECK(db.scalar("SELECT count(*) FROM sqlite_schema WHERE name='owned_item_attributes'")==0);
}

TEST_CASE("Busy schema four migration leaves original tables unchanged and is retryable") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"v4.db"; frozen_four(path);
    {
        const FixtureDb db(path); db.exec("BEGIN EXCLUSIVE");
        expect_error(StoreErrorCode::busy,[&]{LocalStore rejected(path,"server-a");});
        CHECK(db.scalar("PRAGMA user_version")==4);
        CHECK(db.scalar("SELECT count(*) FROM sqlite_schema WHERE name='owned_item_attributes'")==0);
        db.exec("ROLLBACK");
    }
    LocalStore retry(path,"server-a"); CHECK(retry.schema_version()==6);
    CHECK(retry.starter_items({"server-a",17})==std::optional<StarterItems>(fixture_items()));
}

TEST_CASE("Owned starter mech snapshot returns raw saved attributes and survives reopening without writes") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"snapshot.db";
    const auto items=fixture_items(); Account account;
    const ItemAttributes attributes{{"Preset.Label","saved"},{"Preset.Empty",""},{"Preset.PrimaryWeapon_0","0007"}};
    {
        LocalStore store(path,"server-a"); account=store.get_or_create_account("1",0);
        REQUIRE(store.grant_starter_items(account.ref,items));
        REQUIRE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,attributes));
        const auto before=hawken::test::read_bytes(path);
        const auto snapshot=store.owned_starter_mech(account.ref,items[0].instance_id);
        CHECK(snapshot.items==items);
        CHECK(snapshot.attributes==attributes);
        CHECK(hawken::test::read_bytes(path)==before);
        CHECK(store.wallet(account.ref)==WalletBalances{});
    }
    LocalStore reopened(path,"server-a"); const auto before=hawken::test::read_bytes(path);
    auto snapshot=reopened.owned_starter_mech(account.ref,items[0].instance_id);
    CHECK(snapshot.items==items); CHECK(snapshot.attributes==attributes);
    snapshot.items[0].instance_id="changed"; snapshot.attributes["Preset.Label"]="changed";
    CHECK(reopened.owned_starter_mech(account.ref,items[0].instance_id).attributes==attributes);
    CHECK(hawken::test::read_bytes(path)==before);
    const FixtureDb db(path);
    CHECK(db.scalar("SELECT count(*) FROM starter_grants")==1);
    CHECK(db.scalar("SELECT count(*) FROM starter_items")==9);
    CHECK(db.scalar("SELECT count(*) FROM owned_item_attributes")==3);
}

TEST_CASE("Owned starter mech snapshot rejects foreign nonmech malformed and out of scope selections") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"snapshot.db";
    LocalStore store(path,"server-a"); const auto a=store.get_or_create_account("1",0),b=store.get_or_create_account("2",0);
    const auto items=fixture_items(),foreign=fixture_items('3');
    REQUIRE(store.grant_starter_items(a.ref,items)); REQUIRE(store.grant_starter_items(b.ref,foreign));
    const auto before=hawken::test::read_bytes(path);
    for(const auto& id:{foreign[0].instance_id,items[1].instance_id,items[0].type_id,
        std::string("40000000-0000-0000-0000-000000000001")})
        expect_error(StoreErrorCode::not_found,[&]{(void)store.owned_starter_mech(a.ref,id);});
    expect_error(StoreErrorCode::not_found,[&]{(void)store.owned_starter_mech({"server-a",999},items[0].instance_id);});
    expect_error(StoreErrorCode::invalid_scope,[&]{(void)store.owned_starter_mech({"server-b",a.ref.local_id},items[0].instance_id);});
    for(const auto& id:std::array<std::string,5>{"","PRIVATE-ID","00000000-0000-0000-0000-000000000000",
        "A0000000-0000-0000-0000-000000000001",items[0].instance_id+" "})
        expect_error(StoreErrorCode::invalid_input,[&]{(void)store.owned_starter_mech(a.ref,id);});
    CHECK(hawken::test::read_bytes(path)==before);
    CHECK(store.wallet(a.ref)==WalletBalances{}); CHECK(store.wallet(b.ref)==WalletBalances{});
}

TEST_CASE("Owned starter mech snapshot cannot grant repair or seed missing data") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"snapshot.db";
    LocalStore store(path,"server-a"); const auto account=store.get_or_create_account("1",0); const auto items=fixture_items();
    SUBCASE("Missing grant remains absent") {
        const auto before=hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::not_found,[&]{(void)store.owned_starter_mech(account.ref,items[0].instance_id);});
        CHECK(hawken::test::read_bytes(path)==before); CHECK_FALSE(store.starter_items(account.ref));
    }
    SUBCASE("Recorded incomplete grant is not repaired") {
        REQUIRE(store.grant_starter_items(account.ref,items));
        {const FixtureDb db(path); db.exec("DELETE FROM starter_items WHERE slot=8");}
        const auto before=hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_schema,[&]{(void)store.owned_starter_mech(account.ref,items[0].instance_id);});
        CHECK(hawken::test::read_bytes(path)==before);
        const FixtureDb db(path); CHECK(db.scalar("SELECT count(*) FROM starter_items")==8);
    }
    SUBCASE("Owned mech without saved attributes stays empty") {
        REQUIRE(store.grant_starter_items(account.ref,items)); const auto before=hawken::test::read_bytes(path);
        const auto snapshot=store.owned_starter_mech(account.ref,items[0].instance_id);
        CHECK(snapshot.items==items); CHECK(snapshot.attributes.empty());
        CHECK(hawken::test::read_bytes(path)==before);
        const FixtureDb db(path); CHECK(db.scalar("SELECT count(*) FROM owned_item_attributes")==0);
    }
}

TEST_CASE("Owned starter mech snapshot does not mix concurrent type and preset transaction versions") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"snapshot.db";
    LocalStore store(path,"server-a"); const auto account=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(account.ref,items));
    REQUIRE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,{{"Preset.Version","A"}}));
    {const FixtureDb db(path); db.exec("PRAGMA journal_mode=WAL");}
    std::atomic<bool> ready=false; std::exception_ptr writer_error;
    std::jthread writer([&]{
        try {
            const FixtureDb db(path); db.exec("PRAGMA busy_timeout=5000"); ready=true;
            for(int i=0;i<40;++i) {
                db.exec("BEGIN IMMEDIATE; UPDATE starter_items SET type_id='40000000-0000-0000-0000-000000000001' WHERE slot=0; "
                    "UPDATE owned_item_attributes SET value='B' WHERE key='Preset.Version'; COMMIT");
                db.exec("BEGIN IMMEDIATE; UPDATE starter_items SET type_id='10000000-0000-0000-0000-000000000001' WHERE slot=0; "
                    "UPDATE owned_item_attributes SET value='A' WHERE key='Preset.Version'; COMMIT");
            }
        } catch(...) {writer_error=std::current_exception();ready=true;}
    });
    while(!ready.load())std::this_thread::yield();
    for(int i=0;i<80;++i) {
        const auto snapshot=store.owned_starter_mech(account.ref,items[0].instance_id);
        const auto& type=snapshot.items[0].type_id;
        REQUIRE(snapshot.attributes.contains("Preset.Version"));
        const auto& version=snapshot.attributes.at("Preset.Version");
        CHECK(((type=="10000000-0000-0000-0000-000000000001" && version=="A") ||
               (type=="40000000-0000-0000-0000-000000000001" && version=="B")));
    }
    writer.join(); if(writer_error)std::rethrow_exception(writer_error);
    CHECK(store.wallet(account.ref)==WalletBalances{});
}
TEST_CASE("Online starter account snapshot combines raw attributes numeric stats and owned slots across reopen") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"online.db";
    const auto items=fixture_items(); Account account;
    {
        LocalStore store(path,"server-a"); account=store.get_or_create_account("1",0);
        REQUIRE(store.grant_starter_items(account.ref,items));
        REQUIRE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,{{"Preset.Version","saved"},{"Preset.Empty",""}}));
        REQUIRE(store.schema_version()==6); const FixtureDb db(path);
        db.exec("INSERT INTO unranked_gameplay_stats VALUES(1,'fixture.counter',7.5); "
            "INSERT INTO owned_item_stats VALUES(1,1,'fixture.item',-2.25)");
        const auto before=hawken::test::read_bytes(path); const auto snapshot=store.account_online_snapshot(account.ref);
        REQUIRE(snapshot.starter); CHECK(snapshot.starter->items==items);
        CHECK(snapshot.starter->attributes==ItemAttributes{{"Preset.Version","saved"},{"Preset.Empty",""}});
        CHECK(snapshot.unranked_gameplay_stats==NumericStats{{"fixture.counter",7.5}});
        CHECK(snapshot.item_stats_by_slot==std::map<std::size_t,NumericStats>{{1,{{"fixture.item",-2.25}}}});
        CHECK(snapshot.achievement_ids.empty()); CHECK(hawken::test::read_bytes(path)==before);
    }
    LocalStore reopened(path,"server-a"); const auto snapshot=reopened.account_online_snapshot(account.ref);
    REQUIRE(snapshot.starter); CHECK(snapshot.starter->items==items); CHECK(snapshot.starter->attributes.at("Preset.Empty").empty());
    CHECK(snapshot.unranked_gameplay_stats.at("fixture.counter")==7.5); CHECK(snapshot.item_stats_by_slot.at(1).at("fixture.item")==-2.25);
    CHECK(reopened.wallet(account.ref)==WalletBalances{});
}

TEST_CASE("Online starter account snapshot isolates accounts and rejects malformed grant or stats slot") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"isolated.db";
    LocalStore store(path,"server-a"); const auto one=store.get_or_create_account("1",0),two=store.get_or_create_account("2",0);
    REQUIRE(store.grant_starter_items(one.ref,fixture_items())); REQUIRE(store.grant_starter_items(two.ref,fixture_items('3')));
    REQUIRE(store.schema_version()==6); const FixtureDb db(path);
    db.exec("INSERT INTO unranked_gameplay_stats VALUES(1,'same',11.0),(2,'same',22.0); "
        "INSERT INTO owned_item_stats VALUES(1,0,'same',111.0),(2,0,'same',222.0)");
    CHECK(store.account_online_snapshot(one.ref).unranked_gameplay_stats.at("same")==11.0);
    CHECK(store.account_online_snapshot(two.ref).item_stats_by_slot.at(0).at("same")==222.0);
    SUBCASE("Missing ninth owned row") {db.exec("DELETE FROM starter_items WHERE account_id=1 AND slot=8");}
    SUBCASE("Out of range slot") {
        // Deliberately bypass both constraints on this fixture connection only;
        // the product reader must reject an actually persisted orphan slot.
        db.exec("PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON; INSERT INTO owned_item_stats VALUES(1,9,'bad',1.0)");
        REQUIRE(db.scalar("SELECT count(*) FROM owned_item_stats WHERE account_id=1 AND slot=9")==1);
        REQUIRE(db.scalar("SELECT count(*) FROM starter_items WHERE account_id=1 AND slot=9")==0);
    }
    SUBCASE("Reserved instance metadata") {db.exec("PRAGMA ignore_check_constraints=ON; INSERT INTO owned_item_stats VALUES(1,0,'UserGameItemGuid',1.0)");}
    expect_error(StoreErrorCode::invalid_schema,[&]{store.account_online_snapshot(one.ref);});
    const auto untouched=store.account_online_snapshot(two.ref); REQUIRE(untouched.starter);
    CHECK(untouched.starter->items==fixture_items('3')); CHECK(untouched.unranked_gameplay_stats.at("same")==22.0);
}

TEST_CASE("Online starter account snapshot migration preserves nine items attributes and wallets") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"migrated.db"; const auto items=fixture_items(); Account account;
    {LocalStore store(path,"server-a");account=store.get_or_create_account("1",0);
        REQUIRE(store.grant_starter_items(account.ref,items));
        REQUIRE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,{{"Preset.Saved","kept"}}));}
    {const FixtureDb db(path);db.exec("DROP TABLE IF EXISTS owned_item_stats; DROP TABLE IF EXISTS unranked_gameplay_stats; "
        "PRAGMA user_version=5; UPDATE wallets SET hawken_points=73,meteor_points=11 WHERE account_id=1");}
    LocalStore store(path,"server-a"); REQUIRE(store.schema_version()==6);
    const auto snapshot=store.account_online_snapshot(account.ref); REQUIRE(snapshot.starter);
    CHECK(snapshot.starter->items==items); CHECK(snapshot.starter->attributes==ItemAttributes{{"Preset.Saved","kept"}});
    CHECK(snapshot.unranked_gameplay_stats.empty()); CHECK(snapshot.item_stats_by_slot.empty());
    CHECK(store.wallet(account.ref)==WalletBalances{73,11});
    const FixtureDb db(path); CHECK(db.scalar("SELECT count(*) FROM starter_items")==9);
    CHECK(db.scalar("SELECT count(*) FROM owned_item_attributes")==1);
}

TEST_CASE("Online starter account snapshot is atomic across inventory attributes and both numeric collections") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"atomic-online.db";
    LocalStore store(path,"server-a");const auto account=store.get_or_create_account("1",0); const auto items=fixture_items();
    REQUIRE(store.grant_starter_items(account.ref,items));
    REQUIRE(store.merge_owned_item_attributes(account.ref,items[0].instance_id,{{"Preset.Version","A"}}));
    REQUIRE(store.schema_version()==6);
    {const FixtureDb db(path);db.exec("PRAGMA journal_mode=WAL; INSERT INTO unranked_gameplay_stats VALUES(1,'version',1.0); "
        "INSERT INTO owned_item_stats VALUES(1,0,'version',1.0)");}
    std::atomic<bool> ready=false; std::exception_ptr writer_error;
    std::jthread writer([&]{try{const FixtureDb db(path);db.exec("PRAGMA busy_timeout=5000");ready=true;
        for(int i=0;i<40;++i){
            db.exec("BEGIN IMMEDIATE; UPDATE starter_items SET type_id='40000000-0000-0000-0000-000000000001' WHERE slot=0; "
                "UPDATE owned_item_attributes SET value='B' WHERE key='Preset.Version'; UPDATE unranked_gameplay_stats SET value=2.0; "
                "UPDATE owned_item_stats SET value=2.0; COMMIT");
            db.exec("BEGIN IMMEDIATE; UPDATE starter_items SET type_id='10000000-0000-0000-0000-000000000001' WHERE slot=0; "
                "UPDATE owned_item_attributes SET value='A' WHERE key='Preset.Version'; UPDATE unranked_gameplay_stats SET value=1.0; "
                "UPDATE owned_item_stats SET value=1.0; COMMIT");
        }
    }catch(...){writer_error=std::current_exception();ready=true;}});
    while(!ready.load())std::this_thread::yield();
    for(int i=0;i<80;++i){const auto snapshot=store.account_online_snapshot(account.ref); REQUIRE(snapshot.starter);
        const auto& type=snapshot.starter->items[0].type_id; const auto& version=snapshot.starter->attributes.at("Preset.Version");
        const double user=snapshot.unranked_gameplay_stats.at("version"),item=snapshot.item_stats_by_slot.at(0).at("version");
        CHECK(((type=="10000000-0000-0000-0000-000000000001" && version=="A" && user==1.0 && item==1.0) ||
               (type=="40000000-0000-0000-0000-000000000001" && version=="B" && user==2.0 && item==2.0)));
    }
    writer.join();if(writer_error)std::rethrow_exception(writer_error);
    CHECK(store.wallet(account.ref)==WalletBalances{});
}
