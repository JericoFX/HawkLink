#include "hawken/package_integrity.hpp"
#include "hawken/sha256.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr auto abc_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

nlohmann::json entry(const std::string& path) {
    return {{"relative_path", path}, {"sha256", abc_sha256}, {"size_bytes", 3}};
}

struct PackageFixture {
    hawken::test::TempDirectory temp;
    std::filesystem::path root = temp.path() / "game";
    std::filesystem::path manifest = temp.path() / "manifest.json";
    std::string trusted_hash;

    PackageFixture() { std::filesystem::create_directory(root); }

    void package(const std::filesystem::path& relative, const std::string& bytes = "abc") {
        temp.write(std::filesystem::path("game") / relative, bytes);
    }

    void manifest_text(const std::string& bytes) {
        temp.write("manifest.json", bytes);
        trusted_hash = hawken::sha256_file(manifest);
    }

    void baseline(const nlohmann::json& entries) {
        manifest_text(nlohmann::json{
            {"schema", "hawken.original-package-sha256-baseline.v1"},
            {"hash_algorithm", "SHA-256"},
            {"package_count", entries.size()}, {"entries", entries}}.dump());
    }

    hawken::PackageIntegrityReport verify() const {
        return hawken::verify_packages(root, manifest, trusted_hash);
    }
};

void check_issue(const hawken::PackageIntegrityReport& report,
                 const std::string& code, const std::string& path) {
    CHECK_FALSE(report.passed);
    CHECK(std::any_of(report.issues.begin(), report.issues.end(), [&](const auto& issue) {
        return issue.code == code && issue.relative_path == path;
    }));
    for (const auto& issue : report.issues) CHECK_FALSE(issue.detail.empty());
}

// Doctest's skip decorator records unavailable link coverage as skipped, not passed.
bool links_unavailable() {
    hawken::test::TempDirectory probe;
    const auto target = probe.write("target", "probe");
    std::error_code error;
    std::filesystem::create_symlink(target, probe.path() / "file-link", error);
    if (!error) {
        std::filesystem::create_directory_symlink(probe.path(), probe.path() / "directory-link", error);
    }
    if (error) std::cerr << "Package integrity link tests SKIPPED: " << error.message() << '\n';
    return static_cast<bool>(error);
}
const bool skip_links = links_unavailable();
}

TEST_CASE("Integrity accepts intact packages and normalizes Windows separators") {
    PackageFixture fixture;
    fixture.package("CookedPC/Test.upk");
    fixture.baseline(nlohmann::json::array({entry("CookedPC\\Test.upk")}));

    const auto report = fixture.verify();

    CHECK(report.passed);
    CHECK(report.issues.empty());
    CHECK(report.verified_count == 1);
    CHECK(report.manifest_sha256 == fixture.trusted_hash);
}

TEST_CASE("Integrity detects same-size alteration despite historical matching fields") {
    PackageFixture fixture;
    fixture.package("Test.upk", "abd");
    auto expected = entry("Test.upk");
    expected["observed_sha256"] = abc_sha256;
    expected["matches_baseline"] = true;
    fixture.baseline(nlohmann::json::array({expected}));
    check_issue(fixture.verify(), "PACKAGE_HASH_MISMATCH", "Test.upk");
    CHECK(hawken::test::read_bytes(fixture.root / "Test.upk") == "abd");
}

TEST_CASE("Integrity verifies all allowed extensions without altering input bytes") {
    PackageFixture fixture;
    fixture.package("First.UPK");
    fixture.package("Nested/Second.u");
    fixture.package("Third.udk");
    fixture.baseline(nlohmann::json::array({entry("First.UPK"), entry("Nested/Second.u"), entry("Third.udk")}));
    const auto manifest_before = hawken::test::read_bytes(fixture.manifest);

    const auto report = fixture.verify();

    CHECK(report.passed);
    CHECK(report.verified_count == 3);
    CHECK(hawken::test::read_bytes(fixture.manifest) == manifest_before);
    CHECK(hawken::test::read_bytes(fixture.root / "Nested/Second.u") == "abc");
}

TEST_CASE("Integrity detects incorrect package size") {
    PackageFixture fixture;
    fixture.package("Test.upk", "abcd");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(fixture.verify(), "PACKAGE_SIZE_MISMATCH", "Test.upk");
}

