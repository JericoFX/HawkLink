#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hawken::probe {

// Identify supplies fixed metadata names/package and FName instance, never the
// arbitrary name of a live actor. Reader supplies checked, budgeted read<T>.
struct LiveIdentity {
    std::string name;
    std::string package;
    std::uint32_t instance = 0;
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
    bool operator==(const LivePlayerSnapshot&) const = default;
};

// Select the first Engine.GamePlayers entry, not every player/actor in a world.
// Reader policy: count/cap<=4, class chain<=16, children<=512 per class and
// <=2048 in total, Offset<=65536. These are not UE3 or allocation-size limits.
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
// Fixed errors: live_invalid, live_engine_absent, live_array_invalid,
// live_limit, live_identity, live_cycle, live_property_missing,
// live_property_mismatch, live_backlink_mismatch; Reader errors propagate.
template<class Reader, class Identify>
LivePlayerSnapshot read_live_player(Reader& reader, std::uint64_t module, Identify& identify) {
    const auto fail = [](const char* code) { throw std::runtime_error(code); };
    const auto checked = [&](std::uint64_t base, std::uint64_t offset, std::size_t size) {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (!base || offset > maximum - base || size - 1 > maximum - (base + offset))
            fail("live_invalid");
    };
    LivePlayerSnapshot out;
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
    const auto chain = [&](std::uint64_t first) {
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

    struct Property { std::int32_t offset = 0; std::uint64_t expected = 0; };
    std::size_t total_children = 0;
    const auto property = [&](std::uint64_t owner, std::string_view field, std::string_view expected_name) {
        Property result;
        bool found = false;
        std::vector<std::uint64_t> visited;
        auto child = read.template operator()<std::uint64_t>(owner, 0x80);
        while (child) {
            if (std::find(visited.begin(), visited.end(), child) != visited.end()) fail("live_cycle");
            if (visited.size() == 512 || total_children == 2048) fail("live_limit");
            visited.push_back(child);
            ++total_children;
            const auto id = identify(child);
            if (id.name == field) {
                if (found || id.instance != 0 || id.package != "Engine" ||
                    read.template operator()<std::uint64_t>(child, 0x40) != owner)
                    fail("live_property_mismatch");
                const auto type = read.template operator()<std::uint64_t>(child, 0x50);
                if (!type || read.template operator()<std::uint64_t>(type, 0x50) != kind)
                    fail("live_property_mismatch");
                const auto type_id = identify(type);
                if (type_id.name != "ObjectProperty" || type_id.package != "Core" || type_id.instance != 0 ||
                    (read.template operator()<std::uint32_t>(type, 0x128) & 0x10000) == 0 ||
                    read.template operator()<std::int32_t>(child, 0x68) != 1 ||
                    read.template operator()<std::int32_t>(child, 0x6c) != 8)
                    fail("live_property_mismatch");
                result.offset = read.template operator()<std::int32_t>(child, 0x8c);
                if (result.offset < 0) fail("live_property_mismatch");
                if (result.offset > 65536) fail("live_limit");
                result.expected = read.template operator()<std::uint64_t>(child, 0xa8);
                if (!result.expected) fail("live_property_mismatch");
                const auto expected_chain = chain(result.expected);
                if (expected_chain.front().identity.name != expected_name ||
                    expected_chain.front().identity.package != "Engine") fail("live_property_mismatch");
                found = true;
            }
            // Continue after a match: duplicates and cycles remain inconclusive.
            child = read.template operator()<std::uint64_t>(child, 0x60);
        }
        if (!found) fail("live_property_missing");
        return result;
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
    return out;
}

} // namespace hawken::probe
