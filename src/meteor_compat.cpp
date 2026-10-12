#include "hawken/meteor_compat.hpp"

namespace hawken::meteor {
bool result_success(std::uint32_t http_status, std::uint32_t api_status, std::uint32_t parse_status) {
    return http_status >= 200U && http_status <= 299U &&
           api_status >= 200U && api_status <= 299U &&
           (parse_status == 0U || (parse_status >= 200U && parse_status <= 299U));
}

nlohmann::json envelope(std::uint32_t status, std::string_view message, const nlohmann::json& result) {
    return {{"Status", status}, {"Message", std::string(message)}, {"Result", result}};
}

nlohmann::json listing_json(const GameServerListing& listing) {
    return {
        {"Guid", listing.guid},
        {"MatchId", listing.match_id},
        {"Region", listing.region},
        {"GameVersion", listing.game_version},
        {"ServerName", listing.server_name},
        {"Map", listing.map},
        {"GameType", listing.game_type},
        {"Endpoint", listing.endpoint},
        {"Port", listing.port},
        {"VoiceChannelListing", listing.voice_channel_listing},
        {"MaxUsers", listing.max_users},
        {"MinUsers", listing.min_users},
        {"ServerScore", listing.server_score},
        {"ServerRanking", listing.server_ranking},
        {"MatchCompletionPercent", listing.match_completion_percent},
        {"IsMatchmakingVisible", listing.is_matchmaking_visible},
        {"IsPublicVisible", listing.is_public_visible},
        {"Users", listing.users},
        {"AllowedRoles", listing.allowed_roles},
        {"DeveloperData", listing.developer_data}};
}

nlohmann::json session_json(const ClientSession& session) {
    return {
        {"UserGuid", session.user_guid},
        {"SessionId", session.session_id},
        {"SessionKey", session.session_key},
        {"AssignedServerIp", session.assigned_server_ip},
        {"AssignedServerGuid", session.assigned_server_guid},
        {"ClientAdvertisementGuid", session.client_advertisement_guid},
        {"AssignedServerPort", session.assigned_server_port}};
}

std::string escape_component(std::string_view utf8) {
    constexpr std::string_view safe = "-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~";
    constexpr char hex[] = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(utf8.size());
    for (char character : utf8) {
        if (safe.find(character) != std::string_view::npos) {
            escaped.push_back(character);
        } else {
            const auto byte = static_cast<unsigned char>(character);
            escaped.push_back('%');
            escaped.push_back(hex[byte >> 4]);
            escaped.push_back(hex[byte & 0x0f]);
        }
    }
    return escaped;
}
}
