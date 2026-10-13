#include "connection_cache_reader.hpp"
#include "fname_hash_lookup.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using namespace hawken::probe;

// Directed synthetic memory only. Unmapped reads fail rather than returning zeros.
struct Memory {
    std::map<std::uint64_t, unsigned char> bytes;
    std::vector<std::pair<std::uint64_t, std::size_t>> reads;

    template<class T>
    void put(std::uint64_t address, T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = data[i];
    }

    template<class T>
    T read(std::uint64_t base, std::uint64_t offset = 0) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (offset > maximum - base || sizeof(T) - 1 > maximum - (base + offset))
            throw std::runtime_error("reader_overflow");
        const auto address = base + offset;
        reads.emplace_back(address, sizeof(T));
        std::array<unsigned char, sizeof(T)> data{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end()) throw std::runtime_error("reader_error");
            data[i] = found->second;
        }
        T value{};
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }
};

struct FNameBudgetMemory:Memory {
    std::size_t used=0;
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        if (sizeof(T)>1024*1024-used) throw std::runtime_error("read_limit");
        used+=sizeof(T);
        return Memory::read<T>(base,offset);
    }
};

template<class Action>
void expect_error(Action action, const char* code) {
    try { action(); }
    catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == code);
        return;
    }
    catch (...) { FAIL_CHECK("expected fixed runtime_error"); return; }
    FAIL_CHECK("cache reader accepted rejected input");
}

constexpr std::uint64_t map_address = 0x1000;
constexpr std::uint64_t dictionary = map_address + 0xb8;
constexpr std::uint64_t nodes = 0x4000;
constexpr std::uint64_t buckets = 0x5000;
constexpr std::uint64_t leaf_class = 0x10001000;
constexpr std::uint64_t root_class = 0x10002000;
constexpr std::uint64_t leaf_cache = 0x7000;
constexpr std::uint64_t root_cache = 0x7100;
constexpr std::uint64_t leaf_fields = 0x9000;
constexpr std::uint64_t root_fields = 0x9200;
constexpr std::uint64_t leaf_field = 0x20001000;
constexpr std::uint64_t root_field0 = 0x20002000;
constexpr std::uint64_t root_field1 = 0x20002010;

void node(Memory& memory, std::int32_t index, std::uint64_t key,
          std::uint64_t value, std::int32_t next) {
    const auto address = nodes + static_cast<std::uint64_t>(index) * 0x18;
    memory.put(address, key);
    memory.put(address + 8, value);
    memory.put(address + 0x10, next);
}

void cache(Memory& memory, std::uint64_t address, std::uint64_t type,
           std::uint64_t parent, std::int32_t base, std::uint64_t array,
           std::int32_t count, std::int32_t capacity) {
    memory.put(address + 0x10, base);
    memory.put(address + 0x14, parent);
    memory.put(address + 0x20, type);
    memory.put(address + 0x28, array);
    memory.put(address + 0x30, count);
    memory.put(address + 0x34, capacity);
}

void field(Memory& memory, std::uint64_t array, std::int32_t index,
           std::uint64_t address, std::int32_t id, std::int32_t secondary_value) {
    const auto record = array + static_cast<std::uint64_t>(index) * 0x10;
    memory.put(record, address);
    memory.put(record + 8, id);
    memory.put(record + 0xc, secondary_value);
}

struct Fixture {
    Memory memory;
    // Leaf -> root. The second eligible leaf field is deliberately absent:
    // a cache may contain a subset of the supplied eligible fields.
    std::vector<ExpectedClass> expected{
        {leaf_class, {leaf_field, 0x20001010}},
        {root_class, {root_field0, root_field1}}
    };

    Fixture() {
        memory.put(dictionary, nodes);
        memory.put(dictionary + 0x3c, std::uint64_t{0});
        memory.put(dictionary + 0x44, std::int32_t{1});
        memory.put(dictionary + 0x38, std::int32_t{0});
        node(memory, 0, leaf_class, leaf_cache, -1);
        cache(memory, leaf_cache, leaf_class, root_cache, 2, leaf_fields, 1, 2);
        cache(memory, root_cache, root_class, 0, 0, root_fields, 2, 3);
        field(memory, leaf_fields, 0, leaf_field, 2, -1);
        field(memory, root_fields, 0, root_field0, 0, 12);
        field(memory, root_fields, 1, root_field1, 1, 13);
    }

    CacheSnapshot sample() { return read_connection_cache(memory, map_address, expected); }

