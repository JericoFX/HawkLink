#include "hawken/auth_service.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <regex>
#include <vector>

namespace {
using namespace hawken::auth;
// This provider is confined to this unit-test translation unit. These bytes and
// identities are synthetic; they do not establish a Steam callback or license.
constexpr std::uint64_t fixture_id = 76561197960265729ULL;
constexpr const char* fixture_id_text = "76561197960265729";
constexpr const char* ticket_request = R"({"SteamAuthTicket":"AABB0100"})";
class TestVerifier final : public SteamVerifier {
public:
    SteamAuthStatus next_status = SteamAuthStatus::authenticated;
    std::uint64_t next_id = fixture_id;
    bool active = false;
    bool throws = false;
    int verify_calls = 0;
    int claimed_verify_calls = 0;
    std::uint64_t last_claim = 0;
    int end_calls = 0;
    std::vector<std::uint8_t> last_ticket;
    SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        ++verify_calls;
        last_ticket.assign(ticket.begin(), ticket.end());
        if (throws) throw std::runtime_error("never-echo-this-fixture");
        active = next_status == SteamAuthStatus::authenticated && next_id != 0;
        return {next_status, active ? next_id : 0};
    }
    SteamAuthResult verify(std::span<const std::uint8_t> ticket, std::uint64_t claim) override {
        ++claimed_verify_calls;
        last_claim = claim;
        return verify(ticket);
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == next_id;
    }
    void end_session(std::uint64_t id) override {
        ++end_calls;
        if (id == next_id) active = false;
    }
};
struct Fixture {
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store{temporary.path() / "accounts.sqlite", "fixture-one"};
    TestVerifier verifier;
    std::int64_t now = 1000;
    AuthService service{store, verifier, [this] { return now; }};
};
std::string grant(const AuthReply& reply) { return reply.result.at("AccessGrant").get<std::string>(); }
std::string guid(const AuthReply& reply) { return reply.result.at("UserId").get<std::string>(); }
std::string basic(const AuthReply& reply) { return "Basic " + grant(reply); }
void check_failure(const AuthReply& reply, int status) {
    CHECK(reply.status == status);
    CHECK(reply.result.is_null());
    CHECK(reply.message.find("never-echo") == std::string::npos);
    CHECK(reply.message.find(fixture_id_text) == std::string::npos);
    CHECK(reply.message.find("AABB0100") == std::string::npos);
}
}

TEST_CASE("Account login decodes the observed uppercase hex before verified SQL provisioning") {
    Fixture f;
    CHECK_FALSE(f.store.find_account(fixture_id_text));
    const auto reply = f.service.login(ticket_request);
    REQUIRE(reply.status == 200);
    CHECK(f.verifier.verify_calls == 1);
    CHECK(f.verifier.last_ticket == std::vector<std::uint8_t>{0xaa, 0xbb, 1, 0});
    CHECK(f.store.find_account(fixture_id_text).has_value());
    CHECK(reply.result.at("NewUser") == false);
    CHECK(std::regex_match(guid(reply), std::regex("[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}")));
    CHECK(std::regex_match(grant(reply), std::regex("[0-9a-f]{64}")));
    CHECK(reply.result.size() == 3);
    const auto profile = f.service.profile(guid(reply), basic(reply));
    REQUIRE(profile.status == 200);
    CHECK(profile.result.at("Guid") == guid(reply));
    CHECK(profile.result.at("Roles").is_array());
    CHECK(profile.result.at("Roles").empty());
    CHECK_FALSE(profile.result.contains("SteamId"));
    CHECK_FALSE(profile.result.contains("EmailAddress"));
    CHECK_FALSE(profile.result.contains("Callsign"));
}

TEST_CASE("Claim login binds unchanged ticket bytes to the validated client identity") {
    Fixture f;
    const auto reply = f.service.login(ticket_request, fixture_id_text);
    REQUIRE(reply.status == 200);
    CHECK(f.verifier.claimed_verify_calls == 1);
    CHECK(f.verifier.last_claim == fixture_id);
    CHECK(f.verifier.last_ticket == std::vector<std::uint8_t>{0xaa, 0xbb, 1, 0});
    CHECK(f.store.find_account(fixture_id_text).has_value());
    CHECK(f.service.profile(guid(reply), basic(reply)).status == 200);
    CHECK(reply.result.size() == 3);
    CHECK_FALSE(reply.result.contains("SteamId"));
}

