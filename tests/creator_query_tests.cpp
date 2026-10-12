#include "hawken/creator_query.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <array>
#include <limits>

namespace {
using namespace hawken::creator;

class QueryFixture {
public:
    QueryFixture() : path(temp.path() / "local.sqlite") {
        sqlite3* database = nullptr;
        const auto filename = path.string();
        const auto opened = sqlite3_open_v2(filename.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE, nullptr);
        if (opened != SQLITE_OK) {
            sqlite3_close(database);
            throw std::runtime_error("Cannot create isolated query fixture");
        }
        const auto created = sqlite3_exec(database,
            "CREATE TABLE accounts(account_id INTEGER PRIMARY KEY, display_name TEXT,"
            " steam_id TEXT, secret TEXT);"
            "INSERT INTO accounts VALUES(1,'Ada','76561198000000001','never-echo-this'),"
            "(2,'Boris','76561198000000002','private-two'),"
            "(3,'Caf\xc3\xa9','76561198000000003','private-three');"
            "CREATE TABLE visits(account_id INTEGER, occurred_at INTEGER);"
            "INSERT INTO visits VALUES(1,100),(1,101),(2,102);"
            "CREATE TABLE bans(account_id INTEGER, reason TEXT);"
            "INSERT INTO bans VALUES(2,'local-rule');"
            "CREATE TABLE roles(account_id INTEGER, role TEXT);"
            "INSERT INTO roles VALUES(1,'owner');"
            "CREATE VIEW creator_accounts AS SELECT account_id,display_name FROM accounts;"
            "CREATE VIEW creator_visits AS SELECT account_id,occurred_at FROM visits;"
            "CREATE VIEW creator_bans AS SELECT account_id,reason FROM bans;"
            "CREATE VIEW creator_roles AS SELECT account_id,role FROM roles;"
            "CREATE TABLE numbers(n INTEGER);"
            "WITH RECURSIVE x(n) AS(SELECT 0 UNION ALL SELECT n+1 FROM x WHERE n<199)"
            " INSERT INTO numbers SELECT n FROM x;", nullptr, nullptr, nullptr);
        const auto closed = sqlite3_close(database);
        if (created != SQLITE_OK || closed != SQLITE_OK)
            throw std::runtime_error("Cannot initialize isolated query fixture");
        original = hawken::test::read_bytes(path);
    }

    void unchanged() const { CHECK(hawken::test::read_bytes(path) == original); }

    hawken::test::TempDirectory temp;
    std::filesystem::path path;
    std::string original;
};

QueryPolicy public_policy() {
    QueryPolicy policy;
    policy.columns = {{"accounts", "account_id"}, {"accounts", "display_name"},
        {"creator_accounts", "account_id"}, {"creator_accounts", "display_name"},
        {"visits", "account_id"}, {"visits", "occurred_at"},
        {"creator_visits", "account_id"}, {"creator_visits", "occurred_at"},
        {"bans", "account_id"}, {"bans", "reason"},
        {"creator_bans", "account_id"}, {"creator_bans", "reason"}};
    return policy;
}

void reject(const std::filesystem::path& path, const QueryPolicy& policy,
            std::string_view sql, QueryErrorCode expected,
            std::span<const QueryBind> binds = {}) {
    try {
        const auto result = read_creator_query(path, policy, sql, binds);
        static_cast<void>(result);
    } catch (const QueryError& error) {
        CHECK(error.code == expected);
        CHECK(std::string(error.what()) == "Creator query could not be executed.");
        CHECK(std::string(error.what()).find(path.string()) == std::string::npos);
        CHECK(std::string(error.what()).find("never-echo-this") == std::string::npos);
        return;
    } catch (const std::exception&) {
        FAIL_CHECK("Creator query must report a sanitized QueryError");
        return;
    }
    FAIL_CHECK("Creator query accepted a forbidden or over-budget input");
}
}

TEST_CASE("Creator query reads allowed view columns with typed cells") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, public_policy(),
        "SELECT account_id,display_name FROM creator_accounts ORDER BY account_id");
    REQUIRE((result.columns == std::vector<std::string>{"account_id", "display_name"}));
    REQUIRE(result.rows.size() == 3);
    REQUIRE(result.rows[0].size() == 2);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == 1);
    CHECK(std::get<std::string>(result.rows[0][1]) == "Ada");
    CHECK(std::get<std::string>(result.rows[2][1]) == "Caf\xc3\xa9");
    fixture.unchanged();
}