    void heap() {
        memory.put(dictionary + 0x3c, buckets);
        memory.put(dictionary + 0x44, std::int32_t{4});
        for (std::uint64_t i = 0; i < 4; ++i)
            memory.put(buckets + 4 * i, std::int32_t{-1});
        // Independent native hash literal for leaf_class selects bucket 3.
        memory.put(buckets + 12, std::int32_t{0});
    }
};

const std::vector<CacheClass> expected_classes{
    {2, 2, {{leaf_field, 2, -1}}},
    {0, 3, {{root_field0, 0, 12}, {root_field1, 1, 13}}}
};
}

TEST_CASE("Connection cache native hash uses wrapped low32 key") {
    // Literal results from the original 0430d0 arithmetic; no runtime reference hash.
    CHECK(class_hash(0) == 0xbd49d10dU);
    CHECK(class_hash(1) == 0x2027e3eaU);
    CHECK(class_hash(leaf_class) == 0xc52d1f67U);
    CHECK(class_hash(root_class) == 0x2fbaa9b9U);
    CHECK(class_hash(0xdeadbeefULL) == 0xfa26b3aeU);
    CHECK(class_hash(0xffffffffULL) == 0x6477d91bU);
    CHECK(class_hash(0x12345678ULL) == 0x1e6d8a84U);
    CHECK(class_hash(0x1234567810001000ULL) == 0xc52d1f67U);
}

TEST_CASE("Connection cache empty dictionary reads no buckets or array") {
    Memory memory;
    memory.put(dictionary + 0x44, std::int32_t{0});
    const std::array<ExpectedClass, 1> expected{{{leaf_class, {}}}};
    const auto result = read_connection_cache(memory, map_address, expected);
    CHECK_FALSE(result.present);
    CHECK(result.classes.empty());
    const std::vector<std::pair<std::uint64_t, std::size_t>> reads{
        {dictionary + 0x44, sizeof(std::int32_t)}};
    CHECK(memory.reads == reads);
    CHECK_FALSE(result.trace.empty());
}

TEST_CASE("Connection cache inline one bucket preserves two classes and inherited IDs") {
    Fixture fixture;
    const auto result = fixture.sample();
    REQUIRE(result.present);
    CHECK(result.classes == expected_classes);
    CHECK_FALSE(result.trace.empty());
}

TEST_CASE("Connection cache heap lookup compares full64 keys despite low32 collision") {
    Fixture fixture;
    fixture.heap();
    SUBCASE("full64 collision") {
        node(fixture.memory, 0, 0x1000010001000ULL, 0, 1);
        node(fixture.memory, 1, leaf_class, leaf_cache, -1);
    }
    SUBCASE("bucket and node index maximum allowed") {
        fixture.memory.put(dictionary + 0x44, std::int32_t{4096});
        // Native hash c52d1f67 & 4095 = 3943, independently literal.
        fixture.memory.put(buckets + 4 * 3943, std::int32_t{4095});
        node(fixture.memory, 4095, leaf_class, leaf_cache, -1);
    }
    const auto result = fixture.sample();
    REQUIRE(result.present);
    CHECK(result.classes == expected_classes);
    // A mismatching node's null value is not interpreted as a matched null cache.
    CHECK(std::find(fixture.memory.reads.begin(), fixture.memory.reads.end(),
                    std::pair<std::uint64_t, std::size_t>{nodes + 8, 8})
          == fixture.memory.reads.end());
}

TEST_CASE("Connection cache minus one is lookup absence only") {
    Fixture fixture;
    SUBCASE("bucket absent") { fixture.memory.put(dictionary + 0x38, std::int32_t{-1}); }
    SUBCASE("collision chain ends absent") {
        node(fixture.memory, 0, 0x1000010001000ULL, 0, -1);
    }
    const auto result = fixture.sample();
    CHECK_FALSE(result.present);
    CHECK(result.classes.empty());
    CHECK_FALSE(result.trace.empty());
}

TEST_CASE("Connection cache bucket count and inline policy fail closed") {
    Fixture fixture;
    const char* code = "cache_invalid";
    SUBCASE("negative") { fixture.memory.put(dictionary + 0x44, std::int32_t{-1}); }
    SUBCASE("not power of two") { fixture.memory.put(dictionary + 0x44, std::int32_t{3}); }
    SUBCASE("inline supports one bucket only") {
        fixture.memory.put(dictionary + 0x44, std::int32_t{2});
    }
    SUBCASE("heap above bucket cap") {
        fixture.heap();
        fixture.memory.put(dictionary + 0x44, std::int32_t{8192});
        code = "cache_limit";
    }
    expect_error([&] { fixture.sample(); }, code);
}

