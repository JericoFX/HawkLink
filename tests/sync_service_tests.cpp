#include "hawken/sync_service.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using hawken::auth::AuthReply;
using hawken::auth::AuthService;
using hawken::store::LocalStore;
using hawken::sync::SyncService;
constexpr std::uint64_t fixture_id = 76561197960265729ULL;
constexpr const char* fixture_id_text = "76561197960265729";
constexpr std::string_view overflow_path =
    "/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95";
constexpr std::string_view rewards_path = "/game-client/achievement-rewards";
constexpr std::string_view achievements_path = "/game-client/achievements/";

// Only this translation unit substitutes the external Steam verification.
// The service, access checks and SQL store exercised below are production code.
// Fixture bytes/identity establish no real Steam callback, license or admission.
class FakeVerifier final : public hawken::auth::SteamVerifier {
public:
    bool active = false;
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket,
                active ? fixture_id : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == fixture_id;
    }
    void end_session(std::uint64_t id) override {
        if (id == fixture_id) active = false;
    }
};
struct Fixture {
    hawken::test::TempDirectory temporary;
    LocalStore store;
    FakeVerifier verifier;
    std::int64_t now = 1000;
    bool clock_throws = false;
    AuthService auth;
    SyncService sync;

    explicit Fixture(std::string server_id = "sync-server")
        : store(temporary.path() / "accounts.sqlite", std::move(server_id)),
          auth(store, verifier, [this] {
              if (clock_throws) throw std::runtime_error("private-clock-fixture");
              return now;
          }), sync(store, auth, 25000) {}

    AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
std::string guid(const AuthReply& login) { return login.result.at("UserId").get<std::string>(); }
std::string header(const AuthReply& login) {
    return "Basic " + login.result.at("AccessGrant").get<std::string>();
}
std::vector<std::string> targets(const AuthReply& login) {
    return {std::string(overflow_path), std::string(rewards_path), std::string(achievements_path),
            "/gameCurrency/" + guid(login), "/meteorCurrency/" + guid(login)};
}
void failure(const AuthReply& reply, int status, std::string_view secret = {}) {
    CHECK(reply.status == status);
    CHECK(reply.result.is_null());
    CHECK(reply.message.find(fixture_id_text) == std::string::npos);
    CHECK(reply.message.find("private-clock-fixture") == std::string::npos);
    CHECK(reply.message.find("accounts.sqlite") == std::string::npos);
    if (!secret.empty()) CHECK(reply.message.find(secret) == std::string::npos);
}

struct SqlCloser { void operator()(sqlite3* db) const { if (db) sqlite3_close(db); } };
struct StatementCloser { void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); } };
void set_fixture_balances(Fixture& f, std::int32_t hp, std::int32_t mp) {
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    sqlite3* raw = nullptr;
    const auto open_status = sqlite3_open_v2((f.temporary.path() / "accounts.sqlite").string().c_str(),
        &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, SqlCloser> database(raw);
    REQUIRE(open_status == SQLITE_OK);
    sqlite3_stmt* raw_statement = nullptr;
    const auto prepare_status = sqlite3_prepare_v2(database.get(),
        "UPDATE wallets SET hawken_points=?1,meteor_points=?2 WHERE account_id=?3",
        -1, &raw_statement, nullptr);
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    REQUIRE(prepare_status == SQLITE_OK);
    REQUIRE(sqlite3_bind_int(statement.get(), 1, hp) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int(statement.get(), 2, mp) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement.get(), 3, account->ref.local_id) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement.get()) == SQLITE_DONE);
    REQUIRE(sqlite3_changes(database.get()) == 1);
}
}

TEST_CASE("Sync interval must be positive before protected overflow can be served") {
    hawken::test::TempDirectory temporary;
    LocalStore store(temporary.path() / "accounts.sqlite", "sync-server");
    FakeVerifier verifier;
    AuthService auth(store, verifier, [] { return std::int64_t{1000}; });
    CHECK_THROWS_AS(SyncService(store, auth, 0), std::invalid_argument);
    CHECK_THROWS_AS(SyncService(store, auth, -1), std::invalid_argument);
    CHECK_THROWS_AS(SyncService(store, auth, std::numeric_limits<std::int32_t>::min()),
                    std::invalid_argument);
    SyncService service(store, auth, std::numeric_limits<std::int32_t>::max());
    const auto login = auth.login(R"({"SteamAuthTicket":"AABB0100"})");
    REQUIRE(login.status == 200);
    const auto reply = service.get(overflow_path, header(login));
    REQUIRE(reply.status == 200);
    CHECK(reply.result.at("StatPerCurrency") == 2147483647);
}

TEST_CASE("Sync recognizes only the five recovered raw target shapes") {
    Fixture f;
    const std::array<std::string_view, 8> valid{
        overflow_path, rewards_path, achievements_path,
        "/game-client/achievements/EU", "/game-client/achievements/na_1.2-test",
        "/gameCurrency/01234567-89ab-cdef-0123-456789abcdef",
        "/meteorCurrency/01234567-89ab-cdef-0123-456789abcdef",
        "/game-client/achievements/0"};
    for (const auto target : valid) CHECK(f.sync.supports_target(target));
    CHECK(f.sync.supports_target(std::string(achievements_path) + std::string(64, 'A')));
}