TEST_CASE("Malformed or nonindividual claims preserve an existing grant without native calls") {
    Fixture f;
    const auto previous = f.service.login(ticket_request);
    REQUIRE(previous.status == 200);
    // Independent decimal literals cover canonical parsing, overflow and the
    // SDK individual structure. Syntax alone can never authenticate a user.
    const std::array<std::string_view, 18> claims{
        "", "0", "076561197960265729", "+76561197960265729", "-1",
        " 76561197960265729", "76561197960265729 ", "76561197960265729,76561197960265729",
        "76561197960265729x", "18446744073709551616", "999999999999999999999",
        "1", "72057598332895233", "144115192370823169", "365917473429094401",
        "76561193665298432", "76561215140134913", "76561197960265729\n"};
    for (const auto claim : claims) {
        check_failure(f.service.login(ticket_request, claim), 400);
        CHECK(f.service.profile(guid(previous), basic(previous)).status == 200);
    }
    CHECK(f.verifier.claimed_verify_calls == 0);
    CHECK(f.verifier.verify_calls == 1);
    CHECK(f.verifier.end_calls == 0);
}

TEST_CASE("Claim login rejects extra JSON identity before replacing the owned session") {
    Fixture f;
    const auto previous = f.service.login(ticket_request);
    REQUIRE(previous.status == 200);
    check_failure(f.service.login(
        R"({"SteamAuthTicket":"AABB0100","SteamId":"76561197960265729"})", fixture_id_text), 400);
    CHECK(f.verifier.claimed_verify_calls == 0);
    CHECK(f.verifier.end_calls == 0);
    CHECK(f.service.profile(guid(previous), basic(previous)).status == 200);
}

TEST_CASE("Authenticated mismatching identity is ended before any SQL account or grant") {
    Fixture f;
    f.verifier.next_id = 76561197960265730ULL;
    check_failure(f.service.login(ticket_request, fixture_id_text), 403);
    CHECK(f.verifier.claimed_verify_calls == 1);
    CHECK(f.verifier.end_calls == 1);
    CHECK_FALSE(f.verifier.active);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
    CHECK_FALSE(f.store.find_account("76561197960265730"));
    CHECK(f.service.access("Basic anything").status == 401);
}

TEST_CASE("A valid claim never substitutes for unavailable rejected or unlicensed native auth") {
    const std::array<std::pair<SteamAuthStatus, int>, 5> failures{{
        {SteamAuthStatus::unavailable, 503}, {SteamAuthStatus::invalid_ticket, 403},
        {SteamAuthStatus::rejected, 403}, {SteamAuthStatus::license_missing, 403},
        {SteamAuthStatus::timed_out, 503}}};
    for (const auto [status, expected] : failures) {
        Fixture f;
        f.verifier.next_status = status;
        check_failure(f.service.login(ticket_request, fixture_id_text), expected);
        CHECK(f.verifier.claimed_verify_calls == 1);
        CHECK_FALSE(f.store.find_account(fixture_id_text));
    }
}

TEST_CASE("Providers without claim support cannot silently fall back to ticket only login") {
    class TicketOnlyVerifier final : public SteamVerifier {
    public:
        int calls = 0;
        SteamAuthResult verify(std::span<const std::uint8_t>) override {
            ++calls;
            return {SteamAuthStatus::authenticated, fixture_id};
        }
        bool is_authenticated(std::uint64_t) const override { return true; }
        void end_session(std::uint64_t) override {}
    } verifier;
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store{temporary.path() / "accounts.sqlite", "claim-no-fallback"};
    AuthService service{store, verifier, [] { return 1000; }};
    check_failure(service.login(ticket_request, fixture_id_text), 503);
    CHECK(verifier.calls == 0);
    CHECK_FALSE(store.find_account(fixture_id_text));
}

