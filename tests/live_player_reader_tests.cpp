#include "live_player_reader.hpp"
#include <doctest/doctest.h>
#include <array>
#include <bit>
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
    std::map<std::uint64_t,std::size_t> read_counts;
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
        ++read_counts[where+offset];
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

namespace {
// Synthetic reflected metadata and instance storage. These offsets are fixture
// coordinates, never guessed offsets in the original client.
constexpr std::uint64_t inventory_class = 0x13800, manager_class = 0x13c00;
constexpr std::uint64_t hg_manager_class = 0x14000, weapon_class = 0x14400;
constexpr std::uint64_t dual_class = 0x14800, native_pawn_class = 0x14c00;
constexpr std::uint64_t content_pawn_class = 0x15000, byte_property_class = 0x15400;
constexpr std::uint64_t manager_object = 0x34000, dual_object = 0x38000, hand_object = 0x3c000;
constexpr std::uint64_t invmanager_property = 0x51400, weapon_property = 0x51800;
constexpr std::uint64_t dual_property = 0x51c00, instigator_property = 0x52000;
constexpr std::uint64_t chain_property = 0x52400, item_manager_property = 0x52800;
constexpr std::uint64_t role_property = 0x52c00;
constexpr std::uint64_t physics_property = 0x54000, bool_property_class = 0x15800;
constexpr std::uint64_t can_fire_property = 0x54400, shutdown_property = 0x54800;
constexpr std::uint64_t client_shutdown_property = 0x54c00;
constexpr std::uint64_t float_property_class = 0x15c00, jump_capable_property = 0x56000;
constexpr std::uint64_t jump_z_property = 0x56400, thruster_energy_property = 0x56800;
constexpr std::uint64_t thruster_jump_property = 0x56c00, move_state_property = 0x57000;
constexpr std::uint64_t pawn_can_move_property = 0x57800;
constexpr std::uint64_t pawn_no_fire_property = 0x57c00;
constexpr std::uint64_t script_function_class = 0x16000, script_state_class = 0x16400;
constexpr std::uint64_t jump_dispatch_function = 0x58000, fire_dispatch_function = 0x58400;
struct InventoryFixture : Fixture {
    InventoryFixture() {
        klass(inventory_class,"Inventory","Engine",actor_class);
        klass(manager_class,"InventoryManager","Engine",actor_class);
        klass(hg_manager_class,"R_InventoryManager","HawkenGame",manager_class);
        klass(weapon_class,"Weapon","Engine",inventory_class);
        klass(dual_class,"R_Weap_DualWield","HawkenGame",weapon_class);
        klass(native_pawn_class,"R_PawnNative","HawkenGame",pawn_class);
        klass(content_pawn_class,"SyntheticPawnContent","HawkenGameContent",native_pawn_class);
        klass(byte_property_class,"ByteProperty","Core",object_class);
        memory.put(byte_property_class+0x128,std::uint32_t{0x8000});
        klass(bool_property_class,"BoolProperty","Core",object_class);
        memory.put(bool_property_class+0x128,std::uint32_t{0x20000});
        object(pawn_object,content_pawn_class,0);
        object(manager_object,hg_manager_class,0);
        object(dual_object,dual_class,0);
        object(hand_object,weapon_class,0);
        property(invmanager_property,pawn_class,"InvManager",0x300,manager_class);
        property(weapon_property,pawn_class,"Weapon",0x310,weapon_class);
        property(dual_property,native_pawn_class,"DualWeapon",0x320,dual_class);
        identities[dual_property].package="HawkenGame";
        property(instigator_property,actor_class,"Instigator",0x330,pawn_class);
        property(chain_property,manager_class,"InventoryChain",0x350,inventory_class);
        property(item_manager_property,inventory_class,"InvManager",0x340,manager_class);
        property(role_property,actor_class,"Role",0x360,0);
        memory.put(role_property+0x50,byte_property_class);
        memory.put(role_property+0x6c,std::int32_t{1});
        property(physics_property,actor_class,"Physics",0x370,0);
        memory.put(physics_property+0x50,byte_property_class);
        memory.put(physics_property+0x6c,std::int32_t{1});
        const std::array<std::uint64_t,3> gate_properties{can_fire_property,shutdown_property,client_shutdown_property};
        const std::array<const char*,3> gate_names{"bServerCanFire","bShutdown","bClientShutdown"};
        const std::array<std::uint32_t,3> gate_masks{0x2,0x40,0x200};
        for(std::size_t i=0;i<gate_properties.size();++i) {
            property(gate_properties[i],dual_class,gate_names[i],0x380,0);
            identities[gate_properties[i]].package="HawkenGame";
            memory.put(gate_properties[i]+0x50,bool_property_class);
            memory.put(gate_properties[i]+0x6c,std::int32_t{4});
            memory.put(gate_properties[i]+0xa8,gate_masks[i]);
            if(i) memory.put(gate_properties[i-1]+0x60,gate_properties[i]);
        }
        memory.put(dual_class+0x80,can_fire_property);
        memory.put(pawn_class+0x80,invmanager_property);
        memory.put(invmanager_property+0x60,weapon_property);
        memory.put(native_pawn_class+0x80,dual_property);
        memory.put(actor_class+0x80,instigator_property);
        memory.put(instigator_property+0x60,role_property);
        memory.put(role_property+0x60,physics_property);
        memory.put(manager_class+0x80,chain_property);
        memory.put(inventory_class+0x80,item_manager_property);
        memory.put(pawn_object+0x300,manager_object);
        memory.put(pawn_object+0x310,dual_object);
        memory.put(pawn_object+0x320,dual_object);
        memory.put(manager_object+0x330,pawn_object);
        memory.put(manager_object+0x350,dual_object);
        memory.put(manager_object+0x360,std::uint8_t{3});
        memory.put(dual_object+0x330,pawn_object);
        memory.put(dual_object+0x340,manager_object);
        memory.put(pawn_object+0x370,std::uint8_t{1});
        memory.put(dual_object+0x360,std::uint8_t{2});
        memory.put(dual_object+0x380,std::uint32_t{0x202});
        klass(float_property_class,"FloatProperty","Core",object_class);
        // Original f24b0:f252e passes cast0x100; ctor692e8..692ef stores it at+128.
        memory.put(float_property_class+0x128,std::uint32_t{0x100});
        property(jump_capable_property,pawn_class,"bJumpCapable",0x3c0,0);
        memory.put(jump_capable_property+0x50,bool_property_class);
        memory.put(jump_capable_property+0x6c,std::int32_t{4});
        memory.put(jump_capable_property+0xa8,std::uint32_t{0x800});
        property(jump_z_property,pawn_class,"JumpZ",0x3c4,0);
        property(thruster_energy_property,native_pawn_class,"ThrusterEnergy",0x3c8,0);
        property(thruster_jump_property,native_pawn_class,"ThrusterJumpEnergy",0x3cc,0);
        identities[thruster_energy_property].package="HawkenGame";
        identities[thruster_jump_property].package="HawkenGame";
        for(const auto descriptor:{jump_z_property,thruster_energy_property,thruster_jump_property}) {
            memory.put(descriptor+0x50,float_property_class);
            memory.put(descriptor+0x6c,std::int32_t{4});
        }
        property(move_state_property,native_pawn_class,"MoveState",0x3d0,0);
        identities[move_state_property].package="HawkenGame";
        memory.put(move_state_property+0x50,byte_property_class);
        memory.put(move_state_property+0x6c,std::int32_t{1});
        memory.put(weapon_property+0x60,jump_capable_property);
        memory.put(jump_capable_property+0x60,jump_z_property);
        memory.put(dual_property+0x60,thruster_energy_property);
        memory.put(thruster_energy_property+0x60,thruster_jump_property);
        memory.put(thruster_jump_property+0x60,move_state_property);
        memory.put(pawn_object+0x3c0,std::uint32_t{0x810});
        memory.put(pawn_object+0x3c4,725.5F);
        memory.put(pawn_object+0x3c8,2.25F);
        memory.put(pawn_object+0x3cc,0.75F);
        memory.put(pawn_object+0x3d0,std::uint8_t{2});
        property(pawn_can_move_property,native_pawn_class,"bcanMove",0x3d4,0);
        identities[pawn_can_move_property].package="HawkenGame";
        memory.put(pawn_can_move_property+0x50,bool_property_class);
        memory.put(pawn_can_move_property+0x6c,std::int32_t{4});
        memory.put(pawn_can_move_property+0xa8,std::uint32_t{0x4000});
        memory.put(move_state_property+0x60,pawn_can_move_property);
        memory.put(pawn_object+0x3d4,std::uint32_t{0x4200});
        property(pawn_no_fire_property,pawn_class,"bNoWeaponFiring",0x3e8,0);
        memory.put(pawn_no_fire_property+0x50,bool_property_class);
        memory.put(pawn_no_fire_property+0x6c,std::int32_t{4});
        memory.put(pawn_no_fire_property+0xa8,std::uint32_t{0x20000000});
        memory.put(jump_z_property+0x60,pawn_no_fire_property);
        memory.put(pawn_object+0x3e8,std::uint32_t{0x20000001});
        memory.put(pawn_object+0x360,std::uint8_t{1});
        memory.put(controller_object+0x360,std::uint8_t{3});
        // Valid absent frames keep old fixtures independent of state execution.
        for(const auto actor:{pawn_object,manager_object,dual_object})
            memory.put(actor+0x20,std::uint64_t{0});
        klass(script_function_class,"Function","Core",object_class);
        klass(script_state_class,"State","Core",object_class);
        dispatch_function(jump_dispatch_function,native_pawn_class,"CheckJumpOrHealOrTaunt",
            0x20802,0,module_base+0x2345);
        dispatch_function(fire_dispatch_function,dual_class,"ChangeCanFire",
            0x20802,17,module_base+0x6789);
        memory.put(pawn_can_move_property+0x60,jump_dispatch_function);
        memory.put(client_shutdown_property+0x60,fire_dispatch_function);
    }
    void dispatch_function(std::uint64_t where,std::uint64_t owner,const char* name,
                           std::uint32_t flags,std::uint16_t token,std::uint64_t target) {
        object(where,script_function_class,owner);
        identities[where]={name,"HawkenGame",0};
        memory.put(where+0x60,std::uint64_t{0});
        memory.put(where+0xd0,flags);memory.put(where+0xd4,token);memory.put(where+0xf0,target);
    }
    LivePlayerSnapshot inventory_sample() {
        auto identify=[&](std::uint64_t where) {
            const auto found=identities.find(where);
            if(found==identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return read_live_player(memory,module_base,identify,true);
    }
};
}

TEST_CASE("Live inventory observes typed equipment relationships and Manager Role") {
    InventoryFixture f;const auto result=f.inventory_sample();
    REQUIRE(result.inventory.has_value());const auto& value=*result.inventory;
    CHECK(value.pawn_invmanager_present);CHECK(value.pawn_dual_weapon_present);
    CHECK(value.pawn_weapon_present);CHECK(value.pawn_weapon_is_dual);
    CHECK(value.manager_instigator_is_pawn);CHECK(value.manager_chain_is_dual);
    CHECK(value.dual_instigator_is_pawn);CHECK(value.dual_invmanager_matches);
    REQUIRE(value.manager_role.has_value());CHECK(*value.manager_role==3);
}
TEST_CASE("Live inventory optional mode preserves old reads and skips absent Pawn") {
    InventoryFixture f;
    SUBCASE("disabled mode never follows unreadable inventory") {
        f.memory.bytes.erase(pawn_object+0x300);
        CHECK_FALSE(f.sample().inventory.has_value());
    }
    SUBCASE("no Pawn leaves inventory absent") {
        f.memory.put(controller_object+0x280,std::uint64_t{0});
        f.memory.bytes.erase(pawn_class+0x80);
        CHECK_FALSE(f.inventory_sample().inventory.has_value());
    }
}
TEST_CASE("Live inventory null links are valid and do not manufacture equality") {
    InventoryFixture f;
    f.memory.put(pawn_object+0x300,std::uint64_t{0});
    f.memory.put(pawn_object+0x310,std::uint64_t{0});
    f.memory.put(pawn_object+0x320,std::uint64_t{0});
    f.memory.bytes.erase(manager_object+0x50);f.memory.bytes.erase(dual_object+0x50);
    const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());const auto& value=*result.inventory;
    CHECK_FALSE(value.pawn_invmanager_present);CHECK_FALSE(value.pawn_dual_weapon_present);
    CHECK_FALSE(value.pawn_weapon_present);CHECK_FALSE(value.pawn_weapon_is_dual);
    CHECK_FALSE(value.manager_instigator_is_pawn);CHECK_FALSE(value.manager_chain_is_dual);
    CHECK_FALSE(value.dual_instigator_is_pawn);CHECK_FALSE(value.dual_invmanager_matches);
    CHECK_FALSE(value.manager_role.has_value());
}
TEST_CASE("Live inventory distinguishes valid nonmatching typed references") {
    InventoryFixture f;
    constexpr std::uint64_t other_pawn=0x60000,other_manager=0x64000;
    f.object(other_pawn,pawn_class,0);f.object(other_manager,hg_manager_class,0);
    f.memory.put(pawn_object+0x310,hand_object);
    f.memory.put(manager_object+0x330,other_pawn);
    f.memory.put(manager_object+0x350,hand_object);
    f.memory.put(dual_object+0x330,other_pawn);
    f.memory.put(dual_object+0x340,other_manager);
    const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());const auto& value=*result.inventory;
    CHECK(value.pawn_weapon_present);CHECK_FALSE(value.pawn_weapon_is_dual);
    CHECK_FALSE(value.manager_instigator_is_pawn);CHECK_FALSE(value.manager_chain_is_dual);
    CHECK_FALSE(value.dual_instigator_is_pawn);CHECK_FALSE(value.dual_invmanager_matches);
}
TEST_CASE("Live inventory rejects missing duplicate or malformed reflected object descriptors") {
    InventoryFixture f;
    SUBCASE("missing DualWeapon") {f.memory.put(native_pawn_class+0x80,std::uint64_t{0});}
    SUBCASE("wrong package") {f.identities[dual_property].package="Engine";}
    SUBCASE("wrong owner") {f.memory.put(invmanager_property+0x40,actor_class);}
    SUBCASE("wrong property type") {f.memory.put(weapon_property+0x50,byte_property_class);}
    SUBCASE("array instead of scalar") {f.memory.put(chain_property+0x68,std::int32_t{4});}
    SUBCASE("wrong element size") {f.memory.put(item_manager_property+0x6c,std::int32_t{4});}
    SUBCASE("wrong declared expected class") {f.memory.put(dual_property+0xa8,weapon_class);}
    SUBCASE("negative offset") {f.memory.put(weapon_property+0x8c,std::int32_t{-1});}
    SUBCASE("duplicate metadata") {
        constexpr std::uint64_t extra=0x53000;
        f.property(extra,pawn_class,"Weapon",0x310,weapon_class);
        f.memory.put(weapon_property+0x60,extra);
    }
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory requires ByteProperty Role metadata rather than a guessed live offset") {
    InventoryFixture f;
    SUBCASE("missing Role") {f.memory.put(instigator_property+0x60,physics_property);}
    SUBCASE("Role object instead of byte") {f.memory.put(role_property+0x50,property_class);}
    SUBCASE("Role class missing UProperty base flag") {f.memory.put(byte_property_class+0x128,std::uint32_t{0});}
    SUBCASE("Role class has only ObjectProperty flag") {f.memory.put(byte_property_class+0x128,std::uint32_t{0x10000});}
    SUBCASE("Role element size") {f.memory.put(role_property+0x6c,std::int32_t{4});}
    SUBCASE("Role array dim") {f.memory.put(role_property+0x68,std::int32_t{2});}
    SUBCASE("Role offset over bound") {f.memory.put(role_property+0x8c,std::int32_t{65537});}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory validates all followed instances before dereferencing relationships") {
    InventoryFixture f;
    SUBCASE("Manager wrong type") {f.memory.put(manager_object+0x50,weapon_class);}
    SUBCASE("Dual wrong type") {f.memory.put(dual_object+0x50,weapon_class);}
    SUBCASE("Weapon not Inventory") {f.memory.put(pawn_object+0x310,pri_object);}
    SUBCASE("Manager CDO") {f.memory.put(hg_manager_class+0x1d4,manager_object);}
    SUBCASE("Dual template flag") {f.memory.put(dual_object+0x10,std::uint64_t{0x400});}
    SUBCASE("Chain invalid type") {f.memory.put(manager_object+0x350,pri_object);}
    SUBCASE("Instigator invalid type") {f.memory.put(dual_object+0x330,manager_object);}
    SUBCASE("Dual manager invalid type") {f.memory.put(dual_object+0x340,hand_object);}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory keeps cycle offset and shared reader budget guards") {
    InventoryFixture f;
    SUBCASE("inventory descriptor cycle") {f.memory.put(weapon_property+0x60,invmanager_property);}
    SUBCASE("inventory offset over bound") {f.memory.put(dual_property+0x8c,std::int32_t{65537});}
    SUBCASE("budget has only enough bytes for old mode") {
        static_cast<void>(f.sample());const auto used=1024*1024-f.memory.budget;
        f.memory.budget=used;
    }
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory reads reflected offsets and traces Role or reference changes") {
    InventoryFixture f;
    f.memory.put(invmanager_property+0x8c,std::int32_t{0x3a0});
    f.memory.put(pawn_object+0x3a0,manager_object);f.memory.bytes.erase(pawn_object+0x300);
    const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    CHECK(first.inventory->pawn_invmanager_present);
    f.memory.put(manager_object+0x360,std::uint8_t{2});
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    CHECK(second.inventory->manager_role==std::optional<std::uint8_t>{2});
    CHECK_FALSE(first.trace==second.trace);CHECK_FALSE(first==second);
}

