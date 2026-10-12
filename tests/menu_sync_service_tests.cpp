#include "hawken/menu_sync_service.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using hawken::auth::AuthReply;
using hawken::auth::AuthService;
using hawken::store::LocalStore;
using hawken::sync::MenuSyncReply;
using hawken::sync::MenuSyncService;
constexpr std::uint64_t fixture_id = 76561197960265729ULL;
constexpr const char* fixture_id_text = "76561197960265729";
constexpr std::string_view example_guid = "01234567-89ab-cdef-0123-456789abcdef";
constexpr std::array<std::string_view, 5> user_prefixes{
    "/userPublicReadOnlyData/", "/userGameSettings/", "/userGameItemStats/",
    "/userStats/", "/userAchievements/"};

// Only the external Steam verifier is substituted in this translation unit.
// AuthService, its access checks and LocalStore are production implementations.
// Fixture identities/tickets do not establish native Steam authentication.
class FakeVerifier final : public hawken::auth::SteamVerifier {
public:
    bool active = false;
    int verify_calls = 0;
    mutable int access_calls = 0;
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        ++verify_calls;
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket,
                active ? fixture_id : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        ++access_calls;
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
    MenuSyncService menu;
    explicit Fixture(std::string server_id = "menu-server")
        : store(temporary.path() / "accounts.sqlite", std::move(server_id)),
          auth(store, verifier, [this] {
              if (clock_throws) throw std::runtime_error("private-menu-clock");
              return now;
          }), menu(auth, store) {}
    AuthReply login() { return auth.login(R"({"SteamAuthTicket":"AABB0100"})"); }
};
std::string guid(const AuthReply& reply) { return reply.result.at("UserId").get<std::string>(); }
std::string header(const AuthReply& reply) {
    return "Basic " + reply.result.at("AccessGrant").get<std::string>();
}
std::vector<std::string> user_targets(std::string_view user_guid) {
    return {"/userPublicReadOnlyData/" + std::string(user_guid),
            "/userGameSettings/" + std::string(user_guid),
            "/userGameItemStats/" + std::string(user_guid),
            "/userStats/" + std::string(user_guid),
            "/userAchievements/" + std::string(user_guid) + "?countryCode=EU"};
}
std::vector<std::string> targets(std::string_view user_guid) {
    auto result = user_targets(user_guid);
    result.emplace_back("/bundles/");
    return result;
}
void failure(const MenuSyncReply& reply, int status, std::string_view secret = {}) {
    CHECK(reply.http_status == status);
    CHECK(reply.api_status == status);
    CHECK(reply.result.is_null());
    const std::string_view expected = status == 401 ? "Authentication required."
        : status == 403 ? "Access was rejected."
        : status == 400 ? "Invalid settings request."
        : status == 404 ? "Resource not found." : "Request could not be processed.";
    CHECK(reply.message == expected);
    CHECK(reply.message.find(fixture_id_text) == std::string::npos);
    CHECK(reply.message.find("private-menu-clock") == std::string::npos);
    CHECK(reply.message.find("accounts.sqlite") == std::string::npos);
    if (!secret.empty()) CHECK(reply.message.find(secret) == std::string::npos);
}
struct SqlCloser { void operator()(sqlite3* db) const { if (db) sqlite3_close(db); } };
struct StatementCloser { void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); } };