TEST_CASE("Malformed login never calls Steam or creates an SQL account") {
    Fixture f;
    const std::array<std::string_view, 19> bodies{
        "", "{", "[]", "null", "{}", R"({"Ticket":"AA"})",
        R"({"SteamAuthTicket":null})", R"({"SteamAuthTicket":5})",
        R"({"SteamAuthTicket":[]})", R"({"SteamAuthTicket":""})",
        R"({"SteamAuthTicket":"A"})", R"({"SteamAuthTicket":"aa"})",
        R"({"SteamAuthTicket":"GG"})", R"({"SteamAuthTicket":"AA BB"})",
        R"({"SteamAuthTicket":"AA\u0000"})",
        R"({"SteamAuthTicket":"AA","SteamAuthTicket":"BB"})",
        R"({"SteamAuthTicket":"AA","SteamId":"76561197960265729"})",
        R"({"SteamAuthTicket":"AA","extra":true})", R"({"SteamAuthTicket":"AA"} {})"};
    for (const auto body : bodies) check_failure(f.service.login(body), 400);
    CHECK(f.verifier.verify_calls == 0);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Ticket bound rejects oversize before Steam and accepts the native maximum") {
    Fixture f;
    check_failure(f.service.login(R"({"SteamAuthTicket":")" + std::string(2050, 'A') + "\"}"), 400);
    check_failure(f.service.login(std::string(8193, ' ')), 400);
    CHECK(f.verifier.verify_calls == 0);
    const auto reply = f.service.login(R"({"SteamAuthTicket":")" + std::string(2048, 'A') + "\"}");
    REQUIRE(reply.status == 200);
    CHECK(f.verifier.last_ticket.size() == 1024);
}

TEST_CASE("Only ticket callback and license success permits an account or grant") {
    const std::array<std::pair<SteamAuthStatus, int>, 5> failures{{
        {SteamAuthStatus::unavailable, 503}, {SteamAuthStatus::invalid_ticket, 403},
        {SteamAuthStatus::rejected, 403}, {SteamAuthStatus::license_missing, 403},
        {SteamAuthStatus::timed_out, 503}}};
    for (const auto [status, http] : failures) {
        Fixture f;
        f.verifier.next_status = status;
        check_failure(f.service.login(ticket_request), http);
        CHECK(f.verifier.verify_calls == 1);
        CHECK_FALSE(f.store.find_account(fixture_id_text));
    }
}

TEST_CASE("Provider exceptions and zero identities never create credentials") {
    Fixture f;
    f.verifier.throws = true;
    check_failure(f.service.login(ticket_request), 503);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
    f.verifier.throws = false;
    f.verifier.next_id = 0;
    check_failure(f.service.login(ticket_request), 503);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Profile requires the exact raw Basic grant and account path") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    for (const auto& header : std::vector<std::string>{"", grant(login), "Bearer " + grant(login),
            "basic " + grant(login), basic(login) + " ", "Basic wrong"})
        check_failure(f.service.profile(guid(login), header), 401);
    check_failure(f.service.profile("00000000-0000-0000-0000-000000000000", basic(login)), 401);
    check_failure(f.service.profile(guid(login) + "?other", basic(login)), 401);
    CHECK(f.service.profile(guid(login), basic(login)).status == 200);
}

TEST_CASE("Grant expires at the policy boundary and ends the owned Steam session") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    f.now += AuthService::grant_lifetime_seconds - 1;
    CHECK(f.service.profile(guid(login), basic(login)).status == 200);
    ++f.now;
    check_failure(f.service.profile(guid(login), basic(login)), 401);
    CHECK_FALSE(f.verifier.active);
    CHECK(f.verifier.end_calls == 1);
}

TEST_CASE("Reversed clocks and overflowing deadlines cannot extend a grant") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    f.now = 999;
    check_failure(f.service.profile(guid(login), basic(login)), 401);
    CHECK_FALSE(f.verifier.active);
    f.now = std::numeric_limits<std::int64_t>::max() - 899;
    check_failure(f.service.login(ticket_request), 500);
    CHECK_FALSE(f.verifier.active);
}

TEST_CASE("Steam revocation immediately invalidates the HTTP access grant") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    f.verifier.active = false;
    check_failure(f.service.profile(guid(login), basic(login)), 401);
}

TEST_CASE("A second login rotates the grant without changing the server account") {
    Fixture f;
    const auto first = f.service.login(ticket_request);
    REQUIRE(first.status == 200);
    const auto account = f.store.find_account(fixture_id_text);
    const auto second = f.service.login(ticket_request);
    REQUIRE(second.status == 200);
    CHECK(guid(first) == guid(second));
    CHECK(grant(first) != grant(second));
    CHECK(f.store.find_account(fixture_id_text) == account);
    check_failure(f.service.profile(guid(first), basic(first)), 401);
    CHECK(f.service.profile(guid(second), basic(second)).status == 200);
    CHECK(f.verifier.end_calls == 1);
}

