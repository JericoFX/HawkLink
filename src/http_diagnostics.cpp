#include "hawken/http_diagnostics.hpp"
#include <utility>

namespace hawken::http {
RequestDiagnostic classify_request(std::string_view method, std::string_view path) noexcept {
    const std::string_view method_label = method == "GET" ? "GET"
        : method == "POST" ? "POST" : method == "HEAD" ? "HEAD"
        : method == "PUT" ? "PUT" : "other";
    if (path.find_first_of("%?#") != std::string_view::npos
        || path.find('\0') != std::string_view::npos)
        return {"other", method_label};
    struct Route {
        std::string_view path;
        std::string_view category;
        std::string_view alternate_slash;
        std::string_view slash_category;
    };
    static constexpr Route routes[] = {
        {"/status/services/", "status-services", "/status/services", "status-services-unexpected-slash"},
        {"/status/game_client/", "status-game-client", "/status/game_client", "status-game-client-unexpected-slash"},
        {"/version", "version", "/version/", "version-unexpected-slash"},
        {"/steam/login", "steam-login", "/steam/login/", "steam-login-unexpected-slash"},
        {"/steam/create-account", "steam-create-account", "/steam/create-account/", "steam-create-account-unexpected-slash"},
        {"/steam/link-account", "steam-link-account", "/steam/link-account/", "steam-link-account-unexpected-slash"},
        {"/steam/client-dlc-installed", "steam-client-dlc-installed", "/steam/client-dlc-installed/", "steam-client-dlc-installed-unexpected-slash"},
        {"/gameServerListings", "listings", "/gameServerListings/", "listings-unexpected-slash"},
        {"/game-client/game-items", "game-items", "/game-client/game-items/", "game-items-unexpected-slash"},
        {"/game-client/game-offers", "game-offers", "/game-client/game-offers/", "game-offers-unexpected-slash"},
        {"/game-client/achievements/", "achievements", "/game-client/achievements", "achievements-unexpected-slash"},
        {"/game-client/achievement-rewards", "achievement-rewards", "/game-client/achievement-rewards/", "achievement-rewards-unexpected-slash"},
        {"/gameCurrency", "game-currency", "/gameCurrency/", "game-currency-unexpected-slash"},
        {"/meteorCurrency", "meteor-currency", "/meteorCurrency/", "meteor-currency-unexpected-slash"},
        {"/bulk", "bulk", "/bulk/", "bulk-unexpected-slash"},
        {"/gameClientEvent", "game-client-event", "/gameClientEvent/", "game-client-event-unexpected-slash"},
        {"/achievementsBatch", "achievements-batch", "/achievementsBatch/", "achievements-batch-unexpected-slash"},
        {"/gameItemsBatch", "game-items-batch", "/gameItemsBatch/", "game-items-batch-unexpected-slash"},
        {"/gameOffersBatch", "game-offers-batch", "/gameOffersBatch/", "game-offers-batch-unexpected-slash"},
        {"/session", "game-session", "/session/", "game-session-unexpected-slash"},
        {"/userStats/", "user-stats", "/userStats", "other"},
        {"/userGameItemStats/", "user-game-item-stats", "/userGameItemStats", "other"},
        {"/bundles/", "bundles", "/bundles", "bundles-unexpected-slash"}
    };
    for (const auto& route : routes) {
        if (path == route.path) return {route.category, method_label};
        if (path == route.alternate_slash) return {route.slash_category, method_label};
    }
    // Family labels identify only a public prefix, never a user/item suffix.
    // They neither validate an ID nor enable a route or an account operation.
    static constexpr std::pair<std::string_view, std::string_view> families[] = {
        {"/gameCurrency/", "game-currency"},
        {"/meteorCurrency/", "meteor-currency"},
        {"/game-client/achievements/", "achievements"},
        {"/users/", "user-profile-family"},
        {"/userPublicReadOnlyData/", "user-public-read-only"},
        {"/userGameSettings/", "user-game-settings"},
        {"/statOverflow/", "stat-overflow"},
        {"/userStats/", "user-stats"},
        {"/userGameItems/", "user-game-items"},
        {"/userGameItemsBroker/", "user-game-items-broker"},
        {"/userGameItemStats/", "user-game-item-stats"},
        {"/userGameTransaction/", "user-game-transaction"},
        {"/achievementRewardBatch/", "achievement-reward-batch"},
        {"/userGameItemsBatch/", "user-game-items-batch"},
        {"/userAchievementsQuery/", "user-achievements-query"},
        {"/userAchievements/", "user-achievements"},
        {"/bundles/", "bundles"},
        {"/game-client/", "game-client-other"}
    };
    for (const auto& [prefix, category] : families)
        if (path.starts_with(prefix) && path.size() > prefix.size())
            return {category, method_label};
    return {"other", method_label};
}
}
