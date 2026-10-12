#include "hawken/meteor_compat.hpp"
#include <doctest/doctest.h>
#include <limits>

namespace {
using nlohmann::json;

void check_listing_types(const json& value) {
    for (const auto* key : {"Guid", "MatchId", "Region", "GameVersion", "ServerName", "Map",
                            "GameType", "Endpoint", "Port", "VoiceChannelListing"}) {
        CHECK(value.at(key).is_string());
    }
    for (const auto* key : {"MaxUsers", "MinUsers", "ServerScore", "ServerRanking", "MatchCompletionPercent"}) {
        CHECK(value.at(key).is_number_integer());
        CHECK_FALSE(value.at(key).is_number_unsigned());
    }
    CHECK(value.at("IsMatchmakingVisible").is_boolean());
    CHECK(value.at("IsPublicVisible").is_boolean());
    for (const auto* key : {"Users", "AllowedRoles"}) {
        REQUIRE(value.at(key).is_array());
        for (const auto& item : value.at(key)) CHECK(item.is_string());
    }
    REQUIRE(value.at("DeveloperData").is_object());
    for (const auto& item : value.at("DeveloperData")) CHECK(item.is_string());
}
}

TEST_CASE("Meteor listing preserves exact recovered casing values and field types") {
    hawken::meteor::GameServerListing listing;
    listing.guid = "listing-fixture";
    listing.match_id = "match-fixture";
    listing.region = "World";
    listing.game_version = "fixture-build";
    listing.server_name = "Fixture Server";
    listing.map = "fixture-map";
    listing.game_type = "fixture-mode";
    listing.endpoint = "127.0.0.1";
    listing.port = "07777";
    listing.voice_channel_listing = "fixture-voice";
    listing.max_users = 12;
    listing.min_users = 2;
    listing.server_score = -5;
    listing.server_ranking = 8;
    listing.match_completion_percent = 37;
    listing.is_matchmaking_visible = true;
    listing.is_public_visible = false;
    listing.users = {"user-one", "user-two"};
    listing.allowed_roles = {"role-one", "role-two"};
    listing.developer_data = {{"AveragePilotLevel", "0"}, {"PasswordHash", "fixture-only"}};
    const json expected = {
        {"Guid", "listing-fixture"}, {"MatchId", "match-fixture"}, {"Region", "World"},
        {"GameVersion", "fixture-build"}, {"ServerName", "Fixture Server"}, {"Map", "fixture-map"},
        {"GameType", "fixture-mode"}, {"Endpoint", "127.0.0.1"}, {"Port", "07777"},
        {"VoiceChannelListing", "fixture-voice"}, {"MaxUsers", 12}, {"MinUsers", 2},
        {"ServerScore", -5}, {"ServerRanking", 8}, {"MatchCompletionPercent", 37},
        {"IsMatchmakingVisible", true}, {"IsPublicVisible", false},
        {"Users", json::array({"user-one", "user-two"})},
        {"AllowedRoles", json::array({"role-one", "role-two"})},
        {"DeveloperData", {{"AveragePilotLevel", "0"}, {"PasswordHash", "fixture-only"}}}};

    const auto actual = hawken::meteor::listing_json(listing);

    CHECK(actual == expected);
    CHECK(actual.size() == 20);
    check_listing_types(actual);
}

TEST_CASE("Meteor default listing retains arrays objects strings and zero false fields") {
    const auto actual = hawken::meteor::listing_json({});
    const json expected = {
        {"Guid", ""}, {"MatchId", ""}, {"Region", ""}, {"GameVersion", ""}, {"ServerName", ""},
        {"Map", ""}, {"GameType", ""}, {"Endpoint", ""}, {"Port", ""}, {"VoiceChannelListing", ""},
        {"MaxUsers", 0}, {"MinUsers", 0}, {"ServerScore", 0}, {"ServerRanking", 0},
        {"MatchCompletionPercent", 0}, {"IsMatchmakingVisible", false}, {"IsPublicVisible", false},
        {"Users", json::array()}, {"AllowedRoles", json::array()}, {"DeveloperData", json::object()}};

    CHECK(actual == expected);
    check_listing_types(actual);
}

TEST_CASE("Meteor session preserves all required fields and a signed numeric destination port") {
    hawken::meteor::ClientSession session;
    session.user_guid = "user-fixture";
    session.session_id = "id-fixture";
    session.session_key = "synthetic-key";
    session.assigned_server_ip = "127.0.0.1";
    session.assigned_server_guid = "server-fixture";
    session.client_advertisement_guid = "advertisement-fixture";
    session.assigned_server_port = 7777;
    const json expected = {{"UserGuid", "user-fixture"}, {"SessionId", "id-fixture"},
        {"SessionKey", "synthetic-key"}, {"AssignedServerIp", "127.0.0.1"},
        {"AssignedServerGuid", "server-fixture"}, {"ClientAdvertisementGuid", "advertisement-fixture"},
        {"AssignedServerPort", 7777}};

    const auto actual = hawken::meteor::session_json(session);

    CHECK(actual == expected);
    CHECK(actual.size() == 7);
    for (const auto* key : {"UserGuid", "SessionId", "SessionKey", "AssignedServerIp",
                            "AssignedServerGuid", "ClientAdvertisementGuid"}) {
        CHECK(actual.at(key).is_string());
    }
    CHECK(actual.at("AssignedServerPort").is_number_integer());
    CHECK_FALSE(actual.at("AssignedServerPort").is_number_unsigned());
    CHECK_FALSE(actual.contains("Port"));
}

