#include "hawken/meteor_ini.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <initializer_list>
#include <string_view>

namespace {
std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    for (const auto value : values) result.push_back(static_cast<char>(value));
    return result;
}

std::string utf16_fixture(std::u16string_view text, bool big_endian) {
    auto result = big_endian ? bytes({0xfe, 0xff}) : bytes({0xff, 0xfe});
    for (const auto unit : text) {
        const auto low = static_cast<char>(unit & 0xffU);
        const auto high = static_cast<char>(unit >> 8U);
        result.push_back(big_endian ? high : low);
        result.push_back(big_endian ? low : high);
    }
    return result;
}

void expect_error(const std::filesystem::path& path, const std::string& code,
                  const std::string& rejected_value = {}) {
    try {
        hawken::read_meteor_ini(path);
    } catch (const hawken::MeteorIniError& error) {
        CHECK(error.code == code);
        const std::string message = error.what();
        CHECK(message.find("never-echo-this") == std::string::npos);
        CHECK(message.find(path.string()) == std::string::npos);
        if (!rejected_value.empty()) CHECK(message.find(rejected_value) == std::string::npos);
        return;
    } catch (const std::exception&) {
        FAIL_CHECK("read_meteor_ini must report a sanitized MeteorIniError");
        return;
    }
    FAIL_CHECK("read_meteor_ini accepted a rejected configuration");
}

void check_provenance(const std::string& data, std::uint64_t offset, const std::string& encoding) {
    hawken::test::TempDirectory temp;
    const auto path = temp.write("StormConsumerSettings.ini", data);
    const auto setting = hawken::read_meteor_ini(path);
    CHECK(setting.uri == "http://localhost:8123/api/v2/");
    CHECK(setting.line_number == 2);
    CHECK(setting.byte_offset == offset);
    CHECK(setting.encoding_name == encoding);
    CHECK(hawken::test::read_bytes(path) == data);
}
}

TEST_CASE("Meteor INI reads original flat format without adding a URI slash") {
    const std::string data = "# Use '#' to comment to the end of a line\r\n\r\n"
        "# Base URI for all storm calls\r\n"
        "MeteorUri = https://v2-services-live-pc.playhawken.com\r\n";
    hawken::test::TempDirectory temp;
    const auto path = temp.write("StormConsumerSettings.ini", data);
    const auto setting = hawken::read_meteor_ini(path);
    CHECK(setting.uri == "https://v2-services-live-pc.playhawken.com");
    CHECK(setting.line_number == 4);
    CHECK(setting.byte_offset == 0x4d);
    CHECK(setting.encoding_name == "utf-8");
    CHECK(hawken::test::read_bytes(path) == data);
}

TEST_CASE("Meteor INI measures original encoded bytes at the indented case-insensitive key") {
    const std::string utf8 = "# caf\xc3\xa9\n  mEtEoRuRi \t=\t http://localhost:8123/api/v2/ \n";
    const std::u16string text = u"# caf\u00e9\n  mEtEoRuRi \t=\t http://localhost:8123/api/v2/ \n";
    SUBCASE("UTF8 without BOM") { check_provenance(utf8, 10, "utf-8"); }
    SUBCASE("UTF8 BOM") { check_provenance(bytes({0xef, 0xbb, 0xbf}) + utf8, 13, "utf-8"); }
    SUBCASE("UTF16LE BOM") { check_provenance(utf16_fixture(text, false), 20, "utf-16"); }
    SUBCASE("UTF16BE BOM") { check_provenance(utf16_fixture(text, true), 20, "utf-16BE"); }
}

TEST_CASE("Meteor INI supports LF and CRLF with an unterminated final line") {
    const auto check = [](const std::string& newline, std::uint64_t offset) {
        hawken::test::TempDirectory temp;
        const auto path = temp.write("settings.ini", "#x" + newline + "MeteorUri=https://example.test/base");
        const auto setting = hawken::read_meteor_ini(path);
        CHECK(setting.uri == "https://example.test/base");
        CHECK(setting.line_number == 2);
        CHECK(setting.byte_offset == offset);
    };
    SUBCASE("LF") { check("\n", 3); }
    SUBCASE("CRLF") { check("\r\n", 4); }
}

