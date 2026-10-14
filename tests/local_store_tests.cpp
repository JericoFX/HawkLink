#include <doctest/doctest.h>
#include <hawken/local_store.hpp>
#include <sqlite3.h>
#include "test_support.hpp"
#include <array>
#include <exception>
#include <memory>
#include <thread>
#include <string>
#include <utility>

namespace {
using namespace hawken::store;

// Real SQLite connections construct or inspect isolated fixtures only. They do
// not expose or replace the product connection.
class FixtureDb {
public:
    explicit FixtureDb(const std::filesystem::path& path) {
        const auto utf8 = path.u8string();
        const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        const int result = sqlite3_open_v2(name.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (result != SQLITE_OK) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error("Cannot open SQLite fixture");
        }
    }
    ~FixtureDb() { sqlite3_close_v2(db_); }
    FixtureDb(const FixtureDb&) = delete;
    FixtureDb& operator=(const FixtureDb&) = delete;
    void exec(const char* sql) const {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot prepare SQLite fixture");
    }
    std::int64_t scalar(const char* sql) const {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot inspect SQLite fixture");
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (sqlite3_step(statement.get()) != SQLITE_ROW)
            throw std::runtime_error("Missing SQLite fixture result");
        return sqlite3_column_int64(statement.get(), 0);
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

void initialize(const std::filesystem::path& path) {
    LocalStore store(path, "server-a");
    REQUIRE(store.schema_version() == 5);
}
}

TEST_CASE("StoreError exposes a stable code and a fixed redacted message") {
    const StoreError error(StoreErrorCode::io_error);
    CHECK(error.code() == StoreErrorCode::io_error);
    CHECK(std::string(error.what()) == "Local store operation failed.");
}

TEST_CASE("LocalStore initializes schema five and preserves a canonical Steam text link") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    CHECK(store.schema_version() == 5);
    CHECK_FALSE(store.find_account("18446744073709551615").has_value());
    const auto account = store.get_or_create_account("18446744073709551615", 10);
    CHECK(account.ref.server_id == "server-a");
    CHECK(account.ref.local_id > 0);
    CHECK(account.steam_id64 == "18446744073709551615");
    CHECK(account.created_at == 10);
    CHECK(store.get_or_create_account(account.steam_id64, 100) == account);
    CHECK(store.find_account(account.steam_id64) == std::optional<Account>(account));
    CHECK_FALSE(store.role(account.ref).has_value());
    CHECK_FALSE(store.ban(account.ref).has_value());
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("PRAGMA user_version") == 5);
    CHECK(fixture.scalar("PRAGMA application_id") == 0x4841574B);
    CHECK(fixture.scalar("SELECT count(*) FROM accounts WHERE typeof(steam_id64)='text'") == 1);
    CHECK(fixture.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

TEST_CASE("LocalStore persists account visits aliases roles and bans across reopen") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    Account account;
    const Ban saved{15, 80, "local rule"};
    {
        LocalStore store(path, "server-a");
        account = store.get_or_create_account("76561198000000001", 10);
        CHECK(store.record_visit({"v-1", account.ref, "Pilot", 20}));
        store.set_role(account.ref, Role::analyst);
        store.set_ban(account.ref, saved);
    }
    LocalStore reopened(path, "server-a");
    CHECK(reopened.find_account(account.steam_id64) == std::optional<Account>(account));
    CHECK(reopened.aliases(account.ref) == std::vector<Alias>{{"Pilot", 20, 20}});
    CHECK(reopened.visits(account.ref) == std::vector<Visit>{{"v-1", account.ref, "Pilot", 20}});
    CHECK(reopened.role(account.ref) == std::optional<Role>(Role::analyst));
    CHECK(reopened.ban(account.ref) == std::optional<Ban>(saved));
}

TEST_CASE("Two server databases isolate the same Steam link and visit identifier") {
    hawken::test::TempDirectory temp;
    LocalStore first(temp.path() / "a.db", "server-a");
    LocalStore second(temp.path() / "b.db", "server-b");
    const auto a = first.get_or_create_account("76561198000000001", 1);
    const auto b = second.get_or_create_account(a.steam_id64, 2);
    CHECK(first.record_visit({"shared-visit", a.ref, "Alice", 3}));
    CHECK(second.record_visit({"shared-visit", b.ref, "Bob", 4}));
    first.set_role(a.ref, Role::owner);
    first.set_ban(a.ref, {0, std::nullopt, "Only A"});
    CHECK_FALSE(second.role(b.ref).has_value());
    CHECK_FALSE(second.is_banned(b.ref, 10));
    REQUIRE(first.aliases(a.ref).size() == 1);
    REQUIRE(second.aliases(b.ref).size() == 1);
    CHECK(first.aliases(a.ref).front().value == "Alice");
    CHECK(second.aliases(b.ref).front().value == "Bob");
    expect_error(StoreErrorCode::invalid_scope, [&] { second.role(a.ref); });
    expect_error(StoreErrorCode::invalid_scope, [&] { first.set_role(b.ref, Role::owner); });
    expect_error(StoreErrorCode::invalid_scope, [&] { second.record_visit({"foreign", a.ref, "X", 5}); });
}

TEST_CASE("Visit replay is idempotent and changed payloads leave aliases and visits unchanged") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto a = store.get_or_create_account("1", 0);
    const auto b = store.get_or_create_account("2", 0);
    const Visit visit{"once", a.ref, "Original", 20};
    CHECK(store.record_visit(visit));
    CHECK_FALSE(store.record_visit(visit));
    expect_error(StoreErrorCode::conflict, [&] { store.record_visit({"once", a.ref, "Changed", 20}); });
    expect_error(StoreErrorCode::conflict, [&] { store.record_visit({"once", a.ref, "Original", 99}); });
    expect_error(StoreErrorCode::conflict, [&] { store.record_visit({"once", b.ref, "Other", 20}); });
    CHECK(store.visits(a.ref) == std::vector<Visit>{visit});
    CHECK(store.aliases(a.ref) == std::vector<Alias>{{"Original", 20, 20}});
    CHECK(store.visits(b.ref).empty());
    CHECK(store.aliases(b.ref).empty());
}