TEST_CASE("Integrity detects a missing package") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Missing.u")}));
    check_issue(fixture.verify(), "PACKAGE_MISSING", "Missing.u");
}

TEST_CASE("Integrity detects recursively nested extra packages") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.package("Nested/Extra.UDK");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(fixture.verify(), "PACKAGE_EXTRA", "Nested/Extra.UDK");
}

TEST_CASE("Integrity permits additional non-package files") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.package("Config/Settings.ini", "unrelated");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    CHECK(fixture.verify().passed);
}

TEST_CASE("Integrity rejects changed manifest bytes before verifying packages") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    fixture.temp.write("manifest.json", hawken::test::read_bytes(fixture.manifest) + " ");
    const auto report = fixture.verify();
    check_issue(report, "MANIFEST_HASH_MISMATCH", "");
    CHECK(report.verified_count == 0);
}

TEST_CASE("Integrity rejects an incorrect trusted manifest digest") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(hawken::verify_packages(fixture.root, fixture.manifest, std::string(64, '0')),
                "MANIFEST_HASH_MISMATCH", "");
}

TEST_CASE("Integrity rejects escaped absolute and drive-relative baseline paths") {
    for (const auto* path : {"../outside.upk", "..\\outside.upk", "CookedPC/../Test.upk",
                             "/absolute.upk", "C:\\absolute.upk", "\\\\server\\share\\absolute.upk",
                             "C:relative.upk", "Test.upk:stream.upk", "folder./Test.upk",
                             "NUL.upk", "./Test.upk", "CookedPC//Test.upk"}) {
        CAPTURE(path);
        PackageFixture fixture;
        fixture.baseline(nlohmann::json::array({entry(path)}));
        std::string normalized(path);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        check_issue(fixture.verify(), "MANIFEST_PATH_INVALID", normalized);
    }
}

TEST_CASE("Integrity rejects duplicates and case separator collisions in a Windows baseline") {
    for (const auto* second : {"CookedPC/Test.upk", "cookedpc\\test.UPK"}) {
        CAPTURE(second);
        PackageFixture fixture;
        fixture.baseline(nlohmann::json::array({entry("CookedPC/Test.upk"), entry(second)}));
        std::string normalized(second);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        check_issue(fixture.verify(), "MANIFEST_PATH_DUPLICATE", normalized);
    }
}

TEST_CASE("Integrity fails closed for Unicode baseline paths without claiming portable casefold") {
    PackageFixture fixture;
    const std::string unicode_path = "Test-\xC3\xA9.upk";
    fixture.baseline(nlohmann::json::array({entry(unicode_path)}));
    check_issue(fixture.verify(), "MANIFEST_PATH_INVALID", unicode_path);
}

TEST_CASE("Integrity still discovers Unicode extra packages recursively") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.package(std::filesystem::path(u8"Nested-\u00e9/Extra.upk"));
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(fixture.verify(), "PACKAGE_EXTRA", "Nested-\xC3\xA9/Extra.upk");
}

TEST_CASE("Integrity rejects invalid schema algorithm and package count") {
    PackageFixture fixture;
    nlohmann::json manifest = {{"schema", "hawken.original-package-sha256-baseline.v1"},
        {"hash_algorithm", "SHA-256"}, {"package_count", 1},
        {"entries", nlohmann::json::array({entry("Test.upk")})}};
    SUBCASE("Wrong schema") { manifest["schema"] = "unknown-schema"; }
    SUBCASE("Wrong algorithm") { manifest["hash_algorithm"] = "SHA-1"; }
    SUBCASE("Discordant count") { manifest["package_count"] = 2; }
    SUBCASE("Missing entries") { manifest.erase("entries"); }
    SUBCASE("Entries are not an array") { manifest["entries"] = nlohmann::json::object(); }
    SUBCASE("Fractional count") { manifest["package_count"] = 1.5; }
    SUBCASE("Negative count") { manifest["package_count"] = -1; }
    fixture.manifest_text(manifest.dump());
    check_issue(fixture.verify(), "MANIFEST_INVALID", "");
}

TEST_CASE("Integrity rejects empty and malformed manifest structures") {
    for (const auto* bytes : {"", "{}", "null", "not json"}) {
        CAPTURE(bytes);
        PackageFixture fixture;
        fixture.manifest_text(bytes);
        check_issue(fixture.verify(), "MANIFEST_INVALID", "");
    }
}

