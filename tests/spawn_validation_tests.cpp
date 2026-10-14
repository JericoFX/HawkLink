#include "hawken/spawn_validation.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace {
using hawken::game::SpawnValidationChecks;
using hawken::game::SpawnValidationStatus;
using hawken::game::aggregate_spawn_validation;
constexpr auto pending = SpawnValidationStatus::in_process;
constexpr auto failed = SpawnValidationStatus::failed;
constexpr auto succeeded = SpawnValidationStatus::succeeded;
static_assert(noexcept(aggregate_spawn_validation(SpawnValidationChecks{})));
}

TEST_CASE("Spawn validation exhausts the independent four-state truth partition") {
    // Base-four word space: declaratively mark the Cartesian slices whose
    // selected digit is 2. This oracle never inspects the production result.
    std::array<SpawnValidationStatus, 1024> expected;
    expected.fill(pending);
    for (std::size_t slot = 0; slot < 5; ++slot) {
        const std::size_t stride = std::size_t{1} << (2 * slot);
        for (std::size_t upper = 0; upper < 1024 / (4 * stride); ++upper)
            for (std::size_t lower = 0; lower < stride; ++lower)
                expected[upper * (4 * stride) + 2 * stride + lower] = failed;
    }
    expected[1023] = succeeded; // The sole word 33333.
    CHECK(std::count(expected.begin(), expected.end(), failed) == 781);
    CHECK(std::count(expected.begin(), expected.end(), succeeded) == 1);
    CHECK(std::count(expected.begin(), expected.end(), pending) == 242);

    for (std::size_t word = 0; word < expected.size(); ++word) {
        CAPTURE(word);
        SpawnValidationChecks checks{};
        for (std::size_t slot = 0; slot < checks.size(); ++slot)
            checks[slot] = static_cast<std::uint8_t>((word >> (2 * slot)) & 3);
        const auto before = checks;
        CHECK(aggregate_spawn_validation(checks) == expected[word]);
        CHECK(checks == before);
    }
}

TEST_CASE("Spawn validation preserves every raw-byte boundary in each position") {
    // Fixed single-coordinate truth table with the other four succeeded.
    std::array<SpawnValidationStatus, 256> expected;
    expected.fill(pending);
    expected[2] = failed;
    expected[3] = succeeded;
    for (std::size_t slot = 0; slot < 5; ++slot) {
        CAPTURE(slot);
        for (std::size_t raw = 0; raw < expected.size(); ++raw) {
            CAPTURE(raw);
            SpawnValidationChecks checks{3, 3, 3, 3, 3};
            checks[slot] = static_cast<std::uint8_t>(raw);
            CHECK(aggregate_spawn_validation(checks) == expected[raw]);
        }
    }
}

TEST_CASE("Spawn validation failure takes priority over all other byte values") {
    for (std::size_t slot = 0; slot < 5; ++slot) {
        CAPTURE(slot);
        for (std::size_t raw = 0; raw < 256; ++raw) {
            CAPTURE(raw);
            SpawnValidationChecks checks{};
            checks.fill(static_cast<std::uint8_t>(raw));
            checks[slot] = 2;
            CHECK(aggregate_spawn_validation(checks) == failed);
        }
    }
}

TEST_CASE("Spawn validation role permutations retain explicit native outcomes") {
    struct Example {
        SpawnValidationChecks checks;
        SpawnValidationStatus expected;
    };
    const std::array<Example, 5> examples{{
        {{0, 0, 0, 0, 0}, pending},
        {{1, 1, 1, 1, 1}, pending},
        {{3, 3, 3, 3, 3}, succeeded},
        {{0, 1, 3, 4, 255}, pending},
        {{0, 2, 3, 4, 255}, failed}
    }};
    for (const auto& example : examples) {
        auto permutation = example.checks;
        std::sort(permutation.begin(), permutation.end());
        do {
            CHECK(aggregate_spawn_validation(permutation) == example.expected);
        } while (std::next_permutation(permutation.begin(), permutation.end()));
    }
}
