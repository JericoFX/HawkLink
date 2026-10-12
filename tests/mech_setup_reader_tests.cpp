#include "mech_setup_reader.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>

namespace {
using namespace hawken::probe;
// Synthetic typed memory: tests do not attest original process identities.
struct Memory {
    std::map<std::uint64_t, unsigned char> bytes;
    std::vector<std::pair<std::uint64_t, std::size_t>> reads;
    std::size_t budget = 1024 * 1024;
    template<class T> void put(std::uint64_t address, T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = data[i];
    }
    template<class T> T read(std::uint64_t base, std::uint64_t offset = 0) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (offset > maximum - base || sizeof(T) - 1 > maximum - (base + offset))
            throw std::runtime_error("reader_overflow");
        if (sizeof(T) > budget) throw std::runtime_error("read_limit");
        budget -= sizeof(T);
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
constexpr std::uint64_t setup_property = 0x1000;
constexpr std::array<std::uint64_t, 6> structs = {0x2000, 0x2200, 0x2400, 0x2600, 0x2800, 0x2a00};
constexpr std::array<const char*, 6> names = {"SMechSetupStruct", "WeaponInfo", "SAllocatedOptimization",
    "VisualInfo", "PartVisualInfo", "InternalsInfo"};
constexpr std::array<std::uint64_t, 5> types = {0x4000, 0x4200, 0x4400, 0x4600, 0x4800};
constexpr std::array<const char*, 5> type_names = {"StructProperty", "ByteProperty", "IntProperty", "BoolProperty", "ObjectProperty"};
constexpr std::uint64_t enum_address = 0x4a00;
constexpr std::uint64_t ability = 0x5000, setup_id = 0x5200, ready = 0x5400, object = 0x5600, mech_type = 0x5800;
constexpr std::array<std::uint64_t, 4> nested = {0x6000, 0x6200, 0x6400, 0x6600};
constexpr std::uint64_t part = 0x6800, decoration = 0x6a00;
constexpr std::uint64_t original_module = 0x140000000;

struct Fixture {
    Memory memory;
    std::map<std::uint64_t, MechIdentity> identities;
    Fixture() {
        identities[setup_property] = {"MechSetupStruct", "R_PlayerReplicationInfo", "HawkenGame", "StructProperty"};
        memory.put(setup_property + 0xa8, structs[0]);
        for (std::size_t i = 0; i < structs.size(); ++i) {
            identities[structs[i]] = {names[i], i < 2 ? "R_PlayerReplicationInfo" : "HawkenTypes", "HawkenGame", "ScriptStruct"};
            memory.put(structs[i] + 0x80, std::uint64_t{0});
            // No +78 storage: reading a guessed parent pointer must fail.
        }
        for (std::size_t i = 0; i < types.size(); ++i) {
            identities[types[i]] = {type_names[i], "", "Core", "Class"};
            memory.put(types[i] + 0x128, std::uint32_t{0x8000});
        }
        identities[enum_address] = {"EAbilities", "HawkenTypes", "HawkenGame", "Enum"};
        memory.put(enum_address + 0x70, std::int32_t{36});
        field(ability, 0, "AbilityIndex", 1, 9); memory.put(ability + 0xa8, enum_address);
        field(setup_id, 0, "SetupID", 1, 0); memory.put(setup_id + 0xa8, std::uint64_t{0});
        field(ready, 0, "bReadyForSetup", 3, 2);
        field(object, 0, "Template", 4, 3); // No Template value or property+a8 read.
        field(mech_type, 0, "MechType", 2, 4);
        const std::array<const char*, 4> nested_names = {"MechWeapons", "AllocatedOptimizations", "MechAppearance", "MechInternals"};
        const std::array<std::size_t, 4> destinations = {1, 2, 3, 5};
        for (std::size_t i = 0; i < nested.size(); ++i) {
            field(nested[i], 0, nested_names[i], 0, static_cast<std::int32_t>(10 + i));
            memory.put(nested[i] + 0xa8, structs[destinations[i]]);
        }
        field(part, 3, "Upper", 0, 20); memory.put(part + 0xa8, structs[4]);
        field(decoration, 3, "CockpitDecoration", 1, 21);
        memory.put(decoration + 0x68, std::int32_t{3});
        memory.put(decoration + 0xa8, std::uint64_t{0});
        chain(0, {ability, nested[0], setup_id, nested[1], ready, nested[2], object, nested[3], mech_type});
        chain(3, {part, decoration});
    }
    void field(std::uint64_t address, std::size_t owner, const char* name, std::size_t type, std::int32_t index) {
        identities[address] = {name, names[owner], "HawkenGame", type_names[type]};
        memory.put(address + 0x40, structs[owner]);
        memory.put(address + 0x50, types[type]);
        memory.put(address + 0x60, std::uint64_t{0});
        memory.put(address + 0x3c, index);
        memory.put(address + 0x68, std::int32_t{1});
        memory.put(address + 0x6c, std::int32_t{4});
        memory.put(address + 0x8c, std::int32_t{0});
    }
    void chain(std::size_t owner, std::initializer_list<std::uint64_t> fields) {
        memory.put(structs[owner] + 0x80, fields.size() ? *fields.begin() : std::uint64_t{0});
        for (auto it = fields.begin(); it != fields.end(); ++it)
            memory.put(*it + 0x60, std::next(it) == fields.end() ? std::uint64_t{0} : *std::next(it));
    }
    void bind_ancestors() {
        memory.put(original_module + 0x12bb230 + 0x2a0, original_module + 0x73a70);
        for (const auto address : structs) {
            memory.put(address, original_module + 0x12bb230);
            memory.put(address + 0x78, std::uint64_t{0});
        }
    }
    MechSetupSnapshot read(std::optional<MechPackageRange> range = std::nullopt,
                           std::optional<std::uint64_t> module = std::nullopt) {
        auto identify = [&](std::uint64_t address) {
            const auto found = identities.find(address);
            if (found == identities.end()) throw std::runtime_error("mech_identity");
            return found->second;
        };
        return read_mech_setup(memory, setup_property, identify, range, module);
    }
};
const MechStruct& find_struct(const MechSetupSnapshot& snapshot, const char* name) {
    const auto found = std::find_if(snapshot.structs.begin(), snapshot.structs.end(),
        [&](const auto& item) { return item.identity.name == name; });
    REQUIRE(found != snapshot.structs.end());
    return *found;
}
template<class Action> void expect_error(Action action, const char* code) {
    try { action(); }
    catch (const std::runtime_error& error) { CHECK(std::string(error.what()) == code); return; }
    catch (...) { FAIL_CHECK("expected fixed reader error"); return; }
    FAIL_CHECK("reader accepted rejected metadata");
}
}

TEST_CASE("Mech reader follows own Next order rather than sorting NetIndex") {
    Fixture fixture;
    const auto result = fixture.read();
    REQUIRE(result.structs.size() == 6);
    const auto& root = find_struct(result, "SMechSetupStruct");
    std::vector<std::string> order;
    for (const auto& member : root.members) order.push_back(member.identity.name);
    CHECK(order == std::vector<std::string>{"AbilityIndex", "MechWeapons", "SetupID", "AllocatedOptimizations",
        "bReadyForSetup", "MechAppearance", "Template", "MechInternals", "MechType"});
    CHECK_FALSE(result.ancestor_binding_verified);
    CHECK_FALSE(result.complete_wire_metadata);
    CHECK_FALSE(result.virtual_eligibility_invoked);
}
TEST_CASE("Mech reader retains property types dimensions and enum count without values") {
    Fixture fixture;
    const auto result = fixture.read();
    const auto& root = find_struct(result, "SMechSetupStruct");
    REQUIRE(root.members.size() == 9);
    CHECK(root.members[0].identity.type == "ByteProperty");
    CHECK(root.members[0].enum_count == 36);
    REQUIRE(root.members[0].referenced_type);
    CHECK(root.members[0].referenced_type->name == "EAbilities");
    CHECK(root.members[2].enum_count == std::nullopt);
    CHECK(root.members[4].identity.type == "BoolProperty");
    CHECK(root.members[6].identity.type == "ObjectProperty");
    CHECK(root.members[8].identity.type == "IntProperty");
    CHECK(find_struct(result, "VisualInfo").members[1].array_dim == 3);
    CHECK(std::none_of(fixture.memory.reads.begin(), fixture.memory.reads.end(),
        [](const auto& read) { return read.first == object + 0xa8; }));
}
TEST_CASE("Mech reader with no connection keeps every eligibility unknown") {
    Fixture fixture;
    const auto result = fixture.read();
    for (const auto& item : result.structs) for (const auto& member : item.members) {
        CHECK_FALSE(member.eligible.has_value());
        CHECK_FALSE(member.mapped_index.has_value());
    }
    CHECK_FALSE(result.complete_wire_metadata);
}
TEST_CASE("Mech reader reconstructs zero and last valid mapped coordinate") {
    Fixture fixture;
    fixture.memory.put(ability + 0x3c, std::int32_t{4});
    fixture.memory.put(ready + 0x3c, std::int32_t{-1});
    const auto result = fixture.read(MechPackageRange{100, 5});
    const auto& root = find_struct(result, "SMechSetupStruct");
    CHECK(root.members[0].eligible == true);
    CHECK(root.members[0].mapped_index == 104);
    CHECK(root.members[2].mapped_index == 100);
    CHECK(root.members[4].eligible == false);
    CHECK_FALSE(root.members[4].mapped_index);
    CHECK(root.members[1].eligible == false);
    CHECK_FALSE(root.members[1].mapped_index);
    CHECK_FALSE(result.virtual_eligibility_invoked);
    CHECK_FALSE(result.complete_wire_metadata);
}
TEST_CASE("Mech reader empty negotiated range is known ineligible rather than unknown") {
    Fixture fixture;
    const auto result = fixture.read(MechPackageRange{0, 0});
    for (const auto& item : result.structs) for (const auto& member : item.members)
        CHECK(member.eligible == false);
}
TEST_CASE("Mech reader rejects malformed or oversized negotiated range") {
    Fixture fixture;
    SUBCASE("negative base") { expect_error([&] { fixture.read(MechPackageRange{-1, 1}); }, "mech_invalid"); }
    SUBCASE("negative count") { expect_error([&] { fixture.read(MechPackageRange{0, -1}); }, "mech_invalid"); }
    SUBCASE("coordinate overflow") { expect_error([&] { fixture.read(MechPackageRange{2147483647, 2}); }, "mech_limit"); }
}
TEST_CASE("Mech reader detects a Next cycle") {
    Fixture fixture;
    fixture.memory.put(mech_type + 0x60, ability);
    expect_error([&] { fixture.read(); }, "mech_cycle");
}
TEST_CASE("Mech reader rejects alien struct or wrong outer property identity") {
    Fixture fixture;
    SUBCASE("alien nested struct") { fixture.identities[structs[1]].name = "ItemsInfo"; }
    SUBCASE("root name") { fixture.identities[setup_property].name = "OtherField"; }
    SUBCASE("instanced name") { fixture.identities[structs[1]].instance = 1; }
    SUBCASE("foreign package") { fixture.identities[structs[1]].package = "OtherPackage"; }
    SUBCASE("wrong enum") { fixture.identities[enum_address].name = "OtherEnum"; }
    SUBCASE("wrong enum owner") { fixture.identities[enum_address].owner = "OtherClass"; }
    expect_error([&] { fixture.read(); }, "mech_identity");
}
TEST_CASE("Mech reader rejects property outside its owning struct") {
    Fixture fixture;
    fixture.memory.put(ability + 0x40, structs[1]);
    expect_error([&] { fixture.read(); }, "mech_identity");
}
TEST_CASE("Mech reader skips nonproperty children with no property payload read") {
    Fixture fixture;
    constexpr std::uint64_t child = 0x8000, type = 0x8200;
    fixture.memory.put(structs[0] + 0x80, child);
    fixture.memory.put(child + 0x50, type);
    fixture.memory.put(type + 0x128, std::uint32_t{0x80000});
    fixture.memory.put(child + 0x60, ability);
    CHECK(find_struct(fixture.read(), "SMechSetupStruct").members.size() == 9);
}
TEST_CASE("Mech reader validates dimensions and scalar metadata") {
    Fixture fixture;
    SUBCASE("dimension zero") { fixture.memory.put(ability + 0x68, std::int32_t{0}); }
    SUBCASE("negative element size") { fixture.memory.put(ability + 0x6c, std::int32_t{-1}); }
    SUBCASE("negative offset") { fixture.memory.put(ability + 0x8c, std::int32_t{-1}); }
    SUBCASE("noncanonical NetIndex") { fixture.memory.put(ability + 0x3c, std::int32_t{-2}); }
    expect_error([&] { fixture.read(); }, "mech_invalid");
}
TEST_CASE("Mech reader enforces array and enum policy bounds") {
    Fixture fixture;
    SUBCASE("dimension over cap") { fixture.memory.put(ability + 0x68, std::int32_t{257}); }
    SUBCASE("enum over cap") { fixture.memory.put(enum_address + 0x70, std::int32_t{257}); }
    expect_error([&] { fixture.read(); }, "mech_limit");
}
TEST_CASE("Mech reader propagates unavailable reads and shared byte budget") {
    Fixture fixture;
    SUBCASE("unmapped Children") { fixture.memory.bytes.erase(structs[0] + 0x80); expect_error([&] { fixture.read(); }, "reader_error"); }
    SUBCASE("attempt budget") { fixture.memory.budget = 1; expect_error([&] { fixture.read(); }, "read_limit"); }
}
TEST_CASE("Mech reader retains internal metadata changes for two sample comparison") {
    Fixture fixture;
    const auto first = fixture.read(MechPackageRange{10, 100});
    CHECK(first == fixture.read(MechPackageRange{10, 100}));
    fixture.memory.put(decoration + 0x68, std::int32_t{2});
    CHECK_FALSE(first == fixture.read(MechPackageRange{10, 100}));
}
TEST_CASE("Mech ancestor exact original binding observes six null parents without wire acceptance") {
    Fixture fixture;
    fixture.bind_ancestors();
    const auto result = fixture.read(std::nullopt, original_module);
    CHECK(result.ancestor_binding_verified);
    REQUIRE(result.structs.size() == 6);
    for (const auto& item : result.structs) {
        CHECK(item.ancestor_binding_verified);
        CHECK(item.ancestor_chain_terminated);
        CHECK(item.ancestor_present == false);
    }
    CHECK_FALSE(result.complete_wire_metadata);
    CHECK_FALSE(result.virtual_eligibility_invoked);
}
TEST_CASE("Mech ancestor absent module does not infer binding or read parent") {
    Fixture fixture;
    fixture.bind_ancestors();
    const auto result = fixture.read();
    for (const auto& item : result.structs) {
        CHECK_FALSE(item.ancestor_binding_verified);
        CHECK_FALSE(item.ancestor_chain_terminated);
        CHECK_FALSE(item.ancestor_present.has_value());
    }
    CHECK(std::none_of(fixture.memory.reads.begin(), fixture.memory.reads.end(), [](const auto& read) {
        return std::any_of(structs.begin(), structs.end(), [&](const auto address) { return read.first == address + 0x78; });
    }));
}
TEST_CASE("Mech ancestor nonnull parent remains incomplete without traversing unknown object") {
    Fixture fixture;
    fixture.bind_ancestors();
    constexpr std::uint64_t unknown_parent = 0xb000;
    fixture.memory.put(structs[0] + 0x78, unknown_parent); // No memory/identity for this object.
    const auto result = fixture.read(std::nullopt, original_module);
    const auto& root = find_struct(result, "SMechSetupStruct");
    CHECK(root.ancestor_binding_verified);
    CHECK_FALSE(root.ancestor_chain_terminated);
    CHECK(root.ancestor_present == true);
    CHECK_FALSE(result.ancestor_binding_verified);
    CHECK_FALSE(result.complete_wire_metadata);
    CHECK(std::none_of(fixture.memory.reads.begin(), fixture.memory.reads.end(),
        [](const auto& read) { return read.first >= unknown_parent && read.first < unknown_parent + 0x200; }));
}
TEST_CASE("Mech ancestor foreign vtable or getter rejects before reading parent") {
    Fixture fixture;
    fixture.bind_ancestors();
    SUBCASE("foreign vtable") { fixture.memory.put(structs[0], original_module + 0x12bb240); }
    SUBCASE("foreign getter") { fixture.memory.put(original_module + 0x12bb230 + 0x2a0, original_module + 0x73a80); }
    expect_error([&] { fixture.read(std::nullopt, original_module); }, "mech_identity");
    CHECK(std::none_of(fixture.memory.reads.begin(), fixture.memory.reads.end(),
        [](const auto& read) { return read.first == structs[0] + 0x78; }));
}
