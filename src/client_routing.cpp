#include <hawken/client_routing.hpp>
#include <tomlplusplus/toml.hpp>
#include <fstream>

namespace {
using hawken::client::ClientRoutingError;
using hawken::client::ClientRoutingErrorCode;
constexpr std::size_t max_toml_bytes = 65536;
constexpr std::size_t max_uri_bytes = 2048;
constexpr std::size_t command_line_limit = 32767;

bool ascii_alphanumeric(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

std::string normalized_uri(std::string_view uri) {
    if (uri.empty() || uri.size() > max_uri_bytes)
        throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
    for (const unsigned char value : uri) {
        if (value <= 32 || value >= 127 || value == '\'' || value == '"')
            throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
    }
    std::size_t scheme_length = 0;
    if (uri.starts_with("http://")) scheme_length = 7;
    else if (uri.starts_with("https://")) scheme_length = 8;
    else throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
    if (uri.size() <= scheme_length)
        throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
    if (uri.back() == '/') uri.remove_suffix(1);
    const auto host = uri.substr(scheme_length);
    if (host.empty() || host.size() > 253)
        throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);

    bool numeric_host = true;
    std::size_t label_start = 0;
    std::size_t label_count = 0;
    while (label_start < host.size()) {
        const auto dot = host.find('.', label_start);
        const auto end = dot == std::string_view::npos ? host.size() : dot;
        const auto label = host.substr(label_start, end - label_start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
        for (const char value : label) {
            if (!ascii_alphanumeric(value) && value != '-')
                throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
            if (value < '0' || value > '9') numeric_host = false;
        }
        ++label_count;
        if (dot == std::string_view::npos) break;
        label_start = dot + 1;
        if (label_start == host.size())
            throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
    }
    // A wholly numeric dotted host must be a four-octet IPv4 address rather
    // than relying on platform-specific interpretations of malformed IP text.
    if (numeric_host && label_count > 1) {
        if (label_count != 4)
            throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
        label_start = 0;
        while (label_start < host.size()) {
            const auto dot = host.find('.', label_start);
            const auto end = dot == std::string_view::npos ? host.size() : dot;
            const auto label = host.substr(label_start, end - label_start);
            unsigned int octet = 0;
            if (label.size() > 3)
                throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
            for (const char digit : label)
                octet = octet * 10 + static_cast<unsigned int>(digit - '0');
            if (octet > 255)
                throw ClientRoutingError(ClientRoutingErrorCode::invalid_uri);
            if (dot == std::string_view::npos) break;
            label_start = dot + 1;
        }
    }
    return std::string(uri);
}

wchar_t ascii_lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}
}

namespace hawken::client {
ClientRoutingError::ClientRoutingError(ClientRoutingErrorCode code)
    : std::runtime_error("Client routing operation failed."), code_(code) {}

ClientRoutingErrorCode ClientRoutingError::code() const noexcept {
    return code_;
}

RoutingConfig parse_routing_toml(std::string_view text) {
    if (text.size() > max_toml_bytes)
        throw ClientRoutingError(ClientRoutingErrorCode::resource_limit);
    try {
        const auto document = toml::parse(text);
        const auto* network = document["network"].as_table();
        if (document.size() != 1 || network == nullptr || network->size() != 1 ||
            !(*network)["meteor_uri"].is_string())
            throw ClientRoutingError(ClientRoutingErrorCode::invalid_schema);
        const auto uri = (*network)["meteor_uri"].value<std::string>();
        if (!uri) throw ClientRoutingError(ClientRoutingErrorCode::invalid_schema);
        return RoutingConfig{normalized_uri(*uri)};
    }
    catch (const toml::parse_error&) {
        throw ClientRoutingError(ClientRoutingErrorCode::invalid_toml);
    }
}

RoutingConfig read_routing_toml(const std::filesystem::path& path) {
    std::string text;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) throw ClientRoutingError(ClientRoutingErrorCode::io_error);
        text.resize(max_toml_bytes + 1);
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        const auto count = stream.gcount();
        if (count > static_cast<std::streamsize>(max_toml_bytes))
            throw ClientRoutingError(ClientRoutingErrorCode::resource_limit);
        if (stream.bad() || (stream.fail() && !stream.eof()))
            throw ClientRoutingError(ClientRoutingErrorCode::io_error);
        text.resize(static_cast<std::size_t>(count));
    }
    catch (const ClientRoutingError&) { throw; }
    catch (...) { throw ClientRoutingError(ClientRoutingErrorCode::io_error); }
    return parse_routing_toml(text);
}

std::wstring meteor_command_line(std::wstring_view original, const RoutingConfig& config) {
    if (original.size() >= command_line_limit)
        throw ClientRoutingError(ClientRoutingErrorCode::resource_limit);
    if (original.find(L'\0') != std::wstring_view::npos)
        throw ClientRoutingError(ClientRoutingErrorCode::invalid_command_line);
    const auto uri = normalized_uri(config.meteor_uri);
    constexpr std::wstring_view argument = L"meteoruri=";
    for (std::size_t start = 0; start + argument.size() <= original.size(); ++start) {
        bool matches = true;
        for (std::size_t offset = 0; offset < argument.size(); ++offset) {
            if (ascii_lower(original[start + offset]) != argument[offset]) {
                matches = false;
                break;
            }
        }
        if (matches) throw ClientRoutingError(ClientRoutingErrorCode::command_line_conflict);
    }
    constexpr std::wstring_view suffix_prefix = L" MeteorURI=";
    if (original.size() >= command_line_limit - suffix_prefix.size() - uri.size())
        throw ClientRoutingError(ClientRoutingErrorCode::resource_limit);
    std::wstring result;
    result.reserve(original.size() + suffix_prefix.size() + uri.size());
    result.append(original);
    result.append(suffix_prefix);
    for (const unsigned char value : uri) result.push_back(static_cast<wchar_t>(value));
    return result;
}
}