TEST_CASE("An active server ban prevents login and revokes existing grants") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    const auto account = f.store.find_account(fixture_id_text);
    REQUIRE(account.has_value());
    f.store.set_ban(account->ref, {1000, std::nullopt, "fixture ban"});
    check_failure(f.service.profile(guid(login), basic(login)), 403);
    CHECK_FALSE(f.verifier.active);
    check_failure(f.service.login(ticket_request), 403);
    CHECK_FALSE(f.verifier.active);
    f.store.set_ban(account->ref, {900, 1000, "expired fixture ban"});
    CHECK(f.service.login(ticket_request).status == 200);
}

TEST_CASE("Logout requires the matching body grant and then invalidates native and HTTP sessions") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    check_failure(f.service.logout(guid(login), basic(login), "{}"), 400);
    check_failure(f.service.logout(guid(login), basic(login), R"({"AccessGrant":"wrong"})"), 400);
    const auto body = nlohmann::json{{"AccessGrant", grant(login)}}.dump();
    CHECK(f.service.profile(guid(login), basic(login)).status == 200);
    CHECK(f.service.logout(guid(login), basic(login), body).status == 200);
    CHECK_FALSE(f.verifier.active);
    check_failure(f.service.profile(guid(login), basic(login)), 401);
}

TEST_CASE("Account GUID survives backend restart while access grants do not") {
    hawken::test::TempDirectory temporary;
    TestVerifier verifier;
    std::string saved_guid, saved_authorization;
    {
        hawken::store::LocalStore store(temporary.path() / "accounts.sqlite", "fixture-one");
        AuthService service(store, verifier, [] { return 1000; });
        const auto reply = service.login(ticket_request);
        REQUIRE(reply.status == 200);
        saved_guid = guid(reply);
        saved_authorization = basic(reply);
    }
    CHECK_FALSE(verifier.active);
    hawken::store::LocalStore store(temporary.path() / "accounts.sqlite", "fixture-one");
    AuthService service(store, verifier, [] { return 1001; });
    check_failure(service.profile(saved_guid, saved_authorization), 401);
    const auto reply = service.login(ticket_request);
    REQUIRE(reply.status == 200);
    CHECK(guid(reply) == saved_guid);
    CHECK(basic(reply) != saved_authorization);
}

TEST_CASE("Identical synthetic identities have a different GUID in each server database") {
    hawken::test::TempDirectory temporary;
    TestVerifier a, b;
    hawken::store::LocalStore store_a(temporary.path() / "a.sqlite", "server-a");
    hawken::store::LocalStore store_b(temporary.path() / "b.sqlite", "server-b");
    AuthService service_a(store_a, a, [] { return 1000; });
    AuthService service_b(store_b, b, [] { return 1000; });
    const auto first = service_a.login(ticket_request);
    const auto second = service_b.login(ticket_request);
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(guid(first) != guid(second));
    check_failure(service_a.profile(guid(second), basic(second)), 401);
    check_failure(service_b.profile(guid(first), basic(first)), 401);
}

TEST_CASE("Protected account access requires an existing verified grant without provisioning") {
    Fixture f;
    for (const auto header : {"", "Basic wrong", "Bearer wrong"}) {
        const auto reply = f.service.access(header);
        CHECK(reply.status == 401);
        CHECK_FALSE(reply.account.has_value());
    }
    CHECK(f.verifier.verify_calls == 0);
    CHECK_FALSE(f.store.find_account(fixture_id_text));
}

TEST_CASE("Protected account access resolves its own SQL scope with optional matching path") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    const auto stored = f.store.find_account(fixture_id_text);
    REQUIRE(stored.has_value());
    const auto account_guid = guid(login);
    for (const auto path : {std::optional<std::string_view>{},
            std::optional<std::string_view>{account_guid}}) {
        const auto reply = f.service.access(basic(login), path);
        REQUIRE(reply.status == 200);
        REQUIRE(reply.account.has_value());
        CHECK(reply.account->account == stored->ref);
        CHECK(reply.account->user_guid == guid(login));
    }
    CHECK(f.verifier.verify_calls == 1);
}

TEST_CASE("Protected account access rejects malformed grants and other account paths") {
    Fixture f;
    const auto login = f.service.login(ticket_request);
    REQUIRE(login.status == 200);
    for (const auto& header : std::vector<std::string>{"", grant(login), "Bearer " + grant(login),
            "basic " + grant(login), basic(login) + " ", "Basic wrong"}) {
        const auto reply = f.service.access(header);
        CHECK(reply.status == 401);
        CHECK_FALSE(reply.account.has_value());
    }
    for (const auto& path : std::vector<std::string>{"", "00000000-0000-0000-0000-000000000000",
            guid(login) + "?other"}) {
        const auto reply = f.service.access(basic(login), path);
        CHECK(reply.status == 401);
        CHECK_FALSE(reply.account.has_value());
    }
    CHECK(f.service.access(basic(login)).status == 200);
    CHECK(f.verifier.end_calls == 0);
}