TEST_CASE("Connection cache indexes and matched null pointer are not absence") {
    Fixture fixture;
    const char* code = "cache_invalid";
    SUBCASE("bucket below minus one") {
        fixture.memory.put(dictionary + 0x38, std::int32_t{-2});
    }
    SUBCASE("chain below minus one") {
        node(fixture.memory, 0, 0x1000010001000ULL, 0, -2);
    }
    SUBCASE("index above cap") {
        fixture.memory.put(dictionary + 0x38, std::int32_t{4096});
        code = "cache_limit";
    }
    SUBCASE("matched cache null") { fixture.memory.put(nodes + 8, std::uint64_t{0}); }
    SUBCASE("nonnull bucket index needs array") { fixture.memory.put(dictionary, std::uint64_t{0}); }
    expect_error([&] { fixture.sample(); }, code);
}

TEST_CASE("Connection cache collision traversal is bounded and detects cycles") {
    Fixture fixture;
    SUBCASE("cycle") {
        node(fixture.memory, 0, 0x1000010001000ULL, 0, 1);
        node(fixture.memory, 1, 0x2000010001000ULL, 0, 0);
        expect_error([&] { fixture.sample(); }, "cache_cycle");
    }
    SUBCASE("256 nodes can reach exact match") {
        for (std::int32_t i = 0; i < 255; ++i)
            node(fixture.memory, i, 0x30000000ULL + static_cast<std::uint64_t>(i), 0, i + 1);
        node(fixture.memory, 255, leaf_class, leaf_cache, -1);
        const auto result = fixture.sample();
        REQUIRE(result.present);
        CHECK(result.classes == expected_classes);
    }
    SUBCASE("257th node cannot be reached") {
        for (std::int32_t i = 0; i < 256; ++i)
            node(fixture.memory, i, 0x30000000ULL + static_cast<std::uint64_t>(i), 0, i + 1);
        node(fixture.memory, 256, leaf_class, leaf_cache, -1);
        expect_error([&] { fixture.sample(); }, "cache_limit");
    }
}

TEST_CASE("Connection cache pointer arithmetic is checked and reader errors propagate") {
    Fixture fixture;
    SUBCASE("map offset overflow") {
        expect_error([&] { read_connection_cache(fixture.memory,
            std::numeric_limits<std::uint64_t>::max() - 8, fixture.expected); }, "cache_invalid");
    }
    SUBCASE("bucket address overflow") {
        fixture.heap();
        fixture.memory.put(dictionary + 0x3c, std::numeric_limits<std::uint64_t>::max() - 8);
        expect_error([&] { fixture.sample(); }, "cache_invalid");
    }
    SUBCASE("node record overflow") {
        fixture.memory.put(dictionary, std::numeric_limits<std::uint64_t>::max() - 8);
        fixture.memory.put(dictionary + 0x38, std::int32_t{1});
        expect_error([&] { fixture.sample(); }, "cache_invalid");
    }
    SUBCASE("cache offset overflow") {
        fixture.memory.put(nodes + 8, std::numeric_limits<std::uint64_t>::max() - 8);
        expect_error([&] { fixture.sample(); }, "cache_invalid");
    }
    SUBCASE("missing readable scalar") {
        fixture.memory.bytes.erase(root_cache + 0x34);
        expect_error([&] { fixture.sample(); }, "reader_error");
    }
}

TEST_CASE("Connection cache expected class and field inputs are bounded and unique") {
    Fixture fixture;
    const char* code = "cache_invalid";
    SUBCASE("no classes") { fixture.expected.clear(); }
    SUBCASE("eleven classes") {
        fixture.expected.resize(11, {0x3000, {}});
        for (std::size_t i = 0; i < fixture.expected.size(); ++i)
            fixture.expected[i].address = 0x3000 + i * 0x100;
        code = "cache_limit";
    }
    SUBCASE("null class") { fixture.expected[0].address = 0; }
    SUBCASE("duplicate class") { fixture.expected[1].address = leaf_class; }
    SUBCASE("null expected field") { fixture.expected[0].fields.push_back(0); }
    SUBCASE("duplicate expected field") { fixture.expected[0].fields.push_back(leaf_field); }
    SUBCASE("too many eligible fields") {
        fixture.expected[0].fields.clear();
        for (std::uint64_t i = 0; i < 257; ++i)
            fixture.expected[0].fields.push_back(0x30000000 + i * 0x10);
        code = "cache_limit";
    }
    expect_error([&] { fixture.sample(); }, code);
}