TEST_CASE("Alias times handle out of order visits and reads have deterministic limits") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    CHECK(store.record_visit({"z", account.ref, "Zulu", 20}));
    CHECK(store.record_visit({"a", account.ref, "Alpha", 20}));
    CHECK(store.record_visit({"old", account.ref, "Alpha", 5}));
    CHECK(store.record_visit({"new", account.ref, "Alpha", 30}));
    CHECK(store.aliases(account.ref) == std::vector<Alias>{{"Alpha", 5, 30}, {"Zulu", 20, 20}});
    CHECK(store.aliases(account.ref, 1) == std::vector<Alias>{{"Alpha", 5, 30}});
    const auto visits = store.visits(account.ref, 3);
    REQUIRE(visits.size() == 3);
    CHECK(visits[0].visit_id == "new");
    CHECK(visits[1].visit_id == "a");
    CHECK(visits[2].visit_id == "z");
    CHECK(store.visits(account.ref, 256).size() == 4);
    expect_error(StoreErrorCode::invalid_input, [&] { store.aliases(account.ref, 0); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.aliases(account.ref, 257); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.visits(account.ref, 0); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.visits(account.ref, 257); });
}

TEST_CASE("Missing account writes fail without adding a visit or alias") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const AccountRef missing{"server-a", 999};
    expect_error(StoreErrorCode::not_found, [&] { store.record_visit({"missing", missing, "Ghost", 1}); });
    expect_error(StoreErrorCode::not_found, [&] { store.role(missing); });
    expect_error(StoreErrorCode::not_found, [&] { store.set_role(missing, Role::owner); });
    expect_error(StoreErrorCode::not_found, [&] { store.ban(missing); });
    expect_error(StoreErrorCode::not_found, [&] { store.set_ban(missing, {0, 1, "Missing"}); });
    const FixtureDb fixture(temp.path() / "local.db");
    CHECK(fixture.scalar("SELECT count(*) FROM visits") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM aliases") == 0);
}

TEST_CASE("Roles are explicit scoped closed values and may be revoked") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    CHECK_FALSE(store.role(account.ref).has_value());
    for (const auto value : {Role::owner, Role::moderator, Role::analyst}) {
        store.set_role(account.ref, value);
        CHECK(store.role(account.ref) == std::optional<Role>(value));
    }
    store.set_role(account.ref, std::nullopt);
    CHECK_FALSE(store.role(account.ref).has_value());
    store.set_role(account.ref, std::nullopt);
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_role(account.ref, static_cast<Role>(99)); });
    CHECK_FALSE(store.role(account.ref).has_value());
}

TEST_CASE("Ban intervals use explicit time with inclusive start and exclusive expiry") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    CHECK_FALSE(store.is_banned(account.ref, 0));
    store.set_ban(account.ref, {10, 20, "Local ban"});
    CHECK_FALSE(store.is_banned(account.ref, 9));
    CHECK(store.is_banned(account.ref, 10));
    CHECK(store.is_banned(account.ref, 19));
    CHECK_FALSE(store.is_banned(account.ref, 20));
    CHECK_FALSE(store.is_banned(account.ref, 21));
    store.set_ban(account.ref, {0, std::nullopt, ""});
    CHECK(store.is_banned(account.ref, 0));
    CHECK(store.is_banned(account.ref, INT64_MAX));
    expect_error(StoreErrorCode::invalid_input, [&] { store.is_banned(account.ref, -1); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {-1, 20, "x"}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {10, 10, "x"}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {10, 9, "x"}); });
    CHECK(store.ban(account.ref) == std::optional<Ban>(Ban{0, std::nullopt, ""}));
}

TEST_CASE("Steam link validation rejects noncanonical or out of range decimal strings") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    for (const auto& invalid : std::array<std::string, 10>{
            "", "0", "01", "+1", "-1", " 1", "1 ", "1.0", "18446744073709551616", std::string("1\0", 2)}) {
        CAPTURE(invalid);
        expect_error(StoreErrorCode::invalid_input, [&] { store.get_or_create_account(invalid, 0); });
        expect_error(StoreErrorCode::invalid_input, [&] { store.find_account(invalid); });
    }
    expect_error(StoreErrorCode::invalid_input, [&] { store.get_or_create_account("1", -1); });
    CHECK_FALSE(store.find_account("1").has_value());
}

