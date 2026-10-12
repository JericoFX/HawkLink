#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hawken::client {
struct RoutingConfig {
    std::string meteor_uri;
};

enum class ClientRoutingErrorCode {
    invalid_toml, invalid_schema, invalid_uri, resource_limit, io_error,
    command_line_conflict, invalid_command_line, not_implemented
};

class ClientRoutingError : public std::runtime_error {
public:
    explicit ClientRoutingError(ClientRoutingErrorCode code);
    ClientRoutingErrorCode code() const noexcept;
private:
    ClientRoutingErrorCode code_;
};

// Client destination only: no authentication, Steam identity or Master schema.
// Exact schema: [network] with only meteor_uri:string. Text/file <=64 KiB.
// URI <=2048 ASCII bytes, lowercase http:// or https://, DNS/IPv4 host only.
// No explicit port, IPv6, userinfo, query, fragment or path; one final '/' is
// removed. DNS labels: 1..63 alphanumeric/hyphen, no edge hyphens; host <=253.
RoutingConfig parse_routing_toml(std::string_view text);
RoutingConfig read_routing_toml(const std::filesystem::path& path);

// Validates even a directly constructed config. Preserves original and appends
// " MeteorURI=" plus normalized URI. Rejects NUL, an existing case-insensitive
// MeteorURI= substring and a result >=32767 wchar_t units (excluding NUL).
// This builder does not execute the client or prove effective routing.
std::wstring meteor_command_line(std::wstring_view original, const RoutingConfig& config);
}
