#include "hawken/sha256.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>

TEST_CASE("SHA-256 matches independent empty and abc vectors") {
    CHECK(hawken::sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(hawken::sha256(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST_CASE("File hashing streams every block and final remainder") {
    hawken::test::TempDirectory fixture;
    const auto path = fixture.write("million-a.bin", std::string(1000000, 'a'));
    CHECK(hawken::sha256_file(path) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(std::filesystem::file_size(path) == 1000000);
}
TEST_CASE("Missing file hashing fails rather than producing empty digest") {
    hawken::test::TempDirectory fixture;
    CHECK_THROWS(hawken::sha256_file(fixture.path() / "missing.bin"));
}