TEST_CASE("Server and visit identifiers use bounded safe ASCII") {
    hawken::test::TempDirectory temp;
    for (const auto& invalid : std::array<std::string, 8>{"", ".", "..", std::string(65, 'x'), "a/b", "a b", "a:b", "\xc3\xa9"}) {
        CAPTURE(invalid);
        expect_error(StoreErrorCode::invalid_input, [&] { LocalStore rejected(temp.path() / "invalid.db", invalid); });
    }
    LocalStore store(temp.path() / "valid.db", std::string(64, 'a'));
    const auto account = store.get_or_create_account("1", 0);
    CHECK(store.record_visit({std::string(64, 'v'), account.ref, "A", 0}));
    for (const auto& invalid : std::array<std::string, 7>{"", ".", "..", std::string(65, 'v'), "a/b", "a b", std::string("v\0", 2)}) {
        CAPTURE(invalid);
        expect_error(StoreErrorCode::invalid_input, [&] { store.record_visit({invalid, account.ref, "A", 0}); });
    }
    expect_error(StoreErrorCode::invalid_input, [&] { store.role({account.ref.server_id, 0}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.role({account.ref.server_id, -1}); });
}

TEST_CASE("Alias UTF8 and byte bounds reject invalid observations atomically") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    CHECK(store.record_visit({"unicode", account.ref, "Piloto \xc3\xa9 \xf0\x9f\x9a\x80", 0}));
    CHECK(store.record_visit({"boundary", account.ref, std::string(128, 'x'), 0}));
    for (const auto& invalid : std::array<std::string, 11>{"", std::string(129, 'x'), "line\n", "tab\t", std::string("x\0y", 3),
            "\x7f", "\xc2\x85", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
        CAPTURE(invalid);
        expect_error(StoreErrorCode::invalid_input, [&] { store.record_visit({"invalid", account.ref, invalid, 0}); });
    }
    expect_error(StoreErrorCode::invalid_input, [&] { store.record_visit({"negative", account.ref, "A", -1}); });
    CHECK(store.visits(account.ref).size() == 2);
    CHECK(store.aliases(account.ref).size() == 2);
}

TEST_CASE("Ban reason is UTF8 with a bounded byte length and invalid updates preserve state") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const Ban saved{0, 10, std::string(256, 'x')};
    store.set_ban(account.ref, saved);
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {0, 10, std::string(257, 'x')}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {0, 10, "\xc0\xaf"}); });
    for (const auto& control : std::array<std::string, 4>{"\n", "\x7f", "\xc2\x85", std::string("x\0y", 3)})
        expect_error(StoreErrorCode::invalid_input, [&] { store.set_ban(account.ref, {0, 10, control}); });
    CHECK(store.ban(account.ref) == std::optional<Ban>(saved));
}

TEST_CASE("SQL shaped user strings are stored as literal bound values") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const std::string payload = "O'Reilly'); DROP TABLE accounts;--";
    CHECK(store.record_visit({"literal", account.ref, payload, 0}));
    store.set_ban(account.ref, {0, 1, payload});
    REQUIRE(store.aliases(account.ref).size() == 1);
    CHECK(store.aliases(account.ref).front().value == payload);
    REQUIRE(store.ban(account.ref).has_value());
    CHECK(store.ban(account.ref)->reason == payload);
    CHECK(store.find_account("1") == std::optional<Account>(account));
    const FixtureDb fixture(temp.path() / "local.db");
    CHECK(fixture.scalar("SELECT count(*) FROM accounts") == 1);
}

TEST_CASE("Future schema foreign application and another server ID fail without adopting the database") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    initialize(path);
    SUBCASE("future version") {
        { const FixtureDb fixture(path); fixture.exec("PRAGMA user_version=999"); }
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::unsupported_schema, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
    SUBCASE("foreign application") {
        { const FixtureDb fixture(path); fixture.exec("PRAGMA application_id=12345"); }
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::unsupported_schema, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
    SUBCASE("different server") {
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_scope, [&] { LocalStore rejected(path, "server-b"); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
}

TEST_CASE("Version zero with an existing user schema is rejected without partial migration") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "foreign.db";
    { const FixtureDb fixture(path); fixture.exec("CREATE TABLE preserved(value TEXT); INSERT INTO preserved VALUES('sentinel')"); }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
    CHECK(hawken::test::read_bytes(path) == before);
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("PRAGMA user_version") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM sqlite_schema WHERE name='accounts'") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM preserved") == 1);
}

TEST_CASE("Version zero views are user schema even when no table exists") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "foreign.db";
    { const FixtureDb fixture(path); fixture.exec("CREATE VIEW foreign_view AS SELECT 1 AS value"); }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Incomplete schema and altered constraints fail closed rather than being repaired") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    initialize(path);
    SUBCASE("missing table") {
        const FixtureDb fixture(path);
        fixture.exec("DROP TABLE bans");
    }
    SUBCASE("same columns but missing role constraints") {
        const FixtureDb fixture(path);
        fixture.exec("DROP TABLE roles; CREATE TABLE roles(account_id INTEGER PRIMARY KEY, role TEXT)");
    }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Unexpected triggers and views are rejected without executing them") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    initialize(path);
    SUBCASE("trigger") {
        const FixtureDb fixture(path);
        fixture.exec("CREATE TABLE trigger_audit(n INTEGER); CREATE TRIGGER unexpected AFTER INSERT ON accounts BEGIN INSERT INTO trigger_audit VALUES(1); END");
    }
    SUBCASE("view") {
        const FixtureDb fixture(path);
        fixture.exec("CREATE VIEW unexpected AS SELECT * FROM accounts");
    }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(path, "server-a"); });
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Busy initialization leaves an empty database unmigrated and retry can initialize it") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "locked.db";
    {
        const FixtureDb lock(path);
        lock.exec("BEGIN EXCLUSIVE");
        expect_error(StoreErrorCode::busy, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(lock.scalar("PRAGMA user_version") == 0);
        CHECK(lock.scalar("SELECT count(*) FROM sqlite_schema") == 0);
        lock.exec("ROLLBACK");
    }
    LocalStore retry(path, "server-a");
    CHECK(retry.schema_version() == 5);
}