// Snapshot every product table through an independent read-only SQL connection.
// These constant queries do not derive identifiers from untrusted schema text.
// Comparing values, not total_changes on another connection, detects writes.
nlohmann::json sql_snapshot(const Fixture& f) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2((f.temporary.path() / "accounts.sqlite").string().c_str(),
        &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, SqlCloser> db(raw);
    REQUIRE(opened == SQLITE_OK);
    constexpr std::array<const char*, 11> queries{
        "SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name",
        "SELECT * FROM server_metadata ORDER BY singleton",
        "SELECT * FROM accounts ORDER BY id",
        "SELECT * FROM aliases ORDER BY account_id,value",
        "SELECT * FROM visits ORDER BY visit_id",
        "SELECT * FROM roles ORDER BY account_id",
        "SELECT * FROM bans ORDER BY account_id",
        "SELECT * FROM wallets ORDER BY account_id",
        "SELECT * FROM starter_grants ORDER BY account_id",
        "SELECT * FROM starter_items ORDER BY account_id,slot",
        "SELECT * FROM game_settings ORDER BY account_id,key"};
    auto snapshot = nlohmann::json::array();
    for (const auto query : queries) {
        sqlite3_stmt* raw_statement = nullptr;
        const auto prepared = sqlite3_prepare_v2(db.get(), query, -1, &raw_statement, nullptr);
        std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
        REQUIRE(prepared == SQLITE_OK);
        auto rows = nlohmann::json::array();
        int stepped = SQLITE_OK;
        while ((stepped = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto row = nlohmann::json::array();
            for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
                const auto type = sqlite3_column_type(statement.get(), column);
                if (type == SQLITE_NULL) row.push_back(nullptr);
                else if (type == SQLITE_INTEGER) row.push_back(sqlite3_column_int64(statement.get(), column));
                else {
                    REQUIRE(type == SQLITE_TEXT);
                    const auto* text = sqlite3_column_text(statement.get(), column);
                    REQUIRE(text != nullptr);
                    const auto count = sqlite3_column_bytes(statement.get(), column);
                    REQUIRE(count >= 0);
                    row.push_back(std::string(reinterpret_cast<const char*>(text),
                                              static_cast<std::size_t>(count)));
                }
            }
            rows.push_back(std::move(row));
        }
        REQUIRE(stepped == SQLITE_DONE);
        snapshot.push_back(std::move(rows));
    }
    return snapshot;
}
}

TEST_CASE("Menu recognizes only six recovered raw GET target shapes") {
    Fixture f;
    for (const auto& target : targets(example_guid)) CHECK(f.menu.supports_target(target));
    CHECK(f.menu.supports_target("/userAchievements/" + std::string(example_guid)
                                + "?countryCode=na_1.2-test"));
    CHECK(f.menu.supports_target("/userAchievements/" + std::string(example_guid)
                                + "?countryCode=" + std::string(64, 'A')));
}

TEST_CASE("Menu rejects target aliases detail POST resources and malformed queries before auth") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    f.clock_throws = true;
    const auto checks_before = f.verifier.access_calls;
    std::vector<std::string> invalid{"", "/", "/bundles", "/bundles//", "/bundles/?x=1",
        "/Bundles/", "/steam/login", "/game-client/game-items",
        "/gameBundlesQuery/", "/userAchievementsQuery/" + guid(login) + "?countryCode=EU",
        "/publicReadOnlyData/" + guid(login)};
    for (const auto prefix : user_prefixes) {
        const auto base = std::string(prefix) + guid(login);
        invalid.push_back(base + "/");
        invalid.push_back(base + "?x=1");
        invalid.push_back(base + "#x");
        invalid.push_back(std::string(prefix) + "%30" + guid(login).substr(1));
    }
    const auto unlocked = "/userAchievements/" + guid(login);
    for (const auto suffix : {"", "?countryCode=", "?CountryCode=EU", "?countryCode=EU&x=1",
            "?x=1&countryCode=EU", "?countryCode=EU&countryCode=EU", "?countryCode=%45U",
            "?countryCode=E U", "?countryCode=E\nU", "?countryCode=EU/", "?countryCode=EU#x",
            "?countryCode=\xc3\xa9"}) invalid.push_back(unlocked + suffix);
    invalid.push_back(unlocked + "?countryCode=" + std::string(65, 'A'));
    auto nul = unlocked + "?countryCode=EU";
    nul.push_back('\0');
    invalid.push_back(nul);
    for (const auto& target : invalid) {
        CHECK_FALSE(f.menu.supports_target(target));
        failure(f.menu.get(target, header(login)), 404, header(login).substr(6));
    }
    CHECK(f.verifier.access_calls == checks_before);
    CHECK(f.verifier.verify_calls == 1);
}