TEST_CASE("Connection cache class identities and exact parent chain are required") {
    Fixture fixture;
    const char* code = "cache_class_mismatch";
    SUBCASE("leaf identity mismatch") { fixture.memory.put(leaf_cache + 0x20, root_class); }
    SUBCASE("parent identity mismatch") { fixture.memory.put(root_cache + 0x20, leaf_class); }
    SUBCASE("parent missing") { fixture.memory.put(leaf_cache + 0x14, std::uint64_t{0}); }
    SUBCASE("extra parent") { fixture.memory.put(root_cache + 0x14, std::uint64_t{0x7200}); }
    SUBCASE("parent cycle") {
        fixture.memory.put(leaf_cache + 0x14, leaf_cache);
        code = "cache_cycle";
    }
    expect_error([&] { fixture.sample(); }, code);
}

TEST_CASE("Connection cache field storage count capacity and pointer are validated") {
    Fixture fixture;
    const char* code = "cache_invalid";
    SUBCASE("negative count") { fixture.memory.put(leaf_cache + 0x30, std::int32_t{-1}); }
    SUBCASE("negative capacity") { fixture.memory.put(leaf_cache + 0x34, std::int32_t{-1}); }
    SUBCASE("negative base") { fixture.memory.put(leaf_cache + 0x10, std::int32_t{-1}); }
    SUBCASE("base above aggregate bound") {
        fixture.memory.put(leaf_cache + 0x10, std::int32_t{2049});
        code = "cache_limit";
    }
    SUBCASE("count exceeds capacity") { fixture.memory.put(leaf_cache + 0x34, std::int32_t{0}); }
    SUBCASE("capacity exceeds limit") {
        fixture.memory.put(leaf_cache + 0x34, std::int32_t{257});
        code = "cache_limit";
    }
    SUBCASE("own count exceeds limit") {
        fixture.memory.put(leaf_cache + 0x30, std::int32_t{257});
        fixture.memory.put(leaf_cache + 0x34, std::int32_t{257});
        code = "cache_limit";
    }
    SUBCASE("nonnull count requires array") { fixture.memory.put(leaf_cache + 0x28, std::uint64_t{0}); }
    SUBCASE("field record overflow") {
        fixture.memory.put(leaf_cache + 0x28, std::numeric_limits<std::uint64_t>::max() - 8);
    }
    expect_error([&] { fixture.sample(); }, code);
}

TEST_CASE("Connection cache rejects unknown duplicated or noncontiguous fields") {
    Fixture fixture;
    SUBCASE("field outside expected class") { fixture.memory.put(leaf_fields, root_field0); }
    SUBCASE("null field") { fixture.memory.put(leaf_fields, std::uint64_t{0}); }
    SUBCASE("duplicate field") { fixture.memory.put(root_fields + 0x10, root_field0); }
    SUBCASE("leaf ID gap") { fixture.memory.put(leaf_fields + 8, std::int32_t{3}); }
    SUBCASE("root ID not zero") { fixture.memory.put(root_fields + 8, std::int32_t{1}); }
    SUBCASE("negative dense ID") { fixture.memory.put(leaf_fields + 8, std::int32_t{-1}); }
    SUBCASE("root base not zero") { fixture.memory.put(root_cache + 0x10, std::int32_t{1}); }
    SUBCASE("child base not parent extent") { fixture.memory.put(leaf_cache + 0x10, std::int32_t{3}); }
    expect_error([&] { fixture.sample(); }, "cache_field_mismatch");
}

TEST_CASE("Connection cache zero own fields need no field storage reads") {
    Fixture fixture;
    cache(fixture.memory, root_cache, root_class, 0, 0, 0, 0, 0);
    fixture.memory.put(leaf_cache + 0x10, std::int32_t{0});
    fixture.memory.put(leaf_fields + 8, std::int32_t{0});
    const auto result = fixture.sample();
    REQUIRE(result.present);
    const std::vector<CacheClass> expected{
        {0, 2, {{leaf_field, 0, -1}}}, {0, 0, {}}};
    CHECK(result.classes == expected);
}

