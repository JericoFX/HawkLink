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
