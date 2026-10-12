#include "hawken/http_diagnostics.hpp"
#include <ostream>
#include <doctest/doctest.h>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

using hawken::http::classify_request;

TEST_CASE("Client event diagnostics identify only the original fixed endpoint") {
    std::string path = "/gameClientEvent";
    const auto result = classify_request("POST", path);
    path.assign(100, 'x');
    CHECK(result.category == "game-client-event");
    CHECK(result.method == "POST");
    CHECK(classify_request("POST", "/gameClientEvent/").category
        == "game-client-event-unexpected-slash");
    for (const auto invalid : {"/gameClientEvent/private", "/gameClientEvents",
        "/GameClientEvent", "/gameClientEvent?ticket=private",
        "/gameClientEvent#private", "/gameClient%45vent"})
        CHECK(classify_request("POST", invalid).category == "other");
    path = "/gameClientEvent";
    path.push_back('\0');
    path += "private";
    CHECK(classify_request("POST", path).category == "other");
}

TEST_CASE("Batch diagnostics distinguish catalogue operations without exposing suffixes") {
    for (const auto& [path, category] : std::array{
        std::pair{"/achievementsBatch", "achievements-batch"},
        std::pair{"/gameItemsBatch", "game-items-batch"},
        std::pair{"/gameOffersBatch", "game-offers-batch"},
        std::pair{"/achievementRewardBatch/private-account", "achievement-reward-batch"},
        std::pair{"/userGameItemsBatch/private-account", "user-game-items-batch"}}) {
        for (const auto method : {"GET", "POST", "PUT"}) {
            std::string input(path);
            const auto result = classify_request(method, input);
            input.clear();
            CHECK(result.category == category);
            CHECK(result.method == method);
            CHECK(result.category.find("private") == std::string_view::npos);
        }
    }
    for (const auto path : {"/gameItemsBatch/private", "/GameItemsBatch",
        "/userGameItemsBatchSuffix/private", "/achievementRewardBatch/",
        "/gameOffersBatch?token=private", "/userGameItemsBatch/private%2Faccount"})
        CHECK(classify_request("POST", path).category == "other");
}

TEST_CASE("HTTP diagnostics identify the eight baseline exact routes") {
    const std::array routes{
        std::pair{"/status/services/", "status-services"},
        std::pair{"/status/game_client/", "status-game-client"},
        std::pair{"/version", "version"},
        std::pair{"/steam/login", "steam-login"},
        std::pair{"/steam/create-account", "steam-create-account"},
        std::pair{"/steam/link-account", "steam-link-account"},
        std::pair{"/steam/client-dlc-installed", "steam-client-dlc-installed"},
        std::pair{"/gameServerListings", "listings"}
    };
    for (const auto& [path, category] : routes) {
        const auto result = classify_request("GET", path);
        CHECK(result.category == category);
        CHECK(result.method == "GET");
    }
}

TEST_CASE("HTTP diagnostics distinguish a single unexpected final slash without rewriting") {
    const std::array routes{
        std::pair{"/status/services", "status-services-unexpected-slash"},
        std::pair{"/status/game_client", "status-game-client-unexpected-slash"},
        std::pair{"/version/", "version-unexpected-slash"},
        std::pair{"/steam/login/", "steam-login-unexpected-slash"},
        std::pair{"/steam/create-account/", "steam-create-account-unexpected-slash"},
        std::pair{"/steam/link-account/", "steam-link-account-unexpected-slash"},
        std::pair{"/steam/client-dlc-installed/", "steam-client-dlc-installed-unexpected-slash"},
        std::pair{"/gameServerListings/", "listings-unexpected-slash"}
    };
    for (const auto& [path, category] : routes)
        CHECK(classify_request("POST", path).category == category);
    CHECK(classify_request("GET", "/steam/login//").category == "other");
    CHECK(classify_request("GET", "//steam/login").category == "other");
    CHECK(classify_request("GET", "/status/services//").category == "other");
}

