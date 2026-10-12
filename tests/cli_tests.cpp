#include "hawken/cli.hpp"
#include "hawken/sha256.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {
struct Fixture {
    hawken::test::TempDirectory temp;
    std::filesystem::path root;
    std::filesystem::path manifest;
    Fixture() {
        root = temp.path() / "game";
        temp.write("game/HawkenGame/CookedPCConsole/Test.upk", "abc");
        manifest = temp.write("manifest.json", R"({"schema":"hawken.original-package-sha256-baseline.v1","hash_algorithm":"SHA-256","package_count":1,"entries":[{"relative_path":"HawkenGame\\CookedPCConsole\\Test.upk","size_bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})");
    }
    std::vector<std::string> args(const std::string& command) const {
        return {command, "--game", root.string(), "--manifest", manifest.string(),
                "--manifest-sha256", hawken::sha256_file(manifest)};
    }
};
}

TEST_CASE("CLI accepts intact files and rejects same-size changes without repair") {
    Fixture fixture;
    std::ostringstream output, error;
    CHECK(hawken::run_cli(fixture.args("verify-packages"), output, error) == 0);
    CHECK(nlohmann::json::parse(output.str()).at("passed") == true);
    CHECK(error.str().empty());
    fixture.temp.write("game/HawkenGame/CookedPCConsole/Test.upk", "abd");
    output.str("");
    CHECK(hawken::run_cli(fixture.args("verify-packages"), output, error) == 1);
    CHECK(nlohmann::json::parse(output.str()).at("passed") == false);
    CHECK(hawken::test::read_bytes(fixture.root / "HawkenGame/CookedPCConsole/Test.upk") == "abd");
}

TEST_CASE("Preflight never treats valid configuration and package hashes as admission") {
    Fixture fixture;
    fixture.temp.write("game/Binaries/Win64/StormConsumerSettings.ini", "MeteorUri = http://127.0.0.1:18080/community\r\n");
    fixture.temp.write("game/Binaries/Win64/HawkenGame-Win64-Shipping.exe", "fake-exe");
    fixture.temp.write("game/Binaries/Win64/Meteor.Sdk.Cpp.Win32.Client64.dll", "fake-sdk");
    std::ostringstream output, error;
    CHECK(hawken::run_cli(fixture.args("preflight"), output, error) == 2);
    const auto result = nlohmann::json::parse(output.str());
    CHECK(result.at("readyForPlayers") == false);
    CHECK(result.at("packageIntegrity").at("passed") == true);
    CHECK_FALSE(result.at("meteorIni").contains("uri"));
    CHECK(result.at("meteorIni").at("uriOrigin") == "http://127.0.0.1:18080");
    CHECK(result.at("meteorIni").at("basePathRedacted") == true);
    CHECK(result.at("effectiveEndpoint").at("status") == "UNSUPPORTED_BUILD");
    CHECK(result.at("steamAuthentication").at("status") == "NOT_VERIFIED");
    CHECK(result.at("gameServer").at("status") == "NOT_VERIFIED");
    CHECK(hawken::test::read_bytes(fixture.root / "Binaries/Win64/StormConsumerSettings.ini") ==
          "MeteorUri = http://127.0.0.1:18080/community\r\n");
}

TEST_CASE("CLI requires explicit digest for custom manifest and rejects duplicate options") {
    Fixture fixture;
    std::ostringstream output, error;
    CHECK(hawken::run_cli({"verify-packages", "--game", fixture.root.string(), "--manifest", fixture.manifest.string()}, output, error) == 64);
    CHECK(hawken::run_cli({"verify-packages", "--game", "a", "--game", "b"}, output, error) == 64);
    CHECK(hawken::run_cli({"unknown", "--secret", "never-print-this"}, output, error) == 64);
    CHECK(error.str().find("never-print-this") == std::string::npos);
}

TEST_CASE("Preflight errors never echo a credential-bearing base URI") {
    Fixture fixture;
    fixture.temp.write("game/Binaries/Win64/StormConsumerSettings.ini", "MeteorUri=https://user:never-print-this@localhost/?ticket=secret\n");
    std::ostringstream output, error;
    CHECK(hawken::run_cli(fixture.args("preflight"), output, error) == 2);
    CHECK((output.str() + error.str()).find("never-print-this") == std::string::npos);
    CHECK((output.str() + error.str()).find("ticket=secret") == std::string::npos);
    CHECK(nlohmann::json::parse(output.str()).at("meteorIni").at("errorCode") == "invalid_uri");
}

TEST_CASE("Preflight omits valid URI paths that could carry a secret") {
    Fixture fixture;
    fixture.temp.write("game/Binaries/Win64/StormConsumerSettings.ini",
        "MeteorUri=https://localhost:18080/never-print-this/%32\n");
    std::ostringstream output, error;
    CHECK(hawken::run_cli(fixture.args("preflight"), output, error) == 2);
    const auto combined = output.str() + error.str();
    CHECK(combined.find("never-print-this") == std::string::npos);
    CHECK(combined.find("%32") == std::string::npos);
    const auto ini = nlohmann::json::parse(output.str()).at("meteorIni");
    CHECK_FALSE(ini.contains("uri"));
    CHECK(ini.at("uriOrigin") == "https://localhost:18080");
    CHECK(ini.at("basePathRedacted") == true);
}