TEST_CASE("Sync rejects slash aliases queries escaped targets and unrelated endpoints") {
    Fixture f;
    const std::vector<std::string> invalid{
        "", "/", "/steam/login", "/game-client/game-items", "/game-client/game-offers",
        "/game-client/achievement-rewards/", "/game-client/achievements",
        std::string(overflow_path) + "/", std::string(overflow_path) + "?x=1",
        "/statOverflow/other", "/statoverflow/32f2f08c-87f6-45a3-8440-a84242907a95",
        "/game-client/achievements/EU/", "/game-client/achievements/EU?x=1",
        "/game-client/achievements/EU#x", "/game-client/achievements/%45U",
        "/game-client/achievements/E U", "/game-client/achievements/E\nU",
        "/game-client/achievements/\xc3\xa9", "/game-client/achievements/a/b",
        std::string(achievements_path) + std::string(65, 'A'),
        "/gameCurrency/01234567-89ab-cdef-0123-456789abcdef/",
        "/gameCurrency/01234567-89ab-cdef-0123-456789abcdef?x=1",
        "/users/01234567-89ab-cdef-0123-456789abcdef/gameCurrency",
        "/meteorcurrency/01234567-89ab-cdef-0123-456789abcdef"};
    for (const auto& target : invalid) {
        CHECK_FALSE(f.sync.supports_target(target));
        failure(f.sync.get(target, "Basic private-target-fixture"), 404, "private-target-fixture");
    }
    auto nul = std::string(achievements_path) + "EU";
    nul.push_back('\0');
    CHECK_FALSE(f.sync.supports_target(nul));
    failure(f.sync.get(nul, ""), 404);
}

TEST_CASE("Wallet target GUIDs require lower-case canonical hexadecimal layout") {
    Fixture f;
    const std::vector<std::string> invalid{
        "", "0123456789abcdef0123456789abcdef", "01234567-89AB-cdef-0123-456789abcdef",
        "01234567-89ab-cdef-0123-456789abcdeg", "01234567_89ab-cdef-0123-456789abcdef",
        "01234567-89ab-cdef-0123-456789abcde", "01234567-89ab-cdef-0123-456789abcdef0",
        "%30%31%32%33%34%35%36%37-89ab-cdef-0123-456789abcdef"};
    for (const auto& value : invalid) {
        CHECK_FALSE(f.sync.supports_target("/gameCurrency/" + value));
        CHECK_FALSE(f.sync.supports_target("/meteorCurrency/" + value));
        failure(f.sync.get("/gameCurrency/" + value, ""), 404);
    }
}

TEST_CASE("Protected sync reads cannot create accounts or grants without authentication") {
    Fixture f;
    for (const auto target : {overflow_path, rewards_path, achievements_path,
            std::string_view{"/gameCurrency/01234567-89ab-cdef-0123-456789abcdef"},
            std::string_view{"/meteorCurrency/01234567-89ab-cdef-0123-456789abcdef"}})
        failure(f.sync.get(target, ""), 401);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Overflow supplies only its original public ID and explicit server interval") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto reply = f.sync.get(overflow_path, header(login));
    REQUIRE(reply.status == 200);
    CHECK(reply.message.empty());
    CHECK(reply.result == nlohmann::json{{"OverflowId", "32f2f08c-87f6-45a3-8440-a84242907a95"},
                                       {"StatPerCurrency", 25000}});
    CHECK(reply.result.at("StatPerCurrency").is_number_integer());
    CHECK_FALSE(reply.result.contains("AccessGrant"));
    CHECK_FALSE(reply.result.contains("UserId"));
    CHECK_FALSE(reply.result.contains("CurrencyValue"));
}

TEST_CASE("Achievement and reward definitions remain empty arrays without synthetic grants") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto& target : std::vector<std::string>{std::string(rewards_path),
            std::string(achievements_path), "/game-client/achievements/EU",
            std::string(achievements_path) + std::string(64, 'A')}) {
        const auto reply = f.sync.get(target, header(login));
        REQUIRE(reply.status == 200);
        CHECK(reply.message.empty());
        CHECK(reply.result.is_array());
        CHECK(reply.result.empty());
    }
}

TEST_CASE("Fresh account wallets are numeric zero from the server store") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto& prefix : {std::string{"/gameCurrency/"}, std::string{"/meteorCurrency/"}}) {
        const auto reply = f.sync.get(prefix + guid(login), header(login));
        REQUIRE(reply.status == 200);
        CHECK(reply.message.empty());
        CHECK(reply.result.is_number_integer());
        CHECK(reply.result == 0);
        CHECK_FALSE(reply.result.is_string());
        CHECK_FALSE(reply.result.is_object());
    }
}

