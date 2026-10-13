#include "live_player_reader.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <type_traits>

namespace {
using hawken::probe::LiveIdentity;
using hawken::probe::LivePlayerSnapshot;
using hawken::probe::read_live_player;

// Directed synthetic memory, not a fabricated original process. Missing bytes
// fail instead of supplying zero-filled memory; no instance FNames/NetIndices
// are installed. Tests exercise the reader, not a process or callback mock.
struct Memory {
    std::map<std::uint64_t, unsigned char> bytes;
    std::size_t budget = 1024 * 1024;
    template<class T> void put(std::uint64_t where, T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[where + i] = data[i];
    }
    template<class T> T read(std::uint64_t where, std::uint64_t offset = 0) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (offset > maximum - where || sizeof(T) - 1 > maximum - (where + offset))
            throw std::runtime_error("reader_overflow");
        if (sizeof(T) > budget) throw std::runtime_error("read_limit");
        budget -= sizeof(T);
        std::array<unsigned char, sizeof(T)> data{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const auto found = bytes.find(where + offset + i);
            if (found == bytes.end()) throw std::runtime_error("reader_error");
            data[i] = found->second;
        }
        T value{};
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }
};

constexpr std::uint64_t module_base = 0x140000000;
constexpr std::uint64_t kind_class = 0x10000, object_class = 0x10400;
constexpr std::uint64_t engine_class = 0x10800, subsystem_class = 0x10c00;
constexpr std::uint64_t player_class = 0x11000, local_class = 0x11400;
constexpr std::uint64_t controller_class = 0x11800, pc_class = 0x11c00;
constexpr std::uint64_t hawken_pc_class = 0x12000, actor_class = 0x12400;
constexpr std::uint64_t pawn_class = 0x12800, pri_class = 0x12c00;
constexpr std::uint64_t hawken_pri_class = 0x13000, property_class = 0x13400;
constexpr std::uint64_t engine_object = 0x20000, local_object = 0x24000;
constexpr std::uint64_t controller_object = 0x28000, pawn_object = 0x2c000, pri_object = 0x30000;
constexpr std::uint64_t player_array = 0x40000;
constexpr std::uint64_t actor_property = 0x50000, player_property = 0x50400;
constexpr std::uint64_t pawn_property = 0x50800, pri_property = 0x50c00;