namespace {
// Shipped owner counts are retained in pawn-schema-input.json, serialized-child-fields.json,
// and gameplay-diagnosis.json. Padding is synthetic and retains each exact fixture target.
void pad_live_owner(InventoryFixture& f,std::uint64_t owner,std::size_t count,std::uint64_t storage) {
    auto cursor=f.memory.read<std::uint64_t>(owner,0x80);
    std::uint64_t previous=0;std::size_t present=0;
    while(cursor) {previous=cursor;++present;cursor=f.memory.read<std::uint64_t>(cursor,0x60);}
    REQUIRE(present<=count);
    for(std::size_t i=present;i<count;++i) {
        const auto node=storage+static_cast<std::uint64_t>(i)*0x100;
        f.property(node,owner,"SyntheticPadding",0,object_class);
        f.identities[node].package=f.identities.at(owner).package;
        if(previous) f.memory.put(previous+0x60,node);
        else f.memory.put(owner+0x80,node);
        previous=node;
    }
}
constexpr std::array<std::uint64_t,8> live_original_owners{
    player_class,pc_class,controller_class,pawn_class,native_pawn_class,actor_class,manager_class,inventory_class};
constexpr std::array<std::size_t,8> live_original_counts{10,575,196,423,496,449,36,27};
}
TEST_CASE("Live inventory accepts original full owner sizes and repeated traversal aggregate") {
    InventoryFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    // Original repeated-route upper bound3729 remains; local metadata reuse stays within it.
    const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
    CHECK(result.inventory->pawn_weapon_is_dual);CHECK(result.inventory->manager_chain_is_dual);
    CHECK(result.inventory->manager_role==std::optional<std::uint8_t>{3});
}
TEST_CASE("Live inventory rejects one more than each original owner child cap") {
    for(std::size_t i=0;i<live_original_owners.size();++i) {
        CAPTURE(i);InventoryFixture f;
        pad_live_owner(f,live_original_owners[i],live_original_counts[i]+1,0x80000);
        CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_limit",std::runtime_error);
    }
}
TEST_CASE("Live player default mode accepts original PlayerController575 without inventory reads") {
    InventoryFixture f;
    pad_live_owner(f,player_class,10,0x80000);
    pad_live_owner(f,pc_class,575,0xc0000);
    pad_live_owner(f,controller_class,196,0x100000);
    f.memory.bytes.erase(pawn_object+0x300);
    // Default route: Player10+PlayerController575+Controller196 twice=977 visits.
    const auto result=f.sample();CHECK(result.pawn_present);CHECK_FALSE(result.inventory.has_value());
}

TEST_CASE("Live inventory resolves owner fields once in each independent original-size sample") {
    InventoryFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    std::map<std::uint64_t,std::size_t> identity_counts;
    auto identify_once_test=[&](std::uint64_t where) {
        ++identity_counts[where];return f.identities.at(where);
    };
    f.memory.read_counts.clear();
    const auto first=read_live_player(f.memory,module_base,identify_once_test,true);
    REQUIRE(first.inventory.has_value());CHECK(first.inventory->dual_invmanager_matches);
    for(const auto owner:live_original_owners) CHECK(f.memory.read_counts[owner+0x80]==1);
    CHECK(f.memory.read_counts[instigator_property+0x8c]==1);
    CHECK(f.memory.read_counts[role_property+0x8c]==1);
    for(const auto& item:identity_counts) CHECK(item.second==1);
    // A second call must reload metadata, rather than reuse a prior sample.
    f.memory.put(item_manager_property+0x8c,std::int32_t{0x3b0});
    f.memory.put(dual_object+0x3b0,std::uint64_t{0});
    f.memory.read_counts.clear();identity_counts.clear();
    const auto second=read_live_player(f.memory,module_base,identify_once_test,true);
    REQUIRE(second.inventory.has_value());CHECK_FALSE(second.inventory->dual_invmanager_matches);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
    for(const auto owner:live_original_owners) CHECK(f.memory.read_counts[owner+0x80]==1);
    CHECK(f.memory.read_counts[instigator_property+0x8c]==1);
    for(const auto& item:identity_counts) CHECK(item.second==1);
    f.identities[byte_property_class].package="Engine";
    CHECK_THROWS_WITH_AS(read_live_player(f.memory,module_base,identify_once_test,true),
        "live_property_mismatch",std::runtime_error);
}
TEST_CASE("Live inventory cached owner traversal still rejects late duplicate fields and cycles") {
    InventoryFixture f;pad_live_owner(f,actor_class,449,0x80000);
    auto tail=f.memory.read<std::uint64_t>(actor_class,0x80);
    while(f.memory.read<std::uint64_t>(tail,0x60)) tail=f.memory.read<std::uint64_t>(tail,0x60);
    SUBCASE("duplicate target at last allowed child") {
        f.property(tail,actor_class,"Instigator",0x330,pawn_class);
        CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_property_mismatch",std::runtime_error);
    }
    SUBCASE("cycle after the target and all padding") {
        f.memory.put(tail+0x60,instigator_property);
        CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_cycle",std::runtime_error);
    }
}