TEST_CASE("Connection cache snapshots retain pointer trace for two sample comparison") {
    Fixture fixture;
    const auto first = fixture.sample();
    const auto second = fixture.sample();
    REQUIRE(first.present);
    CHECK(first == second);
    REQUIRE_FALSE(first.trace.empty());
    // Equivalent metadata at a new cache pointer must not look like the same sample.
    constexpr std::uint64_t moved_cache = 0x7300;
    cache(fixture.memory, moved_cache, leaf_class, root_cache, 2, leaf_fields, 1, 2);
    fixture.memory.put(nodes + 8, moved_cache);
    const auto changed = fixture.sample();
    REQUIRE(changed.present);
    CHECK(changed.classes == first.classes);
    CHECK(changed.trace != first.trace);
}

TEST_CASE("Connection cache allows eight classes and 2048 contiguous fields at policy boundary") {
    Fixture fixture;
    fixture.expected.clear();
    for (std::uint64_t depth = 0; depth < 8; ++depth) {
        const auto type = leaf_class + depth * 0x100;
        const auto address = leaf_cache + depth * 0x100;
        const auto parent = depth == 7 ? 0 : address + 0x100;
        const auto array = 0x100000 + depth * 0x2000;
        const auto base = static_cast<std::int32_t>((7 - depth) * 256);
        ExpectedClass expected{type, {}};
        for (std::int32_t j = 0; j < 256; ++j) {
            const auto pointer = 0x200000 + depth * 0x2000 + static_cast<std::uint64_t>(j) * 0x10;
            expected.fields.push_back(pointer);
            field(fixture.memory, array, j, pointer, base + j, -1);
        }
        fixture.expected.push_back(std::move(expected));
        cache(fixture.memory, address, type, parent, base, array, 256, 256);
    }
    const auto result = fixture.sample();
    REQUIRE(result.present);
    REQUIRE(result.classes.size() == 8);
    std::size_t total = 0;
    for (std::size_t i = 0; i < result.classes.size(); ++i) {
        const auto& value = result.classes[i];
        CHECK(value.base == static_cast<std::int32_t>((7 - i) * 256));
        CHECK(value.capacity == 256);
        REQUIRE(value.fields.size() == 256);
        CHECK(value.fields.front().id == value.base);
        CHECK(value.fields.back().id == value.base + 255);
        total += value.fields.size();
    }
    CHECK(total == 2048);
    // This eight-class fixture reaches the independent aggregate 2048 boundary.
}

TEST_CASE("Connection cache retains complete ten-class Pawn and seven-class PRI chains") {
    Fixture fixture;
    std::size_t count = 10;
    SUBCASE("complete Pawn ancestry") {}
    SUBCASE("complete PRI ancestry") { count = 7; }
    fixture.expected.clear();
    for (std::size_t depth = 0; depth < count; ++depth) {
        const auto offset = static_cast<std::uint64_t>(depth) * 0x100;
        const auto type = leaf_class + offset;
        const auto address = leaf_cache + offset;
        const auto parent = depth + 1 == count ? 0 : address + 0x100;
        const auto array = 0x100000ULL + offset;
        const auto pointer = 0x400000ULL + offset;
        const auto id = static_cast<std::int32_t>(count - 1 - depth);
        fixture.expected.push_back({type, {pointer}});
        cache(fixture.memory, address, type, parent, id, array, 1, 1);
        field(fixture.memory, array, 0, pointer, id, -1);
    }
    const auto result = fixture.sample();
    REQUIRE(result.present);
    REQUIRE(result.classes.size() == count);
    REQUIRE(result.classes.front().fields.size() == 1);
    REQUIRE(result.classes.back().fields.size() == 1);
    if (count == 10) {
        CHECK(result.classes.front().base == 9);
        CHECK(result.classes.front().fields.front().id == 9);
    } else {
        CHECK(result.classes.front().base == 6);
        CHECK(result.classes.front().fields.front().id == 6);
    }
    CHECK(result.classes.back().base == 0);
    CHECK(result.classes.back().fields.front().id == 0);
}

TEST_CASE("Connection cache rejects a truncated Pawn ancestry with a remaining parent") {
    Fixture fixture;
    fixture.expected.clear();
    for (std::uint64_t depth = 0; depth < 10; ++depth) {
        const auto type = leaf_class + depth * 0x100;
        const auto address = leaf_cache + depth * 0x100;
        const auto parent = depth == 9 ? 0 : address + 0x100;
        fixture.expected.push_back({type, {}});
        cache(fixture.memory, address, type, parent, 0, 0, 0, 0);
    }
    // Removing the last two expectations must not hide the actual parent chain.
    fixture.expected.resize(8);
    expect_error([&] { fixture.sample(); }, "cache_class_mismatch");
}

