#include "hawken/server_http.hpp"
#include "hawken/auth_service.hpp"
#include "hawken/sync_service.hpp"
#include "hawken/inventory_service.hpp"
#include "hawken/menu_sync_service.hpp"
#include "hawken/session_assignment.hpp"
#include "hawken/master_directory.hpp"
#include "hawken/meteor_compat.hpp"
#include <cpp-httplib/httplib.h>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hawken::server {
namespace {
void reply(httplib::Response& response, int status, std::string_view message,
           const nlohmann::json& result) {
    response.status = status;
    // Some rejections deliberately leave bodies unread. Never reuse the connection.
    response.set_header("Connection", "close");
    response.set_content(meteor::envelope(static_cast<std::uint32_t>(status), message, result).dump(),
        "application/json; charset=utf-8");
}

std::string diagnostic_time() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto day = std::chrono::floor<std::chrono::days>(now);
    const std::chrono::year_month_day date(day);
    const std::chrono::hh_mm_ss time(now - day);
    char text[21]{};
    const int length = std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()), static_cast<long long>(time.hours().count()),
        static_cast<long long>(time.minutes().count()), static_cast<long long>(time.seconds().count()));
    if (length != 20) throw std::runtime_error("Diagnostic time could not be formatted.");
    return text;
}

void auth_reply(httplib::Response& response, const auth::AuthReply& result) {
    reply(response, result.status, result.message, result.result);
}

void menu_reply(httplib::Response& response, const sync::MenuSyncReply& result) {
    reply(response, result.api_status, result.message, result.result);
    // Resource absence retains HTTP200 with API404 for the native reader.
    response.status = result.http_status;
}

// ContentReader stops BEFORE append, including decoded chunked/compressed data.
bool read_body(const httplib::ContentReader& reader, httplib::Response& response,
               std::size_t limit, std::string& body) {
    bool exceeded = false;
    const bool read = reader([&](const char* data, std::size_t size) {
        if (size > limit - body.size()) { exceeded = true; return false; }
        body.append(data, size);
        return true;
    });
    if (!read) {
        reply(response, exceeded || response.status == 413 ? 413 : 400,
            "Request could not be processed.", nullptr);
        return false;
    }
    return true;
}

std::string_view user_guid(std::string_view target, bool logout) {
    constexpr std::string_view prefix = "/users/", suffix = "/accessGrant";
    if (!target.starts_with(prefix)) return {};
    auto guid = target.substr(prefix.size());
    if (logout) {
        if (!guid.ends_with(suffix)) return {};
        guid.remove_suffix(suffix.size());
    }
    if (guid.size() != 36) return {};
    for (std::size_t index = 0; index < guid.size(); ++index) {
        const char value = guid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') return {};
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return {};
    }
    return guid;
}
}

