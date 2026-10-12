#include <hawken/client_routing.hpp>
#include <doctest/doctest.h>
#include "test_support.hpp"
#include <string>

namespace {
using namespace hawken::client;

template<class Operation>
void expect_routing_error(ClientRoutingErrorCode code, Operation operation) {
    bool caught = false;
    try { operation(); }
    catch (const ClientRoutingError& error) {
        caught = true;
        CHECK(error.code() == code);
        CHECK(std::string(error.what()) == "Client routing operation failed.");
    }
    CHECK(caught);
}

std::string toml_for(std::string_view uri) {
    return "[network]\nmeteor_uri = '" + std::string(uri) + "'\n";
}

std::string exact_size_config(std::size_t bytes) {
    std::string text = "[network]\nmeteor_uri='http://127.0.0.1'\n#";
    text.append(bytes - text.size(), '#');
    return text;
}
}

TEST_CASE("Client routing error exposes a code without input or path disclosure") {
    const ClientRoutingError error(ClientRoutingErrorCode::invalid_uri);
    CHECK(error.code() == ClientRoutingErrorCode::invalid_uri);
    CHECK(std::string(error.what()) == "Client routing operation failed.");
}

TEST_CASE("Routing TOML selects HTTP loopback without an explicit port") {
    CHECK(parse_routing_toml("[network]\nmeteor_uri=\"http://127.0.0.1\"\n").meteor_uri ==
          "http://127.0.0.1");
}

TEST_CASE("Routing TOML preserves HTTPS DNS host casing") {
    CHECK(parse_routing_toml(toml_for("https://Server.Example-1.com")).meteor_uri ==
          "https://Server.Example-1.com");
}

TEST_CASE("Routing TOML removes a single final slash") {
    CHECK(parse_routing_toml(toml_for("http://localhost/")).meteor_uri == "http://localhost");
}

TEST_CASE("Routing TOML handles comments literal strings and decoded basic strings") {
    CHECK(parse_routing_toml("# local destination\n[network] # section\n"
          "meteor_uri = 'http://127.0.0.1' # value\n").meteor_uri == "http://127.0.0.1");
    CHECK(parse_routing_toml("[network]\nmeteor_uri=\"https://ex\\u0061mple.com\"\n")
          .meteor_uri == "https://example.com");
}

TEST_CASE("Routing TOML accepts a standard UTF8 BOM") {
    CHECK(parse_routing_toml("\xEF\xBB\xBF[network]\nmeteor_uri='http://127.0.0.1'\n")
          .meteor_uri == "http://127.0.0.1");
}

TEST_CASE("Routing schema requires the network table and meteor_uri string") {
    for (const auto* text : {"", "[network]\n", "meteor_uri='http://localhost'\n",
                            "network='http://localhost'\n", "[Network]\nmeteor_uri='http://localhost'\n",
                            "[network]\nMeteorURI='http://localhost'\n"}) {
        CAPTURE(text);
        expect_routing_error(ClientRoutingErrorCode::invalid_schema,
            [&] { (void)parse_routing_toml(text); });
    }
}

TEST_CASE("Routing schema rejects additional keys and tables") {
    for (const auto* text : {"[network]\nmeteor_uri='http://localhost'\nport=80\n",
                            "extra=true\n[network]\nmeteor_uri='http://localhost'\n",
                            "[network]\nmeteor_uri='http://localhost'\n[master]\n",
                            "[network]\nmeteor_uri='http://localhost'\n[network.auth]\n",
                            "[[network]]\nmeteor_uri='http://localhost'\n"}) {
        CAPTURE(text);
        expect_routing_error(ClientRoutingErrorCode::invalid_schema,
            [&] { (void)parse_routing_toml(text); });
    }
}

TEST_CASE("Routing schema rejects nonstring URI values") {
    for (const auto* value : {"80", "true", "1.5", "[]", "{}", "1979-05-27"}) {
        const auto text = std::string("[network]\nmeteor_uri=") + value + "\n";
        CAPTURE(value);
        expect_routing_error(ClientRoutingErrorCode::invalid_schema,
            [&] { (void)parse_routing_toml(text); });
    }
}