TEST_CASE("Creator query permits direct allowed columns and nonrecursive CTEs") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, public_policy(),
        "WITH names AS(SELECT account_id,display_name FROM accounts)"
        " SELECT display_name FROM names WHERE account_id=2");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == "Boris");
    fixture.unchanged();
}

TEST_CASE("Creator query requires both base and view grants") {
    QueryFixture fixture;
    QueryPolicy view_only;
    view_only.columns = {{"creator_accounts", "display_name"}};
    reject(fixture.path, view_only, "SELECT display_name FROM creator_accounts",
        QueryErrorCode::unauthorized);
    QueryPolicy base_only;
    base_only.columns = {{"accounts", "display_name"}};
    reject(fixture.path, base_only, "SELECT display_name FROM creator_accounts",
        QueryErrorCode::unauthorized);
    fixture.unchanged();
}

TEST_CASE("Creator query visits bans and roles follow explicit column grants") {
    QueryFixture fixture;
    const auto policy = public_policy();
    const auto visits = read_creator_query(fixture.path, policy,
        "SELECT occurred_at FROM creator_visits WHERE account_id=1 ORDER BY occurred_at");
    REQUIRE(visits.rows.size() == 2);
    CHECK(std::get<std::int64_t>(visits.rows[1][0]) == 101);
    const auto bans = read_creator_query(fixture.path, policy,
        "SELECT reason FROM creator_bans WHERE account_id=2");
    REQUIRE(bans.rows.size() == 1);
    CHECK(std::get<std::string>(bans.rows[0][0]) == "local-rule");
    reject(fixture.path, policy, "SELECT role FROM creator_roles", QueryErrorCode::unauthorized);
    auto authorized = policy;
    authorized.columns.push_back({"roles", "account_id"});
    authorized.columns.push_back({"roles", "role"});
    authorized.columns.push_back({"creator_roles", "account_id"});
    authorized.columns.push_back({"creator_roles", "role"});
    const auto roles = read_creator_query(fixture.path, authorized, "SELECT role FROM creator_roles");
    REQUIRE(roles.rows.size() == 1);
    CHECK(std::get<std::string>(roles.rows[0][0]) == "owner");
    fixture.unchanged();
}