void configure_server_http(httplib::Server& server) {
    configure_server_http(server, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth) {
    configure_server_http(server, auth, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth, sync::SyncService* sync) {
    configure_server_http(server, auth, sync, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory) {
    configure_server_http(server, auth, sync, inventory, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu) {
    configure_server_http(server, auth, sync, inventory, menu, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu,
                           assignment::SessionAssignmentService* assignment) {
    configure_server_http(server, auth, sync, inventory, menu, assignment, nullptr);
}

void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu,
                           assignment::SessionAssignmentService* assignment,
                           const master::MasterDirectory* directory) {
    server.set_read_timeout(2);
    server.set_write_timeout(2);
    // Fixed framing ceiling; individual readers enforce their own accumulation cap.
    server.set_payload_max_length(65536);
    server.set_exception_handler([](const httplib::Request&, httplib::Response& response, std::exception_ptr) {
        reply(response, 500, "Request could not be processed.", nullptr);
    });
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.body.empty()) {
            reply(response, response.status, "Request could not be processed.", nullptr);
        }
    });
    const auto handle_listing = [directory, assignment](const httplib::Request& request,
                                                       httplib::Response& response) {
        const auto listing = directory->respond("GET", request.target);
        if (listing.http_status == 200 && assignment
            && request.target.starts_with("/gameServerListings/")) {
            const auto selected = assignment->select(request.get_header_value("Authorization"),
                listing.body.at("Result").at("Guid").get<std::string>());
            if (selected.status != 200) { auth_reply(response, selected); return; }
        }
        response.status = listing.http_status;
        response.set_header("Connection", "close");
        response.set_content(listing.body.dump(), "application/json; charset=utf-8");
    };
    server.set_pre_routing_handler([auth, sync, inventory, menu, assignment, directory, handle_listing](const httplib::Request& request, httplib::Response& response) {
        // Compare the raw target: decoded paths, queries, prefixes and slash aliases are not accepted.
        const bool services = request.target == "/status/services/";
        const bool client = request.target == "/status/game_client/";
        const bool version = request.target == "/version";
        const bool login = request.target == "/steam/login";
        const auto profile_guid = auth ? user_guid(request.target, false) : std::string_view{};
        const auto logout_guid = auth ? user_guid(request.target, true) : std::string_view{};
        const bool profile = !profile_guid.empty(), logout = !logout_guid.empty();
        const bool initial_sync = sync && sync->supports_target(request.target);
        const bool inventory_read = inventory && inventory->supports_target(request.target);
        const bool inventory_update = inventory && inventory->supports_update_target(request.target);
        const bool inventory_put = inventory_update && request.method == "PUT";
        const bool menu_read = menu && menu->supports_target(request.target);
        const bool settings = menu && menu->supports_settings_target(request.target);
        const bool settings_put = settings && request.method == "PUT";
        std::uint64_t length = 0;
        if (request.has_header("Content-Length")) {
            const auto text = request.get_header_value("Content-Length");
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
            if (parsed.ec == std::errc::result_out_of_range ||
                (parsed.ec == std::errc{} && length > ((settings_put || inventory_put) ? 65536U : 8192U))) {
                reply(response, 413, "Request could not be processed.", nullptr);
                return httplib::Server::HandlerResponse::Handled;
            }
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                reply(response, 400, "Request could not be processed.", nullptr);
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        const bool assignment_route = assignment && assignment->supports_target(request.target);
        const bool listing_route = directory && (request.target == "/gameServerListings"
            || request.target.starts_with("/gameServerListings?")
            || request.target.starts_with("/gameServerListings/"));
        const bool advertisement_post = assignment_route
            && request.target == "/hawkenClientMatchmakingAdvertisements";
        const std::string_view method = login || advertisement_post ? "POST" : (logout || inventory_update) ? "PUT" : "GET";
        if (!services && !client && !version && !login && !profile && !logout
            && !initial_sync && !inventory_read && !inventory_update && !menu_read && !assignment_route && !listing_route) {
            reply(response, 404, "Route not found.", nullptr);
        } else if ((request.method == "GET" || request.method == "HEAD") &&
                   (length != 0 || request.has_header("Transfer-Encoding"))) {
            // GET resources have no request body. Do not drain rejected framing.
            reply(response, 400, "Invalid request.", nullptr);
        } else if (request.method != method && !settings_put) {
            response.set_header("Allow", settings ? "GET, PUT" : std::string(method));
            reply(response, 405, "Method not allowed.", nullptr);
        } else if (settings_put || inventory_put || advertisement_post) {
            return httplib::Server::HandlerResponse::Unhandled;
        } else if (assignment_route) {
            auth_reply(response, assignment->handle(request.method, request.target,
                request.get_header_value("Authorization"), {}));
        } else if (listing_route) {
            handle_listing(request, response);
        } else if (profile) {
            auth_reply(response, auth->profile(profile_guid, request.get_header_value("Authorization")));
        } else if (initial_sync) {
            auth_reply(response, sync->get(request.target, request.get_header_value("Authorization")));
        } else if (inventory_read) {
            auth_reply(response, inventory->get(request.target, request.get_header_value("Authorization")));
        } else if (menu_read) {
            menu_reply(response, menu->get(request.target, request.get_header_value("Authorization")));
        } else if ((login || logout) && auth) {
            // Only these exact authorized route shapes reach bounded body reading.
            return httplib::Server::HandlerResponse::Unhandled;
        } else if (login) {
            // No body access, ticket processing, grant creation or player admission.
            reply(response, 503, "Steam authentication is not available.", nullptr);
        } else if (services) {
            reply(response, 200, "", "hawken-local available");
        } else if (client) {
            reply(response, 200, "", "1.6.0.154767");
        } else {
            // Facade identity and UTC diagnostic formatting are local design choices.
            reply(response, 200, "", nlohmann::json{
                {"Version", "hawken-local"}, {"Time", diagnostic_time()}});
        }
        return httplib::Server::HandlerResponse::Handled;
    });
    if (auth) {
        server.Post("/steam/login", [auth](const httplib::Request& request, httplib::Response& response,
                                         const httplib::ContentReader& reader) {
            std::string body;
            if (!read_body(reader, response, 8192, body)) return;
            const auto claims = request.get_header_value_count("X-Hawken-SteamID");
            if (claims > 1) {
                reply(response, 400, "Invalid authentication request.", nullptr);
            } else if (claims == 1) {
                auth_reply(response, auth->login(body, request.get_header_value("X-Hawken-SteamID")));
            } else {
                auth_reply(response, auth->login(body));
            }
        });
        server.Put(R"(/users/[0-9a-f-]{36}/accessGrant)",
            [auth](const httplib::Request& request, httplib::Response& response,
                   const httplib::ContentReader& reader) {
                std::string body;
                if (!read_body(reader, response, 8192, body)) return;
                auth_reply(response, auth->logout(user_guid(request.target, true),
                    request.get_header_value("Authorization"), body));
            });
    }
    if (inventory) {
        server.Put(R"(/userGameItemsBroker/[0-9a-f-]{36}/[0-9a-f-]{36})",
            [inventory](const httplib::Request& request, httplib::Response& response,
                        const httplib::ContentReader& reader) {
                if (request.get_header_value_count("Authorization") > 1 ||
                    request.get_header_value_count("Content-Type") != 1 ||
                    request.get_header_value("Content-Type") != "application/json") {
                    reply(response, 400, "Invalid inventory request.", nullptr);
                    return;
                }
                std::string body;
                if (!read_body(reader, response, 65536, body)) return;
                auth_reply(response, inventory->put(request.target,
                    request.get_header_value("Authorization"), body));
            });
    }
    if (menu) {
        server.Put(R"(/userGameSettings/[0-9a-f-]{36})",
            [menu](const httplib::Request& request, httplib::Response& response,
                   const httplib::ContentReader& reader) {
                if (request.get_header_value_count("Authorization") > 1 ||
                    request.get_header_value_count("Content-Type") != 1 ||
                    request.get_header_value("Content-Type") != "application/json") {
                    reply(response, 400, "Invalid settings request.", nullptr);
                    return;
                }
                std::string body;
                if (!read_body(reader, response, 65536, body)) return;
                menu_reply(response, menu->put(request.target, request.get_header_value("Authorization"), body));
            });
    }
    if (assignment) {
        server.Post("/hawkenClientMatchmakingAdvertisements",
            [assignment](const httplib::Request& request, httplib::Response& response,
                         const httplib::ContentReader& reader) {
                std::string body;
                if (!read_body(reader, response, 8192, body)) return;
                auth_reply(response, assignment->handle(request.method, request.target,
                    request.get_header_value("Authorization"), body));
            });
    }
}

}