TEST_CASE("Meteor session defaults emit required strings without inventing an issued session") {
    const auto actual = hawken::meteor::session_json({});
    const json expected = {{"UserGuid", ""}, {"SessionId", ""}, {"SessionKey", ""},
        {"AssignedServerIp", ""}, {"AssignedServerGuid", ""}, {"ClientAdvertisementGuid", ""},
        {"AssignedServerPort", 0}};
    CHECK(actual == expected);
    CHECK(actual.at("AssignedServerPort").is_number_integer());
}

TEST_CASE("Meteor envelope keeps Result as its original JSON value and requires Message") {
    json result;
    SUBCASE("Object") { result = {{"nested", json::array({1, "two", nullptr})}}; }
    SUBCASE("Array") { result = json::array({{{"Guid", "listing-fixture"}}, "literal", nullptr}); }
    SUBCASE("String resembling JSON") { result = "{\"Guid\":\"literal\"}"; }
    SUBCASE("Null") { result = nullptr; }

    const auto actual = hawken::meteor::envelope(200, "", result);

    REQUIRE(actual.is_object());
    CHECK(actual.size() == 3);
    CHECK(actual.at("Status") == 200);
    REQUIRE(actual.at("Message").is_string());
    CHECK(actual.at("Message") == "");
    CHECK(actual.at("Result").type() == result.type());
    CHECK(actual.at("Result") == result);
    CHECK_FALSE(actual.contains("status"));
    CHECK_FALSE(actual.contains("message"));
    CHECK_FALSE(actual.contains("result"));
}

TEST_CASE("Meteor envelope preserves error status and message without choosing success") {
    const auto actual = hawken::meteor::envelope(500, "fixture failure", nullptr);
    const json expected = {{"Status", 500}, {"Message", "fixture failure"}, {"Result", nullptr}};
    CHECK(actual == expected);
    CHECK(actual.at("Message").is_string());
}

TEST_CASE("Meteor success predicate honors all three independently recovered status domains") {
    struct StatusCase { std::uint32_t http; std::uint32_t api; std::uint32_t parse; bool success; };
    constexpr StatusCase cases[] = {
        {200, 200, 0, true}, {299, 299, 0, true}, {200, 299, 0, true}, {299, 200, 0, true},
        {200, 200, 200, true}, {200, 200, 299, true}, {299, 299, 200, true}, {299, 299, 299, true},
        {199, 200, 0, false}, {300, 200, 0, false}, {0, 200, 0, false},
        {200, 199, 0, false}, {200, 300, 0, false}, {200, 0, 0, false},
        {200, 200, 199, false}, {200, 200, 300, false}, {200, 200, 500, false},
        {0, 0, 0, false}, {199, 299, 200, false}, {299, 300, 200, false},
        {std::numeric_limits<std::uint32_t>::max(), 200, 0, false},
        {200, std::numeric_limits<std::uint32_t>::max(), 0, false},
        {200, 200, std::numeric_limits<std::uint32_t>::max(), false}};
    for (const auto& value : cases) {
        CAPTURE(value.http);
        CAPTURE(value.api);
        CAPTURE(value.parse);
        CHECK(hawken::meteor::result_success(value.http, value.api, value.parse) == value.success);
    }
}

TEST_CASE("Meteor component escaping preserves exactly the recovered safe characters") {
    constexpr auto safe = "-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~";
    CHECK(hawken::meteor::escape_component(safe) == safe);
    CHECK(hawken::meteor::escape_component("").empty());
}

TEST_CASE("Meteor component escaping encodes separators reserved characters and literal percent") {
    CHECK(hawken::meteor::escape_component("a b+c%/?#") == "a%20b%2Bc%25%2F%3F%23");
    CHECK(hawken::meteor::escape_component("\\&=:@[]") == "%5C%26%3D%3A%40%5B%5D");
}

TEST_CASE("Meteor component escaping uses uppercase percent hex for each UTF8 byte") {
    const std::string utf8 = "\xC3\xA9\xF0\x9F\x98\x80";
    CHECK(hawken::meteor::escape_component(utf8) == "%C3%A9%F0%9F%98%80");
}

TEST_CASE("Meteor component escaping does not decode or normalize an already encoded input") {
    CHECK(hawken::meteor::escape_component("%2F") == "%252F");
    CHECK(hawken::meteor::escape_component("%2f+%20") == "%252f%2B%2520");
}

TEST_CASE("Meteor component escaping retains embedded NUL and treats input as bytes") {
    CHECK(hawken::meteor::escape_component(std::string_view("a\0b", 3)) == "a%00b");
    const std::string bytes = "\x80\xFF\n";
    CHECK(hawken::meteor::escape_component(bytes) == "%80%FF%0A");
}