struct Fixture {
    Memory memory;
    std::map<std::uint64_t, LiveIdentity> identities;
    Fixture() {
        memory.put(module_base + 0x1cfb860, engine_object);
        memory.put(module_base + 0x1bda990, kind_class);
        memory.put(module_base + 0x1d3fa68, hawken_pc_class);
        klass(kind_class, "Class", "Core", object_class);
        klass(object_class, "Object", "Core", 0);
        klass(subsystem_class, "Subsystem", "Core", object_class);
        klass(engine_class, "Engine", "Engine", subsystem_class);
        klass(player_class, "Player", "Engine", object_class);
        klass(local_class, "LocalPlayer", "Engine", player_class);
        klass(actor_class, "Actor", "Engine", object_class);
        klass(controller_class, "Controller", "Engine", actor_class);
        klass(pc_class, "PlayerController", "Engine", controller_class);
        klass(hawken_pc_class, "R_PlayerController", "HawkenGame", pc_class);
        klass(pawn_class, "Pawn", "Engine", actor_class);
        klass(pri_class, "PlayerReplicationInfo", "Engine", actor_class);
        klass(hawken_pri_class, "R_PlayerReplicationInfo", "HawkenGame", pri_class);
        klass(property_class, "ObjectProperty", "Core", object_class);
        memory.put(property_class + 0x128, std::uint32_t{0x18000});
        object(engine_object, engine_class, 0);
        object(local_object, local_class, engine_object);
        object(controller_object, hawken_pc_class, 0);
        object(pawn_object, pawn_class, 0);
        object(pri_object, hawken_pri_class, 0);
        memory.put(engine_object + 0x700, player_array);
        memory.put(engine_object + 0x708, std::int32_t{1});
        memory.put(engine_object + 0x70c, std::int32_t{1});
        memory.put(player_array, local_object);
        property(actor_property, player_class, "Actor", 0x68, pc_class);
        property(player_property, pc_class, "Player", 0x300, player_class);
        property(pawn_property, controller_class, "Pawn", 0x280, pawn_class);
        property(pri_property, controller_class, "PlayerReplicationInfo", 0x2a0, pri_class);
        memory.put(player_class + 0x80, actor_property);
        memory.put(pc_class + 0x80, player_property);
        memory.put(controller_class + 0x80, pawn_property);
        memory.put(pawn_property + 0x60, pri_property);
        memory.put(local_object + 0x68, controller_object);
        memory.put(controller_object + 0x300, local_object);
        memory.put(controller_object + 0x280, pawn_object);
        memory.put(controller_object + 0x2a0, pri_object);
    }
    void klass(std::uint64_t where, const char* name, const char* package, std::uint64_t parent) {
        identities[where] = {name, package, 0};
        memory.put(where + 0x50, kind_class);
        memory.put(where + 0x78, parent);
        memory.put(where + 0x80, std::uint64_t{0});
        memory.put(where + 0x1d4, where + 0x180);
    }
    void object(std::uint64_t where, std::uint64_t type, std::uint64_t outer) {
        memory.put(where + 0x10, std::uint64_t{0});
        memory.put(where + 0x40, outer);
        memory.put(where + 0x50, type);
    }
    void property(std::uint64_t where, std::uint64_t owner, const char* name,
                  std::int32_t offset, std::uint64_t expected) {
        identities[where] = {name, "Engine", 0};
        memory.put(where + 0x40, owner);
        memory.put(where + 0x50, property_class);
        memory.put(where + 0x60, std::uint64_t{0});
        memory.put(where + 0x68, std::int32_t{1});
        memory.put(where + 0x6c, std::int32_t{8});
        memory.put(where + 0x8c, offset);
        memory.put(where + 0xa8, expected);
    }
    LivePlayerSnapshot sample() {
        auto identify = [&](std::uint64_t where) {
            const auto found = identities.find(where);
            if (found == identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return read_live_player(memory, module_base, identify);
    }
};
}

TEST_CASE("Live player resolves typed Controller Pawn and PRI without field IDs") {
    Fixture f;
    const auto result = f.sample();
    CHECK(result.local_player_count == 1);
    CHECK(result.local_player_present);
    CHECK(result.controller_present);
    CHECK(result.pawn_present);
    CHECK(result.pri_present);
    CHECK(result.controller_player_matches);
    REQUIRE(result.pawn_offset.has_value());
    REQUIRE(result.pri_offset.has_value());
    CHECK(*result.pawn_offset == 0x280);
    CHECK(*result.pri_offset == 0x2a0);
    CHECK_FALSE(result.trace.empty());
}

TEST_CASE("Live player distinguishes no entry null Actor and null Pawn PRI") {
    Fixture f;
    SUBCASE("no local entries does not read an array") {
        f.memory.put(engine_object + 0x708, std::int32_t{0});
        f.memory.put(engine_object + 0x70c, std::int32_t{0});
        f.memory.put(engine_object + 0x700, std::uint64_t{0});
        const auto result = f.sample();
        CHECK(result.local_player_count == 0);
        CHECK_FALSE(result.local_player_present);
        CHECK_FALSE(result.controller_present);
    }
    SUBCASE("typed LocalPlayer has no Controller") {
        f.memory.put(local_object + 0x68, std::uint64_t{0});
        f.memory.bytes.erase(controller_object + 0x50);
        const auto result = f.sample();
        CHECK(result.local_player_present);
        CHECK_FALSE(result.controller_present);
        CHECK_FALSE(result.pawn_offset.has_value());
        CHECK_FALSE(result.pri_offset.has_value());
    }
    SUBCASE("verified Controller has null links") {
        f.memory.put(controller_object + 0x280, std::uint64_t{0});
        f.memory.put(controller_object + 0x2a0, std::uint64_t{0});
        const auto result = f.sample();
        CHECK(result.controller_present);
        CHECK(result.controller_player_matches);
        CHECK_FALSE(result.pawn_present);
        CHECK_FALSE(result.pri_present);
        CHECK(result.pawn_offset.has_value());
        CHECK(result.pri_offset.has_value());
    }
}

TEST_CASE("Live player uses reflected Pawn PRI offsets rather than cached indices") {
    Fixture f;
    f.memory.put(pawn_property + 0x8c, std::int32_t{0x2b0});
    f.memory.put(pri_property + 0x8c, std::int32_t{0x2d0});
    f.memory.put(controller_object + 0x280, std::uint64_t{0});
    f.memory.put(controller_object + 0x2a0, std::uint64_t{0});
    f.memory.put(controller_object + 0x2b0, pawn_object);
    f.memory.put(controller_object + 0x2d0, pri_object);
    const auto result = f.sample();
    CHECK(result.pawn_present);
    CHECK(result.pri_present);
    REQUIRE(result.pawn_offset.has_value());
    REQUIRE(result.pri_offset.has_value());
    CHECK(*result.pawn_offset == 0x2b0);
    CHECK(*result.pri_offset == 0x2d0);
}

TEST_CASE("Live player rejects a LocalPlayer with wrong ownership or template identity") {
    Fixture f;
    SUBCASE("foreign outer") { f.memory.put(local_object + 0x40, pawn_object); }
    SUBCASE("CDO flag") { f.memory.put(local_object + 0x10, std::uint64_t{0x200}); }
    SUBCASE("archetype flag") { f.memory.put(local_object + 0x10, std::uint64_t{0x400}); }
    SUBCASE("CDO equality even if flags clear") { f.memory.put(local_class + 0x1d4, local_object); }
    SUBCASE("wrong class") { f.memory.put(local_object + 0x50, pawn_class); }
    SUBCASE("metadata instance nonzero") { f.identities[local_class].instance = 1; }
    CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
}

TEST_CASE("Live player validates GamePlayers count capacity and bounded storage") {
    Fixture f;
    SUBCASE("negative count") { f.memory.put(engine_object + 0x708, std::int32_t{-1}); }
    SUBCASE("capacity smaller than count") { f.memory.put(engine_object + 0x70c, std::int32_t{0}); }
    SUBCASE("negative capacity") { f.memory.put(engine_object + 0x70c, std::int32_t{-1}); }
    SUBCASE("null nonempty array") { f.memory.put(engine_object + 0x700, std::uint64_t{0}); }
    CHECK_THROWS_WITH_AS(f.sample(), "live_array_invalid", std::runtime_error);
}

TEST_CASE("Live player bounds local array independently of readable entries") {
    Fixture f;
    SUBCASE("capacity exceeds policy") { f.memory.put(engine_object + 0x70c, std::int32_t{5}); }
    SUBCASE("count exceeds policy") {
        f.memory.put(engine_object + 0x708, std::int32_t{5});
        f.memory.put(engine_object + 0x70c, std::int32_t{5});
    }
    CHECK_THROWS_WITH_AS(f.sample(), "live_limit", std::runtime_error);
}

TEST_CASE("Live player requires exact Actor ObjectProperty layout and expected type") {
    Fixture f;
    SUBCASE("different Actor offset") { f.memory.put(actor_property + 0x8c, std::int32_t{0x70}); }
    SUBCASE("wrong property owner") { f.memory.put(actor_property + 0x40, local_class); }
    SUBCASE("wrong property type") { f.memory.put(actor_property + 0x50, pawn_class); }
    SUBCASE("ArrayDim two") { f.memory.put(actor_property + 0x68, std::int32_t{2}); }
    SUBCASE("ElementSize four") { f.memory.put(actor_property + 0x6c, std::int32_t{4}); }
    SUBCASE("expected Object instead of PlayerController") { f.memory.put(actor_property + 0xa8, object_class); }
    SUBCASE("field name instance nonzero") { f.identities[actor_property].instance = 1; }
    CHECK_THROWS_WITH_AS(f.sample(), "live_property_mismatch", std::runtime_error);
}

TEST_CASE("Live player rejects missing or duplicate Actor metadata") {
    Fixture f;
    SUBCASE("absent own field") {
        f.memory.put(player_class + 0x80, std::uint64_t{0});
        CHECK_THROWS_WITH_AS(f.sample(), "live_property_missing", std::runtime_error);
    }
    SUBCASE("duplicate exact owner field") {
        constexpr std::uint64_t duplicate = 0x51000;
        f.property(duplicate, player_class, "Actor", 0x68, pc_class);
        f.memory.put(actor_property + 0x60, duplicate);
        CHECK_THROWS_WITH_AS(f.sample(), "live_property_mismatch", std::runtime_error);
    }
}

TEST_CASE("Live player requires typed Controller and reciprocal Player backlink") {
    Fixture f;
    SUBCASE("wrong Controller type") {
        f.memory.put(controller_object + 0x50, pawn_class);
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("wrong Controller global identity") {
        f.identities[hawken_pc_class].name = "OtherController";
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("template Controller") {
        f.memory.put(controller_object + 0x10, std::uint64_t{0x400});
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("null backlink") {
        f.memory.put(controller_object + 0x300, std::uint64_t{0});
        CHECK_THROWS_WITH_AS(f.sample(), "live_backlink_mismatch", std::runtime_error);
    }
    SUBCASE("different player backlink") {
        f.memory.put(controller_object + 0x300, engine_object);
        CHECK_THROWS_WITH_AS(f.sample(), "live_backlink_mismatch", std::runtime_error);
    }
}

TEST_CASE("Live player validates referenced Pawn PRI classes and metadata offsets") {
    Fixture f;
    SUBCASE("Pawn has PRI type") {
        f.memory.put(pawn_object + 0x50, hawken_pri_class);
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("PRI has Pawn type") {
        f.memory.put(pri_object + 0x50, pawn_class);
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("Pawn is CDO") {
        f.memory.put(pawn_class + 0x1d4, pawn_object);
        CHECK_THROWS_WITH_AS(f.sample(), "live_identity", std::runtime_error);
    }
    SUBCASE("wrong declared Pawn expected class") {
        f.memory.put(pawn_property + 0xa8, pri_class);
        CHECK_THROWS_WITH_AS(f.sample(), "live_property_mismatch", std::runtime_error);
    }
    SUBCASE("negative PRI offset") {
        f.memory.put(pri_property + 0x8c, std::int32_t{-1});
        CHECK_THROWS_WITH_AS(f.sample(), "live_property_mismatch", std::runtime_error);
    }
    SUBCASE("over-budget Pawn offset") {
        f.memory.put(pawn_property + 0x8c, std::int32_t{65537});
        CHECK_THROWS_WITH_AS(f.sample(), "live_limit", std::runtime_error);
    }
}

TEST_CASE("Live player bounds class and child traversals with cycle detection") {
    Fixture f;
    SUBCASE("class parent cycle") {
        f.memory.put(local_class + 0x78, local_class);
        CHECK_THROWS_WITH_AS(f.sample(), "live_cycle", std::runtime_error);
    }
    SUBCASE("property Next cycle") {
        f.memory.put(actor_property + 0x60, actor_property);
        CHECK_THROWS_WITH_AS(f.sample(), "live_cycle", std::runtime_error);
    }
    SUBCASE("more than sixteen class nodes") {
        std::uint64_t parent = local_class;
        for (std::uint64_t i = 0; i < 16; ++i) {
            const auto where = 0x70000 + i * 0x400;
            f.klass(where, "SyntheticDerivedLocal", "Engine", parent);
            parent = where;
        }
        f.memory.put(local_object + 0x50, parent);
        CHECK_THROWS_WITH_AS(f.sample(), "live_limit", std::runtime_error);
    }
    SUBCASE("more than512 own children after found Actor") {
        auto previous = actor_property;
        for (std::uint64_t i = 0; i < 512; ++i) {
            const auto where = 0x90000 + i * 0x100;
            f.property(where, player_class, "SyntheticOtherField", 0, object_class);
            f.memory.put(previous + 0x60, where);
            previous = where;
        }
        CHECK_THROWS_WITH_AS(f.sample(), "live_limit", std::runtime_error);
    }
}

TEST_CASE("Live player preserves Reader failures and detects changed sampled pointers") {
    Fixture f;
    SUBCASE("unreadable Actor pointer") {
        f.memory.bytes.erase(local_object + 0x68);
        CHECK_THROWS_WITH_AS(f.sample(), "reader_error", std::runtime_error);
    }
    SUBCASE("shared reader budget exhaustion") {
        f.memory.budget = 1;
        CHECK_THROWS_WITH_AS(f.sample(), "read_limit", std::runtime_error);
    }
    SUBCASE("overflowing instance address") {
        f.memory.put(player_array, std::numeric_limits<std::uint64_t>::max() - 1);
        CHECK_THROWS_WITH_AS(f.sample(), "live_invalid", std::runtime_error);
    }
    SUBCASE("same metadata different actor pointer must change internal trace") {
        const auto first = f.sample();
        constexpr std::uint64_t other_pawn = 0x34000;
        f.object(other_pawn, pawn_class, 0);
        f.memory.put(controller_object + 0x280, other_pawn);
        const auto second = f.sample();
        CHECK(first.pawn_present);
        CHECK(second.pawn_present);
        CHECK(first.pawn_offset == second.pawn_offset);
        CHECK_FALSE(first.trace == second.trace);
        CHECK_FALSE(first == second);
    }
}