TEST_CASE("Live inventory observes reflected Pawn Physics Dual Role and fire gates with metadata") {
    InventoryFixture f;const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
    const auto& v=*result.inventory;
    CHECK(v.pawn_physics==std::optional<std::uint8_t>{1});
    CHECK(v.dual_role==std::optional<std::uint8_t>{2});
    REQUIRE(v.dual_server_can_fire.has_value());REQUIRE(v.dual_shutdown.has_value());
    REQUIRE(v.dual_client_shutdown.has_value());
    CHECK(v.dual_server_can_fire->value);CHECK_FALSE(v.dual_shutdown->value);
    CHECK(v.dual_client_shutdown->value);
    CHECK(v.dual_server_can_fire->metadata_offset==0x380);
    CHECK(v.dual_server_can_fire->bit_mask==0x2);
    CHECK(v.dual_shutdown->bit_mask==0x40);CHECK(v.dual_client_shutdown->bit_mask==0x200);
}
TEST_CASE("Live inventory fire gate metadata is typed scalar and single bit") {
    InventoryFixture f;
    SUBCASE("missing descriptor") {f.memory.put(dual_class+0x80,shutdown_property);}
    SUBCASE("wrong descriptor owner") {f.memory.put(can_fire_property+0x40,weapon_class);}
    SUBCASE("wrong descriptor package") {f.identities[can_fire_property].package="Engine";}
    SUBCASE("wrong type") {f.memory.put(can_fire_property+0x50,byte_property_class);}
    SUBCASE("Physics wrong type") {f.memory.put(physics_property+0x50,bool_property_class);}
    SUBCASE("Physics wrong scalar size") {f.memory.put(physics_property+0x6c,std::int32_t{4});}
    SUBCASE("duplicate bool metadata") {
        constexpr std::uint64_t extra_gate=0x55000;
        f.property(extra_gate,dual_class,"bServerCanFire",0x380,0);
        f.identities[extra_gate].package="HawkenGame";
        f.memory.put(client_shutdown_property+0x60,extra_gate);
    }
    SUBCASE("bool owner cycle") {f.memory.put(client_shutdown_property+0x60,can_fire_property);}
    SUBCASE("wrong Core type package") {f.identities[bool_property_class].package="Engine";}
    SUBCASE("wrong type kind") {f.memory.put(bool_property_class+0x50,object_class);}
    SUBCASE("missing bool cast mask") {f.memory.put(bool_property_class+0x128,std::uint32_t{0x8000});}
    SUBCASE("array bool") {f.memory.put(can_fire_property+0x68,std::int32_t{2});}
    SUBCASE("wrong bool size") {f.memory.put(can_fire_property+0x6c,std::int32_t{1});}
    SUBCASE("zero mask") {f.memory.put(can_fire_property+0xa8,std::uint32_t{0});}
    SUBCASE("multibit mask") {f.memory.put(can_fire_property+0xa8,std::uint32_t{3});}
    SUBCASE("negative offset") {f.memory.put(can_fire_property+0x8c,std::int32_t{-1});}
    SUBCASE("offset above bound") {f.memory.put(can_fire_property+0x8c,std::int32_t{65537});}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory fire gate samples reread changed metadata and values independently") {
    InventoryFixture f;const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    f.memory.put(can_fire_property+0x8c,std::int32_t{0x390});
    f.memory.put(can_fire_property+0xa8,std::uint32_t{0x80000000});
    f.memory.put(dual_object+0x390,std::uint32_t{0});
    f.memory.put(pawn_object+0x370,std::uint8_t{2});
    f.memory.put(dual_object+0x360,std::uint8_t{3});
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    REQUIRE(second.inventory->dual_server_can_fire.has_value());
    CHECK_FALSE(second.inventory->dual_server_can_fire->value);
    CHECK(second.inventory->dual_server_can_fire->metadata_offset==0x390);
    CHECK(second.inventory->dual_server_can_fire->bit_mask==0x80000000);
    CHECK(second.inventory->pawn_physics==std::optional<std::uint8_t>{2});
    CHECK(second.inventory->dual_role==std::optional<std::uint8_t>{3});
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
    f.memory.put(can_fire_property+0xa8,std::uint32_t{0});
    CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_property_mismatch",std::runtime_error);
}
TEST_CASE("Live inventory absent Dual leaves fire gates absent without reading descriptors") {
    InventoryFixture f;
    f.memory.put(pawn_object+0x310,std::uint64_t{0});
    f.memory.put(pawn_object+0x320,std::uint64_t{0});
    f.memory.put(manager_object+0x350,std::uint64_t{0});
    f.memory.bytes.erase(dual_class+0x80);
    const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
    CHECK(result.inventory->pawn_physics==std::optional<std::uint8_t>{1});
    CHECK_FALSE(result.inventory->dual_role.has_value());
    CHECK_FALSE(result.inventory->dual_server_can_fire.has_value());
    CHECK_FALSE(result.inventory->dual_shutdown.has_value());
    CHECK_FALSE(result.inventory->dual_client_shutdown.has_value());
}
TEST_CASE("Live inventory preserves exact DualWield59 owner cap and default mode isolation") {
    InventoryFixture f;
    SUBCASE("original59 fits existing shared budget") {
        for(std::size_t i=0;i<live_original_owners.size();++i)
            pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
        pad_live_owner(f,dual_class,59,0x300000);
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
        CHECK(result.inventory->dual_server_can_fire.has_value());CHECK(f.memory.budget>0);
    }
    SUBCASE("60 rejected") {
        pad_live_owner(f,dual_class,60,0x300000);
        CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_limit",std::runtime_error);
    }
    SUBCASE("default does not inspect fire metadata") {
        f.memory.bytes.erase(physics_property+0x50);f.memory.bytes.erase(dual_class+0x80);
        CHECK_FALSE(f.sample().inventory.has_value());
    }
}

TEST_CASE("Live inventory observes five reflected Pawn jump gates without interpreting heavy mode") {
    InventoryFixture f;
    const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
    const auto& jump=*result.inventory;
    REQUIRE(jump.pawn_jump_capable.has_value());
    CHECK(jump.pawn_jump_capable->value);
    CHECK(jump.pawn_jump_capable->metadata_offset==0x3c0);
    CHECK(jump.pawn_jump_capable->bit_mask==0x800);
    CHECK(jump.pawn_jump_z==std::optional<float>{725.5F});
    CHECK(jump.pawn_thruster_energy==std::optional<float>{2.25F});
    CHECK(jump.pawn_thruster_jump_energy==std::optional<float>{0.75F});
    CHECK(jump.pawn_move_state==std::optional<std::uint8_t>{2});
    f.memory.put(pawn_object+0x3c0,std::uint32_t{0x10});
    f.memory.put(pawn_object+0x3d0,std::uint8_t{255});
    const auto changed=f.inventory_sample();REQUIRE(changed.inventory.has_value());
    REQUIRE(changed.inventory->pawn_jump_capable.has_value());
    CHECK_FALSE(changed.inventory->pawn_jump_capable->value);
    // Observed bytes remain raw; no inferred enum range or native heavy predicate.
    CHECK(changed.inventory->pawn_move_state==std::optional<std::uint8_t>{255});
}
TEST_CASE("Live inventory jump gate descriptors require exact owners and scalar Core types") {
    InventoryFixture f;
    SUBCASE("missing Pawn field") {f.memory.put(weapon_property+0x60,jump_z_property);}
    SUBCASE("wrong native owner") {f.memory.put(thruster_energy_property+0x40,pawn_class);}
    SUBCASE("wrong native package") {f.identities[thruster_jump_property].package="Engine";}
    SUBCASE("wrong float type") {f.memory.put(jump_z_property+0x50,property_class);}
    SUBCASE("wrong float Core package") {f.identities[float_property_class].package="Engine";}
    SUBCASE("wrong float class metadata") {f.memory.put(float_property_class+0x50,object_class);}
    SUBCASE("missing float cast bit") {f.memory.put(float_property_class+0x128,std::uint32_t{0x8000});}
    SUBCASE("array float") {f.memory.put(thruster_energy_property+0x68,std::int32_t{2});}
    SUBCASE("wrong float size") {f.memory.put(thruster_jump_property+0x6c,std::int32_t{8});}
    SUBCASE("negative float offset") {f.memory.put(jump_z_property+0x8c,std::int32_t{-1});}
    SUBCASE("float offset above reader bound") {f.memory.put(jump_z_property+0x8c,std::int32_t{65537});}
    SUBCASE("wrong jump bool type") {f.memory.put(jump_capable_property+0x50,byte_property_class);}
    SUBCASE("jump bool multibit mask") {f.memory.put(jump_capable_property+0xa8,std::uint32_t{3});}
    SUBCASE("wrong MoveState type") {f.memory.put(move_state_property+0x50,bool_property_class);}
    SUBCASE("wrong MoveState size") {f.memory.put(move_state_property+0x6c,std::int32_t{4});}
    SUBCASE("duplicate jump target") {
        constexpr std::uint64_t jump_duplicate=0x57400;
        f.property(jump_duplicate,pawn_class,"JumpZ",0x3c4,0);
        f.memory.put(jump_z_property+0x60,jump_duplicate);
    }
    SUBCASE("native owner cycle after MoveState") {f.memory.put(move_state_property+0x60,dual_property);}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory rejects nonfinite jump samples as an explicit observer policy") {
    // This observer cannot emit nonfinite JSON numbers. It does not declare a
    // universal native physics invariant or clamp/change the original values.
    for(const auto offset:{0x3c4U,0x3c8U,0x3ccU}) {
        for(const float bad:{std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
            CAPTURE(offset);InventoryFixture f;f.memory.put(pawn_object+offset,bad);
            CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
        }
    }
}
TEST_CASE("Live inventory jump samples preserve original owner limits and per sample metadata trace") {
    InventoryFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    f.memory.put(pawn_object+0x3c4,-0.0F);
    f.memory.read_counts.clear();
    const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    REQUIRE(first.inventory->pawn_jump_z.has_value());
    CHECK(*first.inventory->pawn_jump_z==0.0F);
    CHECK(f.memory.read_counts[pawn_class+0x80]==1);
    CHECK(f.memory.read_counts[native_pawn_class+0x80]==1);
    CHECK(f.memory.read_counts[float_property_class+0x128]>0);
    f.memory.put(pawn_object+0x3c4,0.0F);
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    CHECK(second.inventory->pawn_jump_z==std::optional<float>{0.0F});
    // Equal floating values with different raw bits must still differ in the
    // private equality trace used for two samples, without serializing bytes.
    CHECK_FALSE(first.trace==second.trace);
    f.memory.put(thruster_energy_property+0x8c,std::int32_t{0x3e0});
    f.memory.put(pawn_object+0x3e0,-12.5F);
    f.memory.put(jump_capable_property+0xa8,std::uint32_t{0x10});
    f.memory.put(pawn_object+0x3c0,std::uint32_t{0});
    const auto third=f.inventory_sample();REQUIRE(third.inventory.has_value());
    CHECK(third.inventory->pawn_thruster_energy==std::optional<float>{-12.5F});
    REQUIRE(third.inventory->pawn_jump_capable.has_value());
    CHECK_FALSE(third.inventory->pawn_jump_capable->value);
    CHECK(third.inventory->pawn_jump_capable->bit_mask==0x10);
    CHECK_FALSE(second==third);CHECK_FALSE(second.trace==third.trace);
    CHECK(f.memory.budget>0);
}
TEST_CASE("Live inventory jump observations remain absent without Pawn or inventory inspection") {
    InventoryFixture f;
    SUBCASE("no Pawn does not read jump descriptors") {
        f.memory.put(controller_object+0x280,std::uint64_t{0});
        f.memory.bytes.erase(jump_z_property+0x50);
        const auto result=f.inventory_sample();CHECK_FALSE(result.pawn_present);
        CHECK_FALSE(result.inventory.has_value());
    }
    SUBCASE("default mode does not read jump descriptors") {
        f.memory.bytes.erase(jump_z_property+0x50);
        const auto result=f.sample();CHECK(result.pawn_present);
        CHECK_FALSE(result.inventory.has_value());
    }
}

TEST_CASE("Live inventory observes reflected bcanMove without assuming native offset or mask") {
    InventoryFixture f;const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    REQUIRE(first.inventory->pawn_can_move.has_value());
    CHECK(first.inventory->pawn_can_move->value);
    CHECK(first.inventory->pawn_can_move->metadata_offset==0x3d4);
    CHECK(first.inventory->pawn_can_move->bit_mask==0x4000);
    f.memory.put(pawn_object+0x3d4,std::uint32_t{0x200});
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    REQUIRE(second.inventory->pawn_can_move.has_value());
    CHECK_FALSE(second.inventory->pawn_can_move->value);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
}
TEST_CASE("Live inventory bcanMove rejects malformed exact owner Bool metadata") {
    InventoryFixture f;
    SUBCASE("missing field") {f.memory.put(move_state_property+0x60,std::uint64_t{0});}
    SUBCASE("wrong owner") {f.memory.put(pawn_can_move_property+0x40,pawn_class);}
    SUBCASE("wrong package") {f.identities[pawn_can_move_property].package="Engine";}
    SUBCASE("wrong type") {f.memory.put(pawn_can_move_property+0x50,byte_property_class);}
    SUBCASE("array bool") {f.memory.put(pawn_can_move_property+0x68,std::int32_t{2});}
    SUBCASE("wrong scalar size") {f.memory.put(pawn_can_move_property+0x6c,std::int32_t{1});}
    SUBCASE("zero mask") {f.memory.put(pawn_can_move_property+0xa8,std::uint32_t{0});}
    SUBCASE("multiple mask bits") {f.memory.put(pawn_can_move_property+0xa8,std::uint32_t{3});}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory bcanMove revalidates changed metadata in each original size sample") {
    InventoryFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    f.memory.read_counts.clear();
    const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    REQUIRE(first.inventory->pawn_can_move.has_value());CHECK(first.inventory->pawn_can_move->value);
    CHECK(f.memory.read_counts[native_pawn_class+0x80]==1);
    f.memory.put(pawn_can_move_property+0x8c,std::int32_t{0x3e4});
    f.memory.put(pawn_can_move_property+0xa8,std::uint32_t{0x1});
    f.memory.put(pawn_object+0x3e4,std::uint32_t{0x2});
    f.memory.read_counts.clear();
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    REQUIRE(second.inventory->pawn_can_move.has_value());
    CHECK_FALSE(second.inventory->pawn_can_move->value);
    CHECK(second.inventory->pawn_can_move->metadata_offset==0x3e4);
    CHECK(second.inventory->pawn_can_move->bit_mask==0x1);
    CHECK(f.memory.read_counts[native_pawn_class+0x80]==1);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
    CHECK(f.memory.budget>0);
}

TEST_CASE("Live inventory observes Pawn Controller roles and reflected no weapon firing without forcing policy") {
    InventoryFixture f;const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    CHECK(first.inventory->pawn_role==std::optional<std::uint8_t>{1});
    CHECK(first.inventory->controller_role==std::optional<std::uint8_t>{3});
    REQUIRE(first.inventory->pawn_no_weapon_firing.has_value());
    CHECK(first.inventory->pawn_no_weapon_firing->value);
    CHECK(first.inventory->pawn_no_weapon_firing->metadata_offset==0x3e8);
    CHECK(first.inventory->pawn_no_weapon_firing->bit_mask==0x20000000);
    f.memory.put(pawn_object+0x3e8,std::uint32_t{1});
    f.memory.put(pawn_object+0x360,std::uint8_t{255});
    f.memory.put(controller_object+0x360,std::uint8_t{0});
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    CHECK(second.inventory->pawn_role==std::optional<std::uint8_t>{255});
    CHECK(second.inventory->controller_role==std::optional<std::uint8_t>{0});
    REQUIRE(second.inventory->pawn_no_weapon_firing.has_value());
    CHECK_FALSE(second.inventory->pawn_no_weapon_firing->value);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
}
TEST_CASE("Live inventory role and no weapon firing descriptors remain exact scalar metadata") {
    InventoryFixture f;
    SUBCASE("Role wrong owner") {f.memory.put(role_property+0x40,pawn_class);}
    SUBCASE("Role wrong type") {f.memory.put(role_property+0x50,bool_property_class);}
    SUBCASE("missing no firing field") {f.memory.put(jump_z_property+0x60,std::uint64_t{0});}
    SUBCASE("no firing wrong owner") {f.memory.put(pawn_no_fire_property+0x40,native_pawn_class);}
    SUBCASE("no firing wrong package") {f.identities[pawn_no_fire_property].package="HawkenGame";}
    SUBCASE("no firing wrong type") {f.memory.put(pawn_no_fire_property+0x50,byte_property_class);}
    SUBCASE("no firing is array") {f.memory.put(pawn_no_fire_property+0x68,std::int32_t{2});}
    SUBCASE("no firing mask invalid") {f.memory.put(pawn_no_fire_property+0xa8,std::uint32_t{3});}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory roles and no weapon firing reread reflected metadata between original size samples") {
    InventoryFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    f.memory.read_counts.clear();
    const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    CHECK(first.inventory->pawn_role==std::optional<std::uint8_t>{1});
    REQUIRE(first.inventory->pawn_no_weapon_firing.has_value());
    CHECK(f.memory.read_counts[role_property+0x8c]==1);
    CHECK(f.memory.read_counts[pawn_class+0x80]==1);
    f.memory.put(role_property+0x8c,std::int32_t{0x3f0});
    f.memory.put(pawn_object+0x3f0,std::uint8_t{3});
    f.memory.put(controller_object+0x3f0,std::uint8_t{1});
    f.memory.put(manager_object+0x3f0,std::uint8_t{3});
    f.memory.put(dual_object+0x3f0,std::uint8_t{2});
    f.memory.put(pawn_no_fire_property+0x8c,std::int32_t{0x3f4});
    f.memory.put(pawn_no_fire_property+0xa8,std::uint32_t{0x2});
    f.memory.put(pawn_object+0x3f4,std::uint32_t{0x4});
    f.memory.read_counts.clear();
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());
    CHECK(second.inventory->pawn_role==std::optional<std::uint8_t>{3});
    CHECK(second.inventory->controller_role==std::optional<std::uint8_t>{1});
    REQUIRE(second.inventory->pawn_no_weapon_firing.has_value());
    CHECK_FALSE(second.inventory->pawn_no_weapon_firing->value);
    CHECK(second.inventory->pawn_no_weapon_firing->metadata_offset==0x3f4);
    CHECK(second.inventory->pawn_no_weapon_firing->bit_mask==0x2);
    CHECK(f.memory.read_counts[role_property+0x8c]==1);
    CHECK(f.memory.read_counts[pawn_class+0x80]==1);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);CHECK(f.memory.budget>0);
}

