#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace hawken::probe {

// Identify supplies fixed metadata names/package and FName instance, never the
// arbitrary name of a live actor. Reader supplies checked, budgeted read<T>.
struct LiveIdentity {
    std::string name;
    std::string package;
    std::uint32_t instance = 0;
};

struct LiveBoolSnapshot {
    bool value = false;
    std::int32_t metadata_offset = 0;
    std::uint32_t bit_mask = 0;
    bool operator==(const LiveBoolSnapshot&) const = default;
};

enum class LiveScriptStateKind { no_frame, no_node, uninitialized, named };
struct LiveScriptStateSnapshot {
    LiveScriptStateKind kind = LiveScriptStateKind::no_frame;
    std::string name, package;
    bool operator==(const LiveScriptStateSnapshot&) const = default;
};
struct LiveFunctionDispatchSnapshot {
    std::uint32_t flags = 0;
    std::uint16_t native_token = 0;
    bool target_in_original_text = false;
    std::optional<std::uint32_t> target_rva;
    bool operator==(const LiveFunctionDispatchSnapshot&) const = default;
};

struct LiveInventorySnapshot {
    bool pawn_invmanager_present = false;
    bool pawn_dual_weapon_present = false;
    bool pawn_weapon_present = false;
    bool pawn_weapon_is_dual = false;
    bool manager_instigator_is_pawn = false;
    bool manager_chain_is_dual = false;
    bool dual_instigator_is_pawn = false;
    bool dual_invmanager_matches = false;
    std::optional<std::uint8_t> manager_role;
    std::optional<std::uint8_t> pawn_role, controller_role;
    std::optional<std::uint8_t> pawn_physics;
    std::optional<std::uint8_t> dual_role;
    std::optional<LiveBoolSnapshot> dual_server_can_fire, dual_shutdown, dual_client_shutdown;
    std::optional<LiveBoolSnapshot> pawn_jump_capable;
    std::optional<LiveBoolSnapshot> pawn_can_move;
    std::optional<LiveBoolSnapshot> pawn_no_weapon_firing;
    std::optional<float> pawn_jump_z, pawn_thruster_energy, pawn_thruster_jump_energy;
    std::optional<std::uint8_t> pawn_move_state;
    std::optional<LiveScriptStateSnapshot> pawn_script_state, manager_script_state, dual_script_state;
    std::optional<LiveFunctionDispatchSnapshot> pawn_jump_dispatch, dual_can_fire_dispatch;
    bool operator==(const LiveInventorySnapshot&) const = default;
};

// Optional reflected observations only, not a network possession or timestamp
// acceptance decision. No identity value or address is exposed by this DTO.
struct LiveDualFireTimingSnapshot {
    float client_can_fire_timestamp = 0.0F;
    float server_can_fire_timestamp = 0.0F;
    std::int32_t client_can_fire_timestamp_offset = 0;
    std::int32_t server_can_fire_timestamp_offset = 0;
    std::int32_t instigator_offset = 0;
    bool operator==(const LiveDualFireTimingSnapshot&) const = default;
};

struct LiveFireTimingSnapshot {
    float controller_current_timestamp = 0.0F;
    std::int32_t controller_current_timestamp_offset = 0;
    std::int32_t pawn_controller_offset = 0;
    std::int32_t acknowledged_pawn_offset = 0;
    bool pawn_controller_present = false;
    bool pawn_controller_matches = false;
    bool acknowledged_pawn_present = false;
    bool acknowledged_pawn_matches = false;
    std::optional<LiveDualFireTimingSnapshot> dual_weapon;
    bool operator==(const LiveFireTimingSnapshot&) const = default;
};

struct LivePlayerSnapshot {
    std::int32_t local_player_count = 0;
    bool local_player_present = false;
    bool controller_present = false;
    bool pawn_present = false;
    bool pri_present = false;
    bool controller_player_matches = false;
    std::optional<std::int32_t> pawn_offset;
    std::optional<std::int32_t> pri_offset;
    // Internal equality trace only; never serialize addresses/raw FNames.
    std::vector<std::uint64_t> trace;
    std::optional<LiveInventorySnapshot> inventory;
    std::optional<LiveFireTimingSnapshot> fire_timing;
    bool operator==(const LivePlayerSnapshot&) const = default;
};

