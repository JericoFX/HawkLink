#include "hawken/weapon_refire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>

namespace {
using hawken::game::weapon_fire::OwnerRefireScales;
using hawken::game::weapon_fire::refire_interval;

void check_bits(const std::optional<float>& result, std::uint32_t expected) {
    REQUIRE(result.has_value());
    CHECK(std::bit_cast<std::uint32_t>(*result) == expected);
}

static_assert(noexcept(refire_interval(std::span<const float>{}, 0, 0, std::nullopt)));
}

TEST_CASE("Weapon refire without a cast owner preserves the selected raw float") {
    // The native no-owner MOVSS/RET bypasses multiplication and the minimum.
    constexpr std::array<std::uint32_t, 8> words{
        0x00000000, 0x80000000, 0x3f000000, 0xc0000000,
        0x7fc12345, 0x7f800000, 0xff800000, 0x00000001};
    for (const auto word : words) {
        CAPTURE(word);
        const std::array values{std::bit_cast<float>(word)};
        check_bits(refire_interval(values, 0, -7, std::nullopt), word);
        CHECK(std::bit_cast<std::uint32_t>(values[0]) == word);
    }
}

TEST_CASE("Weapon refire selects the byte mode and treats every nonzero hand alike") {
    const std::array values{0.25F, 0.5F, 2.0F};
    const auto before = values;
    const OwnerRefireScales scales{2.0F, 4.0F};
    check_bits(refire_interval(values, 1, 0, scales), 0x3f800000); // 0.5 * 2 = 1.
    check_bits(refire_interval(values, 1, 1, scales), 0x40000000); // 0.5 * 4 = 2.
    check_bits(refire_interval(values, 1, -1, scales), 0x40000000);
    check_bits(refire_interval(values, 1, INT32_MIN, scales), 0x40000000);
    check_bits(refire_interval(values, 2, INT32_MAX, scales), 0x41000000); // 2 * 4 = 8.
    CHECK(values == before);
    CHECK(scales.hand_zero == 2.0F);
    CHECK(scales.other_hand == 4.0F);
}

TEST_CASE("Weapon refire owner path applies the exact ordered minimum boundary") {
    struct Example { std::uint32_t input; std::uint32_t expected; };
    // Literal IEEE words around 0.01F; expected values do not call a clamp helper.
    constexpr std::array examples{
        Example{0x00000000, 0x3c23d70a}, Example{0x80000000, 0x3c23d70a},
        Example{0xbf800000, 0x3c23d70a}, Example{0xff800000, 0x3c23d70a},
        Example{0x3c23d709, 0x3c23d70a}, Example{0x3c23d70a, 0x3c23d70a},
        Example{0x3c23d70b, 0x3c23d70b}};
    for (const auto& example : examples) {
        CAPTURE(example.input);
        const std::array values{std::bit_cast<float>(example.input)};
        check_bits(refire_interval(values, 0, 0, OwnerRefireScales{1.0F, 1.0F}),
                   example.expected);
    }
}

TEST_CASE("Weapon refire unordered products clamp while positive infinity survives") {
    struct Example { std::uint32_t base; std::uint32_t scale; std::uint32_t expected; };
    // COMISS/JAE takes neither the below nor unordered value through unchanged.
    constexpr std::array examples{
        Example{0x7fc12345, 0x3f800000, 0x3c23d70a},
        Example{0x3f800000, 0x7fc54321, 0x3c23d70a},
        Example{0x7f800000, 0x00000000, 0x3c23d70a},
        Example{0x7f800000, 0x3f800000, 0x7f800000},
        Example{0x3f800000, 0xff800000, 0x3c23d70a}};
    for (const auto& example : examples) {
        CAPTURE(example.base);
        CAPTURE(example.scale);
        const std::array values{std::bit_cast<float>(example.base)};
        check_bits(refire_interval(values, 0, 0,
                   OwnerRefireScales{std::bit_cast<float>(example.scale), 1.0F}),
                   example.expected);
    }
}

TEST_CASE("Weapon refire uses a single precision product before its minimum") {
    struct Example { std::uint32_t base; std::uint32_t scale; std::uint32_t expected; };
    constexpr std::array examples{
        // (1 + 2^-23)^2 rounds to 1 + 2^-22 in binary32.
        Example{0x3f800001, 0x3f800001, 0x3f800002},
        // Finite overflow remains +infinity, rather than being rejected.
        Example{0x7f7fffff, 0x40000000, 0x7f800000},
        // Smallest positive subnormal times 0.5 rounds to zero, then clamps.
        Example{0x00000001, 0x3f000000, 0x3c23d70a},
        Example{0xbf800000, 0xc0000000, 0x40000000}};
    for (const auto& example : examples) {
        CAPTURE(example.base);
        CAPTURE(example.scale);
        const std::array values{std::bit_cast<float>(example.base)};
        check_bits(refire_interval(values, 0, 1,
                   OwnerRefireScales{1.0F, std::bit_cast<float>(example.scale)}),
                   example.expected);
    }
}

TEST_CASE("Weapon refire rejects only modes outside the supplied interval span") {
    const std::array one{0.5F};
    CHECK_FALSE(refire_interval(std::span<const float>{}, 0, 0, std::nullopt));
    CHECK_FALSE(refire_interval(one, 1, 0, std::nullopt));
    CHECK_FALSE(refire_interval(one, 255, 1, OwnerRefireScales{2.0F, 4.0F}));
    std::array<float, 256> all_modes{};
    all_modes[255] = 0.25F;
    check_bits(refire_interval(all_modes, 255, 0, std::nullopt), 0x3e800000);
    check_bits(refire_interval(all_modes, 255, -2, OwnerRefireScales{2.0F, 4.0F}),
               0x3f800000);
}