TEST_CASE("Connection cache rejects 2050 fields spread over ten individually bounded classes") {
    Fixture fixture;
    fixture.expected.clear();
    for (std::uint64_t depth = 0; depth < 10; ++depth) {
        const auto type = leaf_class + depth * 0x100;
        const auto address = leaf_cache + depth * 0x100;
        const auto parent = depth == 9 ? 0 : address + 0x100;
        const auto array = 0x100000 + depth * 0x2000;
        ExpectedClass expected{type, {}};
        for (std::int32_t j = 0; j < 205; ++j) {
            const auto pointer = 0x200000 + depth * 0x2000 + static_cast<std::uint64_t>(j) * 0x10;
            expected.fields.push_back(pointer);
            field(fixture.memory, array, j, pointer, j, -1);
        }
        fixture.expected.push_back(std::move(expected));
        // Base zero isolates the aggregate check: every individual base+count is 205.
        // Aggregate rejection precedes the later inherited-base consistency check.
        cache(fixture.memory, address, type, parent, 0, array, 205, 205);
    }
    expect_error([&] { fixture.sample(); }, "cache_limit");
}

TEST_CASE("Player FName lookup reads only target buckets and slots within a large pool") {
    FNameBudgetMemory memory;
    constexpr std::uint64_t module=0x50000000,pool=0x60000000;
    constexpr std::array<std::uint64_t,2> entries={0x70000000,0x70000100};
    memory.put(module+0x1cee4b8,pool);
    memory.put(module+0x1cee4c0,std::int32_t{200000});
    // Only the two target index slots are readable. A pool scan must fail.
    memory.put(pool+3*8,entries[0]);
    memory.put(pool+7*8,entries[1]);
    std::array<std::uint32_t,256> table{};
    for (std::uint32_t i=0;i<256;++i) table[i]=0x10000U|i;
    memory.put(module+0x1bee030,table);
    // Independently calculated low16 buckets for this synthetic table:
    // ANSI HGC=0158/Pawn=014d; UTF16 HGC=0159/Pawn=014c.
    memory.put(module+0x1bee430+8*0x158,entries[0]);
    memory.put(module+0x1bee430+8*0x14d,entries[1]);
    memory.put(module+0x1bee430+8*0x159,std::uint64_t{0});
    memory.put(module+0x1bee430+8*0x14c,std::uint64_t{0});
    constexpr std::array<std::string_view,2> targets={"HawkenGameContent","R_PawnContent"};
    for (std::size_t i=0;i<2;++i) {
        memory.put(entries[i]+8,static_cast<std::int32_t>((i==0 ? 3 : 7)*2));
        memory.put(entries[i]+0xc,std::uint64_t{0});
        for (std::size_t j=0;j<=targets[i].size();++j)
            memory.put(entries[i]+0x14+j,j==targets[i].size() ? std::uint8_t{0} :
                static_cast<std::uint8_t>(targets[i][j]));
    }
    const auto result=read_player_fname_targets(memory,module);
    CHECK((result.indices==std::array<std::uint32_t,2>{3,7}));
    CHECK(result.name_count==200000);
    CHECK(memory.used<1024*1024);
    for (const auto& [address,width]:memory.reads) {
        if (address>=pool && address<pool+200000*8) {
            CHECK((address==pool+3*8 || address==pool+7*8));
            CHECK(width==8);
        }
    }
}