TEST_CASE("Menu user GUID layout is canonical lowercase with no path decoding") {
    Fixture f;
    for (const auto& value : std::vector<std::string>{"", "0123456789abcdef0123456789abcdef",
            "01234567-89AB-cdef-0123-456789abcdef", "01234567-89ab-cdef-0123-456789abcdeg",
            "01234567_89ab-cdef-0123-456789abcdef", "01234567-89ab-cdef-0123-456789abcde",
            "01234567-89ab-cdef-0123-456789abcdef0"}) {
        for (const auto& target : user_targets(value)) {
            CHECK_FALSE(f.menu.supports_target(target));
            failure(f.menu.get(target, ""), 404);
        }
    }
}

TEST_CASE("Menu protected reads without a grant never create SQL accounts or starter inventory") {
    Fixture f;
    const auto before = sql_snapshot(f);
    for (const auto& target : targets(example_guid)) failure(f.menu.get(target, ""), 401);
    CHECK(sql_snapshot(f) == before);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
    CHECK(f.verifier.verify_calls == 0);
}

TEST_CASE("Menu readonly settings and user statistics return exactly empty objects") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto prefix : {"/userPublicReadOnlyData/", "/userGameSettings/", "/userStats/"}) {
        const auto reply = f.menu.get(std::string(prefix) + guid(login), header(login));
        CHECK(reply.http_status == 200);
        CHECK(reply.api_status == 200);
        CHECK(reply.message.empty());
        CHECK(reply.result == nlohmann::json::object());
        CHECK(reply.result.is_object());
        for (const auto key : {"ExpPoints", "MMR", "UniqueCaseInsensitive_Callsign", "Callsign",
                "HawkenBalance", "MeteorBalance", "EULA", "FTUX", "AccessGrant", "UserId"})
            CHECK_FALSE(reply.result.contains(key));
    }
}

TEST_CASE("Menu instance statistics and bundles are empty arrays without invented rows") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto& target : std::vector<std::string>{"/userGameItemStats/" + guid(login), "/bundles/"}) {
        const auto reply = f.menu.get(target, header(login));
        CHECK(reply.http_status == 200);
        CHECK(reply.api_status == 200);
        CHECK(reply.message.empty());
        CHECK(reply.result == nlohmann::json::array());
        CHECK(reply.result.is_array());
    }
}

TEST_CASE("Menu unlocked absence preserves HTTP200 API404 and empty array instead of success IDs") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto& region : std::vector<std::string>{"EU", "na_1.2-test", std::string(64, 'A')}) {
        const auto reply = f.menu.get("/userAchievements/" + guid(login) + "?countryCode=" + region,
                                      header(login));
        CHECK(reply.http_status == 200);
        CHECK(reply.api_status == 404);
        CHECK(reply.message == "Resource not found.");
        CHECK(reply.result == nlohmann::json::array());
        CHECK(reply.result.is_array());
    }
    // The reachable absence branch has no details POST. The GET-only service
    // has no body/method dispatch or API that can issue a backend request.
    failure(f.menu.get("/userAchievementsQuery/" + guid(login) + "?countryCode=EU", header(login)), 404);
}

TEST_CASE("Every menu read rechecks native access without starting Steam authentication") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto before = f.verifier.access_calls;
    for (const auto& target : targets(guid(login))) CHECK(f.menu.get(target, header(login)).http_status == 200);
    CHECK(f.verifier.access_calls == before + 6);
    CHECK(f.verifier.verify_calls == 1);
}

TEST_CASE("Menu user paths cannot select another GUID with an otherwise valid grant") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    for (const auto& target : user_targets("00000000-0000-0000-0000-000000000000"))
        failure(f.menu.get(target, header(login)), 401, header(login).substr(6));
    for (const auto& target : targets(guid(login))) CHECK(f.menu.get(target, header(login)).http_status == 200);
}

