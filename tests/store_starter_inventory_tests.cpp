#include <doctest/doctest.h>
#include <hawken/local_store.hpp>
#include <sqlite3.h>
#include "test_support.hpp"
#include <array>
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

TEST_CASE("Schema four creates empty starter tables without silently granting items") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    REQUIRE(store.schema_version() == 4);
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

TEST_CASE("Frozen schema two migrates to four retaining accounts and nonzero earned balances") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "v2.db";
    const auto fixture = std::filesystem::path(HAWKEN_PROJECT_ROOT) / "tests/fixtures/local-store-v2.sqlite";
    const auto original = hawken::test::read_bytes(fixture);
    REQUIRE(std::filesystem::copy_file(fixture, path));
    LocalStore store(path, "server-a");
    REQUIRE(store.schema_version() == 4);
    REQUIRE(store.find_account("1").has_value());
    CHECK(store.find_account("1")->ref.local_id == 17);
    CHECK(store.wallet({"server-a", 17}) == WalletBalances{200, 0});
    CHECK(store.wallet({"server-a", 23}) == WalletBalances{INT32_MAX, 50});
    CHECK(store.role({"server-a", 17}) == std::optional<Role>(Role::analyst));
    CHECK(store.visits({"server-a", 17}).size() == 1);
    CHECK_FALSE(store.starter_items({"server-a", 17}).has_value());
    CHECK_FALSE(store.starter_items({"server-a", 23}).has_value());
    const FixtureDb db(path);
    CHECK(db.scalar("PRAGMA user_version") == 4);
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
    CHECK(retry.schema_version() == 4);
    CHECK(retry.wallet({"server-a", 23}) == WalletBalances{INT32_MAX, 50});
}