TEST_CASE("Wallet responses read updated SQL balances rather than fixed successful placeholders") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    set_fixture_balances(f, 73, 0);
    CHECK(f.sync.get("/gameCurrency/" + guid(login), header(login)).result == 73);
    CHECK(f.sync.get("/meteorCurrency/" + guid(login), header(login)).result == 0);
    // Nonzero premium field below is an isolated SQL read fixture, not an API
    // for purchasing/awarding premium credits or a production economic policy.
    set_fixture_balances(f, 2147483647, 19);
    const auto hp = f.sync.get("/gameCurrency/" + guid(login), header(login));
    const auto mp = f.sync.get("/meteorCurrency/" + guid(login), header(login));
    REQUIRE(hp.status == 200);
    REQUIRE(mp.status == 200);
    CHECK(hp.result.is_number_integer());
    CHECK(hp.result == 2147483647);
    CHECK(mp.result == 19);
    set_fixture_balances(f, 0, 2147483647);
    CHECK(f.sync.get("/gameCurrency/" + guid(login), header(login)).result == 0);
    CHECK(f.sync.get("/meteorCurrency/" + guid(login), header(login)).result == 2147483647);
}

TEST_CASE("Each sync route rejects malformed and rotated access grants") {
    Fixture f;
    const auto first = f.login();
    REQUIRE(first.status == 200);
    const auto valid_header = header(first);
    for (const auto& target : targets(first)) {
        for (const auto& value : std::vector<std::string>{"", "Basic wrong", "Bearer wrong",
                valid_header.substr(6), "basic " + valid_header.substr(6), valid_header + " "})
            failure(f.sync.get(target, value), 401, valid_header.substr(6));
    }
    const auto second = f.login();
    REQUIRE(second.status == 200);
    for (const auto& target : targets(second)) {
        failure(f.sync.get(target, valid_header), 401, valid_header.substr(6));
        CHECK(f.sync.get(target, header(second)).status == 200);
    }
}

TEST_CASE("Wallet path cannot select a different account with a valid own grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto prefix : {"/gameCurrency/", "/meteorCurrency/"})
        failure(f.sync.get(std::string(prefix) + "00000000-0000-0000-0000-000000000000",
                           header(login)), 401, header(login).substr(6));
    CHECK(f.sync.get("/gameCurrency/" + guid(login), header(login)).status == 200);
}

TEST_CASE("Every sync request rechecks grant expiry at the inclusive deadline") {
    for (int index = 0; index < 5; ++index) {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        const auto target = targets(login).at(static_cast<std::size_t>(index));
        f.now = 1899;
        CHECK(f.sync.get(target, header(login)).status == 200);
        f.now = 1900;
        failure(f.sync.get(target, header(login)), 401, header(login).substr(6));
    }
}

TEST_CASE("Every sync request rechecks bans and native session revocation") {
    SUBCASE("ban") {
        for (int index = 0; index < 5; ++index) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            const auto account = f.store.find_account(fixture_id_text);
            REQUIRE(account.has_value());
            f.store.set_ban(account->ref, {1000, std::nullopt, "private-ban-fixture"});
            failure(f.sync.get(targets(login).at(static_cast<std::size_t>(index)), header(login)),
                    403, "private-ban-fixture");
        }
    }
    SUBCASE("revocation") {
        for (int index = 0; index < 5; ++index) {
            Fixture f;
            const auto login = f.login();
            REQUIRE(login.status == 200);
            f.verifier.active = false;
            failure(f.sync.get(targets(login).at(static_cast<std::size_t>(index)), header(login)), 401);
        }
    }
}

TEST_CASE("Clock reversal and clock exceptions cannot serve protected sync data") {
    SUBCASE("reversal") {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        f.now = 999;
        for (const auto& target : targets(login)) failure(f.sync.get(target, header(login)), 401);
    }
    SUBCASE("exception") {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        f.clock_throws = true;
        for (const auto& target : targets(login)) failure(f.sync.get(target, header(login)), 500);
    }
}

TEST_CASE("Grants and balances remain isolated between two server databases") {
    Fixture a("server-a"), b("server-b");
    const auto first = a.login(), second = b.login();
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(guid(first) != guid(second));
    set_fixture_balances(a, 41, 0);
    set_fixture_balances(b, 99, 0);
    CHECK(a.sync.get("/gameCurrency/" + guid(first), header(first)).result == 41);
    CHECK(b.sync.get("/gameCurrency/" + guid(second), header(second)).result == 99);
    for (const auto& target : targets(first)) failure(a.sync.get(target, header(second)), 401);
    for (const auto& target : targets(second)) failure(b.sync.get(target, header(first)), 401);
}

TEST_CASE("Mismatched store scope fails closed without SQL or account diagnostics") {
    Fixture f;
    hawken::test::TempDirectory other;
    LocalStore other_store(other.path() / "private-other.sqlite", "other-server");
    SyncService mismatched(other_store, f.auth, 25000);
    const auto login = f.login();
    REQUIRE(login.status == 200);
    failure(mismatched.get("/gameCurrency/" + guid(login), header(login)), 500,
            "private-other.sqlite");
    CHECK_FALSE(other_store.find_account(fixture_id_text));
}
