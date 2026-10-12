#include "hawken/sync_service.hpp"
#include <optional>
#include <stdexcept>

namespace {
constexpr std::string_view overflow_path =
    "/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95";
constexpr std::string_view achievements_prefix = "/game-client/achievements/";
constexpr std::string_view game_currency_prefix = "/gameCurrency/";
constexpr std::string_view meteor_currency_prefix = "/meteorCurrency/";

enum class TargetKind { unknown, overflow, rewards, achievements, game_currency, meteor_currency };
struct Target {
    TargetKind kind = TargetKind::unknown;
    std::optional<std::string_view> guid;
};
bool region_valid(std::string_view region) noexcept {
    if (region.size() > 64) return false;
    for (const char value : region) {
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-'))
            return false;
    }
    return true;
}
bool guid_valid(std::string_view guid) noexcept {
    if (guid.size() != 36) return false;
    for (std::size_t index = 0; index < guid.size(); ++index) {
        const char value = guid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') return false;
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}
Target parse_target(std::string_view raw_target) noexcept {
    if (raw_target == overflow_path) return {TargetKind::overflow, std::nullopt};
    if (raw_target == "/game-client/achievement-rewards") return {TargetKind::rewards, std::nullopt};
    if (raw_target.starts_with(achievements_prefix)
        && region_valid(raw_target.substr(achievements_prefix.size())))
        return {TargetKind::achievements, std::nullopt};
    if (raw_target.starts_with(game_currency_prefix)) {
        const auto guid = raw_target.substr(game_currency_prefix.size());
        if (guid_valid(guid)) return {TargetKind::game_currency, guid};
    }
    if (raw_target.starts_with(meteor_currency_prefix)) {
        const auto guid = raw_target.substr(meteor_currency_prefix.size());
        if (guid_valid(guid)) return {TargetKind::meteor_currency, guid};
    }
    return {};
}
hawken::auth::AuthReply failure(int status) {
    switch (status) {
    case 401: return {401, "Authentication required.", nullptr};
    case 403: return {403, "Access was rejected.", nullptr};
    case 404: return {404, "Resource not found.", nullptr};
    default: return {500, "Request could not be processed.", nullptr};
    }
}
}

namespace hawken::sync {
SyncService::SyncService(store::LocalStore& store, auth::AuthService& auth,
                         std::int32_t overflow_interval)
    : store_(store), auth_(auth), overflow_interval_(overflow_interval) {
    if (overflow_interval <= 0) throw std::invalid_argument("Invalid overflow interval.");
}
bool SyncService::supports_target(std::string_view raw_target) const {
    return parse_target(raw_target).kind != TargetKind::unknown;
}
auth::AuthReply SyncService::get(std::string_view raw_target, std::string_view authorization) {
    const auto target = parse_target(raw_target);
    if (target.kind == TargetKind::unknown) return failure(404);
    try {
        const auto access = auth_.access(authorization, target.guid);
        if (access.status != 200) return failure(access.status);
        if (!access.account) return failure(500);
        switch (target.kind) {
        case TargetKind::overflow:
            return {200, "", {{"OverflowId", "32f2f08c-87f6-45a3-8440-a84242907a95"},
                              {"StatPerCurrency", overflow_interval_}}};
        case TargetKind::rewards:
        case TargetKind::achievements:
            return {200, "", nlohmann::json::array()};
        case TargetKind::game_currency:
        case TargetKind::meteor_currency: {
            const auto balances = store_.wallet(access.account->account);
            return {200, "", target.kind == TargetKind::game_currency
                ? nlohmann::json(balances.hawken_points) : nlohmann::json(balances.meteor_points)};
        }
        case TargetKind::unknown:
            return failure(404);
        }
    } catch (...) {
        return failure(500);
    }
    return failure(500);
}
}
