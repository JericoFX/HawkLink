#include "hawken/wreckage_trial.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <array>
#include <stdexcept>
#include <string>

namespace {
constexpr std::array<const char*,7> package_names{
    "Core.upk","Engine.upk","HawkenGame.upk","VS-Wreckage.upk","HawkenDM_SF.upk",
    "GameFramework.upk","UDKBase.upk"};
const std::filesystem::path cooked="HawkenGame/CookedPCConsole";
void expect_asset_error(const std::filesystem::path& root) {
    try { (void)hawken::game::load_wreckage_trial(root); }
    catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "invalid Wreckage trial assets");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed trial asset error"); return; }
    FAIL_CHECK("unverified trial assets accepted");
}
}

// Ordinary tests only touch private synthetic files, never require an installed
// client and cannot spoof a successful hash. Original-file success is checked
// separately by the parent-owned read-only probe, with exact plan assertions.
TEST_CASE("Wreckage trial rejects relative roots with a fixed error") {
    for (const std::filesystem::path root: {std::filesystem::path{},
             std::filesystem::path("."),std::filesystem::path("private-relative-root")})
        expect_asset_error(root);
}
TEST_CASE("Wreckage trial rejects an absent absolute root without creating it") {
    hawken::test::TempDirectory directory;
    const auto root=directory.path()/"absent-private-root";
    REQUIRE(root.is_absolute());
    expect_asset_error(root);
    CHECK_FALSE(std::filesystem::exists(root));
}
TEST_CASE("Wreckage trial rejects a regular file as game root without mutation") {
    hawken::test::TempDirectory directory;
    const auto root=directory.write("private-root-file","public synthetic root bytes");
    expect_asset_error(root);
    CHECK(hawken::test::read_bytes(root) == "public synthetic root bytes");
}
TEST_CASE("Wreckage trial rejects missing fixed packages in an existing directory") {
    hawken::test::TempDirectory directory;
    std::filesystem::create_directories(directory.path()/cooked);
    expect_asset_error(directory.path());
    for (const auto name:package_names)
        CHECK_FALSE(std::filesystem::exists(directory.path()/cooked/name));
}
TEST_CASE("Wreckage trial does not accept a partial dummy package set") {
    hawken::test::TempDirectory directory;
    const auto core=directory.write(cooked/"Core.upk","public synthetic package bytes");
    expect_asset_error(directory.path());
    CHECK(hawken::test::read_bytes(core) == "public synthetic package bytes");
    CHECK_FALSE(std::filesystem::exists(directory.path()/cooked/"Engine.upk"));
}
TEST_CASE("Wreckage trial rejects a complete altered dummy set without changing files") {
    hawken::test::TempDirectory directory;
    for (const auto name:package_names)
        directory.write(cooked/name,"public synthetic package bytes");
    directory.write(cooked/"HawkenDM_SF.upk","public synthetic package bytes altered");
    expect_asset_error(directory.path());
    for (const auto name:package_names) {
        const auto expected=std::string(name)=="HawkenDM_SF.upk"
            ? "public synthetic package bytes altered" : "public synthetic package bytes";
        CHECK(hawken::test::read_bytes(directory.path()/cooked/name) == expected);
    }
}
