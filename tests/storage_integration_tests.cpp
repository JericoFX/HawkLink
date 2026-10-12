#include "hawken/creator_query.hpp"
#include "hawken/local_store.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>

namespace {
hawken::creator::QueryPolicy alias_policy() {
    hawken::creator::QueryPolicy policy;
    policy.columns = {{"accounts", "id"}, {"aliases", "account_id"},
        {"aliases", "value"}, {"aliases", "last_seen"}};
    return policy;
}
}

TEST_CASE("Creator queries the actual local schema independently for each server") {
    hawken::test::TempDirectory temp;
    const auto first_path = temp.path() / "first.db";
    const auto second_path = temp.path() / "second.db";
    hawken::store::LocalStore first(first_path, "first");
    hawken::store::LocalStore second(second_path, "second");
    const auto a = first.get_or_create_account("76561198000000001", 1);
    const auto b = second.get_or_create_account(a.steam_id64, 2);
    REQUIRE(first.record_visit({"visit", a.ref, "First Pilot", 10}));
    REQUIRE(second.record_visit({"visit", b.ref, "Second Pilot", 20}));
    const auto sql = "SELECT a.id AS account_id,h.value AS alias,h.last_seen "
        "FROM accounts a JOIN aliases h ON h.account_id=a.id ORDER BY a.id,h.value";
    const auto first_result = hawken::creator::read_creator_query(first_path, alias_policy(), sql);
    const auto second_result = hawken::creator::read_creator_query(second_path, alias_policy(), sql);
    REQUIRE(first_result.rows.size() == 1);
    REQUIRE(second_result.rows.size() == 1);
    CHECK(std::get<std::int64_t>(first_result.rows[0][0]) == a.ref.local_id);
    CHECK(std::get<std::int64_t>(second_result.rows[0][0]) == b.ref.local_id);
    CHECK(std::get<std::string>(first_result.rows[0][1]) == "First Pilot");
    CHECK(std::get<std::string>(second_result.rows[0][1]) == "Second Pilot");
    CHECK(std::get<std::int64_t>(first_result.rows[0][2]) == 10);
    CHECK(std::get<std::int64_t>(second_result.rows[0][2]) == 20);
}

TEST_CASE("Creator cannot read ungranted identity or roles or mutate the actual store") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "local.db";
    hawken::store::LocalStore store(path, "server");
    const auto account = store.get_or_create_account("76561198000000001", 1);
    REQUIRE(store.record_visit({"visit", account.ref, "Local Pilot", 10}));
    store.set_role(account.ref, hawken::store::Role::owner);
    const auto policy = alias_policy();
    for (const auto* sql : {"SELECT steam_id64 FROM accounts", "SELECT role FROM roles",
        "WITH aliases AS(SELECT steam_id64 AS value FROM accounts) SELECT value FROM aliases",
        "UPDATE aliases SET value='Changed'"}) {
        try {
            static_cast<void>(hawken::creator::read_creator_query(path, policy, sql));
            FAIL_CHECK("A restricted Creator query crossed the local store boundary");
        } catch (const hawken::creator::QueryError& error) {
            CHECK(error.code == hawken::creator::QueryErrorCode::unauthorized);
            CHECK(std::string(error.what()) == "Creator query could not be executed.");
        }
    }
    CHECK(store.find_account(account.steam_id64) == std::optional<hawken::store::Account>(account));
    CHECK(store.role(account.ref) == std::optional<hawken::store::Role>(hawken::store::Role::owner));
    const auto aliases = store.aliases(account.ref);
    REQUIRE(aliases.size() == 1);
    CHECK(aliases[0].value == "Local Pilot");
}
