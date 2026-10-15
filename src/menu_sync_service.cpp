#include "hawken/menu_sync_service.hpp"
#include <array>
#include <optional>
#include <set>
#include <stdexcept>

namespace {
enum class TargetKind { unknown, empty_object, settings, empty_array, unlocked, user_stats, item_stats };
struct Target {
    TargetKind kind = TargetKind::unknown;
    std::optional<std::string_view> guid;
};
struct UserPrefix {
    std::string_view path;
    TargetKind kind;
};
constexpr std::array<UserPrefix, 5> user_prefixes{{
    {"/userPublicReadOnlyData/", TargetKind::empty_object},
    {"/userGameSettings/", TargetKind::settings},
    {"/userGameItemStats/", TargetKind::item_stats},
    {"/userStats/", TargetKind::user_stats},
    {"/userAchievements/", TargetKind::unlocked}}};
constexpr std::string_view region_query = "?countryCode=";

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
bool region_valid(std::string_view region) noexcept {
    if (region.empty() || region.size() > 64) return false;
    for (const char value : region) {
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-'))
            return false;
    }
    return true;
}
Target parse_target(std::string_view raw_target) noexcept {
    if (raw_target == "/bundles/") return {TargetKind::empty_array, std::nullopt};
    for (const auto& prefix : user_prefixes) {
        if (!raw_target.starts_with(prefix.path)) continue;
        auto guid = raw_target.substr(prefix.path.size());
        if (prefix.kind == TargetKind::unlocked) {
            if (guid.size() < 36) return {};
            const auto query = guid.substr(36);
            if (!query.starts_with(region_query)
                || !region_valid(query.substr(region_query.size()))) return {};
            guid = guid.substr(0, 36);
        }
        if (!guid_valid(guid)) return {};
        return {prefix.kind, guid};
    }
    return {};
}
hawken::sync::MenuSyncReply failure(int status) {
    switch (status) {
    case 400: return {400, 400, "Invalid settings request.", nullptr};
    case 401: return {401, 401, "Authentication required.", nullptr};
    case 403: return {403, 403, "Access was rejected.", nullptr};
    case 404: return {404, 404, "Resource not found.", nullptr};
    default: return {500, 500, "Request could not be processed.", nullptr};
    }
}
}

namespace hawken::sync {
MenuSyncService::MenuSyncService(auth::AuthService& auth, store::LocalStore& store)
    : auth_(auth), store_(store) {}

bool MenuSyncService::supports_settings_target(std::string_view raw_target) const {
    return parse_target(raw_target).kind == TargetKind::settings;
}
MenuSyncReply MenuSyncService::put(std::string_view raw_target, std::string_view authorization,
                                 std::string_view body) {
    const auto target = parse_target(raw_target);
    if (target.kind != TargetKind::settings) return failure(404);
    try {
        const auto access = auth_.access(authorization, target.guid);
        if (access.status != 200) return failure(access.status);
        if (!access.account) return failure(500);
        if (body.size() > 65536) return failure(400);
        store::GameSettings update;
        try {
            std::set<std::string, std::less<>> keys;
            const auto parsed = nlohmann::json::parse(body, [&keys](int depth,
                nlohmann::json::parse_event_t event, nlohmann::json& value) {
                using Event = nlohmann::json::parse_event_t;
                if (event == Event::array_start || (event == Event::object_start && depth != 0))
                    throw std::invalid_argument("Invalid settings request.");
                if (event == Event::key && (!keys.insert(value.get<std::string>()).second || keys.size() > 256))
                    throw std::invalid_argument("Invalid settings request.");
                return true;
            });
            if (!parsed.is_object()) return failure(400);
            for (const auto& [key, value] : parsed.items()) {
                if (!value.is_string()) return failure(400);
                update.emplace(key, value.get<std::string>());
            }
        } catch (const nlohmann::json::exception&) { return failure(400); }
          catch (const std::invalid_argument&) { return failure(400); }
        store_.merge_game_settings(access.account->account, update);
        return {200, 200, "", nullptr};
    } catch (const store::StoreError& error) {
        return failure(error.code() == store::StoreErrorCode::invalid_input ? 400 : 500);
    } catch (...) {
        return failure(500);
    }
}

bool MenuSyncService::supports_target(std::string_view raw_target) const {
    return parse_target(raw_target).kind != TargetKind::unknown;
}

MenuSyncReply MenuSyncService::get(std::string_view raw_target, std::string_view authorization) {
    const auto target = parse_target(raw_target);
    if (target.kind == TargetKind::unknown) return failure(404);
    try {
        const auto access = auth_.access(authorization, target.guid);
        if (access.status != 200) return failure(access.status);
        if (!access.account) return failure(500);
        switch (target.kind) {
        case TargetKind::settings:
            return {200, 200, "", nlohmann::json(store_.game_settings(access.account->account))};
        case TargetKind::empty_object:
            return {200, 200, "", nlohmann::json::object()};
        case TargetKind::empty_array:
            return {200, 200, "", nlohmann::json::array()};
        case TargetKind::user_stats:
            return {200, 200, "", nlohmann::json(store_.account_online_snapshot(access.account->account).unranked_gameplay_stats)};
        case TargetKind::item_stats: {
            const auto snapshot = store_.account_online_snapshot(access.account->account);
            auto rows = nlohmann::json::array();
            for (const auto& [slot, stats] : snapshot.item_stats_by_slot) {
                if (!snapshot.starter || slot >= snapshot.starter->items.size()) return failure(500);
                nlohmann::json row(stats);
                row["UserGuid"] = access.account->user_guid;
                row["UserGameItemGuid"] = snapshot.starter->items.at(slot).instance_id;
                rows.push_back(std::move(row));
            }
            return {200, 200, "", std::move(rows)};
        }
        case TargetKind::unlocked:
            return {200, 404, "Resource not found.", nlohmann::json::array()};
        case TargetKind::unknown:
            return failure(404);
        }
    } catch (...) {
        return failure(500);
    }
    return failure(500);
}
}
