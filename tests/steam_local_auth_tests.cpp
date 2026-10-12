#include <hawken/steam_local_auth.hpp>
#include <doctest/doctest.h>
#include <array>
#include <vector>

namespace {
// Hand-checked synthetic bytes: SteamID at0, response at8, owner at12.
// A positive decoding result is not a native callback, license or login fixture.
constexpr std::uint64_t expected_hint = 0x0102030405060708ULL;
constexpr std::array<std::uint8_t, 20> ok_payload{
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0, 0, 0, 0,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11
};
using hawken::auth::detail::callback143_is_ok;
}

TEST_CASE("Steam callback decoder recognizes only the exact packed approved record") {
    CHECK(callback143_is_ok(ok_payload, expected_hint));
}

TEST_CASE("Steam callback decoder rejects truncated payloads before field access") {
    for (std::size_t size = 0; size < 20; ++size) {
        INFO(size);
        CHECK_FALSE(callback143_is_ok(std::span(ok_payload).first(size), expected_hint));
    }
}

TEST_CASE("Steam callback decoder rejects natural Win64 padding and overlong records") {
    for (const std::size_t size : {21U, 24U, 1024U}) {
        auto payload = std::vector<std::uint8_t>(ok_payload.begin(), ok_payload.end());
        payload.resize(size);
        CHECK_FALSE(callback143_is_ok(payload, expected_hint));
    }
}

TEST_CASE("Steam callback decoder compares all 64 identity bits") {
    CHECK_FALSE(callback143_is_ok(ok_payload, 0x0102030405060709ULL));
    CHECK_FALSE(callback143_is_ok(ok_payload, 0x1102030405060708ULL));
    CHECK_FALSE(callback143_is_ok(ok_payload, 0));
}

TEST_CASE("Steam callback decoder never approves a zero identity hint") {
    auto payload = ok_payload;
    for (std::size_t index = 0; index < 8; ++index) payload[index] = 0;
    CHECK_FALSE(callback143_is_ok(payload, 0));
}

TEST_CASE("Steam callback decoder rejects nonzero response at the packed offset") {
    for (const auto response : std::array<std::uint8_t, 11>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 255}) {
        auto payload = ok_payload;
        payload[8] = response;
        CHECK_FALSE(callback143_is_ok(payload, expected_hint));
    }
}

TEST_CASE("Steam callback decoder rejects high bytes of the 32 bit response") {
    for (const std::size_t index : {9U, 10U, 11U}) {
        auto payload = ok_payload;
        payload[index] = 1;
        CHECK_FALSE(callback143_is_ok(payload, expected_hint));
    }
}