TEST_CASE("Corrupt databases and invalid paths return redacted storage errors") {
    hawken::test::TempDirectory temp;
    const auto corrupt = temp.write("private-sentinel.db", "not a SQLite database: private-user-value");
    expect_error(StoreErrorCode::invalid_schema, [&] { LocalStore rejected(corrupt, "server-a"); });
    const auto missing = temp.path() / "private-missing-directory" / "private.db";
    expect_error(StoreErrorCode::io_error, [&] { LocalStore rejected(missing, "server-a"); });
}

TEST_CASE("SQLite extension loading support is absent from the embedded dependency") {
    CHECK(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 1);
    hawken::test::TempDirectory temp;
    const FixtureDb fixture(temp.path() / "fixture.db");
    CHECK_THROWS_AS(fixture.exec("SELECT load_extension('never-load-this')"), std::runtime_error);
}

TEST_CASE("One LocalStore serializes complete concurrent account and visit operations") {
    hawken::test::TempDirectory temp;
    LocalStore store(temp.path() / "local.db", "server-a");
    std::array<Account, 2> accounts;
    std::array<std::exception_ptr, 2> failures;
    std::array<std::jthread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread([&, index] {
            try {
                for (std::int64_t visit = 0; visit < 32; ++visit) {
                    accounts[index] = store.get_or_create_account("76561198000000001", 0);
                    store.record_visit({"t" + std::to_string(index) + "-" + std::to_string(visit),
                        accounts[index].ref, "Thread-" + std::to_string(index),
                        static_cast<std::int64_t>(index) * 100 + visit});
                }
            } catch (...) { failures[index] = std::current_exception(); }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(failures[0] == nullptr);
    REQUIRE(failures[1] == nullptr);
    REQUIRE(accounts[0].ref.local_id > 0);
    CHECK(accounts[0] == accounts[1]);
    CHECK(store.visits(accounts[0].ref, 256).size() == 64);
    CHECK(store.aliases(accounts[0].ref) == std::vector<Alias>{{"Thread-1", 100, 131}, {"Thread-0", 0, 31}});
    for (std::size_t index = 0; index < workers.size(); ++index) {
        for (std::int64_t visit = 0; visit < 32; ++visit)
            CHECK_FALSE(store.record_visit({"t" + std::to_string(index) + "-" + std::to_string(visit),
                accounts[index].ref, "Thread-" + std::to_string(index),
                static_cast<std::int64_t>(index) * 100 + visit}));
    }
}

TEST_CASE("A schema change after opening is rejected before an unexpected trigger can run") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    {
        const FixtureDb fixture(path);
        fixture.exec("CREATE TRIGGER unexpected AFTER INSERT ON visits BEGIN UPDATE server_metadata SET server_id='hijacked'; END");
    }
    expect_error(StoreErrorCode::invalid_schema, [&] { store.record_visit({"never", account.ref, "Pilot", 0}); });
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("SELECT count(*) FROM visits") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM aliases") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM server_metadata WHERE server_id='server-a'") == 1);
}

TEST_CASE("Returned row data is validated independently of an unchanged schema") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    {
        const FixtureDb fixture(path);
        fixture.exec("INSERT INTO aliases SELECT id,CAST(x'C0AF' AS TEXT),0,0 FROM accounts; "
            "INSERT INTO bans SELECT id,0,NULL,CAST(x'C285' AS TEXT) FROM accounts; "
            "INSERT INTO visits SELECT 'invalid',id,char(10),0 FROM accounts");
    }
    expect_error(StoreErrorCode::invalid_schema, [&] { store.aliases(account.ref); });
    expect_error(StoreErrorCode::invalid_schema, [&] { store.visits(account.ref); });
    expect_error(StoreErrorCode::invalid_schema, [&] { store.ban(account.ref); });
}

TEST_CASE("Every new account receives two zero SQL balances without granting credits") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto first = store.get_or_create_account("1", 0);
    const auto second = store.get_or_create_account("2", 10);
    CHECK(store.wallet(first.ref) == WalletBalances{0, 0});
    CHECK(store.wallet(second.ref) == WalletBalances{0, 0});
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 2);
    CHECK(fixture.scalar("SELECT count(*) FROM wallets WHERE hawken_points=0 AND meteor_points=0") == 2);
    CHECK(fixture.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

TEST_CASE("Wallet reads and repeated login provisioning preserve stored int32 balances") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    Account account;
    {
        LocalStore store(path, "server-a");
        account = store.get_or_create_account("1", 0);
        { const FixtureDb fixture(path); fixture.exec(
            "UPDATE wallets SET hawken_points=2147483647,meteor_points=27"); }
        const auto before = hawken::test::read_bytes(path);
        CHECK(store.wallet(account.ref) == WalletBalances{INT32_MAX, 27});
        CHECK(store.wallet(account.ref) == WalletBalances{INT32_MAX, 27});
        CHECK(hawken::test::read_bytes(path) == before);
        CHECK(store.get_or_create_account("1", 100) == account);
        CHECK(store.wallet(account.ref) == WalletBalances{INT32_MAX, 27});
        CHECK(hawken::test::read_bytes(path) == before);
    }
    LocalStore reopened(path, "server-a");
    CHECK(reopened.wallet(account.ref) == WalletBalances{INT32_MAX, 27});
}