TEST_CASE("Creator query denies private columns in projections and expressions") {
    QueryFixture fixture;
    const auto policy = public_policy();
    for (const auto sql : {
        "SELECT steam_id FROM accounts",
        "SELECT display_name FROM accounts WHERE secret='never-echo-this'",
        "SELECT display_name FROM accounts ORDER BY steam_id",
        "SELECT count(display_name) FROM accounts GROUP BY secret",
        "SELECT display_name FROM accounts WHERE account_id IN(SELECT account_id FROM roles)",
        "SELECT (SELECT secret FROM accounts LIMIT 1)",
        "SELECT max(steam_id) FROM accounts",
        "SELECT secret AS display_name FROM accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query CTE and view aliases never grant access by source context") {
    QueryFixture fixture;
    const auto policy = public_policy();
    for (const auto sql : {
        "WITH creator_accounts AS(SELECT steam_id AS display_name FROM accounts)"
            " SELECT display_name FROM creator_accounts",
        "WITH creator_bans AS(SELECT secret AS reason FROM accounts) SELECT reason FROM creator_bans",
        "WITH allowed AS(SELECT role AS display_name FROM roles) SELECT display_name FROM allowed",
        "SELECT creator_accounts.secret FROM accounts AS creator_accounts",
        "SELECT q.display_name FROM (SELECT secret AS display_name FROM accounts) AS q"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query count star requires an explicit empty-column grant") {
    QueryFixture fixture;
    auto policy = public_policy();
    reject(fixture.path, policy, "SELECT count(*) AS total FROM accounts", QueryErrorCode::unauthorized);
    policy.columns.push_back({"accounts", ""});
    const auto result = read_creator_query(fixture.path, policy, "SELECT count(*) AS total FROM accounts");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == 3);
    fixture.unchanged();
}

TEST_CASE("Creator query bare counts need a row grant for qualified and unqualified tables") {
    QueryFixture fixture;
    auto policy = public_policy();
    for (const auto* sql : {"SELECT count(*) FROM accounts", "SELECT count(*) FROM main.accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    policy.columns.push_back({"accounts", ""});
    for (const auto* sql : {"SELECT count(*) FROM accounts", "SELECT count(*) FROM main.accounts"}) {
        const auto result = read_creator_query(fixture.path, policy, sql);
        REQUIRE(result.rows.size() == 1);
        CHECK(std::get<std::int64_t>(result.rows[0][0]) == 3);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query authorized integer primary keys need no additional row grant") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.columns = {{"accounts", "account_id"}};
    for (const auto* sql : {"SELECT account_id FROM accounts ORDER BY account_id",
        "SELECT account_id FROM main.accounts ORDER BY account_id"}) {
        const auto result = read_creator_query(fixture.path, policy, sql);
        REQUIRE(result.rows.size() == 3);
        CHECK(std::get<std::int64_t>(result.rows[0][0]) == 1);
        CHECK(std::get<std::int64_t>(result.rows[2][0]) == 3);
    }
    const auto aggregate = read_creator_query(fixture.path, policy,
        "SELECT count(account_id),sum(account_id) FROM accounts");
    REQUIRE(aggregate.rows.size() == 1);
    CHECK(std::get<std::int64_t>(aggregate.rows[0][0]) == 3);
    CHECK(std::get<std::int64_t>(aggregate.rows[0][1]) == 6);
    fixture.unchanged();
}

TEST_CASE("Creator query counts with permitted predicates follow column access grants") {
    QueryFixture fixture;
    const auto policy = public_policy();
    const auto keys = read_creator_query(fixture.path, policy,
        "SELECT count(*) FROM accounts WHERE account_id<3");
    REQUIRE(keys.rows.size() == 1);
    CHECK(std::get<std::int64_t>(keys.rows[0][0]) == 2);
    const auto names = read_creator_query(fixture.path, policy,
        "SELECT count(*) FROM accounts WHERE display_name='Ada'");
    REQUIRE(names.rows.size() == 1);
    CHECK(std::get<std::int64_t>(names.rows[0][0]) == 1);
    reject(fixture.path, policy,
        "SELECT count(*) FROM accounts WHERE account_id>0 AND secret='never-echo-this'",
        QueryErrorCode::unauthorized);
    fixture.unchanged();
}

TEST_CASE("Creator query observed columns do not grant another table or an unused grant") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.columns = {{"accounts", "account_id"}, {"numbers", "n"}};
    for (const auto* sql : {
        "SELECT accounts.account_id FROM accounts CROSS JOIN numbers LIMIT 1",
        "SELECT a.account_id FROM main.accounts a CROSS JOIN main.numbers n LIMIT 1",
        "SELECT account_id,(SELECT count(*) FROM numbers) FROM accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    policy.columns.push_back({"accounts", ""});
    reject(fixture.path, policy,
        "SELECT account_id,(SELECT count(*) FROM numbers) FROM accounts",
        QueryErrorCode::unauthorized);
    fixture.unchanged();
}

TEST_CASE("Creator query observed primary key never authorizes a private column or CTE source") {
    QueryFixture fixture;
    auto policy = public_policy();
    policy.columns.push_back({"accounts", ""});
    policy.columns.push_back({"numbers", "n"});
    for (const auto* sql : {
        "SELECT account_id,secret AS display_name FROM accounts AS creator_accounts",
        "WITH creator_accounts AS(SELECT account_id,secret AS display_name FROM accounts)"
            " SELECT account_id,display_name FROM creator_accounts",
        "WITH accounts AS(SELECT 1 FROM numbers) SELECT count(*) FROM accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query observed column state never persists to a later query") {
    QueryFixture fixture;
    const auto policy = public_policy();
    const auto result = read_creator_query(fixture.path, policy, "SELECT account_id FROM accounts");
    REQUIRE(result.rows.size() == 3);
    reject(fixture.path, policy, "SELECT count(*) FROM accounts", QueryErrorCode::unauthorized);
    fixture.unchanged();
}

TEST_CASE("Creator query row grants cannot admit sqlite tables after a permitted read") {
    QueryFixture fixture;
    auto policy = public_policy();
    policy.columns.push_back({"sqlite_schema", ""});
    reject(fixture.path, policy,
        "SELECT account_id,(SELECT count(*) FROM sqlite_schema) FROM accounts",
        QueryErrorCode::unauthorized);
    fixture.unchanged();
}

TEST_CASE("Creator query does not permit schema tables even with erroneous grants") {
    QueryFixture fixture;
    auto policy = public_policy();
    policy.columns.push_back({"sqlite_schema", "name"});
    policy.columns.push_back({"SQLITE_MASTER", "sql"});
    for (const auto sql : {"SELECT name FROM sqlite_schema", "SELECT sql FROM sqlite_master",
        "WITH creator_accounts AS(SELECT name AS display_name FROM sqlite_schema)"
        " SELECT display_name FROM creator_accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query denies mutations DDL attachment pragmas and transactions") {
    QueryFixture fixture;
    const auto policy = public_policy();
    for (const auto sql : {
        "UPDATE accounts SET display_name='never-echo-this' WHERE account_id=1",
        "DELETE FROM accounts", "INSERT INTO accounts VALUES(4,'x','x','x')",
        "CREATE TABLE stolen(x)", "DROP VIEW creator_accounts",
        "CREATE TEMP TABLE stolen(x)", "ALTER TABLE accounts ADD COLUMN extra TEXT",
        "ATTACH DATABASE ':memory:' AS other", "DETACH DATABASE main",
        "PRAGMA table_info(accounts)", "PRAGMA user_version=99",
        "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT injected", "VACUUM"}) {
        CAPTURE(sql);
        reject(fixture.path, policy, sql, QueryErrorCode::unauthorized);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query denies recursive execution and unlisted functions") {
    QueryFixture fixture;
    const auto policy = public_policy();
    reject(fixture.path, policy,
        "WITH RECURSIVE r(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM r WHERE n<5) SELECT n FROM r",
        QueryErrorCode::unauthorized);
    reject(fixture.path, policy, "SELECT random()", QueryErrorCode::unauthorized);
    reject(fixture.path, policy, "SELECT group_concat(display_name) FROM accounts",
        QueryErrorCode::unauthorized);
    // The build omits this function; invalid SQL is distinct from authorizer denial.
    reject(fixture.path, policy, "SELECT load_extension('never-echo-this')", QueryErrorCode::invalid_sql);
    reject(fixture.path, policy, "SELECT name FROM pragma_table_info('accounts')",
        QueryErrorCode::invalid_sql);
    fixture.unchanged();
}

TEST_CASE("Creator query accepts one statement with comments and string semicolons") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, {},
        "SELECT ';never-echo-this;' AS value; -- SELECT denied;\n /* trailing ; */ \t");
    REQUIRE(result.columns == std::vector<std::string>{"value"});
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == ";never-echo-this;");
    fixture.unchanged();
}

TEST_CASE("Creator query ignores empty tail statements according to the SQLite parser") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, {}, "SELECT 7 AS n; ; -- empty ;\n /* gap */ ; ;");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == 7);
    fixture.unchanged();
}

TEST_CASE("Creator query rejects second statements and non-comment tails") {
    QueryFixture fixture;
    for (const auto sql : {"SELECT 1; SELECT 2", "SELECT 1; /* gap */ SELECT 2;",
        "SELECT 1; never-echo-this"}) {
        CAPTURE(sql);
        reject(fixture.path, {}, sql, QueryErrorCode::invalid_sql);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query rejects empty embedded NUL and EXPLAIN SQL") {
    QueryFixture fixture;
    for (const auto sql : {"", " \n -- no statement\n /* comments */", ";",
        "EXPLAIN SELECT 1", "EXPLAIN QUERY PLAN SELECT account_id FROM accounts"}) {
        CAPTURE(sql);
        reject(fixture.path, public_policy(), sql, QueryErrorCode::invalid_sql);
    }
    std::string nul = "SELECT 1";
    nul.push_back('\0');
    nul += "; SELECT secret FROM accounts";
    reject(fixture.path, public_policy(), nul, QueryErrorCode::invalid_sql);
    fixture.unchanged();
}

TEST_CASE("Creator query binds null integer extremes and text with explicit lengths") {
    QueryFixture fixture;
    std::string embedded = "x";
    embedded.push_back('\0');
    embedded += "y";
    const std::array<QueryBind, 4> binds{std::monostate{},
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), embedded};
    const auto result = read_creator_query(fixture.path, {}, "SELECT ? AS n,? AS lo,? AS hi,? AS t", binds);
    REQUIRE(result.rows.size() == 1);
    REQUIRE(result.rows[0].size() == 4);
    CHECK(std::holds_alternative<std::monostate>(result.rows[0][0]));
    CHECK(std::get<std::int64_t>(result.rows[0][1]) == std::numeric_limits<std::int64_t>::min());
    CHECK(std::get<std::int64_t>(result.rows[0][2]) == std::numeric_limits<std::int64_t>::max());
    CHECK(std::get<std::string>(result.rows[0][3]) == embedded);
    fixture.unchanged();
}

TEST_CASE("Creator query bind text cannot alter SQL structure") {
    QueryFixture fixture;
    const std::array<QueryBind, 1> binds{std::string("Ada' OR 1=1; DROP TABLE accounts;--")};
    const auto result = read_creator_query(fixture.path, public_policy(),
        "SELECT account_id FROM accounts WHERE display_name=?", binds);
    CHECK(result.rows.empty());
    fixture.unchanged();
}

TEST_CASE("Creator query requires exact anonymous parameter counts") {
    QueryFixture fixture;
    const std::array<QueryBind, 1> one{std::int64_t{7}};
    const std::array<QueryBind, 2> two{std::int64_t{7}, std::int64_t{8}};
    reject(fixture.path, {}, "SELECT ?", QueryErrorCode::bind_mismatch);
    reject(fixture.path, {}, "SELECT 1", QueryErrorCode::bind_mismatch, one);
    reject(fixture.path, {}, "SELECT ?,?", QueryErrorCode::bind_mismatch, one);
    reject(fixture.path, {}, "SELECT ?", QueryErrorCode::bind_mismatch, two);
    for (const auto sql : {"SELECT ?1", "SELECT ?2", "SELECT :value", "SELECT @value", "SELECT $value"}) {
        CAPTURE(sql);
        reject(fixture.path, {}, sql, QueryErrorCode::invalid_sql, one);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query parameter-looking text inside a literal is not a bind") {
    QueryFixture fixture;
    const std::array<QueryBind, 1> binds{std::int64_t{9}};
    const auto result = read_creator_query(fixture.path, {}, "SELECT '?;?2' AS text,? AS n", binds);
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == "?;?2");
    CHECK(std::get<std::int64_t>(result.rows[0][1]) == 9);
    fixture.unchanged();
}

TEST_CASE("Creator query caps total bound text without truncation") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.limits.max_sql_bytes = 64;
    const std::array<QueryBind, 1> exact{std::string(64, 'x')};
    const auto result = read_creator_query(fixture.path, policy, "SELECT ? AS x", exact);
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == std::string(64, 'x'));
    const std::array<QueryBind, 1> excess{std::string(65, 'x')};
    reject(fixture.path, policy, "SELECT ? AS x", QueryErrorCode::bind_too_large, excess);
    const std::array<QueryBind, 2> combined{std::string(32, 'x'), std::string(33, 'y')};
    reject(fixture.path, policy, "SELECT ?,?", QueryErrorCode::bind_too_large, combined);
    fixture.unchanged();
}

TEST_CASE("Creator query preserves integer real null and embedded-NUL result types") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, {},
        "SELECT 7 AS i,1.5 AS r,NULL AS n,CAST(X'780079' AS TEXT) AS t");
    REQUIRE(result.rows.size() == 1);
    REQUIRE(result.rows[0].size() == 4);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == 7);
    CHECK(std::get<double>(result.rows[0][1]) == 1.5);
    CHECK(std::holds_alternative<std::monostate>(result.rows[0][2]));
    CHECK(std::get<std::string>(result.rows[0][3]) == std::string("x\0y", 3));
    reject(fixture.path, {}, "SELECT X'AB'", QueryErrorCode::execution_failed);
    fixture.unchanged();
}

TEST_CASE("Creator query permits a small explicit builtin whitelist") {
    QueryFixture fixture;
    const auto result = read_creator_query(fixture.path, public_policy(),
        "SELECT lower(display_name),upper(display_name),length(display_name),"
        "coalesce(NULL,display_name),abs(-3) FROM accounts WHERE account_id=1");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == "ada");
    CHECK(std::get<std::string>(result.rows[0][1]) == "ADA");
    CHECK(std::get<std::int64_t>(result.rows[0][2]) == 3);
    CHECK(std::get<std::string>(result.rows[0][3]) == "Ada");
    CHECK(std::get<std::int64_t>(result.rows[0][4]) == 3);
    const auto aggregates = read_creator_query(fixture.path, public_policy(),
        "SELECT count(account_id),sum(account_id),min(account_id),max(account_id),avg(account_id) FROM accounts");
    REQUIRE(aggregates.rows.size() == 1);
    CHECK(std::get<std::int64_t>(aggregates.rows[0][0]) == 3);
    CHECK(std::get<std::int64_t>(aggregates.rows[0][1]) == 6);
    CHECK(std::get<std::int64_t>(aggregates.rows[0][2]) == 1);
    CHECK(std::get<std::int64_t>(aggregates.rows[0][3]) == 3);
    CHECK(std::get<double>(aggregates.rows[0][4]) == 2.0);
    fixture.unchanged();
}