namespace {
struct FNameFixture {
    FNameBudgetMemory memory;
    static constexpr std::uint64_t module=0x50000000,pool=0x60000000;
    static constexpr std::array<std::uint64_t,2> entries={0x70000000,0x70000100};
    static constexpr std::array<std::string_view,2> targets={"HawkenGameContent","R_PawnContent"};
    std::array<std::uint32_t,256> table{};
    explicit FNameFixture(bool first_wide=false,bool second_wide=false) {
        memory.put(module+0x1cee4b8,pool);
        memory.put(module+0x1cee4c0,std::int32_t{200000});
        for (std::uint32_t i=0;i<256;++i) table[i]=0x10000U|i;
        memory.put(module+0x1bee030,table);
        for (const auto bucket:{0x158,0x159,0x14d,0x14c})
            memory.put(module+0x1bee430+8*static_cast<std::uint64_t>(bucket),std::uint64_t{0});
        memory.put(module+0x1bee430+8*(first_wide ? 0x159 : 0x158),entries[0]);
        memory.put(module+0x1bee430+8*(second_wide ? 0x14c : 0x14d),entries[1]);
        entry(entries[0],3,targets[0],first_wide);
        entry(entries[1],7,targets[1],second_wide);
    }
    void entry(std::uint64_t address,std::uint32_t index,std::string_view text,bool wide=false,std::uint64_t next=0) {
        memory.put(address+8,(index<<1)|(wide ? 1U : 0U));
        memory.put(address+0xc,next);
        memory.put(pool+static_cast<std::uint64_t>(index)*8,address);
        for (std::size_t j=0;j<=text.size();++j) {
            const auto unit=j==text.size() ? std::uint16_t{0} : static_cast<std::uint16_t>(text[j]);
            if (wide) memory.put(address+0x14+j*2,unit);
            else memory.put(address+0x14+j,static_cast<std::uint8_t>(unit));
        }
    }
    FNameLookupSnapshot sample() { return read_player_fname_targets(memory,module); }
};
}

TEST_CASE("Player FName ASCII hash uses runtime table zero seed and encoding-specific updates") {
    FNameFixture fixture;
    CHECK(fname_detail::ascii_hash(fixture.table,"",false)==0);
    CHECK(fname_detail::ascii_hash(fixture.table,"A",false)==0x10041U);
    CHECK(fname_detail::ascii_hash(fixture.table,"a",false)==0x10041U);
    CHECK(fname_detail::ascii_hash(fixture.table,"A",true)==0x10141U);
    CHECK(fname_detail::ascii_hash(fixture.table,"AB",false)==0x10103U);
    CHECK(fname_detail::ascii_hash(fixture.table,"a_b",false)==0x1015dU);
    CHECK(fname_detail::ascii_hash(fixture.table,"a_b",true)==0x1015cU);
    CHECK(fname_detail::ascii_hash(fixture.table,"HawkenGameContent",false)==0x10158U);
    CHECK(fname_detail::ascii_hash(fixture.table,"R_PawnContent",true)==0x1014cU);
}

TEST_CASE("Player FName lookup supports exact ANSI and wide targets without pool traversal") {
    bool first=true,second=true;
    bool shared_bucket=false;
    SUBCASE("both wide") {}
    SUBCASE("mixed encodings") { first=false; }
    SUBCASE("one collision bucket for both targets and encodings is visited once") {
        first=false; second=false; shared_bucket=true;
    }
    FNameFixture fixture(first,second);
    if (shared_bucket) {
        fixture.table.fill(1);
        fixture.memory.put(FNameFixture::module+0x1bee030,fixture.table);
        fixture.memory.put(FNameFixture::module+0x1bee430+8,FNameFixture::entries[0]);
        fixture.memory.put(FNameFixture::entries[0]+0xc,FNameFixture::entries[1]);
    }
    const auto sample=fixture.sample();
    CHECK((sample.indices==std::array<std::uint32_t,2>{3,7}));
    CHECK(sample.scanned_entries==2);
    CHECK(sample.head_count==(shared_bucket ? 1 : 4));
    for (const auto& [address,width]:fixture.memory.reads)
        if (address>=FNameFixture::pool && address<FNameFixture::pool+200000*8) {
            CHECK((address==FNameFixture::pool+3*8 || address==FNameFixture::pool+7*8));
            CHECK(width==8);
        }
}

TEST_CASE("Player FName lookup rejects uninitialized table absent targets and changed spelling") {
    FNameFixture fixture;
    SUBCASE("zero initialized PE table is not authoritative") {
        fixture.memory.put(FNameFixture::module+0x1bee030,std::array<std::uint32_t,256>{});
        expect_error([&]{fixture.sample();},"player_names_uninitialized");
    }
    SUBCASE("all selected buckets empty") {
        fixture.memory.put(FNameFixture::module+0x1bee430+8*0x158,std::uint64_t{0});
        fixture.memory.put(FNameFixture::module+0x1bee430+8*0x14d,std::uint64_t{0});
        expect_error([&]{fixture.sample();},"player_names_absent");
    }
    SUBCASE("casefolded hash does not authorize different spelling") {
        fixture.entry(FNameFixture::entries[0],3,"hawkenGameContent");
        expect_error([&]{fixture.sample();},"player_names_absent");
    }
    SUBCASE("target prefix is not a target without exact terminator") {
        fixture.entry(FNameFixture::entries[0],3,"HawkenGameContentX");
        expect_error([&]{fixture.sample();},"player_names_absent");
    }
}