TEST_CASE("Wallet account and server scope cannot read another balance or create a row") {
    hawken::test::TempDirectory temp;
    const auto first_path = temp.path() / "a.db";
    const auto second_path = temp.path() / "b.db";
    LocalStore first(first_path, "server-a");
    LocalStore second(second_path, "server-b");
    const auto a = first.get_or_create_account("1", 0);
    const auto other = first.get_or_create_account("2", 0);
    const auto b = second.get_or_create_account("1", 0);
    { const FixtureDb fixture(first_path); fixture.exec(
        "UPDATE wallets SET hawken_points=31,meteor_points=7 WHERE account_id=1"); }
    { const FixtureDb fixture(second_path); fixture.exec(
        "UPDATE wallets SET hawken_points=8,meteor_points=43"); }
    CHECK(first.wallet(a.ref) == WalletBalances{31, 7});
    CHECK(first.wallet(other.ref) == WalletBalances{0, 0});
    CHECK(second.wallet(b.ref) == WalletBalances{8, 43});
    expect_error(StoreErrorCode::invalid_scope, [&] { second.wallet(a.ref); });
    expect_error(StoreErrorCode::invalid_scope, [&] { first.wallet(b.ref); });
    expect_error(StoreErrorCode::invalid_input, [&] { first.wallet({"server-a", 0}); });
    expect_error(StoreErrorCode::invalid_input, [&] { first.wallet({"server-a", -1}); });
    expect_error(StoreErrorCode::invalid_input, [&] { first.wallet({"", a.ref.local_id}); });
    expect_error(StoreErrorCode::not_found, [&] { first.wallet({"server-a", 999}); });
    const FixtureDb fixture(first_path);
    CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 2);
    CHECK(fixture.scalar("SELECT count(*) FROM accounts") == 2);
}

TEST_CASE("Wallet SQL schema rejects negative oversized nonintegral and null balances") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const FixtureDb fixture(path);
    for (const auto* sql : {
        "UPDATE wallets SET hawken_points=-1", "UPDATE wallets SET meteor_points=-1",
        "UPDATE wallets SET hawken_points=2147483648", "UPDATE wallets SET meteor_points=2147483648",
        "UPDATE wallets SET hawken_points=1.5", "UPDATE wallets SET meteor_points='invalid'",
        "UPDATE wallets SET hawken_points=NULL", "UPDATE wallets SET meteor_points=NULL"}) {
        CHECK_THROWS_AS(fixture.exec(sql), std::runtime_error);
        CHECK(store.wallet(account.ref) == WalletBalances{0, 0});
    }
}

TEST_CASE("Wallet validates persisted values even if a trusted owner bypassed CHECK constraints") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    const FixtureDb fixture(path);
    fixture.exec("PRAGMA ignore_check_constraints=ON");
    for (const auto* sql : {"UPDATE wallets SET hawken_points=-1,meteor_points=0",
        "UPDATE wallets SET hawken_points=0,meteor_points=-1",
        "UPDATE wallets SET hawken_points=2147483648,meteor_points=0",
        "UPDATE wallets SET hawken_points=0,meteor_points=2147483648"}) {
        fixture.exec(sql);
        const auto before = hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_schema, [&] { store.wallet(account.ref); });
        CHECK(hawken::test::read_bytes(path) == before);
    }
    fixture.exec("UPDATE wallets SET hawken_points=9,meteor_points=12");
    CHECK(store.wallet(account.ref) == WalletBalances{9, 12});
}

TEST_CASE("A missing wallet fails closed and reads never silently restore balances") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    { const FixtureDb fixture(path); fixture.exec("DELETE FROM wallets"); }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { store.wallet(account.ref); });
    CHECK(hawken::test::read_bytes(path) == before);
    CHECK(store.get_or_create_account("1", 100) == account);
    CHECK(hawken::test::read_bytes(path) == before);
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 0);
}

TEST_CASE("Account insertion rolls back if its wallet cannot be inserted") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    LocalStore store(path, "server-a");
    { const FixtureDb fixture(path); fixture.exec(
        "PRAGMA foreign_keys=OFF; INSERT INTO wallets VALUES(1,0,0)"); }
    expect_error(StoreErrorCode::invalid_schema, [&] { store.get_or_create_account("1", 0); });
    CHECK_FALSE(store.find_account("1").has_value());
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("SELECT count(*) FROM accounts") == 0);
    CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 1);
}