TEST_CASE("Meteor INI ignores blank and full-comment keys while retaining an unrelated flat key") {
    hawken::test::TempDirectory temp;
    const auto path = temp.write("settings.ini",
        " \n ; MeteorUri=https://ignored.test\n\t# MeteorUri=https://ignored.test\n"
        "OtherSetting=unchanged\n MeteorUri = https://example.test/\n");
    const auto setting = hawken::read_meteor_ini(path);
    CHECK(setting.uri == "https://example.test/");
    CHECK(setting.line_number == 5);
}

TEST_CASE("Meteor INI preserves valid base URI paths casing escapes and variables literally") {
    const char* values[] = {
        "http://127.0.0.1:65535/base/", "https://[::1]:8443/api/",
        "https://[2001:db8::1]/base", "HTTP://Example.test:00080/Base",
        "https://example.test/base%3Fnotes%23part/@archive/", "https://example.test/$HOME/",
        "https://example.test/caf%C3%A9/"
    };
    for (const auto value : values) {
        hawken::test::TempDirectory temp;
        const auto path = temp.write("settings.ini", std::string("MeteorUri = ") + value);
        CHECK(hawken::read_meteor_ini(path).uri == value);
    }
}

TEST_CASE("Meteor INI reports absent duplicate empty and sectioned values explicitly") {
    struct Case { const char* data; const char* code; };
    const Case cases[] = {
        {"", "missing_key"},
        {"# MeteorUri=https://example.test\nOther=1\n", "missing_key"},
        {"MeteorUri=\n", "empty_value"}, {"MeteorUri= \t\r\n", "empty_value"},
        {"MeteorUri=https://example.test\nmeteorURI=https://other.test\n", "duplicate_key"},
        {"[Storm]\nMeteorUri=https://example.test\n", "unsupported_section"},
        {"MeteorUri=https://example.test\n[Other]\nOther=1", "unsupported_section"}
    };
    for (const auto& item : cases) {
        hawken::test::TempDirectory temp;
        const auto path = temp.write("settings.ini", item.data);
        expect_error(path, item.code);
        CHECK(hawken::test::read_bytes(path) == item.data);
    }
}

TEST_CASE("Meteor INI rejects unsupported schemes malformed hosts and invalid ports") {
    const char* values[] = {
        "/relative/base", "ftp://example.test/base", "file:///local/path", "https://",
        "https://example.test:0/", "https://example.test:65536/", "https://example.test:-1/",
        "https://example.test:notaport/", "https://example.test:/", "https://bad..example/",
        "https://-bad.example/", "https://bad-.example/", "https://256.1.2.3/",
        "https://[not-ip]/", "https://[::1::]/", "https://[]/", "https://[::1/",
        "https://[::1]suffix/", "https://example.test/base\tvalue", "https://example.test/a%GG"
    };
    for (const auto value : values) {
        hawken::test::TempDirectory temp;
        const auto path = temp.write("settings.ini", std::string("MeteorUri=") + value);
        expect_error(path, "invalid_uri", value);
    }
}

TEST_CASE("Meteor INI rejects userinfo query and fragment without exposing rejected values") {
    const char* values[] = {
        "https://user:never-echo-this@example.test/base/", "https://user@example.test/base/",
        "https://@example.test/base/", "https://example.test/base/?token=never-echo-this",
        "https://example.test/base/?", "https://example.test/base/#never-echo-this",
        "https://example.test/base/#"
    };
    for (const auto value : values) {
        hawken::test::TempDirectory temp;
        const auto data = std::string("MeteorUri=") + value;
        const auto path = temp.write("settings.ini", data);
        expect_error(path, "invalid_uri", value);
        CHECK(hawken::test::read_bytes(path) == data);
    }
}

TEST_CASE("Meteor INI rejects invalid Unicode and unsupported UTF32 BOM") {
    const std::string values[] = {
        bytes({0xc3, 0x28}), bytes({0xef, 0xbb, 0xbf, 0xff}), bytes({0xc0, 0x80}),
        bytes({0xed, 0xa0, 0x80}), bytes({0xf4, 0x90, 0x80, 0x80}), bytes({0x80}), bytes({0xc3}),
        bytes({0xff, 0xfe, 0x4d}), bytes({0xfe, 0xff, 0x00, 0x4d, 0x00}),
        bytes({0xff, 0xfe, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00}),
        bytes({0x00, 0x00, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x4d}),
        bytes({0xff, 0xfe, 0x00, 0xd8}), bytes({0xfe, 0xff, 0xdc, 0x00})
    };
    for (const auto& data : values) {
        hawken::test::TempDirectory temp;
        const auto path = temp.write("settings.ini", data);
        expect_error(path, "invalid_encoding");
        CHECK(hawken::test::read_bytes(path) == data);
    }
}

