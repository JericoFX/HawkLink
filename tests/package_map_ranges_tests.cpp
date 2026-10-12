#include "hawken/package_map_ranges.hpp"
#include <doctest/doctest.h>
#include <bit>
#include <limits>

namespace {
using namespace hawken::wire;
using namespace hawken::wire::packages;
template<class F> void fixed_error(F action,const char* code) {
    try { action(); }
    catch (const WireError& error) {
        CHECK(error.code == code);
        CHECK(std::string(error.what()) == "Game wire data could not be read or validated.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed WireError"); return; }
    FAIL_CHECK("rejected coordinate was accepted");
}
const std::vector<ObjectRange> manual{{0,7},{7,0},{7,2},{9,4}};
}

TEST_CASE("Ranges use generation counts and published order independently") {
    const std::vector<PackageGenerations> metadata{{{7,2},2},{{0},1},{{2},1},{{4},1}};
    const std::vector<ObjectRange> expected{{0,2},{2,0},{2,2},{4,4}};
    CHECK(build_ranges(metadata) == expected);
}
TEST_CASE("Older peer generation clamps against latest nonmonotonic count") {
    const std::vector<PackageGenerations> metadata{{{9,2,7},1},{{9,2,7},2},{{9,2,7},3}};
    const std::vector<ObjectRange> expected{{0,7},{7,2},{9,7}};
    CHECK(build_ranges(metadata) == expected);
}
TEST_CASE("Older smaller count is retained instead of latest generation") {
    const std::vector<PackageGenerations> metadata{{{2,9},1},{{2,9},2}};
    const std::vector<ObjectRange> expected{{0,2},{2,9}};
    CHECK(build_ranges(metadata) == expected);
}
TEST_CASE("Later peer generation uses local latest count") {
    const std::vector<PackageGenerations> metadata{{{7,2},3},{{7,2},0x7fffffffU}};
    const std::vector<ObjectRange> expected{{0,2},{2,2}};
    CHECK(build_ranges(metadata) == expected);
}
TEST_CASE("Zero generation counts are valid without a fabricated coordinate") {
    const std::vector<PackageGenerations> metadata{{{0,5},1},{{3,0},2}};
    const std::vector<ObjectRange> expected{{0,0},{0,0}};
    CHECK(build_ranges(metadata) == expected);
    CHECK_FALSE(decode_index(expected,0).has_value());
}
TEST_CASE("Empty package generations are invalid metadata") {
    const std::vector<PackageGenerations> metadata{{{},1}};
    fixed_error([&] { (void)build_ranges(metadata); },"invalid_metadata");
}
TEST_CASE("Generation zero is unacknowledged and rejected by this builder") {
    const std::vector<PackageGenerations> metadata{{{3},0}};
    fixed_error([&] { (void)build_ranges(metadata); },"invalid_generation");
}
TEST_CASE("Negative signed generation bit patterns are rejected") {
    for (const auto value : {0x80000000U,0xffffffffU}) {
        const std::vector<PackageGenerations> metadata{{{3},value}};
        fixed_error([&] { (void)build_ranges(metadata); },"invalid_generation");
    }
}
TEST_CASE("All count words must fit nonnegative signed32 even when unselected") {
    for (const auto value : {0x80000000U,0xffffffffU}) {
        CHECK(std::bit_cast<std::int32_t>(value) < 0);
        const std::vector<PackageGenerations> metadata{{{value,2},2}};
        fixed_error([&] { (void)build_ranges(metadata); },"invalid_metadata");
    }
}
TEST_CASE("Checked cumulative total admits exactly 2 to31 coordinates") {
    const std::vector<PackageGenerations> metadata{{{0x7fffffffU},1},{{1},1},{{0},1}};
    const std::vector<ObjectRange> expected{{0,0x7fffffffU},{0x7fffffffU,1},{0x80000000U,0}};
    CHECK(build_ranges(metadata) == expected);
    CHECK(encode_index(expected,1,0) == 0x7fffffffU);
    CHECK((decode_index(expected,0x7fffffffU) == Coordinate{1,0}));
}
TEST_CASE("One count beyond31 bit total is rejected without native wrap") {
    const std::vector<PackageGenerations> metadata{{{0x7fffffffU},1},{{2},1}};
    fixed_error([&] { (void)build_ranges(metadata); },"range_overflow");
}
TEST_CASE("Multiple signed counts cannot wrap cumulative u32") {
    const std::vector<PackageGenerations> metadata{{{0x7fffffffU},1},{{0x7fffffffU},1},{{2},1}};
    fixed_error([&] { (void)build_ranges(metadata); },"range_overflow");
}
TEST_CASE("Empty range utility has no implied world package set") {
    CHECK(build_ranges({}).empty());
    CHECK_FALSE(decode_index({},0).has_value());
    fixed_error([&] { (void)encode_index({},0,0); },"index_unavailable");
}
TEST_CASE("Static zero coordinate is valid and zero count ranges are skipped") {
    CHECK(encode_index(manual,0,0) == 0);
    CHECK((decode_index(manual,0) == Coordinate{0,0}));
    CHECK((decode_index(manual,7) == Coordinate{2,0}));
    fixed_error([&] { (void)encode_index(manual,1,0); },"index_unavailable");
}
TEST_CASE("Forward index matches independent range boundary literals") {
    CHECK(encode_index(manual,0,6) == 6);
    CHECK(encode_index(manual,2,1) == 8);
    CHECK(encode_index(manual,3,0) == 9);
    CHECK(encode_index(manual,3,3) == 12);
}
TEST_CASE("Inverse index matches independent range boundary literals") {
    CHECK((decode_index(manual,6) == Coordinate{0,6}));
    CHECK((decode_index(manual,8) == Coordinate{2,1}));
    CHECK((decode_index(manual,9) == Coordinate{3,0}));
    CHECK((decode_index(manual,12) == Coordinate{3,3}));
}
TEST_CASE("Indices equal total or beyond have no coordinate") {
    CHECK_FALSE(decode_index(manual,13).has_value());
    CHECK_FALSE(decode_index(manual,0x7fffffffU).has_value());
}
TEST_CASE("Flat high signed bit is outside primary31bit wire range") {
    for (const auto index : {0x80000000U,0xffffffffU})
        fixed_error([&] { (void)decode_index(manual,index); },"invalid_value");
}
TEST_CASE("Native minus one sentinel and all other negative locals reject") {
    for (const std::int32_t local : {-1,-2,std::numeric_limits<std::int32_t>::min()})
        fixed_error([&] { (void)encode_index(manual,0,local); },"index_unavailable");
}
TEST_CASE("Local equal count and larger cannot alias following package") {
    fixed_error([&] { (void)encode_index(manual,0,7); },"index_unavailable");
    fixed_error([&] { (void)encode_index(manual,2,2); },"index_unavailable");
    fixed_error([&] { (void)encode_index(manual,0,0x7fffffff); },"index_unavailable");
}
TEST_CASE("Invalid package slots are rejected without dereferencing") {
    fixed_error([&] { (void)encode_index(manual,manual.size(),0); },"index_unavailable");
    fixed_error([&] { (void)encode_index(manual,std::numeric_limits<std::size_t>::max(),0); },"index_unavailable");
}
TEST_CASE("Public ranges require zero first base and contiguous cumulative bases") {
    for (const auto ranges : {std::vector<ObjectRange>{{1,2}},
                              std::vector<ObjectRange>{{0,2},{3,1}},
                              std::vector<ObjectRange>{{0,2},{1,1}}}) {
        fixed_error([&] { (void)encode_index(ranges,0,0); },"invalid_ranges");
        fixed_error([&] { (void)decode_index(ranges,0); },"invalid_ranges");
    }
}
TEST_CASE("Public range count words and total are checked independently") {
    for (const auto ranges : {std::vector<ObjectRange>{{0,0x80000000U}},
                              std::vector<ObjectRange>{{0,0xffffffffU}},
                              std::vector<ObjectRange>{{0,0x7fffffffU},{0x7fffffffU,2}}}) {
        fixed_error([&] { (void)encode_index(ranges,0,0); },"invalid_ranges");
        fixed_error([&] { (void)decode_index(ranges,0); },"invalid_ranges");
    }
}
TEST_CASE("Malformed trailing range is rejected even for valid first coordinate") {
    const std::vector<ObjectRange> ranges{{0,2},{99,0}};
    fixed_error([&] { (void)encode_index(ranges,0,0); },"invalid_ranges");
    fixed_error([&] { (void)decode_index(ranges,0); },"invalid_ranges");
}
TEST_CASE("Arithmetic roundtrip supplements independent boundary vectors") {
    for (std::size_t slot=0;slot<manual.size();++slot)
        for (std::uint32_t local=0;local<manual[slot].count;++local) {
            const auto flat=encode_index(manual,slot,static_cast<std::int32_t>(local));
            CHECK((decode_index(manual,flat) == Coordinate{slot,local}));
        }
}
TEST_CASE("Published trial arithmetic does not substitute CDO export ordinal") {
    const std::vector<PackageGenerations> metadata{{{1639},1},{{36697},1},{{49101},1},{{7468},1},{{533},1}};
    const std::vector<ObjectRange> expected{{0,1639},{1639,36697},{38336,49101},{87437,7468},{94905,533}};
    CHECK(build_ranges(metadata) == expected);
    CHECK(encode_index(expected,2,18053) == 56389);
    CHECK((decode_index(expected,56389) == Coordinate{2,18053}));
    CHECK(encode_index(expected,2,18048) == 56384);
}