namespace {
constexpr std::uint64_t pawn_state_frame=0x70000, manager_state_frame=0x70100, dual_state_frame=0x70200;
constexpr std::uint64_t pawn_state_node=0x71000, manager_state_node=0x71400, dual_state_node=0x71800;
struct ScriptFixture : InventoryFixture {
    ScriptFixture() {
        state(pawn_object,pawn_state_frame,pawn_state_node,native_pawn_class,"JumpActive");
        state(manager_object,manager_state_frame,manager_state_node,hg_manager_class,"InventoryActive");
        state(dual_object,dual_state_frame,dual_state_node,dual_class,"Active");
    }
    void state(std::uint64_t actor,std::uint64_t frame,std::uint64_t node,
               std::uint64_t owner,const char* name) {
        memory.put(actor+0x20,frame);memory.put(frame+0x44,node);
        object(node,script_state_class,owner);
        identities[node]={name,identities.at(owner).package,0};
        memory.put(node+0x38,std::int32_t{7});
    }
};
}
TEST_CASE("Live inventory script observer distinguishes three owned states and exact callback metadata") {
    ScriptFixture f;const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
    const auto& value=*result.inventory;
    REQUIRE(value.pawn_script_state.has_value());REQUIRE(value.manager_script_state.has_value());
    REQUIRE(value.dual_script_state.has_value());
    CHECK(value.pawn_script_state->kind==hawken::probe::LiveScriptStateKind::named);
    CHECK(value.pawn_script_state->name=="JumpActive");CHECK(value.pawn_script_state->package=="HawkenGame");
    CHECK(value.manager_script_state->kind==hawken::probe::LiveScriptStateKind::named);
    CHECK(value.manager_script_state->name=="InventoryActive");CHECK(value.manager_script_state->package=="HawkenGame");
    CHECK(value.dual_script_state->kind==hawken::probe::LiveScriptStateKind::named);
    CHECK(value.dual_script_state->name=="Active");CHECK(value.dual_script_state->package=="HawkenGame");
    REQUIRE(value.pawn_jump_dispatch.has_value());REQUIRE(value.dual_can_fire_dispatch.has_value());
    CHECK(value.pawn_jump_dispatch->flags==0x20802);CHECK(value.pawn_jump_dispatch->native_token==0);
    CHECK(value.pawn_jump_dispatch->target_in_original_text);
    CHECK(value.pawn_jump_dispatch->target_rva==std::optional<std::uint32_t>{0x2345});
    CHECK(value.dual_can_fire_dispatch->flags==0x20802);CHECK(value.dual_can_fire_dispatch->native_token==17);
    CHECK(value.dual_can_fire_dispatch->target_in_original_text);
    CHECK(value.dual_can_fire_dispatch->target_rva==std::optional<std::uint32_t>{0x6789});
}
TEST_CASE("Live inventory script observer preserves absent uninitialized and class node states") {
    ScriptFixture f;
    SUBCASE("no frame and no node are different observations") {
        f.memory.put(pawn_object+0x20,std::uint64_t{0});f.memory.put(manager_state_frame+0x44,std::uint64_t{0});
        f.identities.erase(pawn_state_node);f.identities.erase(manager_state_node);
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
        REQUIRE(result.inventory->pawn_script_state.has_value());REQUIRE(result.inventory->manager_script_state.has_value());
        CHECK(result.inventory->pawn_script_state->kind==hawken::probe::LiveScriptStateKind::no_frame);
        CHECK(result.inventory->manager_script_state->kind==hawken::probe::LiveScriptStateKind::no_node);
        CHECK(result.inventory->pawn_script_state->name.empty());CHECK(result.inventory->pawn_script_state->package.empty());
        CHECK(result.inventory->manager_script_state->name.empty());CHECK(result.inventory->manager_script_state->package.empty());
    }
    SUBCASE("uninitialized validates node kind and owner without identifying its name") {
        f.memory.put(pawn_state_node+0x38,std::int32_t{-1});f.identities.erase(pawn_state_node);
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());REQUIRE(result.inventory->pawn_script_state.has_value());
        CHECK(result.inventory->pawn_script_state->kind==hawken::probe::LiveScriptStateKind::uninitialized);
        CHECK(result.inventory->pawn_script_state->name.empty());CHECK(result.inventory->pawn_script_state->package.empty());
        CHECK(f.memory.read_counts[pawn_state_node+0x48]==0);
    }
    SUBCASE("class node must be in this actor ancestry") {
        f.memory.put(dual_state_frame+0x44,weapon_class);f.memory.put(weapon_class+0x38,std::int32_t{0});
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());REQUIRE(result.inventory->dual_script_state.has_value());
        CHECK(result.inventory->dual_script_state->kind==hawken::probe::LiveScriptStateKind::named);
        CHECK(result.inventory->dual_script_state->name=="Weapon");CHECK(result.inventory->dual_script_state->package=="Engine");
    }
}
TEST_CASE("Live inventory script observer rejects foreign malformed or numbered state nodes") {
    ScriptFixture f;
    SUBCASE("wrong node type") {f.memory.put(pawn_state_node+0x50,property_class);}
    SUBCASE("wrong State metaclass") {f.memory.put(script_state_class+0x50,object_class);}
    SUBCASE("foreign owner") {f.memory.put(pawn_state_node+0x40,pc_class);}
    SUBCASE("self outer is not an owning class") {f.memory.put(pawn_state_node+0x40,pawn_state_node);}
    SUBCASE("numbered state name") {f.identities[pawn_state_node].instance=1;}
    SUBCASE("state package differs from owner") {f.identities[pawn_state_node].package="Engine";}
    SUBCASE("uninitialized cannot bypass owner validation") {
        f.memory.put(pawn_state_node+0x38,std::int32_t{-1});f.memory.put(pawn_state_node+0x40,pc_class);
        f.identities.erase(pawn_state_node);
    }
    SUBCASE("Class node outside actor ancestry") {
        f.memory.put(pawn_state_frame+0x44,pc_class);f.memory.put(pc_class+0x38,std::int32_t{0});
    }
    SUBCASE("overflowing frame pointer") {f.memory.put(pawn_object+0x20,std::numeric_limits<std::uint64_t>::max()-16);}
    CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory script observer requires both exact callback descriptors") {
    for(unsigned callback=0;callback<2;++callback) {
        for(unsigned invalid=0;invalid<8;++invalid) {
            CAPTURE(callback);CAPTURE(invalid);ScriptFixture f;
            const auto descriptor=callback==0?jump_dispatch_function:fire_dispatch_function;
            const auto predecessor=callback==0?pawn_can_move_property:client_shutdown_property;
            const auto owner=callback==0?native_pawn_class:dual_class;
            const auto name=callback==0?"CheckJumpOrHealOrTaunt":"ChangeCanFire";
            if(invalid==0) f.memory.put(predecessor+0x60,std::uint64_t{0});
            if(invalid==1) f.memory.put(descriptor+0x50,bool_property_class);
            if(invalid==2) f.memory.put(descriptor+0x40,pc_class);
            if(invalid==3) f.identities[descriptor].instance=1;
            if(invalid==4) f.identities[descriptor].package="Engine";
            if(invalid==5) {
                constexpr std::uint64_t duplicate=0x58800;
                f.dispatch_function(duplicate,owner,name,0x20802,0,module_base+0x1111);
                f.memory.put(descriptor+0x60,duplicate);
            }
            if(invalid==6) f.memory.put(script_function_class+0x50,object_class);
            if(invalid==7) f.identities[script_function_class].package="Engine";
            CHECK_THROWS_WITH_AS(f.inventory_sample(),invalid==0?"live_property_missing":"live_property_mismatch",std::runtime_error);
        }
    }
}