TEST_CASE("Frozen schema one migrates atomically preserving IDs aliases visits roles and bans") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "migrate.db";
    const auto original = std::filesystem::path(HAWKEN_PROJECT_ROOT) /
        "tests" / "fixtures" / "local-store-v1.sqlite";
    REQUIRE(std::filesystem::copy_file(original, path));
    {
        LocalStore store(path, "server-a");
        CHECK(store.schema_version() == 5);
        const auto first = store.find_account("1");
        const auto second = store.find_account("2");
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->ref == AccountRef{"server-a", 17});
        CHECK(first->created_at == 10);
        CHECK(second->ref == AccountRef{"server-a", 23});
        CHECK(second->created_at == 12);
        CHECK(store.aliases(first->ref) == std::vector<Alias>{{"Fixture Pilot", 10, 20}});
        CHECK(store.visits(first->ref) == std::vector<Visit>{{"fixture-visit", first->ref, "Fixture Pilot", 20}});
        CHECK(store.role(first->ref) == std::optional<Role>(Role::analyst));
        CHECK(store.ban(second->ref) == std::optional<Ban>(Ban{20, 40, "Fixture rule"}));
        CHECK(store.wallet(first->ref) == WalletBalances{0, 0});
        CHECK(store.wallet(second->ref) == WalletBalances{0, 0});
        const FixtureDb fixture(path);
        CHECK(fixture.scalar("PRAGMA user_version") == 5);
        CHECK(fixture.scalar("PRAGMA application_id") == 0x4841574B);
        CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 2);
        CHECK(fixture.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0);
    }
    const auto before = hawken::test::read_bytes(path);
    LocalStore reopened(path, "server-a");
    CHECK(reopened.schema_version() == 5);
    CHECK(reopened.wallet({"server-a", 17}) == WalletBalances{0, 0});
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Invalid schema one is rejected before migration without changing database bytes") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "invalid-v1.db";
    REQUIRE(std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT) /
        "tests" / "fixtures" / "local-store-v1.sqlite", path));
    auto code = StoreErrorCode::invalid_schema;
    auto server = "server-a";
    SUBCASE("another server") { server = "server-b"; code = StoreErrorCode::invalid_scope; }
    SUBCASE("foreign application") {
        const FixtureDb fixture(path); fixture.exec("PRAGMA application_id=12345");
        code = StoreErrorCode::unsupported_schema;
    }
    SUBCASE("extra table") {
        const FixtureDb fixture(path); fixture.exec("CREATE TABLE wallets(account_id INTEGER)");
    }
    SUBCASE("missing table") {
        const FixtureDb fixture(path); fixture.exec("DROP TABLE bans");
    }
    SUBCASE("foreign key violation") {
        const FixtureDb fixture(path); fixture.exec(
            "PRAGMA foreign_keys=OFF; UPDATE aliases SET account_id=999");
    }
    const auto before = hawken::test::read_bytes(path);
    expect_error(code, [&] { LocalStore rejected(path, server); });
    CHECK(hawken::test::read_bytes(path) == before);
    const FixtureDb fixture(path);
    CHECK(fixture.scalar("PRAGMA user_version") == 1);
}

TEST_CASE("Migration cannot partially create wallets while another connection owns the write lock") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "locked-v1.db";
    REQUIRE(std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT) /
        "tests" / "fixtures" / "local-store-v1.sqlite", path));
    {
        const FixtureDb lock(path);
        lock.exec("BEGIN EXCLUSIVE");
        expect_error(StoreErrorCode::busy, [&] { LocalStore rejected(path, "server-a"); });
        CHECK(lock.scalar("PRAGMA user_version") == 1);
        CHECK(lock.scalar("SELECT count(*) FROM sqlite_schema WHERE name='wallets'") == 0);
        lock.exec("ROLLBACK");
    }
    LocalStore retry(path, "server-a");
    CHECK(retry.schema_version() == 5);
    CHECK(retry.wallet({"server-a", 23}) == WalletBalances{0, 0});
}