TEST_CASE("Protected account access rejects rotated and expired grants") {
    Fixture f;
    const auto first = f.service.login(ticket_request);
    const auto second = f.service.login(ticket_request);
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(f.service.access(basic(first)).status == 401);
    CHECK_FALSE(f.service.access(basic(first)).account.has_value());
    f.now += AuthService::grant_lifetime_seconds - 1;
    CHECK(f.service.access(basic(second)).status == 200);
    ++f.now;
    const auto expired = f.service.access(basic(second));
    CHECK(expired.status == 401);
    CHECK_FALSE(expired.account.has_value());
    CHECK_FALSE(f.verifier.active);
}

TEST_CASE("Protected account access rechecks local bans native revocation and reversed clocks") {
    SUBCASE("ban") {
        Fixture f;
        const auto login = f.service.login(ticket_request);
        REQUIRE(login.status == 200);
        const auto account = f.store.find_account(fixture_id_text);
        REQUIRE(account.has_value());
        f.store.set_ban(account->ref, {1000, std::nullopt, "fixture ban"});
        const auto reply = f.service.access(basic(login));
        CHECK(reply.status == 403);
        CHECK_FALSE(reply.account.has_value());
        CHECK_FALSE(f.verifier.active);
    }
    SUBCASE("revocation") {
        Fixture f;
        const auto login = f.service.login(ticket_request);
        REQUIRE(login.status == 200);
        f.verifier.active = false;
        const auto reply = f.service.access(basic(login));
        CHECK(reply.status == 401);
        CHECK_FALSE(reply.account.has_value());
    }
    SUBCASE("reversed clock") {
        Fixture f;
        const auto login = f.service.login(ticket_request);
        REQUIRE(login.status == 200);
        f.now = 999;
        const auto reply = f.service.access(basic(login));
        CHECK(reply.status == 401);
        CHECK_FALSE(reply.account.has_value());
        CHECK_FALSE(f.verifier.active);
    }
}

TEST_CASE("Protected account access returns no context after an internal clock failure") {
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store(temporary.path() / "accounts.sqlite", "fixture-one");
    TestVerifier verifier;
    bool clock_failed = false;
    AuthService service(store, verifier, [&] {
        if (clock_failed) throw std::runtime_error("never-echo-private-clock");
        return std::int64_t{1000};
    });
    const auto login = service.login(ticket_request);
    REQUIRE(login.status == 200);
    clock_failed = true;
    const auto reply = service.access(basic(login));
    CHECK(reply.status == 500);
    CHECK_FALSE(reply.account.has_value());
}

TEST_CASE("Protected account contexts and grants remain separate between server databases") {
    hawken::test::TempDirectory temporary;
    TestVerifier a, b;
    hawken::store::LocalStore store_a(temporary.path() / "a.sqlite", "server-a");
    hawken::store::LocalStore store_b(temporary.path() / "b.sqlite", "server-b");
    AuthService service_a(store_a, a, [] { return 1000; });
    AuthService service_b(store_b, b, [] { return 1000; });
    const auto first = service_a.login(ticket_request);
    const auto second = service_b.login(ticket_request);
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    const auto own_a = service_a.access(basic(first));
    const auto own_b = service_b.access(basic(second));
    REQUIRE(own_a.account.has_value());
    REQUIRE(own_b.account.has_value());
    CHECK(own_a.account->account == store_a.find_account(fixture_id_text)->ref);
    CHECK(own_b.account->account == store_b.find_account(fixture_id_text)->ref);
    CHECK(own_a.account->account != own_b.account->account);
    CHECK(service_a.access(basic(second)).status == 401);
    CHECK_FALSE(service_a.access(basic(second)).account.has_value());
    CHECK(service_b.access(basic(first)).status == 401);
    CHECK_FALSE(service_b.access(basic(first)).account.has_value());
    CHECK(service_a.access(basic(first), guid(second)).status == 401);
    CHECK_FALSE(service_a.access(basic(first), guid(second)).account.has_value());
}
