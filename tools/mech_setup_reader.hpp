#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hawken::probe {
struct MechIdentity {
    std::string name;
    std::string owner;
    std::string package;
    std::string type;
    std::uint32_t instance = 0;
    bool operator==(const MechIdentity&) const = default;
};
// Supplied only after the caller verifies an actual primary-map HawkenGame
// descriptor. These are negotiated network-object counts, not export counts.
struct MechPackageRange {
    std::int32_t base;
    std::int32_t count;
};
struct MechMember {
    MechIdentity identity;
    std::int32_t net_index;
    std::int32_t array_dim;
    std::int32_t element_size;
    std::int32_t offset;
    std::optional<MechIdentity> referenced_type;
    std::optional<std::int32_t> enum_count;
    std::optional<bool> eligible;
    std::optional<std::uint32_t> mapped_index;
    bool operator==(const MechMember&) const = default;
};
struct MechStruct {
    MechIdentity identity;
    std::vector<MechMember> members;
    bool ancestor_binding_verified = false;
    bool ancestor_chain_terminated = false;
    std::optional<bool> ancestor_present;
    bool operator==(const MechStruct&) const = default;
};
struct MechSetupSnapshot {
    // Global summary requires all six exact bindings AND null parents.
    bool ancestor_binding_verified = false;
    bool complete_wire_metadata = false;
    bool virtual_eligibility_invoked = false;
    std::vector<MechStruct> structs;
    // Internal equality trace only; addresses must never enter public JSON.
    std::vector<std::uint64_t> trace;
    bool operator==(const MechSetupSnapshot&) const = default;
};
// Original f9758: property+a8, own Children+80/Next+60, property type+50,
// CastFlags+128 mask8000, ArrayDim+68/ElementSize+6c/Offset+8c.
// Read six named original structs only. Identify supplies bounded, verified
// FName/Outer/class identities through the same Reader; no process invocation.
// Missing connection leaves eligibility unknown. A supplied range reconstructs
// the 7ace0 predicate, not an executed Map+298 call or accepted field ID.
// Optional module base belongs to the hash/path-verified caller. The positive
// ScriptStruct binding is vtable+12bb230, slot2a0 -> getter+73a70 reading+78.
// Verify both before observing the parent; do not traverse nonnull ancestors.
// Even a null parent never makes this complete/accepted wire metadata.
// Policy limits: 256 nodes per struct, 512 total, ArrayDim<=256, enum count<=256,
// nonnegative offset/size and canonical NetIndex>=-1. Shared byte budget belongs
// to Reader (the production probe charges both snapshots against 1MiB).
template<class Reader, class Identify>
MechSetupSnapshot read_mech_setup(Reader& reader, std::uint64_t property, Identify& identify,
                                 std::optional<MechPackageRange> range = std::nullopt,
                                 std::optional<std::uint64_t> module_base = std::nullopt) {
    const auto fail = [](const char* code) { throw std::runtime_error(code); };
    if (module_base && (*module_base == 0 ||
        *module_base > std::numeric_limits<std::uint64_t>::max() - (0x12bb230 + 0x2a0 + 7))) fail("mech_invalid");
    if (range) {
        if (range->base < 0 || range->count < 0) fail("mech_invalid");
        if (static_cast<std::uint64_t>(range->base) + static_cast<std::uint64_t>(range->count) > 0x80000000ULL)
            fail("mech_range_limit");
    }
    MechSetupSnapshot out;
    const auto read = [&]<class T>(std::uint64_t address, std::uint64_t offset) {
        const auto value = reader.template read<T>(address, offset);
        out.trace.insert(out.trace.end(), {address, offset, static_cast<std::uint64_t>(value)});
        return value;
    };
    const auto original = [](const MechIdentity& id) {
        return id.instance == 0 && id.package == "HawkenGame";
    };
    const auto root_identity = identify(property);
    if (!original(root_identity) || root_identity.name != "MechSetupStruct" ||
        root_identity.owner != "R_PlayerReplicationInfo" || root_identity.type != "StructProperty") fail("mech_identity");
    constexpr std::array<const char*, 6> expected_struct_names = {"SMechSetupStruct", "WeaponInfo", "SAllocatedOptimization",
        "VisualInfo", "PartVisualInfo", "InternalsInfo"};
    std::vector<std::uint64_t> pending{read.template operator()<std::uint64_t>(property, 0xa8)};
    std::vector<std::uint64_t> visited, nodes;
    for (std::size_t next = 0; next < pending.size(); ++next) {
        const auto address = pending[next];
        if (!address) fail("mech_identity");
        if (std::find(visited.begin(), visited.end(), address) != visited.end()) continue;
        const auto id = identify(address);
        const auto expected = std::find_if(expected_struct_names.begin(), expected_struct_names.end(), [&](const char* name) { return id.name == name; });
        if (!original(id) || id.type != "ScriptStruct" || expected == expected_struct_names.end()) fail("mech_identity");
        const auto index = static_cast<std::size_t>(expected - expected_struct_names.begin());
        if (id.owner != (index < 2 ? "R_PlayerReplicationInfo" : "HawkenTypes") ||
            (next == 0 && index != 0)) fail("mech_identity");
        for (const auto& prior : out.structs) if (prior.identity.name == id.name) fail("mech_identity");
        if (visited.size() == expected_struct_names.size()) fail("mech_struct_limit");
        visited.push_back(address);
        MechStruct item{id, {}};
        if (module_base) {
            const auto vtable = read.template operator()<std::uint64_t>(address, 0);
            if (vtable != *module_base + 0x12bb230) fail("mech_identity");
            const auto getter = read.template operator()<std::uint64_t>(vtable, 0x2a0);
            if (getter != *module_base + 0x73a70) fail("mech_identity");
            const auto parent = read.template operator()<std::uint64_t>(address, 0x78);
            item.ancestor_binding_verified = true;
            item.ancestor_present = parent != 0;
            item.ancestor_chain_terminated = parent == 0;
            // Read metadata only: never invoke the getter or follow this parent.
        }
        auto child = read.template operator()<std::uint64_t>(address, 0x80);
        std::size_t own = 0;
        while (child) {
            if (std::find(nodes.begin(), nodes.end(), child) != nodes.end()) fail("mech_cycle");
            if (++own > 256) fail("mech_children_limit");
            if (nodes.size() == 512) fail("mech_total_children_limit");
            nodes.push_back(child);
            const auto type = read.template operator()<std::uint64_t>(child, 0x50);
            if (!type) fail("mech_identity");
            const auto cast = read.template operator()<std::uint32_t>(type, 0x128);
            const auto following = read.template operator()<std::uint64_t>(child, 0x60);
            if ((cast & 0x8000) != 0) {
                const auto member_id = identify(child);
                if (!original(member_id) || member_id.owner != id.name ||
                    read.template operator()<std::uint64_t>(child, 0x40) != address) fail("mech_identity");
                const auto type_id = identify(type);
                if (type_id.instance != 0 || type_id.package != "Core" || type_id.type != "Class" ||
                    type_id.name != member_id.type) fail("mech_identity");
                MechMember member{member_id,
                    read.template operator()<std::int32_t>(child, 0x3c),
                    read.template operator()<std::int32_t>(child, 0x68),
                    read.template operator()<std::int32_t>(child, 0x6c),
                    read.template operator()<std::int32_t>(child, 0x8c), {}, {}, {}, {}};
                if (member.net_index < -1 || member.array_dim < 1 || member.element_size < 0 || member.offset < 0)
                    fail("mech_invalid");
                if (member.array_dim > 256) fail("mech_array_dim_limit");
                if (member_id.type == "StructProperty") {
                    const auto target = read.template operator()<std::uint64_t>(child, 0xa8);
                    if (!target) fail("mech_struct_target_null");
                    if (pending.size() >= 512) fail("mech_pending_limit");
                    member.referenced_type = identify(target);
                    pending.push_back(target);
                } else if (member_id.type == "ByteProperty") {
                    const auto target = read.template operator()<std::uint64_t>(child, 0xa8);
                    if (target) {
                        member.referenced_type = identify(target);
                        if (!original(*member.referenced_type) || member.referenced_type->type != "Enum" ||
                            member.referenced_type->name != "EAbilities" || member.referenced_type->owner != "HawkenTypes" ||
                            member_id.name != "AbilityIndex" || id.name != "SMechSetupStruct") fail("mech_identity");
                        member.enum_count = read.template operator()<std::int32_t>(target, 0x70);
                        if (*member.enum_count < 0) fail("mech_invalid");
                        if (*member.enum_count > 256) fail("mech_enum_limit");
                    }
                } else if (member_id.type != "IntProperty" && member_id.type != "BoolProperty" &&
                           member_id.type != "ObjectProperty") fail("mech_identity");
                if (range) {
                    member.eligible = member.net_index >= 0 && member.net_index < range->count;
                    if (*member.eligible) member.mapped_index = static_cast<std::uint32_t>(range->base) +
                        static_cast<std::uint32_t>(member.net_index);
                }
                item.members.push_back(std::move(member));
            }
            child = following;
        }
        out.structs.push_back(std::move(item));
    }
    if (out.structs.size() != expected_struct_names.size()) fail("mech_identity");
    out.ancestor_binding_verified = std::all_of(out.structs.begin(), out.structs.end(),
        [](const auto& item) { return item.ancestor_binding_verified && item.ancestor_chain_terminated; });
    return out;
}
}