TEST_CASE("Settings merge preserves omitted batches and received decisions across reopen") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"settings.db";
    Account account;
    // Synthetic received decisions ONLY; never seeded in a real DB or proof of tutorial completion.
    const GameSettings first{{"UIFlags.FTUXCompletedFlag","4"},{"UIFlags.EULAVersionSeen","1"}};
    {
        LocalStore store(path,"server-a"); account=store.get_or_create_account("1",10);
        CHECK(store.game_settings(account.ref).empty());
        CHECK(store.merge_game_settings(account.ref,first));
        CHECK_FALSE(store.merge_game_settings(account.ref,first));
        CHECK_FALSE(store.merge_game_settings(account.ref,{}));
        CHECK(store.merge_game_settings(account.ref,{{"Assignments.Progress",R"({"done":[1,2]})"}}));
        CHECK(store.merge_game_settings(account.ref,{{"UIFlags.EULAVersionSeen","2"}}));
        CHECK(store.wallet(account.ref)==WalletBalances{}); CHECK_FALSE(store.starter_items(account.ref));
    }
    LocalStore reopened(path,"server-a");
    CHECK(reopened.game_settings(account.ref)==GameSettings{{"UIFlags.FTUXCompletedFlag","4"},
        {"UIFlags.EULAVersionSeen","2"},{"Assignments.Progress",R"({"done":[1,2]})"}});
    CHECK(reopened.visits(account.ref).empty()); CHECK(reopened.aliases(account.ref).empty());
}
TEST_CASE("Settings are isolated by account and server with no default insertion") {
    hawken::test::TempDirectory temp; LocalStore a(temp.path()/"a.db","a"),b(temp.path()/"b.db","b");
    const auto one=a.get_or_create_account("1",0),two=a.get_or_create_account("2",0),other=b.get_or_create_account("1",0);
    CHECK(a.merge_game_settings(one.ref,{{"key","private-value"}}));
    CHECK(a.game_settings(two.ref).empty()); CHECK(b.game_settings(other.ref).empty());
    expect_error(StoreErrorCode::invalid_scope,[&]{b.game_settings(one.ref);});
    expect_error(StoreErrorCode::invalid_scope,[&]{b.merge_game_settings(one.ref,{{"key","value"}});});
    expect_error(StoreErrorCode::not_found,[&]{a.game_settings({"a",999});});
    expect_error(StoreErrorCode::not_found,[&]{a.merge_game_settings({"a",999},{});});
    CHECK_FALSE(a.find_account("999"));
}
TEST_CASE("Settings validate UTF8 byte boundaries and preserve opaque string values") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto account=store.get_or_create_account("1",0);
    CHECK(store.merge_game_settings(account.ref,{{std::string(256,'k'),std::string(16384,'v')},
        {"empty",""},{"unicode",std::string("\xc3\xa9")},{"controls","line\n\tend"}}));
    CHECK(store.game_settings(account.ref).at("controls")=="line\n\tend");
    const auto before=store.game_settings(account.ref);
    for (const GameSettings& update:std::vector<GameSettings>{
        {{"","bad"}},{{std::string(257,'k'),"bad"}},{{"value",std::string(16385,'v')}},
        {{std::string("k\0x",3),"bad"}},{{"nul",std::string("v\0x",3)}},
        {{"\nkey","bad"}},{{std::string("\xc2\x85"),"bad"}},
        {{"invalid",std::string("\xc0\xaf")}},{{std::string("\xed\xa0\x80"),"bad"}}}) {
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_game_settings(account.ref,update);});
        CHECK(store.game_settings(account.ref)==before);
    }
    GameSettings too_many; for(int i=0;i<257;++i) too_many.emplace("key"+std::to_string(i),"v");
    expect_error(StoreErrorCode::invalid_input,[&]{store.merge_game_settings(account.ref,too_many);});
    CHECK(store.game_settings(account.ref)==before);
}
TEST_CASE("Settings capacity rejection rolls back the whole merged batch") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto account=store.get_or_create_account("1",0);
    SUBCASE("1024 keys maximum") {
        for(int batch=0;batch<4;++batch) {
            GameSettings update; for(int i=0;i<256;++i) update.emplace("k"+std::to_string(batch*256+i),"");
            CHECK(store.merge_game_settings(account.ref,update));
        }
        REQUIRE(store.game_settings(account.ref).size()==1024);
        const auto before=store.game_settings(account.ref);
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_game_settings(account.ref,{{"k0","changed"},{"extra","v"}});});
        CHECK(store.game_settings(account.ref)==before);
    }
    SUBCASE("262144 total key plus value bytes") {
        GameSettings update; for(int i=0;i<16;++i) update.emplace("k"+std::to_string(i),std::string(16380,'v'));
        CHECK(store.merge_game_settings(account.ref,update)); const auto before=store.game_settings(account.ref);
        expect_error(StoreErrorCode::invalid_input,[&]{store.merge_game_settings(account.ref,{{"k0",std::string(16380,'q')},{"extra",std::string(1024,'v')}});});
        CHECK(store.game_settings(account.ref)==before);
    }
}
TEST_CASE("Concurrent settings batches preserve both keys and SQL strings stay literal") {
    hawken::test::TempDirectory temp; LocalStore store(temp.path()/"a.db","server-a");
    const auto account=store.get_or_create_account("1",0);
    std::exception_ptr error_a,error_b;
    std::thread a([&]{try {store.merge_game_settings(account.ref,{{"a", "');DROP TABLE accounts;--"}});} catch(...) {error_a=std::current_exception();}});
    std::thread b([&]{try {store.merge_game_settings(account.ref,{{"b","second"}});} catch(...) {error_b=std::current_exception();}});
    a.join(); b.join(); CHECK_FALSE(error_a); CHECK_FALSE(error_b);
    CHECK(store.game_settings(account.ref)==GameSettings{{"a","');DROP TABLE accounts;--"},{"b","second"}});
    CHECK(store.find_account("1")==std::optional<Account>(account));
}

namespace {
void frozen_v3(const std::filesystem::path& path) {
    if (!std::filesystem::copy_file(std::filesystem::path(HAWKEN_PROJECT_ROOT)/"tests/fixtures/local-store-v2.sqlite",path))
        throw std::runtime_error("Could not copy historical fixture.");
    const FixtureDb fixture(path);
    fixture.exec(R"(CREATE TABLE starter_grants (
    account_id INTEGER PRIMARY KEY NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT
) STRICT)");
    fixture.exec(R"(CREATE TABLE starter_items (
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
    fixture.exec("INSERT INTO starter_grants VALUES(17)");
    for(int slot=0;slot<9;++slot) {
        const auto suffix=std::to_string(slot+1);
        const auto sql="INSERT INTO starter_items VALUES(17,"+std::to_string(slot)+
            ",'10000000-0000-0000-0000-00000000000"+suffix+
            "','20000000-0000-0000-0000-00000000000"+suffix+"')";
        fixture.exec(sql.c_str());
    }
    fixture.exec("PRAGMA user_version=3");
}
}
TEST_CASE("Frozen schema three migrates to five without rewriting balances accounts or nine owned rows") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"v3.db"; frozen_v3(path);
    LocalStore store(path,"server-a"); CHECK(store.schema_version()==5);
    const auto account=store.find_account("1"); REQUIRE(account); CHECK(account->ref.local_id==17);
    CHECK(account->created_at==10); CHECK(store.wallet(account->ref)==WalletBalances{200,0});
    REQUIRE(store.starter_items(account->ref)); CHECK(store.starter_items(account->ref)->size()==9);
    CHECK(store.starter_items(account->ref)->at(0)==StarterItem{"10000000-0000-0000-0000-000000000001","20000000-0000-0000-0000-000000000001"});
    CHECK(store.game_settings(account->ref).empty());
    const FixtureDb fixture(path); CHECK(fixture.scalar("PRAGMA user_version")==5);
    CHECK(fixture.scalar("SELECT count(*) FROM starter_items")==9);
    CHECK(fixture.scalar("SELECT count(*) FROM game_settings")==0);
    CHECK(fixture.scalar("SELECT count(*) FROM pragma_foreign_key_check")==0);
}
TEST_CASE("Schema three rejection and busy migration preserve the original DB and have no partial settings table") {
    hawken::test::TempDirectory temp; const auto path=temp.path()/"v3.db"; frozen_v3(path);
    SUBCASE("busy") {
        const FixtureDb lock(path); lock.exec("BEGIN EXCLUSIVE");
        expect_error(StoreErrorCode::busy,[&]{LocalStore store(path,"server-a");});
        CHECK(lock.scalar("PRAGMA user_version")==3);
        CHECK(lock.scalar("SELECT count(*) FROM sqlite_schema WHERE name='game_settings'")==0);
        lock.exec("ROLLBACK");
    }
    SUBCASE("unexpected trigger not executed or repaired") {
        { const FixtureDb db(path); db.exec("CREATE TRIGGER unexpected AFTER INSERT ON accounts BEGIN DELETE FROM wallets; END"); }
        const auto before=hawken::test::read_bytes(path);
        expect_error(StoreErrorCode::invalid_schema,[&]{LocalStore store(path,"server-a");});
        CHECK(hawken::test::read_bytes(path)==before);
    }
}

