#include "hawken/master_directory.hpp"
#include <doctest/doctest.h>
#include <array>

namespace {
using hawken::master::MasterDirectory;
using hawken::master::ServerMetadata;
using nlohmann::json;

ServerMetadata sample(std::string guid = "server-one") {
    return {std::move(guid), "South America", "fixture-build", "My Server", "MapOne",
            "Deathmatch", "127.0.0.1", 7777, 12, true, false};
}

template<class Action>
void expect_code(Action action, const char* expected) {
    try { action(); }
    catch (const hawken::master::DirectoryError& error) {
        CHECK(error.code == expected);
        CHECK(std::string(error.what()).find("never-echo-this") == std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("Expected sanitized DirectoryError"); return; }
    FAIL_CHECK("Invalid directory metadata was accepted");
}
}

TEST_CASE("Master lists server metadata without accounts profiles or progression") {
    const MasterDirectory directory({sample()});
    const auto response = directory.respond("GET", "/gameServerListings");
    CHECK(response.http_status == 200);
    CHECK(response.body.at("Status") == 200);
    CHECK(response.body.at("Message") == "");
    const auto& result = response.body.at("Result");
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);
    const json expected = {{"Guid", "server-one"}, {"MatchId", ""}, {"Region", "South America"},
        {"GameVersion", "fixture-build"}, {"ServerName", "My Server"}, {"Map", "MapOne"},
        {"GameType", "Deathmatch"}, {"Endpoint", "127.0.0.1"}, {"Port", "7777"},
        {"VoiceChannelListing", ""}, {"MaxUsers", 12}, {"MinUsers", 0}, {"ServerScore", 0},
        {"ServerRanking", 0}, {"MatchCompletionPercent", 0}, {"IsPublicVisible", true},
        {"IsMatchmakingVisible", false}, {"Users", json::array()}, {"AllowedRoles", json::array()},
        {"DeveloperData", json::object()}};
    CHECK(result[0] == expected);
    CHECK(result[0].at("Users").empty());
    CHECK(result[0].at("DeveloperData").empty());
}

TEST_CASE("Master returns an empty successful directory and filters public visibility") {
    const MasterDirectory empty({});
    CHECK(empty.respond("GET", "/gameServerListings").body.at("Result") == json::array());
    auto hidden = sample("hidden");
    hidden.is_public_visible = false;
    const MasterDirectory directory({sample(), hidden});
    CHECK(directory.respond("GET", "/gameServerListings").body.at("Result").size() == 1);
    CHECK(directory.respond("GET", "/gameServerListings/hidden").http_status == 404);
}

TEST_CASE("Master applies the five recovered filters conjunctively with one percent decode") {
    auto other = sample("server-two");
    other.region = "Europe";
    other.map = "MapTwo";
    other.game_type = "Siege";
    other.game_version = "other-build";
    other.server_name = "A+B%20";
    const MasterDirectory directory({sample(), other});
    const std::array targets = {
        "/gameServerListings?Region=South%20America", "/gameServerListings?GameVersion=fixture-build",
        "/gameServerListings?Map=MapOne", "/gameServerListings?GameType=Deathmatch",
        "/gameServerListings?ServerName=My%20Server",
        "/gameServerListings?Region=South%20America&Map=MapOne&GameType=Deathmatch&GameVersion=fixture-build&ServerName=My%20Server"};
    for (const auto* target : targets) {
        const auto response = directory.respond("GET", target);
        REQUIRE(response.http_status == 200);
        REQUIRE(response.body.at("Result").size() == 1);
        CHECK(response.body.at("Result")[0].at("Guid") == "server-one");
    }
    CHECK(directory.respond("GET", "/gameServerListings?Region=Europe&Map=MapOne").body.at("Result").empty());
    CHECK(directory.respond("GET", "/gameServerListings?ServerName=A%2BB%2520").body.at("Result")[0].at("Guid") == "server-two");
    CHECK(directory.respond("GET", "/gameServerListings?Region=").body.at("Result").size() == 2);
}