TEST_CASE("Creator query enforces exact row limit and rejects the next row") {
    QueryFixture fixture;
    auto policy = public_policy();
    policy.limits.max_rows = 3;
    const auto result = read_creator_query(fixture.path, policy,
        "SELECT account_id FROM accounts ORDER BY account_id");
    CHECK(result.rows.size() == 3);
    policy.limits.max_rows = 2;
    reject(fixture.path, policy, "SELECT account_id FROM accounts ORDER BY account_id",
        QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query counts metadata text bytes numbers and nulls at exact boundary") {
    QueryFixture fixture;
    QueryPolicy policy;
    // Column names: t,i,n = 3 bytes; text UTF-8 e-acute = 2; integer = 8; null = 0.
    policy.limits.max_result_bytes = 13;
    const auto result = read_creator_query(fixture.path, policy, "SELECT '\xc3\xa9' AS t,7 AS i,NULL AS n");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == "\xc3\xa9");
    policy.limits.max_result_bytes = 12;
    reject(fixture.path, policy, "SELECT '\xc3\xa9' AS t,7 AS i,NULL AS n", QueryErrorCode::resource_limit);
    policy.limits.max_result_bytes = 9;
    const auto real = read_creator_query(fixture.path, policy, "SELECT 1.5 AS r");
    REQUIRE(real.rows.size() == 1);
    CHECK(std::get<double>(real.rows[0][0]) == 1.5);
    policy.limits.max_result_bytes = 8;
    reject(fixture.path, policy, "SELECT 1.5 AS r", QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query charges column metadata even when no rows are returned") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.limits.max_result_bytes = 5;
    const auto result = read_creator_query(fixture.path, policy, "SELECT 1 AS abcde WHERE 0");
    CHECK(result.rows.empty());
    CHECK(result.columns == std::vector<std::string>{"abcde"});
    policy.limits.max_result_bytes = 4;
    reject(fixture.path, policy, "SELECT 1 AS abcde WHERE 0", QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query embedded NUL text consumes its full result byte budget") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.limits.max_result_bytes = 4;
    const auto result = read_creator_query(fixture.path, policy, "SELECT CAST(X'780079' AS TEXT) AS t");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == std::string("x\0y", 3));
    policy.limits.max_result_bytes = 3;
    reject(fixture.path, policy, "SELECT CAST(X'780079' AS TEXT) AS t", QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query SQL limit is measured in original bytes") {
    QueryFixture fixture;
    QueryPolicy policy;
    const std::string sql = "SELECT '\xc3\xa9' AS t";
    policy.limits.max_sql_bytes = sql.size();
    const auto result = read_creator_query(fixture.path, policy, sql);
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == "\xc3\xa9");
    --policy.limits.max_sql_bytes;
    reject(fixture.path, policy, sql, QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query interrupts Cartesian work with a cooperative progress budget") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.columns = {{"numbers", "n"}, {"numbers", ""}};
    policy.limits.progress_callbacks = 1;
    policy.limits.timeout = std::chrono::milliseconds{5000};
    reject(fixture.path, policy,
        "SELECT count(a.n) FROM numbers a,numbers b,numbers c,numbers d",
        QueryErrorCode::resource_limit);
    fixture.unchanged();
}

TEST_CASE("Creator query distinguishes a cooperative clock deadline from other quotas") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.columns = {{"numbers", "n"}, {"numbers", ""}};
    policy.limits.progress_callbacks = 100000;
    policy.limits.timeout = std::chrono::milliseconds{1};
    // 200^4 candidate rows; no assertion about hard wall time or blocked I/O.
    reject(fixture.path, policy,
        "SELECT count(a.n) FROM numbers a,numbers b,numbers c,numbers d",
        QueryErrorCode::deadline_exceeded);
    fixture.unchanged();
}

TEST_CASE("Creator query rejects invalid or above-ceiling limits") {
    QueryFixture fixture;
    QueryPolicy policy;
    for (const auto limit : {std::size_t{0}, std::size_t{1048577},
        std::numeric_limits<std::size_t>::max()}) {
        auto altered = policy;
        altered.limits.max_sql_bytes = limit;
        reject(fixture.path, altered, "SELECT 1", QueryErrorCode::invalid_limits);
    }
    for (const auto limit : {std::size_t{0}, std::size_t{10001}}) {
        auto altered = policy;
        altered.limits.max_rows = limit;
        reject(fixture.path, altered, "SELECT 1", QueryErrorCode::invalid_limits);
    }
    for (const auto limit : {std::size_t{0}, std::size_t{8388609}}) {
        auto altered = policy;
        altered.limits.max_result_bytes = limit;
        reject(fixture.path, altered, "SELECT 1", QueryErrorCode::invalid_limits);
    }
    for (const auto callbacks : {std::uint32_t{0}, std::uint32_t{100001}}) {
        auto altered = policy;
        altered.limits.progress_callbacks = callbacks;
        reject(fixture.path, altered, "SELECT 1", QueryErrorCode::invalid_limits);
    }
    for (const auto duration : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{5001}}) {
        auto altered = policy;
        altered.limits.timeout = std::chrono::milliseconds{duration};
        reject(fixture.path, altered, "SELECT 1", QueryErrorCode::invalid_limits);
    }
    fixture.unchanged();
}