TEST_CASE("Account achievement reader returns no awards without changing real SQLite state") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "achievements.db";
    Account account;
    {
        LocalStore store(path, "server-a");
        account = store.get_or_create_account("1", 10);
        REQUIRE(store.merge_game_settings(account.ref, {{"Fixture", "unchanged"}}));
        const FixtureDb fixture(path);
        const auto version = fixture.scalar("PRAGMA data_version");
        const auto schema_objects = fixture.scalar("SELECT count(*) FROM sqlite_schema");
        const auto before = hawken::test::read_bytes(path);
        CHECK(store.account_achievement_ids(account.ref).empty());
        CHECK(store.account_achievement_ids(account.ref).empty());
        CHECK(hawken::test::read_bytes(path) == before);
        CHECK(fixture.scalar("PRAGMA data_version") == version);
        CHECK(fixture.scalar("SELECT count(*) FROM sqlite_schema") == schema_objects);
        CHECK(fixture.scalar("SELECT count(*) FROM accounts") == 1);
        CHECK(fixture.scalar("SELECT count(*) FROM wallets") == 1);
        CHECK(fixture.scalar("SELECT count(*) FROM starter_grants") == 0);
        CHECK(fixture.scalar("SELECT count(*) FROM starter_items") == 0);
        CHECK(store.wallet(account.ref) == WalletBalances{0, 0});
        CHECK(store.game_settings(account.ref) == GameSettings{{"Fixture", "unchanged"}});
        CHECK_FALSE(store.starter_items(account.ref).has_value());
    }
    LocalStore reopened(path, "server-a");
    const auto before = hawken::test::read_bytes(path);
    CHECK(reopened.account_achievement_ids(account.ref).empty());
    CHECK(reopened.find_account("1") == std::optional<Account>(account));
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Account achievement reader rejects foreign missing and malformed logical references") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "a.db";
    LocalStore store(path, "server-a");
    LocalStore foreign(temp.path() / "b.db", "server-b");
    const auto account = store.get_or_create_account("1", 0);
    const auto other = foreign.get_or_create_account("1", 0);
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_scope, [&] { store.account_achievement_ids(other.ref); });
    expect_error(StoreErrorCode::not_found, [&] { store.account_achievement_ids({"server-a", 999}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.account_achievement_ids({"", account.ref.local_id}); });
    expect_error(StoreErrorCode::invalid_input, [&] { store.account_achievement_ids({"server-a", 0}); });
    CHECK(hawken::test::read_bytes(path) == before);
    CHECK(store.account_achievement_ids(account.ref).empty());
    CHECK(foreign.account_achievement_ids(other.ref).empty());
}

TEST_CASE("Account achievement reader fails closed on schema or account corruption without repair") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "corrupt.db";
    LocalStore store(path, "server-a");
    const auto account = store.get_or_create_account("1", 0);
    SUBCASE("schema changed after opening") {
        const FixtureDb fixture(path);
        fixture.exec("CREATE TABLE unexpected_achievement_data(value TEXT)");
    }
    SUBCASE("malformed stored account identity") {
        const FixtureDb fixture(path);
        fixture.exec("PRAGMA ignore_check_constraints=ON; UPDATE accounts SET steam_id64='broken'");
    }
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_schema, [&] { store.account_achievement_ids(account.ref); });
    CHECK(hawken::test::read_bytes(path) == before);
}

TEST_CASE("Account achievement reader rejects a moved-from store without inventing an empty result") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "moved.db";
    LocalStore original(path, "server-a");
    const auto account = original.get_or_create_account("1", 0);
    LocalStore moved(std::move(original));
    const auto before = hawken::test::read_bytes(path);
    expect_error(StoreErrorCode::invalid_input, [&] { original.account_achievement_ids(account.ref); });
    CHECK(moved.account_achievement_ids(account.ref).empty());
    CHECK(hawken::test::read_bytes(path) == before);
}