TEST_CASE("Master handles Unicode metadata in escaped UTF8 filter values") {
    auto metadata = sample();
    metadata.server_name = "\xC3\xA9";
    const MasterDirectory directory({metadata});
    const auto result = directory.respond("GET", "/gameServerListings?ServerName=%C3%A9");
    CHECK(result.http_status == 200);
    REQUIRE(result.body.at("Result").size() == 1);
    CHECK(result.body.at("Result")[0].at("ServerName") == "\xC3\xA9");
}

TEST_CASE("Master returns a listing object by opaque exact ID and does not expose other APIs") {
    const MasterDirectory directory({sample("server~one")});
    const auto response = directory.respond("GET", "/gameServerListings/server%7Eone");
    CHECK(response.http_status == 200);
    CHECK(response.body.at("Result").is_object());
    CHECK(response.body.at("Result").at("Guid") == "server~one");
    CHECK(directory.respond("GET", "/gameServerListings/SERVER~ONE").http_status == 404);
    for (const auto* target : {"/steam/loginAuthTicket", "/session", "/userGameServers/user", "/profiles/user", "/inventory/user"}) {
        const auto denied = directory.respond("GET", target);
        CHECK(denied.http_status == 404);
        CHECK(denied.body.at("Status") == 404);
        CHECK(denied.body.at("Result").is_null());
    }
    CHECK(directory.respond("POST", "/gameServerListings").http_status == 405);
}

TEST_CASE("Master rejects ambiguous or malformed queries without echoing attacker input") {
    const MasterDirectory directory({sample()});
    const std::array bad = {
        "/gameServerListings?Region=one&Region=two", "/gameServerListings?Region=one&%52egion=two",
        "/gameServerListings?UserId=never-echo-this", "/gameServerListings?Region=%",
        "/gameServerListings?Region=%0", "/gameServerListings?Region=%GG", "/gameServerListings?Region=%FF",
        "/gameServerListings?Region=%00", "/gameServerListings?Region=a%0Ab",
        "/gameServerListings?Region", "/gameServerListings?=one", "/gameServerListings?Region=one&",
        "/gameServerListings?Region=one#fragment", "/gameServerListings/server-one?Region=one",
        "/gameServerListings/server%2Fone", "/gameServerListings/%2E%2E"};
    for (const auto* target : bad) {
        const auto response = directory.respond("GET", target);
        CHECK(response.http_status == 400);
        CHECK(response.body.at("Status") == 400);
        CHECK(response.body.at("Result").is_null());
        CHECK(response.body.dump().find("never-echo-this") == std::string::npos);
    }
}

TEST_CASE("Master validates operator metadata before publishing it") {
    expect_code([] { MasterDirectory directory({sample(), sample()}); }, "duplicate_id");
    for (const auto* id : {"", "../server", "server/one", "never-echo-this?key=x"}) {
        expect_code([&] { MasterDirectory directory({sample(id)}); }, "invalid_metadata");
    }
    for (int variant = 0; variant != 7; ++variant) {
        auto metadata = sample();
        switch (variant) {
        case 0: metadata.port = 0; break;
        case 1: metadata.max_users = 0; break;
        case 2: metadata.endpoint = "https://never-echo-this"; break;
        case 3: metadata.endpoint = "999.0.0.1"; break;
        case 4: metadata.region.clear(); break;
        case 5: metadata.server_name = "never-echo-this\r\nheader"; break;
        case 6: metadata.map = "\xFF"; break;
        }
        expect_code([&] { MasterDirectory directory({metadata}); }, "invalid_metadata");
    }
}

TEST_CASE("Master limits a snapshot to 1000 server records") {
    std::vector<ServerMetadata> servers;
    for (int index = 0; index < 1000; ++index) servers.push_back(sample("server-" + std::to_string(index)));
    const MasterDirectory boundary(servers);
    CHECK(boundary.respond("GET", "/gameServerListings").body.at("Result").size() == 1000);
    servers.push_back(sample("server-1000"));
    expect_code([&] { MasterDirectory excessive(servers); }, "directory_limit");
}