TEST_CASE("Routing TOML rejects syntax failures and duplicate definitions") {
    for (const auto* text : {"[network\n", "[network]\nmeteor_uri=\"unterminated\n",
                            "[network]\nmeteor_uri='http://localhost'\nmeteor_uri='http://other'\n",
                            "[network]\nmeteor_uri='http://localhost'\n[network]\n"}) {
        CAPTURE(text);
        expect_routing_error(ClientRoutingErrorCode::invalid_toml,
            [&] { (void)parse_routing_toml(text); });
    }
}

TEST_CASE("Routing URI excludes unsupported authority and nonroot paths") {
    for (const auto* uri : {"", "http://", "https://", "ftp://localhost", "HTTP://localhost",
                           "http://127.0.0.1:8080", "http://localhost:80", "https://localhost:443",
                           "http://user@localhost", "http://user:password@localhost", "http://[::1]",
                           "http://localhost/api", "http://localhost//", "http://localhost/?q=1",
                           "http://localhost?x=1", "http://localhost#fragment", "http://localhost/#f",
                           "http://localhost\\path", "http://local%68ost"}) {
        CAPTURE(uri);
        expect_routing_error(ClientRoutingErrorCode::invalid_uri,
            [&] { (void)parse_routing_toml(toml_for(uri)); });
    }
}

TEST_CASE("Routing URI rejects whitespace controls and argument delimiters") {
    for (const auto* uri : {" http://localhost", "http://localhost ", "http://local host",
                           "http://local\thost", "http://local\"host", "http://local'host"}) {
        const RoutingConfig config{uri};
        CAPTURE(uri);
        expect_routing_error(ClientRoutingErrorCode::invalid_uri,
            [&] { (void)meteor_command_line(L"game.exe", config); });
    }
    for (const auto* escaped : {"\\u0000", "\\n", "\\r", "\\t", "\\u007F"}) {
        const auto text = std::string("[network]\nmeteor_uri=\"http://local") + escaped + "host\"\n";
        expect_routing_error(ClientRoutingErrorCode::invalid_uri,
            [&] { (void)parse_routing_toml(text); });
    }
}

TEST_CASE("Routing TOML rejects raw NUL and URI nonASCII") {
    std::string nul = "[network]\nmeteor_uri='http://localhost'\n";
    nul.push_back('\0');
    expect_routing_error(ClientRoutingErrorCode::invalid_toml,
        [&] { (void)parse_routing_toml(nul); });
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [] { (void)parse_routing_toml("[network]\nmeteor_uri=\"http://caf\\u00E9.example\"\n"); });
    RoutingConfig config{"http://localhost"};
    config.meteor_uri.push_back('\0');
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [&] { (void)meteor_command_line(L"game.exe", config); });
}

TEST_CASE("Routing DNS host rejects empty labels invalid edges and symbols") {
    for (const auto* uri : {"http://.example", "http://example.", "http://a..example",
                           "http://-host.example", "http://host-.example", "http://local_host",
                           "http://host!name", "http://256.1.2.3", "http://1.2.3.999"}) {
        CAPTURE(uri);
        expect_routing_error(ClientRoutingErrorCode::invalid_uri,
            [&] { (void)parse_routing_toml(toml_for(uri)); });
    }
}

TEST_CASE("Routing DNS label accepts 63 characters and rejects 64") {
    const auto accepted = "http://" + std::string(63, 'a') + ".example";
    CHECK(parse_routing_toml(toml_for(accepted)).meteor_uri == accepted);
    const auto rejected = "http://" + std::string(64, 'a') + ".example";
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [&] { (void)parse_routing_toml(toml_for(rejected)); });
}

TEST_CASE("Routing DNS total accepts 253 characters and rejects 254") {
    const auto host = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                      std::string(63, 'c') + "." + std::string(61, 'd');
    const auto accepted = "https://" + host;
    CHECK(parse_routing_toml(toml_for(accepted)).meteor_uri == accepted);
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [&] { (void)parse_routing_toml(toml_for(accepted + "d")); });
}

TEST_CASE("Routing URI size guard rejects more than 2048 bytes") {
    const RoutingConfig config{"http://" + std::string(2042, 'a')};
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [&] { (void)meteor_command_line(L"game.exe", config); });
}