TEST_CASE("Meteor INI reports missing file without echoing its path") {
    hawken::test::TempDirectory temp;
    const auto path = temp.path() / "never-echo-this-missing.ini";
    expect_error(path, "read_failed");
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Meteor INI counts surrogate pairs in comments using source bytes") {
    const std::u16string text = u"# \U0001f600\nMeteorUri=https://example.test/base";
    const auto check = [&](bool big_endian) {
        hawken::test::TempDirectory temp;
        const auto data = utf16_fixture(text, big_endian);
        const auto path = temp.write("settings.ini", data);
        const auto setting = hawken::read_meteor_ini(path);
        CHECK(setting.uri == "https://example.test/base");
        CHECK(setting.line_number == 2);
        CHECK(setting.byte_offset == 12);
        CHECK(hawken::test::read_bytes(path) == data);
    };
    SUBCASE("UTF16LE") { check(false); }
    SUBCASE("UTF16BE") { check(true); }
}

TEST_CASE("Meteor INI rejects URI controls malformed path and overflowing port") {
    const std::string values[] = {
        "https://example.test/a\\b", "https://example.test/<bad>",
        "https://example.test/%", "https://example.test/%A",
        "https://example.test:18446744073709551617/",
        "https://example.test/caf\xc3\xa9/",
        "https://example.test/" + bytes({0}) + "hidden",
        "https://example.test/" + bytes({0x7f}) + "hidden"
    };
    for (const auto& value : values) {
        hawken::test::TempDirectory temp;
        const auto data = "MeteorUri=" + value;
        const auto path = temp.write("settings.ini", data);
        expect_error(path, "invalid_uri", value);
        CHECK(hawken::test::read_bytes(path) == data);
    }
}

TEST_CASE("Meteor INI validates encoding beyond the first configured URI") {
    hawken::test::TempDirectory temp;
    const auto data = "MeteorUri=https://example.test/base\n# " + bytes({0xc3});
    const auto path = temp.write("settings.ini", data);
    expect_error(path, "invalid_encoding");
    CHECK(hawken::test::read_bytes(path) == data);
}

TEST_CASE("Meteor INI reports a directory input as a sanitized read failure") {
    hawken::test::TempDirectory temp;
    expect_error(temp.path(), "read_failed");
    CHECK(std::filesystem::is_directory(temp.path()));
}

TEST_CASE("Meteor INI rejects files exceeding the local 64KiB byte policy") {
    hawken::test::TempDirectory temp;
    std::string data;
    SUBCASE("UTF8 one byte over the cap") {
        data = "#" + std::string(65499, 'a') + "\nMeteorUri=https://example.test/base\n";
        REQUIRE(data.size() == 65537);
    }
    SUBCASE("UTF16 characters fit but original bytes exceed the cap") {
        const auto text = u"#" + std::u16string(32730, u'a') + u"\nMeteorUri=https://example.test/base\n";
        data = utf16_fixture(text, false);
        REQUIRE(data.size() == 65538);
    }
    const auto path = temp.write("never-echo-this-oversized.ini", data);
    expect_error(path, "file_too_large", data);
    CHECK(hawken::test::read_bytes(path) == data);
}

TEST_CASE("Meteor INI accepts exactly 64KiB retaining original provenance") {
    hawken::test::TempDirectory temp;
    std::string data;
    SUBCASE("UTF8 without BOM") {
        data = "#" + std::string(65498, 'a') + "\nMeteorUri=https://example.test/base\n";
    }
    SUBCASE("UTF8 BOM counts toward original bytes") {
        data = bytes({0xef, 0xbb, 0xbf}) + "#" + std::string(65495, 'a') +
            "\nMeteorUri=https://example.test/base\n";
    }
    REQUIRE(data.size() == 65536);
    const auto path = temp.write("settings.ini", data);
    const auto setting = hawken::read_meteor_ini(path);
    CHECK(setting.uri == "https://example.test/base");
    CHECK(setting.line_number == 2);
    CHECK(setting.byte_offset == 65500);
    CHECK(setting.encoding_name == "utf-8");
    CHECK(hawken::test::read_bytes(path) == data);
}