TEST_CASE("Live inventory script observer bounds executable targets without reading code") {
    struct Target {std::uint64_t address;std::optional<std::uint32_t> rva;};
    const Target targets[]{
        {0,std::nullopt},{module_base+0xfff,std::nullopt},
        {module_base+0x1000,0x1000},{module_base+0x128df78,0x128df78},
        {module_base+0x128df79,std::nullopt},{std::numeric_limits<std::uint64_t>::max(),std::nullopt}};
    for(const auto& target:targets) {
        ScriptFixture f;
        f.memory.put(jump_dispatch_function+0xf0,target.address);f.memory.put(fire_dispatch_function+0xf0,target.address);
        f.memory.put(jump_dispatch_function+0xd0,std::uint32_t{0xfedcba98});
        f.memory.put(jump_dispatch_function+0xd4,std::uint16_t{0xbeef});
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());
        REQUIRE(result.inventory->pawn_jump_dispatch.has_value());REQUIRE(result.inventory->dual_can_fire_dispatch.has_value());
        CHECK(result.inventory->pawn_jump_dispatch->target_in_original_text==target.rva.has_value());
        CHECK(result.inventory->pawn_jump_dispatch->target_rva==target.rva);
        CHECK(result.inventory->dual_can_fire_dispatch->target_in_original_text==target.rva.has_value());
        CHECK(result.inventory->dual_can_fire_dispatch->target_rva==target.rva);
        CHECK(result.inventory->pawn_jump_dispatch->flags==0xfedcba98);CHECK(result.inventory->pawn_jump_dispatch->native_token==0xbeef);
        CHECK(f.memory.read_counts[target.address]==0);
    }
}
TEST_CASE("Live inventory script observer rereads roots and dispatch in independent original size samples") {
    ScriptFixture f;
    for(std::size_t i=0;i<live_original_owners.size();++i)
        pad_live_owner(f,live_original_owners[i],live_original_counts[i],0x80000+i*0x40000);
    pad_live_owner(f,dual_class,59,0x300000);
    f.memory.read_counts.clear();
    const auto first=f.inventory_sample();REQUIRE(first.inventory.has_value());
    REQUIRE(first.inventory->pawn_script_state.has_value());REQUIRE(first.inventory->pawn_jump_dispatch.has_value());
    CHECK(f.memory.read_counts[native_pawn_class+0x80]==1);CHECK(f.memory.read_counts[dual_class+0x80]==1);
    // Equal metadata names at different roots must still change the private trace.
    constexpr std::uint64_t next_frame=0x72000,next_node=0x72400;
    f.state(pawn_object,next_frame,next_node,native_pawn_class,"JumpActive");f.memory.read_counts.clear();
    const auto second=f.inventory_sample();REQUIRE(second.inventory.has_value());REQUIRE(second.inventory->pawn_script_state.has_value());
    CHECK(*first.inventory->pawn_script_state==*second.inventory->pawn_script_state);
    CHECK_FALSE(first.trace==second.trace);CHECK_FALSE(first==second);
    CHECK(f.memory.read_counts[native_pawn_class+0x80]==1);CHECK(f.memory.read_counts[dual_class+0x80]==1);
    f.identities[next_node].name="ChangedState";
    f.memory.put(jump_dispatch_function+0xd0,std::uint32_t{0x20002});
    f.memory.put(jump_dispatch_function+0xd4,std::uint16_t{29});
    f.memory.put(jump_dispatch_function+0xf0,module_base+0x9876);
    const auto third=f.inventory_sample();REQUIRE(third.inventory.has_value());
    REQUIRE(third.inventory->pawn_script_state.has_value());REQUIRE(third.inventory->pawn_jump_dispatch.has_value());
    CHECK(third.inventory->pawn_script_state->name=="ChangedState");
    CHECK(third.inventory->pawn_jump_dispatch->flags==0x20002);CHECK(third.inventory->pawn_jump_dispatch->native_token==29);
    CHECK(third.inventory->pawn_jump_dispatch->target_rva==std::optional<std::uint32_t>{0x9876});
    CHECK_FALSE(second==third);CHECK_FALSE(second.trace==third.trace);
    CHECK(f.memory.budget>0);CHECK(f.memory.budget<1024*1024);
    f.memory.put(next_node+0x40,pc_class);CHECK_THROWS_AS(f.inventory_sample(),std::runtime_error);
}
TEST_CASE("Live inventory script observer skips absent actors without manufacturing state or dispatch") {
    ScriptFixture f;
    SUBCASE("default mode never follows a frame or callback") {
        f.memory.bytes.erase(pawn_object+0x20);f.memory.bytes.erase(jump_dispatch_function+0xd0);
        const auto result=f.sample();CHECK_FALSE(result.inventory.has_value());
    }
    SUBCASE("absent Pawn has no script observation") {
        f.memory.put(controller_object+0x280,std::uint64_t{0});f.memory.bytes.erase(pawn_object+0x20);
        const auto result=f.inventory_sample();CHECK_FALSE(result.inventory.has_value());
    }
    SUBCASE("absent Manager and Dual do not follow stale frames") {
        f.memory.put(pawn_object+0x300,std::uint64_t{0});f.memory.put(pawn_object+0x310,std::uint64_t{0});
        f.memory.put(pawn_object+0x320,std::uint64_t{0});
        f.memory.bytes.erase(manager_object+0x20);f.memory.bytes.erase(dual_object+0x20);
        f.memory.bytes.erase(fire_dispatch_function+0xd0);
        const auto result=f.inventory_sample();REQUIRE(result.inventory.has_value());REQUIRE(result.inventory->pawn_script_state.has_value());
        CHECK_FALSE(result.inventory->manager_script_state.has_value());CHECK_FALSE(result.inventory->dual_script_state.has_value());
        CHECK_FALSE(result.inventory->dual_can_fire_dispatch.has_value());
    }
}
TEST_CASE("Live inventory script observer keeps child cycle and shared read budget failures") {
    ScriptFixture f;
    SUBCASE("cycle after required callback") {
        f.memory.put(jump_dispatch_function+0x60,jump_dispatch_function);
        CHECK_THROWS_WITH_AS(f.inventory_sample(),"live_cycle",std::runtime_error);
    }
    SUBCASE("same shared read budget") {
        f.memory.budget=1;CHECK_THROWS_WITH_AS(f.inventory_sample(),"read_limit",std::runtime_error);
    }
}