TEST_CASE("Integrity rejects an empty package baseline") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array());
    check_issue(fixture.verify(), "MANIFEST_INVALID", "");
}

TEST_CASE("Integrity rejects invalid expected hash and size fields") {
    PackageFixture fixture;
    auto expected = entry("Test.upk");
    SUBCASE("Non SHA-256 hash") { expected["sha256"] = "xyz"; }
    SUBCASE("Nonhexadecimal 64 character hash") { expected["sha256"] = std::string(64, 'z'); }
    SUBCASE("Negative size") { expected["size_bytes"] = -1; }
    SUBCASE("Fractional size") { expected["size_bytes"] = 3.5; }
    SUBCASE("Missing size") { expected.erase("size_bytes"); }
    fixture.baseline(nlohmann::json::array({expected}));
    check_issue(fixture.verify(), "MANIFEST_INVALID", "Test.upk");
}

TEST_CASE("Integrity refuses a baseline entry with a non-package extension") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Settings.ini")}));
    check_issue(fixture.verify(), "MANIFEST_PATH_INVALID", "Settings.ini");
}

TEST_CASE("Integrity rejects a linked package without changing its target" * doctest::skip(skip_links)) {
    PackageFixture fixture;
    const auto outside = fixture.temp.write("external.upk", "abc");
    std::filesystem::create_symlink(outside, fixture.root / "Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(fixture.verify(), "LINK_REJECTED", "Test.upk");
    CHECK(hawken::test::read_bytes(outside) == "abc");
}

TEST_CASE("Integrity rejects a directory link without traversing its target" * doctest::skip(skip_links)) {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.temp.write("outside/Extra.upk", "abc");
    std::filesystem::create_directory_symlink(fixture.temp.path() / "outside", fixture.root / "Linked");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    const auto report = fixture.verify();
    check_issue(report, "LINK_REJECTED", "Linked");
    CHECK(std::none_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.code == "PACKAGE_EXTRA";
    }));
}

TEST_CASE("Integrity reports a missing manifest as a read failure") {
    PackageFixture fixture;
    check_issue(hawken::verify_packages(fixture.root, fixture.manifest, abc_sha256), "READ_ERROR", "");
}

TEST_CASE("Integrity reports a directory manifest as a read failure") {
    PackageFixture fixture;
    std::filesystem::create_directory(fixture.manifest);
    check_issue(hawken::verify_packages(fixture.root, fixture.manifest, abc_sha256), "READ_ERROR", "");
}

TEST_CASE("Integrity rejects a malformed trusted hash") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(hawken::verify_packages(fixture.root, fixture.manifest, "not-a-sha256"),
                "MANIFEST_INVALID", "");
}

TEST_CASE("Integrity requires a readable game root") {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    check_issue(hawken::verify_packages(fixture.temp.path() / "missing", fixture.manifest, fixture.trusted_hash),
                "READ_ERROR", "");
}

#ifdef _WIN32
TEST_CASE("Integrity fails when a package is locked against reading") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    struct FileLock {
        HANDLE handle;
        ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } lock{CreateFileW((fixture.root / "Test.upk").c_str(), GENERIC_READ, 0, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(lock.handle != INVALID_HANDLE_VALUE);
    const auto report = fixture.verify();
    check_issue(report, "READ_ERROR", "Test.upk");
    CHECK(report.verified_count == 0);
}
#endif

TEST_CASE("Integrity rejects a linked game root" * doctest::skip(skip_links)) {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    const auto linked_root = fixture.temp.path() / "linked-game";
    std::filesystem::create_directory_symlink(fixture.root, linked_root);
    check_issue(hawken::verify_packages(linked_root, fixture.manifest, fixture.trusted_hash),
                "LINK_REJECTED", "");
}

TEST_CASE("Integrity rejects a linked manifest" * doctest::skip(skip_links)) {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    const auto linked_manifest = fixture.temp.path() / "linked-manifest.json";
    std::filesystem::create_symlink(fixture.manifest, linked_manifest);
    check_issue(hawken::verify_packages(fixture.root, linked_manifest, fixture.trusted_hash),
                "LINK_REJECTED", "");
}

TEST_CASE("Integrity rejects a link in the manifest ancestor chain" * doctest::skip(skip_links)) {
    PackageFixture fixture;
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    const auto linked_parent = fixture.temp.path() / "linked-parent";
    std::filesystem::create_directory_symlink(fixture.temp.path(), linked_parent);
    check_issue(hawken::verify_packages(fixture.root, linked_parent / "manifest.json", fixture.trusted_hash),
                "LINK_REJECTED", "");
}

TEST_CASE("Integrity rejects manifests above four MiB before hashing or parsing") {
    PackageFixture fixture;
    fixture.temp.write("manifest.json", std::string(4U * 1024U * 1024U + 1U, ' '));

    const auto report = hawken::verify_packages(fixture.root, fixture.manifest, std::string(64, '0'));

    check_issue(report, "MANIFEST_RESOURCE_LIMIT", "");
    CHECK(report.manifest_sha256.empty());
    CHECK(report.verified_count == 0);
}

TEST_CASE("Integrity permits a valid manifest exactly four MiB long") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    auto bytes = hawken::test::read_bytes(fixture.manifest);
    bytes.resize(4U * 1024U * 1024U, ' ');
    fixture.manifest_text(bytes);

    const auto report = fixture.verify();

    CHECK(report.passed);
    CHECK(report.verified_count == 1);
}