TEST_CASE("Player FName lookup rejects malformed packed indices and wrong GNames crosslinks") {
    FNameFixture fixture;
    SUBCASE("signed negative packed index") { fixture.memory.put(FNameFixture::entries[0]+8,std::uint32_t{0xfffffffe}); }
    SUBCASE("index equals count") { fixture.memory.put(FNameFixture::entries[0]+8,std::uint32_t{400000}); }
    SUBCASE("wrong directed slot") { fixture.memory.put(FNameFixture::pool+3*8,FNameFixture::entries[1]); }
    expect_error([&]{fixture.sample();},"player_name_invalid");
}

TEST_CASE("Player FName lookup rejects cycles and ambiguous exact names across encodings") {
    FNameFixture fixture;
    SUBCASE("cycle after otherwise valid target") {
        fixture.memory.put(FNameFixture::entries[0]+0xc,FNameFixture::entries[0]);
        expect_error([&]{fixture.sample();},"player_name_cycle");
    }
    SUBCASE("another wide target is ambiguity") {
        constexpr std::uint64_t duplicate=0x70000200;
        fixture.entry(duplicate,9,FNameFixture::targets[0],true);
        fixture.memory.put(FNameFixture::module+0x1bee430+8*0x159,duplicate);
        expect_error([&]{fixture.sample();},"player_names_ambiguous");
    }
}

TEST_CASE("Player FName lookup caps each selected collision chain at 256 nodes") {
    FNameFixture fixture;
    std::size_t unrelated=255;
    SUBCASE("256 total including target is valid") {}
    SUBCASE("257 total including target rejects") { unrelated=256; }
    for (std::size_t i=0;i<unrelated;++i) {
        const auto node=0x71000000ULL+static_cast<std::uint64_t>(i)*0x100;
        const auto next=i+1==unrelated ? FNameFixture::entries[0] : node+0x100;
        fixture.entry(node,100+static_cast<std::uint32_t>(i),"X",false,next);
    }
    fixture.memory.put(FNameFixture::module+0x1bee430+8*0x158,std::uint64_t{0x71000000});
    if (unrelated==256) expect_error([&]{fixture.sample();},"player_name_limit");
    else CHECK((fixture.sample().indices==std::array<std::uint32_t,2>{3,7}));
}

TEST_CASE("Player FName complete traces detect table and chain changes with unchanged indices") {
    FNameFixture fixture;
    const auto first=fixture.sample();
    SUBCASE("unchanged full second lookup") {
        const auto second=fixture.sample();
        CHECK(second.trace==first.trace);
        CHECK(second.indices==first.indices);
        CHECK(std::count_if(fixture.memory.reads.begin(),fixture.memory.reads.end(),[](const auto& read){return read.second==1024;})==2);
    }
    SUBCASE("unused hash-table word still compared") {
        fixture.table[250]^=1;
        fixture.memory.put(FNameFixture::module+0x1bee030,fixture.table);
        const auto second=fixture.sample();
        CHECK(second.indices==first.indices);
        CHECK(second.trace!=first.trace);
    }
    SUBCASE("different collision chain still compared") {
        constexpr std::uint64_t node=0x70000200;
        fixture.entry(node,9,"X",false,FNameFixture::entries[0]);
        fixture.memory.put(FNameFixture::module+0x1bee430+8*0x158,node);
        const auto second=fixture.sample();
        CHECK(second.indices==first.indices);
        CHECK(second.trace!=first.trace);
    }
}

TEST_CASE("Player FName lookup obeys shared attempted-read budget and pointer overflow guard") {
    FNameFixture fixture;
    SUBCASE("remaining budget does not reset for second snapshot") {
        fixture.sample();
        fixture.memory.used=1024*1024-1024;
        expect_error([&]{fixture.sample();},"read_limit");
    }
    SUBCASE("module address overflow rejects before access") {
        expect_error([&]{read_player_fname_targets(fixture.memory,std::numeric_limits<std::uint64_t>::max());},"player_name_invalid");
        CHECK(fixture.memory.reads.empty());
    }
}