namespace {
constexpr std::uint64_t timing_timestamp_property=0x59000;
constexpr std::uint64_t timing_controller_property=0x59400;
constexpr std::uint64_t timing_acknowledged_property=0x59800;
constexpr std::uint64_t timing_dual_client_property=0x5a000;
constexpr std::uint64_t timing_dual_server_property=0x5a400;
struct FireTimingFixture : InventoryFixture {
    FireTimingFixture() {
        // Deliberately distinct from candidate native offsets4ec/26c. Only the
        // reflected descriptors may select these synthetic instance locations.
        property(timing_timestamp_property,pc_class,"CurrentTimeStamp",0x610,0);
        memory.put(timing_timestamp_property+0x50,float_property_class);
        memory.put(timing_timestamp_property+0x6c,std::int32_t{4});
        property(timing_controller_property,pawn_class,"Controller",0x618,controller_class);
        property(timing_acknowledged_property,pc_class,"AcknowledgedPawn",0x620,pawn_class);
        memory.put(player_property+0x60,timing_timestamp_property);
        memory.put(timing_timestamp_property+0x60,timing_acknowledged_property);
        memory.put(pawn_no_fire_property+0x60,timing_controller_property);
        memory.put(controller_object+0x610,12.5F);
        memory.put(pawn_object+0x618,controller_object);
        memory.put(controller_object+0x620,pawn_object);
        // Neither native candidate3dc/3e0 may substitute for these reflected
        // Dual offsets. Keep pre-existing timing tests supplied with valid data.
        property(timing_dual_client_property,dual_class,"ClientCanFireTimeStamp",0x650,0);
        property(timing_dual_server_property,dual_class,"ServerCanFireTimeStamp",0x674,0);
        for(const auto descriptor:{timing_dual_client_property,timing_dual_server_property}) {
            identities[descriptor].package="HawkenGame";
            memory.put(descriptor+0x50,float_property_class);
            memory.put(descriptor+0x6c,std::int32_t{4});
        }
        memory.put(fire_dispatch_function+0x60,timing_dual_client_property);
        memory.put(timing_dual_client_property+0x60,timing_dual_server_property);
        memory.put(dual_object+0x650,1.5F);
        memory.put(dual_object+0x674,2.75F);
    }
    LivePlayerSnapshot timing_sample(bool inventory=true,bool timing=true) {
        auto identify=[&](std::uint64_t where) {
            const auto found=identities.find(where);
            if(found==identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return read_live_player(memory,module_base,identify,inventory,timing);
    }
};
}

TEST_CASE("Live fire timing reads finite timestamp and typed mutual links at reflected fixture offsets") {
    FireTimingFixture f;const auto sampled=f.timing_sample();
    REQUIRE(sampled.fire_timing.has_value());const auto& value=*sampled.fire_timing;
    CHECK(value.controller_current_timestamp==12.5F);
    CHECK(value.controller_current_timestamp_offset==0x610);
    CHECK(value.pawn_controller_offset==0x618);CHECK(value.acknowledged_pawn_offset==0x620);
    CHECK(value.pawn_controller_present);CHECK(value.pawn_controller_matches);
    CHECK(value.acknowledged_pawn_present);CHECK(value.acknowledged_pawn_matches);
    CHECK(f.memory.read_counts[controller_object+0x610]==1);
    CHECK(f.memory.read_counts[pawn_object+0x618]==1);
    CHECK(f.memory.read_counts[controller_object+0x620]==1);
    CHECK(f.memory.read_counts[controller_object+0x4ec]==0);
    CHECK(f.memory.read_counts[pawn_object+0x26c]==0);
    REQUIRE(sampled.inventory.has_value());CHECK(sampled.inventory->pawn_weapon_is_dual);
}
TEST_CASE("Live fire timing distinguishes null from valid different Controller and Pawn references") {
    FireTimingFixture f;
    SUBCASE("both null") {
        f.memory.put(pawn_object+0x618,std::uint64_t{0});
        f.memory.put(controller_object+0x620,std::uint64_t{0});
        const auto sampled=f.timing_sample();REQUIRE(sampled.fire_timing.has_value());
        CHECK_FALSE(sampled.fire_timing->pawn_controller_present);
        CHECK_FALSE(sampled.fire_timing->pawn_controller_matches);
        CHECK_FALSE(sampled.fire_timing->acknowledged_pawn_present);
        CHECK_FALSE(sampled.fire_timing->acknowledged_pawn_matches);
    }
    SUBCASE("other live instances of the declared expected classes") {
        constexpr std::uint64_t other_controller=0x70000,other_pawn=0x74000;
        f.object(other_controller,hawken_pc_class,0);f.object(other_pawn,content_pawn_class,0);
        f.memory.put(pawn_object+0x618,other_controller);
        f.memory.put(controller_object+0x620,other_pawn);
        const auto sampled=f.timing_sample();REQUIRE(sampled.fire_timing.has_value());
        CHECK(sampled.fire_timing->pawn_controller_present);
        CHECK_FALSE(sampled.fire_timing->pawn_controller_matches);
        CHECK(sampled.fire_timing->acknowledged_pawn_present);
        CHECK_FALSE(sampled.fire_timing->acknowledged_pawn_matches);
    }
}
TEST_CASE("Live fire timing rejects wrong referenced type invalid live flags or class default objects") {
    FireTimingFixture f;
    SUBCASE("Pawn Controller is a PRI") {f.memory.put(pawn_object+0x618,pri_object);}
    SUBCASE("AcknowledgedPawn is a Controller") {f.memory.put(controller_object+0x620,controller_object);}
    SUBCASE("Controller reference has invalid instance flags") {
        constexpr std::uint64_t other=0x70000;f.object(other,hawken_pc_class,0);
        f.memory.put(other+0x10,std::uint64_t{0x200});f.memory.put(pawn_object+0x618,other);
    }
    SUBCASE("Pawn reference is its class CDO") {
        constexpr std::uint64_t other=0x74000;f.object(other,content_pawn_class,0);
        f.memory.put(content_pawn_class+0x1d4,other);f.memory.put(controller_object+0x620,other);
    }
    CHECK_THROWS_WITH_AS(f.timing_sample(),"live_identity",std::runtime_error);
}
TEST_CASE("Live fire timing requires exact scalar descriptor owner type and bounded reflected offsets") {
    FireTimingFixture f;
    SUBCASE("missing timestamp") {f.memory.put(player_property+0x60,timing_acknowledged_property);}
    SUBCASE("missing Pawn Controller") {f.memory.put(pawn_no_fire_property+0x60,std::uint64_t{0});}
    SUBCASE("missing AcknowledgedPawn") {f.memory.put(timing_timestamp_property+0x60,std::uint64_t{0});}
    SUBCASE("timestamp is not a FloatProperty") {f.memory.put(timing_timestamp_property+0x50,byte_property_class);}
    SUBCASE("timestamp FloatProperty lacks cast flag") {f.memory.put(float_property_class+0x128,std::uint32_t{0});}
    SUBCASE("timestamp ArrayDim") {f.memory.put(timing_timestamp_property+0x68,std::int32_t{2});}
    SUBCASE("timestamp element width") {f.memory.put(timing_timestamp_property+0x6c,std::int32_t{8});}
    SUBCASE("wrong timestamp descriptor owner") {f.memory.put(timing_timestamp_property+0x40,controller_class);}
    SUBCASE("wrong Pawn Controller expected class") {f.memory.put(timing_controller_property+0xa8,pc_class);}
    SUBCASE("wrong AcknowledgedPawn expected class") {f.memory.put(timing_acknowledged_property+0xa8,actor_class);}
    SUBCASE("negative timestamp offset") {f.memory.put(timing_timestamp_property+0x8c,std::int32_t{-1});}
    SUBCASE("Controller offset above existing guard") {f.memory.put(timing_controller_property+0x8c,std::int32_t{65537});}
    SUBCASE("duplicate timestamp") {
        constexpr std::uint64_t extra=0x59c00;
        f.property(extra,pc_class,"CurrentTimeStamp",0x610,0);
        f.memory.put(extra+0x50,float_property_class);f.memory.put(extra+0x6c,std::int32_t{4});
        f.memory.put(timing_timestamp_property+0x60,extra);f.memory.put(extra+0x60,timing_acknowledged_property);
    }
    CHECK_THROWS_AS(f.timing_sample(),std::runtime_error);
}
TEST_CASE("Live fire timing rejects nonfinite timestamps without clamping finite values") {
    for(const float bad:{std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        FireTimingFixture f;f.memory.put(controller_object+0x610,bad);
        CHECK_THROWS_WITH_AS(f.timing_sample(),"live_invalid",std::runtime_error);
    }
    FireTimingFixture f;f.memory.put(controller_object+0x610,-3.25F);
    const auto sampled=f.timing_sample();REQUIRE(sampled.fire_timing.has_value());
    CHECK(sampled.fire_timing->controller_current_timestamp==-3.25F);
}
TEST_CASE("Live fire timing is opt in and requires inventory inspection without changing disabled reads") {
    FireTimingFixture f;
    // Disabled observation must not read live timing values or validate their
    // unused descriptors. Existing inventory samples remain byte for byte equal.
    f.memory.bytes.erase(controller_object+0x610);f.memory.bytes.erase(pawn_object+0x618);
    f.memory.bytes.erase(controller_object+0x620);
    f.memory.put(timing_timestamp_property+0x50,byte_property_class);
    const auto previous=f.inventory_sample();f.memory.read_counts.clear();
    const auto disabled=f.timing_sample(true,false);
    CHECK(previous==disabled);CHECK_FALSE(disabled.fire_timing.has_value());
    CHECK(f.memory.read_counts[controller_object+0x610]==0);
    CHECK(f.memory.read_counts[pawn_object+0x618]==0);CHECK(f.memory.read_counts[controller_object+0x620]==0);
    CHECK_THROWS_WITH_AS(f.timing_sample(false,true),"live_invalid",std::runtime_error);
}
TEST_CASE("Live fire timing includes metadata and live values in independent sample equality") {
    FireTimingFixture f;const auto first=f.timing_sample();REQUIRE(first.fire_timing.has_value());
    f.memory.put(timing_timestamp_property+0x8c,std::int32_t{0x640});
    f.memory.put(controller_object+0x640,12.5F);
    const auto second=f.timing_sample();REQUIRE(second.fire_timing.has_value());
    CHECK(first.fire_timing->controller_current_timestamp==second.fire_timing->controller_current_timestamp);
    CHECK(second.fire_timing->controller_current_timestamp_offset==0x640);
    CHECK_FALSE(first==second);CHECK_FALSE(first.trace==second.trace);
    f.memory.put(controller_object+0x640,13.0F);
    const auto third=f.timing_sample();REQUIRE(third.fire_timing.has_value());
    CHECK(third.fire_timing->controller_current_timestamp==13.0F);
    CHECK_FALSE(second==third);CHECK_FALSE(second.trace==third.trace);
}

TEST_CASE("Live dual fire timing reads reflected finite operands and reuses Instigator observation") {
    struct Values {float client;float server;std::uint32_t client_bits;std::uint32_t server_bits;};
    // Hand-derived IEEE754 literals catch clamping, swapped operands and loss
    // of the signed-zero bits without relying on the reader to build an oracle.
    for(const auto values:std::array<Values,3>{{
            {1.5F,2.75F,0x3fc00000U,0x40300000U},
            {-3.25F,-7.5F,0xc0500000U,0xc0f00000U},
            {-0.0F,0.0F,0x80000000U,0x00000000U}}}) {
        FireTimingFixture f;
        f.memory.put(dual_object+0x650,values.client);f.memory.put(dual_object+0x674,values.server);
        const auto sampled=f.timing_sample();REQUIRE(sampled.fire_timing.has_value());
        REQUIRE(sampled.fire_timing->dual_weapon.has_value());const auto& dual_timing=*sampled.fire_timing->dual_weapon;
        CHECK(std::bit_cast<std::uint32_t>(dual_timing.client_can_fire_timestamp)==values.client_bits);
        CHECK(std::bit_cast<std::uint32_t>(dual_timing.server_can_fire_timestamp)==values.server_bits);
        CHECK(dual_timing.client_can_fire_timestamp_offset==0x650);
        CHECK(dual_timing.server_can_fire_timestamp_offset==0x674);
        CHECK(dual_timing.instigator_offset==0x330);
        CHECK(f.memory.read_counts[dual_object+0x650]==1);CHECK(f.memory.read_counts[dual_object+0x674]==1);
        CHECK(f.memory.read_counts[dual_object+0x330]==1);
        CHECK(f.memory.read_counts[instigator_property+0x8c]==1);
        CHECK(f.memory.read_counts[dual_object+0x3dc]==0);CHECK(f.memory.read_counts[dual_object+0x3e0]==0);
        CHECK(f.memory.budget>0);
    }
}
TEST_CASE("Live dual fire timing rejects missing malformed or duplicate scalar descriptors") {
    enum class Malformation {missing,type,owner,dimension,width,cast,duplicate};
    for(const auto descriptor:{timing_dual_client_property,timing_dual_server_property}) {
        for(const auto bad:{Malformation::missing,Malformation::type,Malformation::owner,
                           Malformation::dimension,Malformation::width,Malformation::cast,Malformation::duplicate}) {
            FireTimingFixture f;CAPTURE(descriptor);CAPTURE(static_cast<int>(bad));
            switch(bad) {
            case Malformation::missing:f.identities[descriptor].name="OtherTimestamp";break;
            case Malformation::type:f.memory.put(descriptor+0x50,byte_property_class);break;
            case Malformation::owner:f.memory.put(descriptor+0x40,weapon_class);break;
            case Malformation::dimension:f.memory.put(descriptor+0x68,std::int32_t{2});break;
            case Malformation::width:f.memory.put(descriptor+0x6c,std::int32_t{8});break;
            case Malformation::cast: {
                // Isolate this property's cast validation from the Controller
                // timestamp, whose already-valid FloatProperty must stay valid.
                constexpr std::uint64_t other_float_type=0x16800;
                f.klass(other_float_type,"FloatProperty","Core",object_class);
                f.memory.put(other_float_type+0x128,std::uint32_t{0});
                f.memory.put(descriptor+0x50,other_float_type);break;
            }
            case Malformation::duplicate: {
                constexpr std::uint64_t extra=0x5a800;
                const auto* field=descriptor==timing_dual_client_property?"ClientCanFireTimeStamp":"ServerCanFireTimeStamp";
                f.property(extra,dual_class,field,0x6a0,0);f.identities[extra].package="HawkenGame";
                f.memory.put(extra+0x50,float_property_class);f.memory.put(extra+0x6c,std::int32_t{4});
                f.memory.put(dual_object+0x6a0,5.0F);
                f.memory.put(timing_dual_server_property+0x60,extra);break;
            }
            }
            if(bad==Malformation::missing)
                CHECK_THROWS_WITH_AS(f.timing_sample(),"live_property_missing",std::runtime_error);
            else CHECK_THROWS_WITH_AS(f.timing_sample(),"live_property_mismatch",std::runtime_error);
        }
    }
}
TEST_CASE("Live dual fire timing rejects nonfinite values in either operand") {
    for(const auto offset:{0x650U,0x674U}) {
        for(const auto bits:{0x7f800000U,0xff800000U,0x7fc00001U}) {
            FireTimingFixture f;CAPTURE(offset);CAPTURE(bits);
            f.memory.put(dual_object+offset,bits);
            CHECK_THROWS_WITH_AS(f.timing_sample(),"live_invalid",std::runtime_error);
        }
    }
}
TEST_CASE("Live dual fire timing skips absent Dual and preserves disabled snapshots traces and reads") {
    SUBCASE("absent Dual does not inspect stale operands or descriptors") {
        FireTimingFixture f;
        f.memory.put(pawn_object+0x310,std::uint64_t{0});f.memory.put(pawn_object+0x320,std::uint64_t{0});
        f.memory.put(timing_dual_client_property+0x50,byte_property_class);
        f.memory.bytes.erase(dual_object+0x650);f.memory.bytes.erase(dual_object+0x674);
        const auto sampled=f.timing_sample();REQUIRE(sampled.fire_timing.has_value());
        CHECK_FALSE(sampled.fire_timing->dual_weapon.has_value());
        CHECK(f.memory.read_counts[dual_object+0x650]==0);CHECK(f.memory.read_counts[dual_object+0x674]==0);
    }
    SUBCASE("disabled timing has exactly the existing inventory observations") {
        FireTimingFixture f;const auto previous=f.inventory_sample();const auto previous_reads=f.memory.read_counts;
        f.memory.read_counts.clear();
        f.memory.put(timing_dual_client_property+0x50,byte_property_class);
        f.memory.bytes.erase(dual_object+0x650);f.memory.bytes.erase(dual_object+0x674);
        const auto disabled=f.timing_sample(true,false);
        CHECK(previous==disabled);CHECK(previous.trace==disabled.trace);
        CHECK(previous_reads==f.memory.read_counts);CHECK_FALSE(disabled.fire_timing.has_value());
    }
}
TEST_CASE("Live dual fire timing includes both operand offsets and values in sample equality") {
    FireTimingFixture f;const auto first=f.timing_sample();REQUIRE(first.fire_timing.has_value());
    REQUIRE(first.fire_timing->dual_weapon.has_value());
    SUBCASE("client reflected offset changes with the same value") {
        f.memory.put(timing_dual_client_property+0x8c,std::int32_t{0x690});f.memory.put(dual_object+0x690,1.5F);
        const auto next=f.timing_sample();REQUIRE(next.fire_timing.has_value());REQUIRE(next.fire_timing->dual_weapon.has_value());
        CHECK(next.fire_timing->dual_weapon->client_can_fire_timestamp==1.5F);
        CHECK(next.fire_timing->dual_weapon->client_can_fire_timestamp_offset==0x690);
        CHECK_FALSE(*first.fire_timing->dual_weapon==*next.fire_timing->dual_weapon);
        CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
    SUBCASE("server reflected offset changes with the same value") {
        f.memory.put(timing_dual_server_property+0x8c,std::int32_t{0x694});f.memory.put(dual_object+0x694,2.75F);
        const auto next=f.timing_sample();REQUIRE(next.fire_timing.has_value());REQUIRE(next.fire_timing->dual_weapon.has_value());
        CHECK(next.fire_timing->dual_weapon->server_can_fire_timestamp==2.75F);
        CHECK(next.fire_timing->dual_weapon->server_can_fire_timestamp_offset==0x694);
        CHECK_FALSE(*first.fire_timing->dual_weapon==*next.fire_timing->dual_weapon);
        CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
    SUBCASE("client live value changes") {
        f.memory.put(dual_object+0x650,-2.0F);
        const auto next=f.timing_sample();REQUIRE(next.fire_timing.has_value());REQUIRE(next.fire_timing->dual_weapon.has_value());
        CHECK(next.fire_timing->dual_weapon->client_can_fire_timestamp==-2.0F);
        CHECK_FALSE(*first.fire_timing->dual_weapon==*next.fire_timing->dual_weapon);
        CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
    SUBCASE("server live value changes") {
        f.memory.put(dual_object+0x674,-4.0F);
        const auto next=f.timing_sample();REQUIRE(next.fire_timing.has_value());REQUIRE(next.fire_timing->dual_weapon.has_value());
        CHECK(next.fire_timing->dual_weapon->server_can_fire_timestamp==-4.0F);
        CHECK_FALSE(*first.fire_timing->dual_weapon==*next.fire_timing->dual_weapon);
        CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
}

namespace {
constexpr std::uint64_t fixture_primitive_class=0x16c00,fixture_cylinder_class=0x17000;
constexpr std::uint64_t fixture_component_property_class=0x17400;
constexpr std::uint64_t fixture_component_base_class=0x17c00;
constexpr std::uint64_t fixture_cylinder_object=0x78000,fixture_other_component=0x7c000;
constexpr std::uint64_t fixture_collision_property=0x5b000,fixture_cylinder_property=0x5b400;
constexpr std::uint64_t fixture_radius_property=0x5b800,fixture_height_property=0x5bc00;
struct PawnPhysicsFixture : InventoryFixture {
    PawnPhysicsFixture() {
        klass(fixture_primitive_class,"PrimitiveComponent","Engine",object_class);
        klass(fixture_cylinder_class,"CylinderComponent","Engine",fixture_primitive_class);
        // Observer condition: validate the sampled ComponentProperty ancestry
        // and the known ObjectProperty BASE cast; no derived cast is assumed.
        // This synthetic chain does not prove the original native inheritance.
        klass(fixture_component_base_class,"ObjectProperty","Core",object_class);
        memory.put(fixture_component_base_class+0x128,std::uint32_t{0x10000});
        klass(fixture_component_property_class,"ComponentProperty","Core",fixture_component_base_class);
        object(fixture_cylinder_object,fixture_cylinder_class,pawn_object);
        object(fixture_other_component,fixture_primitive_class,pawn_object);
        property(fixture_collision_property,actor_class,"CollisionComponent",0x710,fixture_primitive_class);
        property(fixture_cylinder_property,pawn_class,"CylinderComponent",0x728,fixture_cylinder_class);
        for(const auto descriptor:{fixture_collision_property,fixture_cylinder_property})
            memory.put(descriptor+0x50,fixture_component_property_class);
        memory.put(physics_property+0x60,fixture_collision_property);
        memory.put(pawn_no_fire_property+0x60,fixture_cylinder_property);
        property(fixture_radius_property,fixture_cylinder_class,"CollisionRadius",0x150,0);
        property(fixture_height_property,fixture_cylinder_class,"CollisionHeight",0x16c,0);
        for(const auto descriptor:{fixture_radius_property,fixture_height_property}) {
            memory.put(descriptor+0x50,float_property_class);
            memory.put(descriptor+0x6c,std::int32_t{4});
        }
        memory.put(fixture_cylinder_class+0x80,fixture_radius_property);
        memory.put(fixture_radius_property+0x60,fixture_height_property);
        memory.put(pawn_object+0x710,fixture_cylinder_object);
        memory.put(pawn_object+0x728,fixture_cylinder_object);
        memory.put(fixture_cylinder_object+0x150,34.5F);
        memory.put(fixture_cylinder_object+0x16c,79.25F);
    }
    LivePlayerSnapshot physics_sample(bool inventory=true,bool physics=true) {
        auto identify=[&](std::uint64_t where) {
            const auto found=identities.find(where);
            if(found==identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return read_live_player(memory,module_base,identify,inventory,false,physics);
    }
};
}
TEST_CASE("Live Pawn physics resolves ComponentProperty links and finite cylinder dimensions") {
    struct Dimensions {float radius;float height;std::uint32_t radius_bits;std::uint32_t height_bits;};
    for(const auto dimensions:std::array<Dimensions,3>{{
            {34.5F,79.25F,0x420a0000U,0x429e8000U},
            {-1.5F,-2.75F,0xbfc00000U,0xc0300000U},
            {-0.0F,0.0F,0x80000000U,0x00000000U}}}) {
        PawnPhysicsFixture f;
        f.memory.put(fixture_cylinder_object+0x150,dimensions.radius);
        f.memory.put(fixture_cylinder_object+0x16c,dimensions.height);
        // Engine class2568's seven own Children are a measured reader cap,
        // not permission to traverse unbounded component metadata.
        pad_live_owner(f,fixture_cylinder_class,7,0x80000);
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        const auto& observed=*sampled.physics;
        CHECK(observed.collision_component_offset==0x710);CHECK(observed.cylinder_component_offset==0x728);
        CHECK(observed.collision_component_present);CHECK(observed.cylinder_component_present);
        CHECK(observed.collision_component_matches_cylinder);REQUIRE(observed.cylinder.has_value());
        CHECK(std::bit_cast<std::uint32_t>(observed.cylinder->radius)==dimensions.radius_bits);
        CHECK(std::bit_cast<std::uint32_t>(observed.cylinder->height)==dimensions.height_bits);
        CHECK(observed.cylinder->radius_offset==0x150);CHECK(observed.cylinder->height_offset==0x16c);
        CHECK(f.memory.read_counts[fixture_cylinder_object+0x150]==1);
        CHECK(f.memory.read_counts[fixture_cylinder_object+0x16c]==1);CHECK(f.memory.budget>0);
    }
}
TEST_CASE("Live Pawn physics distinguishes absent links from a different valid collision component") {
    PawnPhysicsFixture f;
    SUBCASE("both null") {
        f.memory.put(pawn_object+0x710,std::uint64_t{0});f.memory.put(pawn_object+0x728,std::uint64_t{0});
        f.memory.bytes.erase(fixture_cylinder_object+0x150);
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        CHECK_FALSE(sampled.physics->collision_component_present);CHECK_FALSE(sampled.physics->cylinder_component_present);
        CHECK_FALSE(sampled.physics->collision_component_matches_cylinder);CHECK_FALSE(sampled.physics->cylinder.has_value());
    }
    SUBCASE("CollisionComponent alone is absent") {
        f.memory.put(pawn_object+0x710,std::uint64_t{0});
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        CHECK_FALSE(sampled.physics->collision_component_present);CHECK(sampled.physics->cylinder_component_present);
        CHECK_FALSE(sampled.physics->collision_component_matches_cylinder);REQUIRE(sampled.physics->cylinder.has_value());
        CHECK(sampled.physics->cylinder->radius==34.5F);
    }
    SUBCASE("CylinderComponent alone is absent") {
        f.memory.put(pawn_object+0x728,std::uint64_t{0});f.memory.bytes.erase(fixture_cylinder_object+0x150);
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        CHECK(sampled.physics->collision_component_present);CHECK_FALSE(sampled.physics->cylinder_component_present);
        CHECK_FALSE(sampled.physics->collision_component_matches_cylinder);CHECK_FALSE(sampled.physics->cylinder.has_value());
        CHECK(f.memory.read_counts[fixture_cylinder_object+0x150]==0);
    }
    SUBCASE("CollisionComponent is a distinct PrimitiveComponent") {
        f.memory.put(pawn_object+0x710,fixture_other_component);
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        CHECK(sampled.physics->collision_component_present);CHECK(sampled.physics->cylinder_component_present);
        CHECK_FALSE(sampled.physics->collision_component_matches_cylinder);REQUIRE(sampled.physics->cylinder.has_value());
        CHECK(sampled.physics->cylinder->height==79.25F);
    }
    SUBCASE("distinct CylinderComponent instances do not imply matching references") {
        f.object(fixture_other_component,fixture_cylinder_class,pawn_object);
        f.memory.put(pawn_object+0x710,fixture_other_component);
        const auto sampled=f.physics_sample();REQUIRE(sampled.physics.has_value());
        CHECK(sampled.physics->collision_component_present);CHECK(sampled.physics->cylinder_component_present);
        CHECK_FALSE(sampled.physics->collision_component_matches_cylinder);REQUIRE(sampled.physics->cylinder.has_value());
    }
}
TEST_CASE("Live Pawn physics rejects malformed component descriptors and wrongly typed targets") {
    PawnPhysicsFixture f;
    SUBCASE("missing CollisionComponent") {f.identities[fixture_collision_property].name="OtherComponent";}
    SUBCASE("missing CylinderComponent") {f.identities[fixture_cylinder_property].name="OtherCylinder";}
    SUBCASE("ObjectProperty does not substitute for ComponentProperty") {
        f.memory.put(fixture_collision_property+0x50,property_class);
    }
    SUBCASE("component property wrong package") {f.identities[fixture_component_property_class].package="Engine";}
    SUBCASE("component property wrong metaclass") {f.memory.put(fixture_component_property_class+0x50,object_class);}
    SUBCASE("component property lacks ObjectProperty ancestry") {f.memory.put(fixture_component_property_class+0x78,object_class);}
    SUBCASE("ObjectProperty base lacks its validated cast") {f.memory.put(fixture_component_base_class+0x128,std::uint32_t{0});}
    SUBCASE("component descriptor wrong owner") {f.memory.put(fixture_cylinder_property+0x40,actor_class);}
    SUBCASE("component descriptor array dimension") {f.memory.put(fixture_collision_property+0x68,std::int32_t{2});}
    SUBCASE("component descriptor element size") {f.memory.put(fixture_cylinder_property+0x6c,std::int32_t{4});}
    SUBCASE("wrong CollisionComponent PropertyClass") {f.memory.put(fixture_collision_property+0xa8,pawn_class);}
    SUBCASE("wrong CylinderComponent PropertyClass") {f.memory.put(fixture_cylinder_property+0xa8,fixture_primitive_class);}
    SUBCASE("CollisionComponent references a Pawn") {f.memory.put(pawn_object+0x710,pawn_object);}
    SUBCASE("CylinderComponent references only a PrimitiveComponent") {f.memory.put(pawn_object+0x728,fixture_other_component);}
    SUBCASE("component target is a class default object") {
        f.memory.put(fixture_cylinder_class+0x1d4,fixture_cylinder_object);
    }
    CHECK_THROWS_AS(f.physics_sample(),std::runtime_error);
}
TEST_CASE("Live Pawn physics rejects malformed or nonfinite dimensions and enforces the Cylinder owner cap") {
    for(const auto descriptor:{fixture_radius_property,fixture_height_property}) {
        for(int bad=0;bad<7;++bad) {
            PawnPhysicsFixture f;CAPTURE(descriptor);CAPTURE(bad);
            switch(bad) {
            case 0:f.identities[descriptor].name="OtherDimension";break;
            case 1:f.memory.put(descriptor+0x50,byte_property_class);break;
            case 2:f.memory.put(descriptor+0x40,fixture_primitive_class);break;
            case 3:f.memory.put(descriptor+0x68,std::int32_t{2});break;
            case 4:f.memory.put(descriptor+0x6c,std::int32_t{8});break;
            case 5: {
                constexpr std::uint64_t other_float_type=0x17800;
                f.klass(other_float_type,"FloatProperty","Core",object_class);
                f.memory.put(other_float_type+0x128,std::uint32_t{0});
                f.memory.put(descriptor+0x50,other_float_type);break;
            }
            case 6: {
                constexpr std::uint64_t extra=0x5c000;
                f.property(extra,fixture_cylinder_class,descriptor==fixture_radius_property?"CollisionRadius":"CollisionHeight",0x180,0);
                f.memory.put(extra+0x50,float_property_class);f.memory.put(extra+0x6c,std::int32_t{4});
                f.memory.put(fixture_cylinder_object+0x180,1.0F);f.memory.put(fixture_height_property+0x60,extra);break;
            }
            }
            CHECK_THROWS_AS(f.physics_sample(),std::runtime_error);
        }
    }
    for(const auto offset:{0x150U,0x16cU}) {
        for(const auto bits:{0x7f800000U,0xff800000U,0x7fc00001U}) {
            PawnPhysicsFixture f;CAPTURE(offset);CAPTURE(bits);f.memory.put(fixture_cylinder_object+offset,bits);
            CHECK_THROWS_WITH_AS(f.physics_sample(),"live_invalid",std::runtime_error);
        }
    }
    PawnPhysicsFixture f;pad_live_owner(f,fixture_cylinder_class,8,0x80000);
    CHECK_THROWS_WITH_AS(f.physics_sample(),"live_limit",std::runtime_error);
}
TEST_CASE("Live Pawn physics is opt in and preserves default snapshots traces and reads") {
    PawnPhysicsFixture f;
    SUBCASE("default and false never validate component metadata or read values") {
        const auto previous=f.inventory_sample();const auto previous_reads=f.memory.read_counts;f.memory.read_counts.clear();
        f.memory.put(fixture_collision_property+0x50,byte_property_class);
        f.memory.bytes.erase(pawn_object+0x710);f.memory.bytes.erase(fixture_cylinder_object+0x150);
        const auto disabled=f.physics_sample(true,false);
        CHECK(previous==disabled);CHECK(previous.trace==disabled.trace);CHECK(previous_reads==f.memory.read_counts);
        CHECK_FALSE(disabled.physics.has_value());CHECK(f.memory.read_counts[pawn_object+0x710]==0);
    }
    SUBCASE("physics without inventory fails closed") {
        CHECK_THROWS_WITH_AS(f.physics_sample(false,true),"live_invalid",std::runtime_error);
    }
    SUBCASE("absent Pawn does not manufacture a physics snapshot") {
        f.memory.put(controller_object+0x280,std::uint64_t{0});
        const auto sampled=f.physics_sample();CHECK_FALSE(sampled.physics.has_value());
    }
}
TEST_CASE("Live Pawn physics includes reflected offsets and dimensions in independent sample equality") {
    PawnPhysicsFixture f;const auto first=f.physics_sample();REQUIRE(first.physics.has_value());
    REQUIRE(first.physics->cylinder.has_value());
    SUBCASE("radius offset changes but value is unchanged") {
        f.memory.put(fixture_radius_property+0x8c,std::int32_t{0x180});f.memory.put(fixture_cylinder_object+0x180,34.5F);
    }
    SUBCASE("height offset changes but value is unchanged") {
        f.memory.put(fixture_height_property+0x8c,std::int32_t{0x184});f.memory.put(fixture_cylinder_object+0x184,79.25F);
    }
    SUBCASE("radius value changes") {f.memory.put(fixture_cylinder_object+0x150,-3.0F);}
    SUBCASE("height value changes") {f.memory.put(fixture_cylinder_object+0x16c,-5.0F);}
    SUBCASE("component link offset changes with the same object") {
        f.memory.put(fixture_cylinder_property+0x8c,std::int32_t{0x740});f.memory.put(pawn_object+0x740,fixture_cylinder_object);
    }
    const auto next=f.physics_sample();REQUIRE(next.physics.has_value());REQUIRE(next.physics->cylinder.has_value());
    CHECK_FALSE(*first.physics==*next.physics);CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
}

namespace {
constexpr std::uint64_t gravity_fixture_table_rva=0x1900000;
constexpr std::uint64_t gravity_fixture_image_size=0x201e000;
struct PawnGravityFixture : InventoryFixture {
    PawnGravityFixture() {
        // Synthetic pointers inside the original image extent, not a claim
        // about any live Pawn vtable or a callable getter implementation.
        memory.put(pawn_object,module_base+gravity_fixture_table_rva);
        memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0x3456);
    }
    LivePlayerSnapshot gravity_sample(bool inventory=true,bool gravity=true) {
        auto identify=[&](std::uint64_t where) {
            const auto found=identities.find(where);
            if(found==identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return read_live_player(memory,module_base,identify,inventory,false,false,gravity);
    }
};
}
TEST_CASE("live player gravity observes only bounded vtable and target RVAs") {
    PawnGravityFixture f;
    std::uint32_t expected_table_rva=0x1900000,expected_target_rva=0x3456;
    SUBCASE("interior target") {}
    SUBCASE("first text byte is included") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0x1000);
        expected_target_rva=0x1000;
    }
    SUBCASE("last text byte is included") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0x128df78);
        expected_target_rva=0x128df78;
    }
    SUBCASE("last complete slot fits the image") {
        // image size201e000 - slot2e0 - pointer8 = vtable RVA201dd18.
        f.memory.put(pawn_object,module_base+0x201dd18);
        f.memory.put(module_base+0x201dff8,module_base+0x3456);
        expected_table_rva=0x201dd18;
    }
    const auto sampled=f.gravity_sample();REQUIRE(sampled.gravity.has_value());
    CHECK(sampled.gravity->slot_offset==0x2e0);
    CHECK(sampled.gravity->vtable_rva==expected_table_rva);
    CHECK(sampled.gravity->target_rva==expected_target_rva);
    CHECK(f.memory.read_counts[pawn_object]==1);
    CHECK(f.memory.read_counts[module_base+sampled.gravity->vtable_rva+0x2e0]==1);
    // No bytes of the function body are supplied or read by this observer.
    CHECK(f.memory.read_counts[module_base+sampled.gravity->target_rva]==0);
    CHECK(f.memory.budget>0);
}
TEST_CASE("live player gravity rejects null and out of range vptr slots and targets") {
    PawnGravityFixture f;
    SUBCASE("null vptr") {f.memory.put(pawn_object,std::uint64_t{0});}
    SUBCASE("vptr precedes module") {
        f.memory.put(pawn_object,module_base-8);
        f.memory.put(module_base-8+0x2e0,module_base+0x3456);
    }
    SUBCASE("vptr starts at image end") {
        f.memory.put(pawn_object,module_base+gravity_fixture_image_size);
        f.memory.put(module_base+gravity_fixture_image_size+0x2e0,module_base+0x3456);
    }
    SUBCASE("slot starts at image end despite valid target bytes") {
        f.memory.put(pawn_object,module_base+gravity_fixture_image_size-0x2e0);
        f.memory.put(module_base+gravity_fixture_image_size,module_base+0x3456);
    }
    SUBCASE("slot crosses image end by one byte") {
        f.memory.put(pawn_object,module_base+gravity_fixture_image_size-0x2e0-7);
        f.memory.put(module_base+gravity_fixture_image_size-7,module_base+0x3456);
    }
    SUBCASE("vptr arithmetic would overflow") {
        f.memory.put(pawn_object,std::numeric_limits<std::uint64_t>::max()-8);
    }
    SUBCASE("null target") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,std::uint64_t{0});
    }
    SUBCASE("target precedes module") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base-1);
    }
    SUBCASE("target precedes text") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0xfff);
    }
    SUBCASE("text end is excluded") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0x128df79);
    }
    SUBCASE("target at image end") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+gravity_fixture_image_size);
    }
    CHECK_THROWS_AS(f.gravity_sample(),std::runtime_error);
}
TEST_CASE("live player gravity propagates failed pointer reads without returning a snapshot") {
    PawnGravityFixture f;
    SUBCASE("vptr bytes unavailable") {f.memory.bytes.erase(pawn_object);}
    SUBCASE("slot bytes unavailable") {f.memory.bytes.erase(module_base+gravity_fixture_table_rva+0x2e0);}
    CHECK_THROWS_WITH_AS(f.gravity_sample(),"reader_error",std::runtime_error);
}
TEST_CASE("live player gravity is opt in and leaves absent Pawn and default reads unchanged") {
    PawnGravityFixture f;
    SUBCASE("default and explicit false ignore missing vptr bytes") {
        const auto previous=f.inventory_sample();const auto previous_reads=f.memory.read_counts;
        f.memory.read_counts.clear();f.memory.bytes.erase(pawn_object);
        f.memory.bytes.erase(module_base+gravity_fixture_table_rva+0x2e0);
        const auto disabled=f.gravity_sample(true,false);
        CHECK(previous==disabled);CHECK(previous.trace==disabled.trace);
        CHECK(previous_reads==f.memory.read_counts);CHECK_FALSE(disabled.gravity.has_value());
        CHECK(f.memory.read_counts[pawn_object]==0);
        CHECK(f.memory.read_counts[module_base+gravity_fixture_table_rva+0x2e0]==0);
    }
    SUBCASE("requires inventory inspection") {
        CHECK_THROWS_WITH_AS(f.gravity_sample(false,true),"live_invalid",std::runtime_error);
    }
    SUBCASE("no Pawn is explicitly unobserved") {
        f.memory.put(controller_object+0x280,std::uint64_t{0});f.memory.bytes.erase(pawn_object);
        const auto sampled=f.gravity_sample();CHECK_FALSE(sampled.pawn_present);
        CHECK_FALSE(sampled.gravity.has_value());CHECK(f.memory.read_counts[pawn_object]==0);
        CHECK(f.memory.read_counts[module_base+gravity_fixture_table_rva+0x2e0]==0);
    }
}
TEST_CASE("live player gravity includes vtable and dispatch changes in independent sample equality") {
    PawnGravityFixture f;const auto first=f.gravity_sample();REQUIRE(first.gravity.has_value());
    CHECK(first.gravity->vtable_rva==0x1900000);CHECK(first.gravity->target_rva==0x3456);
    SUBCASE("different vtable retains the same target") {
        f.memory.put(pawn_object,module_base+0x1910000);
        f.memory.put(module_base+0x19102e0,module_base+0x3456);
        const auto next=f.gravity_sample();REQUIRE(next.gravity.has_value());
        CHECK(next.gravity->vtable_rva==0x1910000);CHECK(next.gravity->target_rva==0x3456);
        CHECK_FALSE(*first.gravity==*next.gravity);CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
    SUBCASE("same vtable changes its target") {
        f.memory.put(module_base+gravity_fixture_table_rva+0x2e0,module_base+0x5678);
        const auto next=f.gravity_sample();REQUIRE(next.gravity.has_value());
        CHECK(next.gravity->vtable_rva==0x1900000);CHECK(next.gravity->target_rva==0x5678);
        CHECK_FALSE(*first.gravity==*next.gravity);CHECK_FALSE(first==next);CHECK_FALSE(first.trace==next.trace);
    }
}