TEST_CASE("Routing TOML byte budget accepts 64KiB and rejects the next byte") {
    CHECK(parse_routing_toml(exact_size_config(65536)).meteor_uri == "http://127.0.0.1");
    expect_routing_error(ClientRoutingErrorCode::resource_limit,
        [] { (void)parse_routing_toml(exact_size_config(65537)); });
}

TEST_CASE("Routing file reader parses actual binary TOML including a BOM") {
    hawken::test::TempDirectory temp;
    const auto path = temp.write("client-routing.toml",
        "\xEF\xBB\xBF[network]\nmeteor_uri='http://127.0.0.1/'\n");
    CHECK(read_routing_toml(path).meteor_uri == "http://127.0.0.1");
}

TEST_CASE("Routing file reader accepts 64KiB and rejects an oversized file") {
    hawken::test::TempDirectory temp;
    const auto accepted = temp.write("exact.toml", exact_size_config(65536));
    CHECK(read_routing_toml(accepted).meteor_uri == "http://127.0.0.1");
    const auto oversized = temp.write("oversized-private.toml", exact_size_config(65537));
    expect_routing_error(ClientRoutingErrorCode::resource_limit,
        [&] { (void)read_routing_toml(oversized); });
}

TEST_CASE("Routing file reader reports absent files without leaking their path") {
    hawken::test::TempDirectory temp;
    expect_routing_error(ClientRoutingErrorCode::io_error,
        [&] { (void)read_routing_toml(temp.path() / "private-account-secret.toml"); });
}

TEST_CASE("Routing file reader applies schema validation instead of accepting raw text") {
    hawken::test::TempDirectory temp;
    const auto path = temp.write("invalid.toml", "[network]\nmeteor_uri=80\n");
    expect_routing_error(ClientRoutingErrorCode::invalid_schema,
        [&] { (void)read_routing_toml(path); });
}

TEST_CASE("Routing command builder preserves original executable quoting and arguments") {
    const RoutingConfig config{"http://127.0.0.1"};
    CHECK(meteor_command_line(L"\"C:\\Games\\Hawken game.exe\" -windowed", config) ==
          L"\"C:\\Games\\Hawken game.exe\" -windowed MeteorURI=http://127.0.0.1");
}

TEST_CASE("Routing command builder validates and normalizes a directly constructed config") {
    CHECK(meteor_command_line(L"game.exe", RoutingConfig{"https://example.com/"}) ==
          L"game.exe MeteorURI=https://example.com");
    expect_routing_error(ClientRoutingErrorCode::invalid_uri,
        [] { (void)meteor_command_line(L"game.exe", RoutingConfig{"http://localhost:8080"}); });
}

TEST_CASE("Routing command builder rejects conflicting MeteorURI arguments case insensitively") {
    const RoutingConfig config{"http://127.0.0.1"};
    for (const auto* original : {L"game.exe MeteorURI=https://old.example",
                                 L"game.exe meteoruri=http://old.example",
                                 L"game.exe -MeTeOrUrI=\"http://old.example\"",
                                 L"game.exe prefixMeteorURI="}) {
        expect_routing_error(ClientRoutingErrorCode::command_line_conflict,
            [&] { (void)meteor_command_line(original, config); });
    }
}

TEST_CASE("Routing command builder rejects embedded NUL in original arguments") {
    std::wstring original = L"game.exe";
    original.push_back(L'\0');
    original += L" hidden";
    expect_routing_error(ClientRoutingErrorCode::invalid_command_line,
        [&] { (void)meteor_command_line(original, RoutingConfig{"http://127.0.0.1"}); });
}

TEST_CASE("Routing command builder rejects a result of 32767 wide units") {
    const RoutingConfig config{"http://127.0.0.1"};
    // Literal suffix length is 27: space (1) + MeteorURI= (10) + URI (16).
    const std::wstring accepted(32739, L'x');
    const auto built = meteor_command_line(accepted, config);
    CHECK(built.size() == 32766);
    CHECK(built.ends_with(L" MeteorURI=http://127.0.0.1"));
    const std::wstring rejected(32740, L'x');
    expect_routing_error(ClientRoutingErrorCode::resource_limit,
        [&] { (void)meteor_command_line(rejected, config); });
}