TEST_CASE("HTTP diagnostics emit fixed method labels independently of route") {
    CHECK(classify_request("GET", "/steam/login").method == "GET");
    CHECK(classify_request("POST", "/steam/login").method == "POST");
    CHECK(classify_request("HEAD", "/steam/login").method == "HEAD");
    CHECK(classify_request("PUT", "/steam/login").method == "PUT");
    for (const std::string_view method : {"DELETE", "OPTIONS", "get", "", "private-method"}) {
        const auto result = classify_request(method, "/steam/login");
        CHECK(result.method == "other");
        CHECK(result.category == "steam-login");
    }
}

TEST_CASE("Postmatch diagnostics distinguish transactions and stat writes without identifiers") {
    for (const auto& [path, category] : std::array{
        std::pair{"/userGameTransaction/private-user", "user-game-transaction"},
        std::pair{"/userGameTransaction/private-transaction", "user-game-transaction"},
        std::pair{"/userStats/", "user-stats"},
        std::pair{"/userGameItemStats/", "user-game-item-stats"}}) {
        for (const auto method : {"GET", "POST", "PUT"}) {
            std::string owned(path);
            const auto result = classify_request(method, owned);
            owned.clear();
            CHECK(result.category == category);
            CHECK(result.method == method);
            CHECK(result.category.find("private") == std::string_view::npos);
        }
    }
    for (const auto path : {"/userGameTransactionSuffix/private-user", "/UserGameTransaction/private-user",
                           "/userGameTransaction/private-user?ticket=private", "/userGameTransaction/private%2Fuser"})
        CHECK(classify_request("POST", path).category == "other");
}

TEST_CASE("HTTP diagnostics never identify dynamic IDs encoded paths or URI queries") {
    for (const std::string_view path : {
        "/gameServerListings/private-server-id", "/steam/login/private-account",
        "/steam/Login", "/Version",
        "/steam%2Flogin", "/steam/login?ticket=private-ticket",
        "/version#private-fragment", "", "https://private-host/steam/login"})
        CHECK(classify_request("GET", path).category == "other");
}

TEST_CASE("HTTP sync diagnostics identify exact public routes without changing responses") {
    const std::array routes{
        std::pair{"/game-client/game-items", "game-items"},
        std::pair{"/game-client/game-offers", "game-offers"},
        std::pair{"/game-client/achievements/", "achievements"},
        std::pair{"/game-client/achievement-rewards", "achievement-rewards"},
        std::pair{"/gameCurrency", "game-currency"},
        std::pair{"/meteorCurrency", "meteor-currency"},
        std::pair{"/bulk", "bulk"},
        std::pair{"/session", "game-session"}
    };
    for (const auto& [path, category] : routes) {
        CHECK(classify_request("GET", path).category == category);
        CHECK(classify_request("POST", path).category == category);
    }
    CHECK(classify_request("GET", "/game-client/game-items/").category == "game-items-unexpected-slash");
    CHECK(classify_request("GET", "/game-client/game-offers/").category == "game-offers-unexpected-slash");
    CHECK(classify_request("GET", "/game-client/achievements").category == "achievements-unexpected-slash");
}

TEST_CASE("HTTP sync diagnostics redact entire dynamic suffixes into fixed family labels") {
    const std::array routes{
        std::pair{"/users/private-account", "user-profile-family"},
        std::pair{"/userPublicReadOnlyData/private-account", "user-public-read-only"},
        std::pair{"/statOverflow/private-account", "stat-overflow"},
        std::pair{"/userStats/private-steam-id", "user-stats"},
        std::pair{"/userGameItems/private-account/private-item", "user-game-items"},
        std::pair{"/userGameItemStats/private-account/private-item", "user-game-item-stats"},
        std::pair{"/userAchievementsQuery/private-account", "user-achievements-query"},
        std::pair{"/bundles/private-account", "bundles"},
        std::pair{"/game-client/private-resource", "game-client-other"}
    };
    for (const auto& [path, category] : routes) {
        std::string input = path;
        const auto result = classify_request("GET", input);
        input.assign(150, 'x');
        CHECK(result.category == category);
        CHECK(result.category.find("private") == std::string_view::npos);
        CHECK(result.category.find('/') == std::string_view::npos);
        CHECK(result.method == "GET");
    }
    CHECK(classify_request("GET", "/userStats").category == "other");
    CHECK(classify_request("GET", "/userStatsSuffix/private-account").category == "other");
    CHECK(classify_request("GET", "/UserStats/private-account").category == "other");
}

