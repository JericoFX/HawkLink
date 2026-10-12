#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hawken::meteor {
bool result_success(std::uint32_t http_status, std::uint32_t api_status, std::uint32_t parse_status);
nlohmann::json envelope(std::uint32_t status, std::string_view message, const nlohmann::json& result);

struct GameServerListing {
    std::string guid;
    std::string match_id;
    std::string region;
    std::string game_version;
    std::string server_name;
    std::string map;
    std::string game_type;
    std::string endpoint;
    std::string port;
    std::string voice_channel_listing;
    std::int32_t max_users = 0;
    std::int32_t min_users = 0;
    std::int32_t server_score = 0;
    std::int32_t server_ranking = 0;
    std::int32_t match_completion_percent = 0;
    bool is_matchmaking_visible = false;
    bool is_public_visible = false;
    std::vector<std::string> users;
    std::vector<std::string> allowed_roles;
    std::map<std::string, std::string> developer_data;
};
nlohmann::json listing_json(const GameServerListing& listing);

struct ClientSession {
    std::string user_guid;
    std::string session_id;
    std::string session_key;
    std::string assigned_server_ip;
    std::string assigned_server_guid;
    std::string client_advertisement_guid;
    std::int32_t assigned_server_port = 0;
};
nlohmann::json session_json(const ClientSession& session);
std::string escape_component(std::string_view utf8);
}
