#include "hawken/meteor_ini.hpp"
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace hawken {
namespace {
[[noreturn]] void fail(const char* code) { throw MeteorIniError(code); }

struct DecodedText {
    std::string text;
    std::vector<std::uint64_t> offsets;
    std::string encoding;
};

unsigned int octet(const std::string& data, std::size_t index) {
    return static_cast<unsigned char>(data[index]);
}

void append_scalar(DecodedText& out, std::uint32_t scalar, std::size_t offset) {
    const auto append = [&](std::uint32_t value) {
        out.text.push_back(static_cast<char>(value));
        out.offsets.push_back(static_cast<std::uint64_t>(offset));
    };
    if (scalar <= 0x7fU) append(scalar);
    else if (scalar <= 0x7ffU) {
        append(0xc0U | (scalar >> 6U));
        append(0x80U | (scalar & 0x3fU));
    } else if (scalar <= 0xffffU) {
        append(0xe0U | (scalar >> 12U));
        append(0x80U | ((scalar >> 6U) & 0x3fU));
        append(0x80U | (scalar & 0x3fU));
    } else {
        append(0xf0U | (scalar >> 18U));
        append(0x80U | ((scalar >> 12U) & 0x3fU));
        append(0x80U | ((scalar >> 6U) & 0x3fU));
        append(0x80U | (scalar & 0x3fU));
    }
}

DecodedText decode(const std::string& data) {
    const auto starts = [&](std::initializer_list<unsigned int> prefix) {
        if (data.size() < prefix.size()) return false;
        std::size_t index = 0;
        for (const auto value : prefix) if (octet(data, index++) != value) return false;
        return true;
    };
    if (starts({0xff, 0xfe, 0, 0}) || starts({0, 0, 0xfe, 0xff})) fail("invalid_encoding");
    DecodedText out;
    const bool little = starts({0xff, 0xfe});
    const bool big = starts({0xfe, 0xff});
    if (little || big) {
        out.encoding = little ? "utf-16" : "utf-16BE";
        if ((data.size() - 2) % 2 != 0) fail("invalid_encoding");
        const auto unit = [&](std::size_t index) {
            return little ? octet(data, index) | (octet(data, index + 1) << 8U)
                          : (octet(data, index) << 8U) | octet(data, index + 1);
        };
        for (std::size_t index = 2; index < data.size();) {
            const auto offset = index;
            std::uint32_t scalar = unit(index);
            index += 2;
            if (scalar >= 0xd800U && scalar <= 0xdbffU) {
                if (index == data.size()) fail("invalid_encoding");
                const auto low = unit(index);
                if (low < 0xdc00U || low > 0xdfffU) fail("invalid_encoding");
                scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + low - 0xdc00U;
                index += 2;
            } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) fail("invalid_encoding");
            append_scalar(out, scalar, offset);
        }
        return out;
    }
    out.encoding = "utf-8";
    const std::size_t start = starts({0xef, 0xbb, 0xbf}) ? 3 : 0;
    for (std::size_t index = start; index < data.size();) {
        const auto first = octet(data, index);
        std::size_t length = 1;
        std::uint32_t scalar = first;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) { length = 2; scalar &= 0x1fU; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { length = 3; scalar &= 0x0fU; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { length = 4; scalar &= 0x07U; minimum = 0x10000U; }
        else if (first >= 0x80U) fail("invalid_encoding");
        if (length > data.size() - index) fail("invalid_encoding");
        for (std::size_t part = 1; part < length; ++part) {
            const auto next = octet(data, index + part);
            if ((next & 0xc0U) != 0x80U) fail("invalid_encoding");
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        if (scalar < minimum || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU))
            fail("invalid_encoding");
        for (std::size_t part = 0; part < length; ++part) {
            out.text.push_back(data[index + part]);
            out.offsets.push_back(static_cast<std::uint64_t>(index + part));
        }
        index += length;
    }
    return out;
}

bool trim_space(char value) { return value == ' ' || value == '\t' || value == '\r'; }

std::string_view trim(std::string_view text) {
    while (!text.empty() && trim_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && trim_space(text.back())) text.remove_suffix(1);
    return text;
}

bool equals_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
}

bool digit(char value) { return value >= '0' && value <= '9'; }
bool alpha(char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }
bool hex(char value) { return digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); }