TEST_CASE("HTTP sync diagnostics do not treat unsafe raw paths as recovered families") {
    for (const std::string_view path : {
        "/userStats/private-account?grant=private-grant",
        "/userStats/private-account#private-fragment",
        "/userStats/private%2Faccount", "https://private-host/userStats/private-account",
        "/game-client/game-items?ticket=private-ticket"})
        CHECK(classify_request("POST", path).category == "other");
    std::string path = "/userStats/private-account";
    path.push_back('\0');
    path += "private-ticket";
    CHECK(classify_request("GET", path).category == "other");
}

TEST_CASE("HTTP diagnostics retain no input storage and emit no private content") {
    std::string method = "private-ticket-in-method";
    std::string path = "/steam/login/private-account?ticket=private-ticket";
    const auto result = classify_request(method, path);
    method.assign(100, 'x');
    path.clear();
    CHECK(result.method == "other");
    CHECK(result.category == "other");
    const std::string output = std::string(result.method) + " " + std::string(result.category);
    CHECK(output.find("private") == std::string::npos);
    CHECK(output.find("ticket") == std::string::npos);
    CHECK(output.find('/') == std::string::npos);
    CHECK(output.find('?') == std::string::npos);
    method = "HEAD";
    path = "/steam/login";
    const auto known = classify_request(method, path);
    method.clear();
    path.assign(100, 'x');
    CHECK(known.method == "HEAD");
    CHECK(known.category == "steam-login");
}

TEST_CASE("HTTP diagnostics do not truncate inputs at embedded NUL") {
    std::string path = "/steam/login";
    path.push_back('\0');
    path += "private-ticket";
    std::string method = "GET";
    method.push_back('\0');
    method += "private-method";
    CHECK(classify_request("GET", path).category == "other");
    CHECK(classify_request(method, "/steam/login").method == "other");
}

TEST_CASE("Initial sync wallet and region diagnostics use fixed categories without suffixes") {
    for (const auto& [path, category] : std::array{
        std::pair{"/gameCurrency/private-account", "game-currency"},
        std::pair{"/meteorCurrency/private-account", "meteor-currency"},
        std::pair{"/game-client/achievements/EU", "achievements"},
        std::pair{"/game-client/achievements/private-region", "achievements"}}) {
        std::string input = path;
        const auto result = classify_request("GET", input);
        input.clear();
        CHECK(result.category == category);
        CHECK(result.category.find("private") == std::string_view::npos);
        CHECK(result.method == "GET");
    }
    for (const auto path : {"/gameCurrencySuffix/private-account", "/MeteorCurrency/private-account",
        "/gameCurrency/private-account?x=private-grant", "/meteorCurrency/private%2Faccount",
        "/game-client/achievements/EU#private-fragment"})
        CHECK(classify_request("GET", path).category == "other");
}

TEST_CASE("Remaining menu diagnostics redact account suffixes and identify absent bundles") {
    for (const auto& [path, category] : std::array{
        std::pair{"/userGameSettings/private-account", "user-game-settings"},
        std::pair{"/userAchievements/private-account", "user-achievements"}}) {
        std::string input = path;
        const auto result = classify_request("GET", input);
        input.clear();
        CHECK(result.category == category);
        CHECK(result.category.find("private") == std::string_view::npos);
        CHECK(result.category.find('/') == std::string_view::npos);
        CHECK(result.method == "GET");
    }
    CHECK(classify_request("GET", "/bundles/").category == "bundles");
    CHECK(classify_request("GET", "/bundles").category == "bundles-unexpected-slash");
    for (const auto path : {"/userGameSettingsSuffix/private-account", "/UserAchievements/private-account",
                           "/userAchievements/private-account?countryCode=private-region",
                           "/userGameSettings/private%2Faccount"})
        CHECK(classify_request("GET", path).category == "other");
}
