#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hawken::master {
class DirectoryError : public std::runtime_error {
public:
    explicit DirectoryError(std::string error_code);
    std::string code;
};

// This directory has no account, inventory, balance, session or player-ID fields.
struct ServerMetadata {
    std::string guid;
    std::string region;
    std::string game_version;
    std::string server_name;
    std::string map;
    std::string game_type;
    std::string endpoint;
    std::uint16_t port = 0;
    std::int32_t max_users = 0;
    bool is_public_visible = true;
    bool is_matchmaking_visible = false;
};

struct DirectoryResponse {
    int http_status = 500;
    nlohmann::json body;
};

class MasterDirectory {
public:
    explicit MasterDirectory(std::vector<ServerMetadata> servers);
    DirectoryResponse respond(std::string_view method, std::string_view raw_target) const;
private:
    std::vector<ServerMetadata> servers_;
};
}