TEST_CASE("Menu rejects malformed authorization and rotated grants on every route") {
    Fixture f;
    const auto first = f.login();
    REQUIRE(first.status == 200);
    const auto first_header = header(first);
    for (const auto& target : targets(guid(first))) {
        for (const auto& authorization : std::vector<std::string>{"", "Basic wrong", "Bearer wrong",
                first_header.substr(6), "basic " + first_header.substr(6), first_header + " "})
            failure(f.menu.get(target, authorization), 401, first_header.substr(6));
    }
    const auto second = f.login();
    REQUIRE(second.status == 200);
    for (const auto& target : targets(guid(second))) {
        failure(f.menu.get(target, first_header), 401, first_header.substr(6));
        CHECK(f.menu.get(target, header(second)).http_status == 200);
    }
}

TEST_CASE("Menu expires access at the inclusive grant deadline for each operation") {
    for (std::size_t index = 0; index < 6; ++index) {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        const auto target = targets(guid(login)).at(index);
        f.now = 1899;
        CHECK(f.menu.get(target, header(login)).http_status == 200);
        f.now = 1900;
        failure(f.menu.get(target, header(login)), 401);
    }
}

TEST_CASE("Menu observes session revocation on every operation") {
    for (std::size_t index = 0; index < 6; ++index) {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        const auto target = targets(guid(login)).at(index);
        CHECK(f.menu.get(target, header(login)).http_status == 200);
        f.verifier.active = false;
        failure(f.menu.get(target, header(login)), 401);
    }
}

TEST_CASE("Menu rejects active bans on every operation without leaking a reason") {
    for (std::size_t index = 0; index < 6; ++index) {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        const auto account = f.store.find_account(fixture_id_text);
        REQUIRE(account.has_value());
        f.store.set_ban(account->ref, {1000, std::nullopt, "private-menu-ban"});
        failure(f.menu.get(targets(guid(login)).at(index), header(login)), 403, "private-menu-ban");
    }
}

TEST_CASE("Menu clock reversal and clock failure cannot serve protected defaults") {
    SUBCASE("reversal") {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        f.now = 999;
        for (const auto& target : targets(guid(login))) failure(f.menu.get(target, header(login)), 401);
    }
    SUBCASE("exception") {
        Fixture f;
        const auto login = f.login();
        REQUIRE(login.status == 200);
        f.clock_throws = true;
        for (const auto& target : targets(guid(login))) failure(f.menu.get(target, header(login)), 500);
    }
}

TEST_CASE("Menu grants remain isolated between independent server databases") {
    Fixture a("menu-a"), b("menu-b");
    const auto first = a.login(), second = b.login();
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(guid(first) != guid(second));
    for (const auto& target : targets(guid(first))) failure(a.menu.get(target, header(second)), 401);
    for (const auto& target : targets(guid(second))) failure(b.menu.get(target, header(first)), 401);
    for (const auto& target : user_targets(guid(second))) failure(a.menu.get(target, header(first)), 401);
    for (const auto& target : user_targets(guid(first))) failure(b.menu.get(target, header(second)), 401);
}

TEST_CASE("Menu reads preserve every SQL row and cannot provision starter items or expose visit aliases") {
    Fixture f;
    const auto login = f.login();
    REQUIRE(login.status == 200);
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    REQUIRE(f.store.record_visit({"menu-fixture-visit", account->ref, "PrivateAlias", 1000}));
    f.store.set_role(account->ref, hawken::store::Role::analyst);
    f.store.set_ban(account->ref, {2000, std::nullopt, "future-private-ban"});
    const auto before = sql_snapshot(f);
    for (const auto& target : targets(guid(login))) CHECK(f.menu.get(target, header(login)).http_status == 200);
    const auto readonly = f.menu.get("/userPublicReadOnlyData/" + guid(login), header(login));
    CHECK(readonly.result == nlohmann::json::object());
    CHECK(readonly.result.dump().find("PrivateAlias") == std::string::npos);
    CHECK(sql_snapshot(f) == before);
    CHECK_FALSE(f.store.starter_items(account->ref));
    CHECK(f.store.wallet(account->ref) == hawken::store::WalletBalances{});
    CHECK(f.verifier.verify_calls == 1);
    failure(f.menu.get("/userAchievementsQuery/" + guid(login) + "?countryCode=EU", header(login)), 404);
    CHECK(sql_snapshot(f) == before);
}