TEST_CASE("Integrity rejects more than 4096 declared package entries") {
    PackageFixture fixture;
    auto entries = nlohmann::json::array();
    for (int index = 0; index < 4097; ++index) {
        entries.push_back(entry("Package" + std::to_string(index) + ".upk"));
    }
    fixture.baseline(entries);

    const auto report = fixture.verify();

    check_issue(report, "MANIFEST_RESOURCE_LIMIT", "");
    CHECK(report.verified_count == 0);
}

TEST_CASE("Integrity accepts the 4096 entry boundary before reporting missing packages") {
    PackageFixture fixture;
    auto entries = nlohmann::json::array();
    for (int index = 0; index < 4096; ++index) {
        entries.push_back(entry("Package" + std::to_string(index) + ".upk"));
    }
    fixture.baseline(entries);

    const auto report = fixture.verify();

    check_issue(report, "PACKAGE_MISSING", "Package0.upk");
    CHECK(std::none_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.code == "MANIFEST_RESOURCE_LIMIT";
    }));
}

TEST_CASE("Integrity rejects tree entries deeper than 64 with the root at depth zero") {
    PackageFixture fixture;
    std::string relative;
    for (int depth = 0; depth < 64; ++depth) relative += "d/";
    relative += "Test.upk";
    fixture.package(relative);
    fixture.baseline(nlohmann::json::array({entry(relative)}));

    const auto report = fixture.verify();

    check_issue(report, "SCAN_RESOURCE_LIMIT", relative);
    CHECK(report.verified_count == 0);
}

TEST_CASE("Integrity permits a tree entry at depth 64 with the root at depth zero") {
    PackageFixture fixture;
    std::string relative;
    for (int depth = 0; depth < 63; ++depth) relative += "d/";
    relative += "Test.upk";
    fixture.package(relative);
    fixture.baseline(nlohmann::json::array({entry(relative)}));

    const auto report = fixture.verify();

    CHECK(report.passed);
    CHECK(report.verified_count == 1);
}

TEST_CASE("Integrity rejects JSON deeper than 16 containers even in ignored metadata") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    auto document = nlohmann::json::parse(hawken::test::read_bytes(fixture.manifest));
    auto metadata = nlohmann::json(nullptr);
    // Root object is container level one; sixteen arrays exceed the limit.
    for (int level = 0; level < 16; ++level) metadata = nlohmann::json::array({metadata});
    document["metadata"] = metadata;
    fixture.manifest_text(document.dump());

    const auto report = fixture.verify();

    check_issue(report, "MANIFEST_RESOURCE_LIMIT", "");
    CHECK(report.verified_count == 0);
}

TEST_CASE("Integrity permits JSON at 16 container levels including its root") {
    PackageFixture fixture;
    fixture.package("Test.upk");
    fixture.baseline(nlohmann::json::array({entry("Test.upk")}));
    auto document = nlohmann::json::parse(hawken::test::read_bytes(fixture.manifest));
    auto metadata = nlohmann::json(nullptr);
    for (int level = 0; level < 15; ++level) metadata = nlohmann::json::array({metadata});
    document["metadata"] = metadata;
    fixture.manifest_text(document.dump());

    const auto report = fixture.verify();

    CHECK(report.passed);
    CHECK(report.verified_count == 1);
}