TEST_CASE("Creator query validates identifier names and policy grant count") {
    QueryFixture fixture;
    for (const auto& table : {std::string{}, std::string("accounts;--"), std::string("1accounts"),
        std::string(129, 'a'), std::string("accounts\0hidden", 15), std::string("caf\xc3\xa9")}) {
        QueryPolicy policy;
        policy.columns = {{table, "display_name"}};
        reject(fixture.path, policy, "SELECT 1", QueryErrorCode::invalid_policy);
    }
    for (const auto& column : {std::string("secret AS display_name"), std::string(129, 'a'),
        std::string("x\0y", 3)}) {
        QueryPolicy policy;
        policy.columns = {{"accounts", column}};
        reject(fixture.path, policy, "SELECT 1", QueryErrorCode::invalid_policy);
    }
    QueryPolicy too_many;
    for (int i = 0; i < 257; ++i) too_many.columns.push_back({"accounts", "c" + std::to_string(i)});
    reject(fixture.path, too_many, "SELECT 1", QueryErrorCode::invalid_policy);
    fixture.unchanged();
}

TEST_CASE("Creator query accepts exact policy ceilings with a small valid query") {
    QueryFixture fixture;
    QueryPolicy policy;
    policy.limits.max_sql_bytes = 1048576;
    policy.limits.max_rows = 10000;
    policy.limits.max_result_bytes = 8388608;
    policy.limits.progress_callbacks = 100000;
    policy.limits.timeout = std::chrono::milliseconds{5000};
    for (int i = 0; i < 256; ++i) policy.columns.push_back({"accounts", "c" + std::to_string(i)});
    policy.columns[0] = {std::string(128, 'a'), std::string(128, 'b')};
    const auto result = read_creator_query(fixture.path, policy, "SELECT 1 AS n");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == 1);
    fixture.unchanged();
}

TEST_CASE("Creator query opens only an existing database without creating a missing path") {
    hawken::test::TempDirectory temp;
    const auto missing = temp.path() / "missing.sqlite";
    reject(missing, {}, "SELECT 1", QueryErrorCode::database_unavailable);
    CHECK_FALSE(std::filesystem::exists(missing));
    const auto invalid = temp.write("invalid.sqlite", "never-echo-this is not a database");
    const auto bytes = hawken::test::read_bytes(invalid);
    reject(invalid, public_policy(), "SELECT display_name FROM accounts", QueryErrorCode::database_unavailable);
    CHECK(hawken::test::read_bytes(invalid) == bytes);
}

TEST_CASE("Creator query errors redact SQL values and paths after prepare or step failure") {
    QueryFixture fixture;
    reject(fixture.path, public_policy(), "SELECT never_echo_this FROM accounts",
        QueryErrorCode::invalid_sql);
    reject(fixture.path, {}, "SELECT abs(-9223372036854775808) AS never_echo_this",
        QueryErrorCode::execution_failed);
    fixture.unchanged();
}