TEST_CASE("Settings service recognizes only the exact raw account target for PUT") {
    Fixture f;
    CHECK(f.menu.supports_settings_target("/userGameSettings/"+std::string(example_guid)));
    for(const auto& target:std::vector<std::string>{"/userGameSettings/", "/userGameSettings/"+std::string(example_guid)+"/",
        "/userGameSettings/"+std::string(example_guid)+"?x=1", "/userStats/"+std::string(example_guid),
        "/userGameSettings/01234567-89AB-cdef-0123-456789abcdef", "/userGameSettings/%30"+std::string(example_guid).substr(1)}) {
        CHECK_FALSE(f.menu.supports_settings_target(target)); failure(f.menu.put(target,"private-grant","{}"),404);
    }
    CHECK(f.verifier.access_calls==0); CHECK(f.verifier.verify_calls==0);
}
TEST_CASE("Settings service merges received strings and returns null only for a committed ACK") {
    Fixture f; const auto login=f.login(); REQUIRE(login.status==200);
    const auto target="/userGameSettings/"+guid(login);
    // Synthetic opaque inputs; not seeded consent, not a claim of tutorial completion.
    const auto put=f.menu.put(target,header(login),R"({"Fixture.Decision":"4","Assignments.Progress":"{\"done\":[1,2]}"})");
    CHECK(put.http_status==200); CHECK(put.api_status==200); CHECK(put.result.is_null()); CHECK(put.message.empty());
    CHECK(f.menu.get(target,header(login)).result==nlohmann::json{{"Fixture.Decision","4"},{"Assignments.Progress",R"({"done":[1,2]})"}});
    const auto updated=f.menu.put(target,header(login),R"({"Fixture.Decision":"5"})"); CHECK(updated.http_status==200);
    CHECK(f.menu.get(target,header(login)).result==nlohmann::json{{"Fixture.Decision","5"},{"Assignments.Progress",R"({"done":[1,2]})"}});
    const auto before=sql_snapshot(f); CHECK(f.menu.put(target,header(login),R"({"Fixture.Decision":"5"})").http_status==200);
    CHECK(f.menu.put(target,header(login),"{}").http_status==200); CHECK(sql_snapshot(f)==before);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    CHECK(f.store.wallet(account->ref)==hawken::store::WalletBalances{}); CHECK_FALSE(f.store.starter_items(account->ref));
    CHECK(f.store.aliases(account->ref).empty()); CHECK(f.store.visits(account->ref).empty());
}
TEST_CASE("Settings reject malformed types duplicated escaped keys and bounds atomically") {
    Fixture f; const auto login=f.login(); const auto target="/userGameSettings/"+guid(login);
    REQUIRE(f.menu.put(target,header(login),R"({"saved":"original"})").http_status==200);
    const auto before=sql_snapshot(f);
    for(const auto& value:std::vector<std::string>{"", "null", "[]", "[{}]", "true", "{", "{} {}",
        R"({"x":1})",R"({"x":false})",R"({"x":null})",R"({"x":[]})",R"({"x":{}})",
        R"({"x":"a","x":"b"})",R"({"x":"a","\u0078":"b"})",R"({"":"v"})",
        R"({"nul":"\u0000"})",R"({"\u0000":"value"})",R"({"\nkey":"v"})",
        nlohmann::json{{"large",std::string(16385,'v')}}.dump(),std::string(65537,' ')}) {
        failure(f.menu.put(target,header(login),value),400,value); CHECK(sql_snapshot(f)==before);
    }
    nlohmann::json too_many=nlohmann::json::object(); for(int i=0;i<257;++i) too_many["k"+std::to_string(i)]="v";
    failure(f.menu.put(target,header(login),too_many.dump()),400); CHECK(sql_snapshot(f)==before);
}
TEST_CASE("Settings accept inclusive body value and pair limits with empty and opaque UTF8 values") {
    Fixture f; const auto login=f.login(); const auto target="/userGameSettings/"+guid(login);
    auto empty=std::string("{}"); empty.append(65536-empty.size(),' ');
    CHECK(f.menu.put(target,header(login),empty).http_status==200);
    nlohmann::json values{{std::string(256,'k'),std::string(16384,'v')},{"empty",""},{"utf8",std::string("\xc3\xa9")},{"controls","line\n\tend"}};
    CHECK(f.menu.put(target,header(login),values.dump()).http_status==200);
    CHECK(f.menu.get(target,header(login)).result==values);
    nlohmann::json many=nlohmann::json::object(); for(int i=0;i<256;++i) many["k"+std::to_string(i)]="";
    CHECK(f.menu.put(target,header(login),many.dump()).http_status==200);
}
TEST_CASE("Settings PUT rechecks GUID ban expiry revocation and clock without changing SQL on failure") {
    Fixture f; const auto login=f.login(); const auto target="/userGameSettings/"+guid(login);
    const auto account=f.store.find_account(fixture_id_text); REQUIRE(account);
    int expected=401;
    SUBCASE("missing grant") {}
    SUBCASE("foreign GUID") { failure(f.menu.put("/userGameSettings/"+std::string(example_guid),header(login),"{}"),401); return; }
    SUBCASE("ban") {f.store.set_ban(account->ref,{1000,std::nullopt,"private-rule"});expected=403;}
    SUBCASE("grant expiry") {f.now=100000;}
    SUBCASE("Steam session revocation") {f.verifier.active=false;}
    SUBCASE("clock failure") {f.clock_throws=true;expected=500;}
    const auto before=sql_snapshot(f);
    failure(f.menu.put(target,expected==401 && f.now==1000 && f.verifier.active && !f.clock_throws ? "" : header(login),R"({"x":"private-value"})"),expected);
    CHECK(sql_snapshot(f)==before);
}
TEST_CASE("Settings services cannot cross server GUIDs and do not interpret stats or currency keys") {
    Fixture a("a"),b("b");const auto one=a.login(),two=b.login();
    failure(a.menu.put("/userGameSettings/"+guid(two),header(one),R"({"x":"v"})"),401);
    CHECK(a.menu.put("/userGameSettings/"+guid(one),header(one),R"({"ExpPoints":"999999","HawkenPoints":"999999"})").http_status==200);
    const auto account=a.store.find_account(fixture_id_text);REQUIRE(account);
    CHECK(a.store.wallet(account->ref)==hawken::store::WalletBalances{}); CHECK_FALSE(a.store.starter_items(account->ref));
    CHECK(a.menu.get("/userStats/"+guid(one),header(one)).result==nlohmann::json::object());
    CHECK(b.menu.get("/userGameSettings/"+guid(two),header(two)).result==nlohmann::json::object());
}
TEST_CASE("Settings GET after restarting store and Auth returns the exact originally received strings") {
    hawken::test::TempDirectory temporary;const auto path=temporary.path()/"restart.sqlite";
    const nlohmann::json expected{{"Fixture.Completed","received-by-PUT"},{"Assignments.Progress",R"({"steps":["A","B"]})"}};
    {
        LocalStore store(path,"restart");FakeVerifier verifier;AuthService auth(store,verifier,[]{return 1000LL;});MenuSyncService menu(auth,store);
        const auto login=auth.login(R"({"SteamAuthTicket":"AABB0100"})");REQUIRE(login.status==200);
        REQUIRE(menu.put("/userGameSettings/"+guid(login),header(login),expected.dump()).http_status==200);
    }
    LocalStore store(path,"restart");FakeVerifier verifier;AuthService auth(store,verifier,[]{return 1001LL;});MenuSyncService menu(auth,store);
    const auto login=auth.login(R"({"SteamAuthTicket":"AABB0100"})");REQUIRE(login.status==200);
    CHECK(menu.get("/userGameSettings/"+guid(login),header(login)).result==expected);
    CHECK(menu.get("/userPublicReadOnlyData/"+guid(login),header(login)).result==nlohmann::json::object());
}
