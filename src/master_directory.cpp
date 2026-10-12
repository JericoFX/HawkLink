#include "hawken/master_directory.hpp"
#include "hawken/meteor_compat.hpp"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <utility>

namespace hawken::master {
namespace {
[[noreturn]] void fail(const char* code) { throw DirectoryError(code); }

bool valid_text(std::string_view text, std::size_t maximum, bool allow_empty = false) {
    if (text.size() > maximum || (!allow_empty && text.empty())) return false;
    for (char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) return false;
    }
    try { static_cast<void>(nlohmann::json(std::string(text)).dump()); }
    catch (const nlohmann::json::type_error&) { return false; }
    return true;
}

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    constexpr std::string_view safe = "-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~";
    return std::all_of(id.begin(), id.end(), [safe](char byte) { return safe.find(byte) != safe.npos; });
}

bool ipv4(std::string_view value) {
    for (int part = 0; part != 4; ++part) {
        const auto end = value.find('.');
        const auto text = value.substr(0, end);
        unsigned int number = 0;
        if (text.empty() || text.size() > 3 || (text.size() > 1 && text.front() == '0')) return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number > 255) return false;
        if (part == 3) return end == value.npos;
        if (end == value.npos) return false;
        value.remove_prefix(end + 1);
    }
    return false;
}

int hex(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string decode(std::string_view component) {
    std::string result;
    result.reserve(component.size());
    for (std::size_t index = 0; index < component.size(); ++index) {
        if (component[index] != '%') result.push_back(component[index]);
        else {
            if (component.size() - index < 3) fail("invalid_target");
            const int high = hex(component[index + 1]);
            const int low = hex(component[index + 2]);
            if (high < 0 || low < 0) fail("invalid_target");
            result.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        }
    }
    if (!valid_text(result, 256, true)) fail("invalid_target");
    return result;
}

std::map<std::string, std::string> parse_filters(std::string_view query) {
    std::map<std::string, std::string> filters;
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        const auto equal = pair.find('=');
        if (equal == pair.npos) fail("invalid_query");
        const auto key = decode(pair.substr(0, equal));
        if (key != "Region" && key != "GameVersion" && key != "Map" && key != "GameType" && key != "ServerName")
            fail("invalid_query");
        if (!filters.emplace(key, decode(pair.substr(equal + 1))).second) fail("invalid_query");
        if (end == query.npos) break;
        query.remove_prefix(end + 1);
        if (query.empty()) fail("invalid_query");
    }
    return filters;
}

bool matches(const ServerMetadata& server, const std::map<std::string, std::string>& filters) {
    for (const auto& [key, value] : filters) {
        if (value.empty()) continue;
        const auto& actual = key == "Region" ? server.region :
            key == "GameVersion" ? server.game_version : key == "Map" ? server.map :
            key == "GameType" ? server.game_type : server.server_name;
        if (value != actual) return false;
    }
    return true;
}

nlohmann::json project(const ServerMetadata& server) {
    meteor::GameServerListing listing;
    listing.guid = server.guid;
    listing.region = server.region;
    listing.game_version = server.game_version;
    listing.server_name = server.server_name;
    listing.map = server.map;
    listing.game_type = server.game_type;
    listing.endpoint = server.endpoint;
    listing.port = std::to_string(server.port);
    listing.max_users = server.max_users;
    listing.is_public_visible = server.is_public_visible;
    listing.is_matchmaking_visible = server.is_matchmaking_visible;
    auto result = meteor::listing_json(listing);
    result["DeveloperData"]["MatchState"] = std::to_string(static_cast<std::int32_t>(server.match_state));
    return result;
}

DirectoryResponse response(int status, const char* message, const nlohmann::json& result = nullptr) {
    return {status, meteor::envelope(static_cast<std::uint32_t>(status), message, result)};
}
}

DirectoryError::DirectoryError(std::string error_code)
    : std::runtime_error("Directory metadata or request could not be validated."),
      code(std::move(error_code)) {}

MasterDirectory::MasterDirectory(std::vector<ServerMetadata> servers) : servers_(std::move(servers)) {
    // Our snapshot resource policy, not a limit inferred from the original service.
    if (servers_.size() > 1000) fail("directory_limit");
    std::set<std::string> ids;
    for (const auto& server : servers_) {
        if (!valid_id(server.guid) || !valid_text(server.region, 256) || !valid_text(server.game_version, 256) ||
            !valid_text(server.server_name, 256) || !valid_text(server.map, 256) || !valid_text(server.game_type, 256) ||
            !ipv4(server.endpoint) || server.port == 0 || server.max_users < 1 ||
            server.match_state < MatchState::unavailable || server.match_state > MatchState::postmatch)
            fail("invalid_metadata");
        if (!ids.insert(server.guid).second) fail("duplicate_id");
    }
}

DirectoryResponse MasterDirectory::respond(std::string_view method, std::string_view raw_target) const {
    if (method != "GET") return response(405, "Method not allowed.");
    if (!valid_text(raw_target, 4096) || raw_target.find('#') != raw_target.npos)
        return response(400, "Invalid request.");
    constexpr std::string_view collection = "/gameServerListings";
    constexpr std::string_view prefix = "/gameServerListings/";
    const auto separator = raw_target.find('?');
    const auto path = raw_target.substr(0, separator);
    const auto query = separator == raw_target.npos ? std::string_view{} : raw_target.substr(separator + 1);
    try {
        if (path == collection) {
            const auto filters = parse_filters(query);
            auto listings = nlohmann::json::array();
            for (const auto& server : servers_)
                if (server.is_public_visible && matches(server, filters)) listings.push_back(project(server));
            return response(200, "", listings);
        }
        if (path.starts_with(prefix)) {
            if (separator != raw_target.npos) fail("invalid_query");
            const auto id = decode(path.substr(prefix.size()));
            if (!valid_id(id)) fail("invalid_target");
            for (const auto& server : servers_)
                if (server.guid == id && server.is_public_visible) return response(200, "", project(server));
        }
        return response(404, "Not found.");
    } catch (const DirectoryError&) {
        return response(400, "Invalid request.");
    }
}
}