bool ip_address(std::string_view host, int family) {
    const std::string terminated(host);
    unsigned char address[16]{};
#ifdef _WIN32
    return InetPtonA(family, terminated.c_str(), address) == 1;
#else
    return inet_pton(family, terminated.c_str(), address) == 1;
#endif
}

bool dns_or_ipv4(std::string_view host) {
    if (host.empty()) return false;
    bool numeric = host.find('.') != std::string_view::npos;
    for (const auto value : host) if (!digit(value) && value != '.') numeric = false;
    if (numeric) return ip_address(host, AF_INET);
    if (host.back() == '.') host.remove_suffix(1);
    if (host.size() > 253) return false;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (const auto value : label) if (!alpha(value) && !digit(value) && value != '-') return false;
        if (dot == std::string_view::npos) return true;
        host.remove_prefix(dot + 1);
        if (host.empty()) return false;
    }
    return false;
}

void validate_uri(std::string_view uri) {
    for (const auto value : uri) {
        const auto code = static_cast<unsigned char>(value);
        if (code <= 0x20U || code >= 0x7fU) fail("invalid_uri");
    }
    const auto delimiter = uri.find("://");
    if (delimiter == std::string_view::npos) fail("invalid_uri");
    const auto scheme = uri.substr(0, delimiter);
    if (!equals_ascii(scheme, "http") && !equals_ascii(scheme, "https")) fail("invalid_uri");
    uri.remove_prefix(delimiter + 3);
    if (uri.find_first_of("?#") != std::string_view::npos) fail("invalid_uri");
    const auto slash = uri.find('/');
    auto authority = uri.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string_view::npos) fail("invalid_uri");
    std::string_view port;
    bool has_port = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || !ip_address(authority.substr(1, close - 1), AF_INET6))
            fail("invalid_uri");
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (authority.front() != ':') fail("invalid_uri");
            port = authority.substr(1);
            has_port = true;
        }
    } else {
        const auto colon = authority.find(':');
        if (!dns_or_ipv4(authority.substr(0, colon))) fail("invalid_uri");
        if (colon != std::string_view::npos) { port = authority.substr(colon + 1); has_port = true; }
    }
    if (has_port) {
        if (port.empty()) fail("invalid_uri");
        unsigned int number = 0;
        for (const auto value : port) {
            if (!digit(value)) fail("invalid_uri");
            number = number * 10U + static_cast<unsigned int>(value - '0');
            if (number > 65535U) fail("invalid_uri");
        }
        if (number == 0) fail("invalid_uri");
    }
    if (slash == std::string_view::npos) return;
    const auto path = uri.substr(slash);
    for (std::size_t index = 0; index < path.size(); ++index) {
        const auto value = path[index];
        if (value == '%') {
            if (path.size() - index < 3 || !hex(path[index + 1]) || !hex(path[index + 2])) fail("invalid_uri");
            index += 2;
        } else if (!alpha(value) && !digit(value) && std::string_view("-._~!$&'()*+,;=:@/").find(value) == std::string_view::npos)
            fail("invalid_uri");
    }
}
}

MeteorIniError::MeteorIniError(std::string error_code)
    : std::runtime_error("Meteor INI configuration could not be read or validated."),
      code(std::move(error_code)) {}

MeteorIniSetting read_meteor_ini(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("read_failed");
    constexpr std::size_t max_ini_bytes = 64 * 1024;
    std::string data;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() != 0) {
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count > max_ini_bytes - data.size()) fail("file_too_large");
        data.append(buffer, count);
    }
    if (!stream.eof() || stream.bad()) fail("read_failed");
    const auto decoded = decode(data);
    std::optional<MeteorIniSetting> setting;
    const std::string_view text(decoded.text);
    std::size_t start = 0;
    std::size_t line_number = 1;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto raw = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        const auto line = trim(raw);
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            if (line.front() == '[') fail("unsupported_section");
            const auto equal = line.find('=');
            if (equal != std::string_view::npos && equals_ascii(trim(line.substr(0, equal)), "MeteorUri")) {
                if (setting) fail("duplicate_key");
                const auto value = trim(line.substr(equal + 1));
                if (value.empty()) fail("empty_value");
                const auto key_index = start + static_cast<std::size_t>(line.data() - raw.data());
                setting = MeteorIniSetting{std::string(value), line_number, decoded.offsets[key_index], decoded.encoding};
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
        ++line_number;
    }
    if (!setting) fail("missing_key");
    validate_uri(setting->uri);
    return *setting;
}
}