// Select the first Engine.GamePlayers entry, not every player/actor in a world.
// Reader policy: count/cap<=4, class chain<=16, Offset<=65536. Own Children
// caps match the nine shipped metadata owners below. Retained route caps are
// default977/inventory3729; sample-local owner reuse reduces actual visits.
// These are observer policies, not UE3 or allocation-size limits.
// Engine global1cfb860 -> array700/count708/cap70c; typed LocalPlayer outer must
// equal Engine. Exact Player.Actor ObjectProperty Offset8c must equal68.
// Controller class must derive the validated R_PlayerController global1d3fa68;
// reflected PC.Player must point back to that LocalPlayer. Pawn/PRI offsets
// come from exact Engine.Controller properties, not NetIndex/cache/UC order.
// UObject flags+10 mask600 must be clear, and instances must differ from CDO;
// this reuses the existing probe/connection-object-identities instance policy.
// Only null/reference/type state is observed. No current-World membership,
// actor channel, possession, admission or universal absence is established.
// Caller must compare two complete snapshots (including trace) before output.
// inspect_fire_timing requires inspect_inventory. It observes finite reflected
// timestamps and typed reference relationships, not accepted network possession.
// Fixed errors: live_invalid, live_engine_absent, live_array_invalid,
// live_limit, live_identity, live_cycle, live_property_missing,
// live_property_mismatch, live_backlink_mismatch; Reader errors propagate.
template<class Reader, class Identify>
LivePlayerSnapshot read_live_player(Reader& reader, std::uint64_t module, Identify& source_identify,
                                   bool inspect_inventory = false, bool inspect_fire_timing = false) {
    const auto fail = [](const char* code) { throw std::runtime_error(code); };
    if (inspect_fire_timing && !inspect_inventory) fail("live_invalid");
    const auto checked = [&](std::uint64_t base, std::uint64_t offset, std::size_t size) {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (!base || offset > maximum - base || size - 1 > maximum - (base + offset))
            fail("live_invalid");
    };
    LivePlayerSnapshot out;
    // Metadata caches belong to this sample only. Live instance/reference reads
    // remain checked and traced; the next call repeats every metadata validation.
    struct MetadataIdentity { std::uint64_t address; LiveIdentity identity; };
    std::vector<MetadataIdentity> metadata_identities;
    const auto identify = [&](std::uint64_t address) {
        for (const auto& cached : metadata_identities)
            if (cached.address == address) return cached.identity;
        const auto identity = source_identify(address);
        metadata_identities.push_back({address, identity});
        return identity;
    };
    const auto read = [&]<class T>(std::uint64_t base, std::uint64_t offset = 0) {
        checked(base, offset, sizeof(T));
        const auto value = reader.template read<T>(base, offset);
        out.trace.insert(out.trace.end(), {base, offset, static_cast<std::uint64_t>(value)});
        return value;
    };
    const auto engine = read.template operator()<std::uint64_t>(module, 0x1cfb860);
    if (!engine) fail("live_engine_absent");
    const auto kind = read.template operator()<std::uint64_t>(module, 0x1bda990);
    if (!kind) fail("live_identity");
    const auto kind_id = identify(kind);
    if (kind_id.name != "Class" || kind_id.package != "Core" || kind_id.instance != 0)
        fail("live_identity");
    struct ClassNode { std::uint64_t address; LiveIdentity identity; };
    struct MetadataChain { std::uint64_t root; std::vector<ClassNode> nodes; };
    std::vector<MetadataChain> metadata_chains;
    const auto chain = [&](std::uint64_t first) {
        for (const auto& cached : metadata_chains)
            if (cached.root == first) return cached.nodes;
        std::vector<ClassNode> nodes;
        auto current = first;
        while (current) {
            if (std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) { return node.address == current; }))
                fail("live_cycle");
            if (nodes.size() == 16) fail("live_limit");
            if (read.template operator()<std::uint64_t>(current, 0x50) != kind) fail("live_identity");
            const auto id = identify(current);
            if (id.instance != 0) fail("live_identity");
            nodes.push_back({current, id});
            current = read.template operator()<std::uint64_t>(current, 0x78);
        }
        if (nodes.empty() || nodes.back().identity.name != "Object" || nodes.back().identity.package != "Core")
            fail("live_identity");
        metadata_chains.push_back({first, nodes});
        return nodes;
    };
    const auto named = [&](const auto& nodes, std::string_view name, std::string_view package) {
        std::uint64_t found = 0;
        for (const auto& node : nodes) {
            if (node.identity.name == name && node.identity.package == package) {
                if (found) fail("live_identity");
                found = node.address;
            }
        }
        if (!found) fail("live_identity");
        return found;
    };
    const auto contains = [](const auto& nodes, std::uint64_t expected) {
        return std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) { return node.address == expected; });
    };
    const auto instance = [&](std::uint64_t object) {
        if (!object || (read.template operator()<std::uint64_t>(object, 0x10) & 0x600) != 0)
            fail("live_identity");
        const auto type = read.template operator()<std::uint64_t>(object, 0x50);
        if (!type) fail("live_identity");
        const auto nodes = chain(type);
        if (read.template operator()<std::uint64_t>(type, 0x1d4) == object) fail("live_identity");
        return nodes;
    };
    static_cast<void>(named(instance(engine), "Engine", "Engine"));
    const auto array = read.template operator()<std::uint64_t>(engine, 0x700);
    const auto count = read.template operator()<std::int32_t>(engine, 0x708);
    const auto capacity = read.template operator()<std::int32_t>(engine, 0x70c);
    if (count < 0 || capacity < count || capacity < 0) fail("live_array_invalid");
    if (count > 4 || capacity > 4) fail("live_limit");
    if (count && !array) fail("live_array_invalid");
    out.local_player_count = count;
    if (count == 0) return out;
    const auto local = read.template operator()<std::uint64_t>(array);
    const auto local_chain = instance(local);
    static_cast<void>(named(local_chain, "LocalPlayer", "Engine"));
    const auto player_type = named(local_chain, "Player", "Engine");
    if (read.template operator()<std::uint64_t>(local, 0x40) != engine) fail("live_identity");
    out.local_player_present = true;

    struct Property { std::int32_t offset = 0; std::uint64_t expected = 0; std::uint32_t mask = 0; };
    struct MetadataField { std::uint64_t address; LiveIdentity identity; };
    struct MetadataOwner { std::uint64_t address; std::vector<MetadataField> fields; };
    std::vector<MetadataOwner> metadata_owners;
    std::size_t total_children = 0;
    const auto owner_fields = [&](std::uint64_t owner) -> const std::vector<MetadataField>& {
        for (const auto& cached : metadata_owners)
            if (cached.address == owner) return cached.fields;
        const auto inv_owner_id = identify(owner);
        std::size_t inv_owner_limit = 0;
        if (inv_owner_id.instance == 0) {
            if (inv_owner_id.package == "Engine") {
                if (inv_owner_id.name == "Player") inv_owner_limit = 10;
                else if (inv_owner_id.name == "PlayerController") inv_owner_limit = 575;
                else if (inv_owner_id.name == "Controller") inv_owner_limit = 196;
                else if (inv_owner_id.name == "Pawn") inv_owner_limit = 423;
                else if (inv_owner_id.name == "Actor") inv_owner_limit = 449;
                else if (inv_owner_id.name == "InventoryManager") inv_owner_limit = 36;
                else if (inv_owner_id.name == "Inventory") inv_owner_limit = 27;
            } else if (inv_owner_id.package == "HawkenGame") {
                if (inv_owner_id.name == "R_PawnNative") inv_owner_limit = 496;
                else if (inv_owner_id.name == "R_Weap_DualWield") inv_owner_limit = 59;
            }
        }
        if (!inv_owner_limit) fail("live_identity");
        // Preserve prior route caps; caching each owner does not raise any limit.
        const std::size_t inv_total_limit = inspect_inventory ? 3729 : 977;
        std::vector<MetadataField> fields;
        auto child = read.template operator()<std::uint64_t>(owner, 0x80);
        while (child) {
            if (std::any_of(fields.begin(), fields.end(), [&](const auto& item) { return item.address == child; }))
                fail("live_cycle");
            if (fields.size() == inv_owner_limit || total_children == inv_total_limit) fail("live_limit");
            ++total_children;
            fields.push_back({child, identify(child)});
            child = read.template operator()<std::uint64_t>(child, 0x60);
        }
        metadata_owners.push_back({owner, std::move(fields)});
        return metadata_owners.back().fields;
    };
    struct MetadataProperty {
        std::uint64_t owner;
        std::string field, expected_name, field_package, expected_package;
        bool byte_kind, bool_kind, float_kind;
        Property value;
    };
    std::vector<MetadataProperty> metadata_properties;
    const auto property = [&](std::uint64_t owner, std::string_view field, std::string_view expected_name,
                              std::string_view field_package = "Engine",
                              std::string_view expected_package = "Engine", bool byte_kind = false,
                              bool bool_kind = false, bool float_kind = false) {
        for (const auto& cached : metadata_properties) if (cached.owner == owner && cached.field == field) {
            if (cached.expected_name != expected_name || cached.field_package != field_package ||
                cached.expected_package != expected_package || cached.byte_kind != byte_kind ||
                cached.bool_kind != bool_kind || cached.float_kind != float_kind)
                fail("live_property_mismatch");
            return cached.value;
        }
        Property result;
        bool found = false;
        for (const auto& metadata_field : owner_fields(owner)) {
            const auto child = metadata_field.address;
            const auto& id = metadata_field.identity;
            if (id.name == field) {
                if (found || id.instance != 0 || id.package != field_package ||
                    read.template operator()<std::uint64_t>(child, 0x40) != owner)
                    fail("live_property_mismatch");
                const auto type = read.template operator()<std::uint64_t>(child, 0x50);
                if (!type || read.template operator()<std::uint64_t>(type, 0x50) != kind)
                    fail("live_property_mismatch");
                const auto type_id = identify(type);
                if (type_id.name != (bool_kind ? "BoolProperty" : byte_kind ? "ByteProperty" : float_kind ? "FloatProperty" : "ObjectProperty") ||
                    type_id.package != "Core" || type_id.instance != 0 ||
                    (read.template operator()<std::uint32_t>(type, 0x128) & (bool_kind ? 0x20000U : byte_kind ? 0x8000U : float_kind ? 0x100U : 0x10000U)) == 0 ||
                    read.template operator()<std::int32_t>(child, 0x68) != 1 ||
                    read.template operator()<std::int32_t>(child, 0x6c) != (bool_kind || float_kind ? 4 : byte_kind ? 1 : 8))
                    fail("live_property_mismatch");
                result.offset = read.template operator()<std::int32_t>(child, 0x8c);
                if (result.offset < 0) fail("live_property_mismatch");
                if (result.offset > 65536) fail("live_limit");
                if (bool_kind) {
                    result.mask = read.template operator()<std::uint32_t>(child, 0xa8);
                    if (!result.mask || (result.mask & (result.mask - 1)) != 0)
                        fail("live_property_mismatch");
                } else if (!byte_kind && !float_kind) {
                    result.expected = read.template operator()<std::uint64_t>(child, 0xa8);
                    if (!result.expected) fail("live_property_mismatch");
                    const auto expected_chain = chain(result.expected);
                    if (expected_chain.front().identity.name != expected_name ||
                        expected_chain.front().identity.package != expected_package) fail("live_property_mismatch");
                }
                found = true;
            }
        }
        if (!found) fail("live_property_missing");
        metadata_properties.push_back({owner, std::string(field), std::string(expected_name),
            std::string(field_package), std::string(expected_package), byte_kind, bool_kind, float_kind, result});
        return result;
    };
    const auto script_state = [&](std::uint64_t live_script_actor, const auto& live_script_chain) {
        LiveScriptStateSnapshot live_script_out;
        const auto live_script_frame = read.template operator()<std::uint64_t>(live_script_actor, 0x20);
        if (!live_script_frame) return live_script_out;
        live_script_out.kind = LiveScriptStateKind::no_node;
        const auto live_script_node = read.template operator()<std::uint64_t>(live_script_frame, 0x44);
        if (!live_script_node) return live_script_out;
        const auto live_script_type = read.template operator()<std::uint64_t>(live_script_node, 0x50);
        if (!live_script_type || read.template operator()<std::uint64_t>(live_script_type, 0x50) != kind)
            fail("live_identity");
        auto live_script_owner = live_script_node;
        if (live_script_type != kind) {
            const auto live_script_type_id = identify(live_script_type);
            if (live_script_type_id.name != "State" || live_script_type_id.package != "Core" ||
                live_script_type_id.instance != 0)
                fail("live_identity");
            live_script_owner = read.template operator()<std::uint64_t>(live_script_node, 0x40);
        }
        const auto live_script_owner_node = std::find_if(live_script_chain.begin(), live_script_chain.end(),
            [&](const auto& live_script_item) { return live_script_item.address == live_script_owner; });
        if (live_script_owner_node == live_script_chain.end()) fail("live_identity");
        // GetStateName's sentinel precedes Node's FName. Never identify an
        // uninitialized Node, even though its metadata type/owner must be valid.
        if (read.template operator()<std::int32_t>(live_script_node, 0x38) == -1) {
            live_script_out.kind = LiveScriptStateKind::uninitialized;
            return live_script_out;
        }
        const auto live_script_id = identify(live_script_node);
        if (live_script_id.instance != 0 ||
            live_script_id.package != live_script_owner_node->identity.package)
            fail("live_identity");
        live_script_out.kind = LiveScriptStateKind::named;
        live_script_out.name = live_script_id.name;
        live_script_out.package = live_script_id.package;
        return live_script_out;
    };
    const auto function_dispatch = [&](std::uint64_t live_function_owner,
                                       std::string_view live_function_name,
                                       std::string_view live_function_package) {
        LiveFunctionDispatchSnapshot live_function_out;
        bool live_function_found = false;
        for (const auto& live_function_field : owner_fields(live_function_owner)) {
            const auto& live_function_id = live_function_field.identity;
            if (live_function_id.name != live_function_name) continue;
            const auto live_function_address = live_function_field.address;
            if (live_function_found || live_function_id.instance != 0 ||
                live_function_id.package != live_function_package ||
                read.template operator()<std::uint64_t>(live_function_address, 0x40) != live_function_owner)
                fail("live_property_mismatch");
            const auto live_function_type = read.template operator()<std::uint64_t>(live_function_address, 0x50);
            if (!live_function_type || read.template operator()<std::uint64_t>(live_function_type, 0x50) != kind)
                fail("live_property_mismatch");
            const auto live_function_type_id = identify(live_function_type);
            if (live_function_type_id.name != "Function" || live_function_type_id.package != "Core" ||
                live_function_type_id.instance != 0)
                fail("live_property_mismatch");
            live_function_out.flags = read.template operator()<std::uint32_t>(live_function_address, 0xd0);
            live_function_out.native_token = read.template operator()<std::uint16_t>(live_function_address, 0xd4);
            const auto live_function_target = read.template operator()<std::uint64_t>(live_function_address, 0xf0);
            if (live_function_target >= module) {
                const auto live_function_rva = live_function_target - module;
                if (live_function_rva >= 0x1000 && live_function_rva < 0x128df79) {
                    live_function_out.target_in_original_text = true;
                    live_function_out.target_rva = static_cast<std::uint32_t>(live_function_rva);
                }
            }
            live_function_found = true;
        }
        if (!live_function_found) fail("live_property_missing");
        return live_function_out;
    };
    const auto actor = property(player_type, "Actor", "PlayerController");
    if (actor.offset != 0x68) fail("live_property_mismatch");
    const auto controller = read.template operator()<std::uint64_t>(local, static_cast<std::uint64_t>(actor.offset));
    if (!controller) return out;
    const auto controller_chain = instance(controller);
    const auto controller_root = read.template operator()<std::uint64_t>(module, 0x1d3fa68);
    if (!controller_root) fail("live_identity");
    const auto root_chain = chain(controller_root);
    if (root_chain.front().identity.name != "R_PlayerController" ||
        root_chain.front().identity.package != "HawkenGame" || !contains(controller_chain, controller_root) ||
        !contains(controller_chain, actor.expected)) fail("live_identity");
    const auto pc_type = named(controller_chain, "PlayerController", "Engine");
    const auto base_controller = named(controller_chain, "Controller", "Engine");
    const auto backlink = property(pc_type, "Player", "Player");
    if (backlink.expected != player_type) fail("live_property_mismatch");
    if (read.template operator()<std::uint64_t>(controller, static_cast<std::uint64_t>(backlink.offset)) != local)
        fail("live_backlink_mismatch");
    out.controller_present = true;
    out.controller_player_matches = true;
    const auto pawn = property(base_controller, "Pawn", "Pawn");
    const auto pri = property(base_controller, "PlayerReplicationInfo", "PlayerReplicationInfo");
    out.pawn_offset = pawn.offset;
    out.pri_offset = pri.offset;
    const auto pawn_value = read.template operator()<std::uint64_t>(controller, static_cast<std::uint64_t>(pawn.offset));
    const auto pri_value = read.template operator()<std::uint64_t>(controller, static_cast<std::uint64_t>(pri.offset));
    if (pawn_value) {
        if (!contains(instance(pawn_value), pawn.expected)) fail("live_identity");
        out.pawn_present = true;
    }
    if (pri_value) {
        if (!contains(instance(pri_value), pri.expected)) fail("live_identity");
        out.pri_present = true;
    }
    if (inspect_inventory && pawn_value) {
        const auto inv_pawn_chain = instance(pawn_value);
        const auto inv_pawn_type = named(inv_pawn_chain, "Pawn", "Engine");
        const auto inv_native_type = named(inv_pawn_chain, "R_PawnNative", "HawkenGame");
        if (inspect_fire_timing) {
            const auto fire_stamp_metadata = property(pc_type, "CurrentTimeStamp", {},
                "Engine", "Engine", false, false, true);
            const auto fire_controller_metadata = property(inv_pawn_type, "Controller", "Controller");
            const auto fire_acknowledged_metadata = property(pc_type, "AcknowledgedPawn", "Pawn");
            const auto fire_stamp_bits = read.template operator()<std::uint32_t>(controller,
                static_cast<std::uint64_t>(fire_stamp_metadata.offset));
            const auto fire_stamp_value = std::bit_cast<float>(fire_stamp_bits);
            if (!std::isfinite(fire_stamp_value)) fail("live_invalid");
            const auto fire_controller_value = read.template operator()<std::uint64_t>(pawn_value,
                static_cast<std::uint64_t>(fire_controller_metadata.offset));
            const auto fire_acknowledged_value = read.template operator()<std::uint64_t>(controller,
                static_cast<std::uint64_t>(fire_acknowledged_metadata.offset));
            if (fire_controller_value &&
                !contains(instance(fire_controller_value), fire_controller_metadata.expected))
                fail("live_identity");
            if (fire_acknowledged_value &&
                !contains(instance(fire_acknowledged_value), fire_acknowledged_metadata.expected))
                fail("live_identity");
            LiveFireTimingSnapshot fire_timing_out;
            fire_timing_out.controller_current_timestamp = fire_stamp_value;
            fire_timing_out.controller_current_timestamp_offset = fire_stamp_metadata.offset;
            fire_timing_out.pawn_controller_offset = fire_controller_metadata.offset;
            fire_timing_out.acknowledged_pawn_offset = fire_acknowledged_metadata.offset;
            fire_timing_out.pawn_controller_present = fire_controller_value != 0;
            fire_timing_out.pawn_controller_matches = fire_controller_value && fire_controller_value == controller;
            fire_timing_out.acknowledged_pawn_present = fire_acknowledged_value != 0;
            fire_timing_out.acknowledged_pawn_matches = fire_acknowledged_value && fire_acknowledged_value == pawn_value;
            out.fire_timing = fire_timing_out;
        }
        const auto inv_manager_property = property(inv_pawn_type, "InvManager", "InventoryManager");
        const auto inv_weapon_property = property(inv_pawn_type, "Weapon", "Weapon");
        const auto inv_dual_property = property(inv_native_type, "DualWeapon", "R_Weap_DualWield",
                                                "HawkenGame", "HawkenGame");
        const auto inv_manager = read.template operator()<std::uint64_t>(pawn_value,
            static_cast<std::uint64_t>(inv_manager_property.offset));
        const auto inv_weapon = read.template operator()<std::uint64_t>(pawn_value,
            static_cast<std::uint64_t>(inv_weapon_property.offset));
        const auto inv_dual = read.template operator()<std::uint64_t>(pawn_value,
            static_cast<std::uint64_t>(inv_dual_property.offset));
        LiveInventorySnapshot inv_out;
        inv_out.pawn_script_state = script_state(pawn_value, inv_pawn_chain);
        inv_out.pawn_jump_dispatch = function_dispatch(inv_native_type,
            "CheckJumpOrHealOrTaunt", "HawkenGame");
        const auto inv_pawn_actor_type = named(inv_pawn_chain, "Actor", "Engine");
        const auto inv_physics_property = property(inv_pawn_actor_type, "Physics", {}, "Engine", "Engine", true);
        inv_out.pawn_physics = read.template operator()<std::uint8_t>(pawn_value,
            static_cast<std::uint64_t>(inv_physics_property.offset));
        const auto inv_pawn_role_property = property(inv_pawn_actor_type, "Role", {}, "Engine", "Engine", true);
        const auto inv_controller_role_property = property(named(controller_chain, "Actor", "Engine"),
            "Role", {}, "Engine", "Engine", true);
        inv_out.pawn_role = read.template operator()<std::uint8_t>(pawn_value,
            static_cast<std::uint64_t>(inv_pawn_role_property.offset));
        inv_out.controller_role = read.template operator()<std::uint8_t>(controller,
            static_cast<std::uint64_t>(inv_controller_role_property.offset));
        const auto inv_jump_property = property(inv_pawn_type, "bJumpCapable", {}, "Engine", "Engine", false, true);
        const auto inv_jump_bits = read.template operator()<std::uint32_t>(pawn_value,
            static_cast<std::uint64_t>(inv_jump_property.offset));
        inv_out.pawn_jump_capable = LiveBoolSnapshot{(inv_jump_bits & inv_jump_property.mask) != 0,
            inv_jump_property.offset, inv_jump_property.mask};
        const auto inv_can_move_property = property(inv_native_type, "bcanMove", {}, "HawkenGame", "Engine", false, true);
        const auto inv_can_move_bits = read.template operator()<std::uint32_t>(pawn_value,
            static_cast<std::uint64_t>(inv_can_move_property.offset));
        inv_out.pawn_can_move = LiveBoolSnapshot{(inv_can_move_bits & inv_can_move_property.mask) != 0,
            inv_can_move_property.offset, inv_can_move_property.mask};
        const auto inv_no_fire_property = property(inv_pawn_type, "bNoWeaponFiring", {}, "Engine", "Engine", false, true);
        const auto inv_no_fire_bits = read.template operator()<std::uint32_t>(pawn_value,
            static_cast<std::uint64_t>(inv_no_fire_property.offset));
        inv_out.pawn_no_weapon_firing = LiveBoolSnapshot{(inv_no_fire_bits & inv_no_fire_property.mask) != 0,
            inv_no_fire_property.offset, inv_no_fire_property.mask};
        const auto inv_float = [&](std::uint64_t owner, std::string_view field, std::string_view package) {
            const auto metadata = property(owner, field, {}, package, "Engine", false, false, true);
            const auto bits = read.template operator()<std::uint32_t>(pawn_value,
                static_cast<std::uint64_t>(metadata.offset));
            const auto sampled_float = std::bit_cast<float>(bits);
            // Observer JSON policy only: retain every finite value, including
            // negative values and signed zero; the private trace keeps raw bits.
            if (!std::isfinite(sampled_float)) fail("live_invalid");
            return sampled_float;
        };
        inv_out.pawn_jump_z = inv_float(inv_pawn_type, "JumpZ", "Engine");
        inv_out.pawn_thruster_energy = inv_float(inv_native_type, "ThrusterEnergy", "HawkenGame");
        inv_out.pawn_thruster_jump_energy = inv_float(inv_native_type, "ThrusterJumpEnergy", "HawkenGame");
        const auto inv_move_property = property(inv_native_type, "MoveState", {}, "HawkenGame", "Engine", true);
        inv_out.pawn_move_state = read.template operator()<std::uint8_t>(pawn_value,
            static_cast<std::uint64_t>(inv_move_property.offset));
        inv_out.pawn_invmanager_present = inv_manager != 0;
        inv_out.pawn_dual_weapon_present = inv_dual != 0;
        inv_out.pawn_weapon_present = inv_weapon != 0;
        if (inv_weapon && !contains(instance(inv_weapon), inv_weapon_property.expected)) fail("live_identity");
        inv_out.pawn_weapon_is_dual = inv_weapon && inv_dual && inv_weapon == inv_dual;
        if (inv_manager) {
            const auto inv_manager_chain = instance(inv_manager);
            if (!contains(inv_manager_chain, inv_manager_property.expected)) fail("live_identity");
            inv_out.manager_script_state = script_state(inv_manager, inv_manager_chain);
            const auto inv_actor_type = named(inv_manager_chain, "Actor", "Engine");
            const auto inv_manager_type = named(inv_manager_chain, "InventoryManager", "Engine");
            const auto inv_instigator_property = property(inv_actor_type, "Instigator", "Pawn");
            const auto inv_chain_property = property(inv_manager_type, "InventoryChain", "Inventory");
            const auto inv_role_property = property(inv_actor_type, "Role", {}, "Engine", "Engine", true);
            const auto inv_instigator = read.template operator()<std::uint64_t>(inv_manager,
                static_cast<std::uint64_t>(inv_instigator_property.offset));
            const auto inv_chain = read.template operator()<std::uint64_t>(inv_manager,
                static_cast<std::uint64_t>(inv_chain_property.offset));
            if (inv_instigator && !contains(instance(inv_instigator), inv_instigator_property.expected))
                fail("live_identity");
            if (inv_chain && !contains(instance(inv_chain), inv_chain_property.expected)) fail("live_identity");
            inv_out.manager_instigator_is_pawn = inv_instigator && inv_instigator == pawn_value;
            inv_out.manager_chain_is_dual = inv_chain && inv_dual && inv_chain == inv_dual;
            inv_out.manager_role = read.template operator()<std::uint8_t>(inv_manager,
                static_cast<std::uint64_t>(inv_role_property.offset));
        }
        if (inv_dual) {
            const auto inv_dual_chain = instance(inv_dual);
            if (!contains(inv_dual_chain, inv_dual_property.expected)) fail("live_identity");
            const auto inv_actor_type = named(inv_dual_chain, "Actor", "Engine");
            const auto inv_inventory_type = named(inv_dual_chain, "Inventory", "Engine");
            const auto inv_instigator_property = property(inv_actor_type, "Instigator", "Pawn");
            const auto inv_manager_link_property = property(inv_inventory_type, "InvManager", "InventoryManager");
            const auto inv_dual_type = named(inv_dual_chain, "R_Weap_DualWield", "HawkenGame");
            if (inspect_fire_timing) {
                const auto fire_dual_client_metadata = property(inv_dual_type, "ClientCanFireTimeStamp", {},
                    "HawkenGame", "Engine", false, false, true);
                const auto fire_dual_server_metadata = property(inv_dual_type, "ServerCanFireTimeStamp", {},
                    "HawkenGame", "Engine", false, false, true);
                const auto fire_dual_client_bits = read.template operator()<std::uint32_t>(inv_dual,
                    static_cast<std::uint64_t>(fire_dual_client_metadata.offset));
                const auto fire_dual_server_bits = read.template operator()<std::uint32_t>(inv_dual,
                    static_cast<std::uint64_t>(fire_dual_server_metadata.offset));
                const auto fire_dual_client_value = std::bit_cast<float>(fire_dual_client_bits);
                const auto fire_dual_server_value = std::bit_cast<float>(fire_dual_server_bits);
                if (!std::isfinite(fire_dual_client_value) || !std::isfinite(fire_dual_server_value))
                    fail("live_invalid");
                out.fire_timing->dual_weapon = LiveDualFireTimingSnapshot{
                    fire_dual_client_value, fire_dual_server_value,
                    fire_dual_client_metadata.offset, fire_dual_server_metadata.offset,
                    inv_instigator_property.offset};
            }
            inv_out.dual_script_state = script_state(inv_dual, inv_dual_chain);
            inv_out.dual_can_fire_dispatch = function_dispatch(inv_dual_type, "ChangeCanFire", "HawkenGame");
            const auto inv_dual_role_property = property(inv_actor_type, "Role", {}, "Engine", "Engine", true);
            inv_out.dual_role = read.template operator()<std::uint8_t>(inv_dual,
                static_cast<std::uint64_t>(inv_dual_role_property.offset));
            const auto inv_bool = [&](std::string_view field) {
                const auto metadata = property(inv_dual_type, field, {}, "HawkenGame", "Engine", false, true);
                const auto bits = read.template operator()<std::uint32_t>(inv_dual,
                    static_cast<std::uint64_t>(metadata.offset));
                return LiveBoolSnapshot{(bits & metadata.mask) != 0, metadata.offset, metadata.mask};
            };
            inv_out.dual_server_can_fire = inv_bool("bServerCanFire");
            inv_out.dual_shutdown = inv_bool("bShutdown");
            inv_out.dual_client_shutdown = inv_bool("bClientShutdown");
            const auto inv_instigator = read.template operator()<std::uint64_t>(inv_dual,
                static_cast<std::uint64_t>(inv_instigator_property.offset));
            const auto inv_manager_link = read.template operator()<std::uint64_t>(inv_dual,
                static_cast<std::uint64_t>(inv_manager_link_property.offset));
            if (inv_instigator && !contains(instance(inv_instigator), inv_instigator_property.expected))
                fail("live_identity");
            if (inv_manager_link && !contains(instance(inv_manager_link), inv_manager_link_property.expected))
                fail("live_identity");
            inv_out.dual_instigator_is_pawn = inv_instigator && inv_instigator == pawn_value;
            inv_out.dual_invmanager_matches = inv_manager_link && inv_manager && inv_manager_link == inv_manager;
        }
        out.inventory = inv_out;
    }
    return out;
}

} // namespace hawken::probe
