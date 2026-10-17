#pragma once
#include "live_player_reader.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <string_view>
#include <vector>

namespace hawken::probe {
inline constexpr std::array<std::string_view, 15> main_menu_sync_flag_names = {
    "hasReceivedDailyCalendarRewards", "hasCheckedIntoDailyCalendar",
    "hasReadOnlinePlayerNickname", "hasReadOnlinePlayerAccountInfo", "hasReadOverflowStats",
    "hasReadOnlinePlayerStats", "hasReadOnlineLegacyStats", "hasReadGameItemTypeCollection",
    "hasReadGameItemInstanceCollection", "hasSyncingAccount_ReadAllUserGameItemStats",
    "hasReadGameOfferCollection", "hasReadGameBundleCollection", "hasReadAchievementCollection",
    "hasReadUnlockedAchievementCollection", "bFinishedInitGameOfferCollectionForUI"};

enum class MainMenuSyncState {
    no_local_player, no_controller, no_menu, closed_menu, owner_mismatch, observed
};
struct MainMenuSyncSnapshot {
    MainMenuSyncState state = MainMenuSyncState::no_local_player;
    std::array<bool, 15> flags{};
    bool all_synced = false;
    bool movie_open = false;
    // Internal two-sample equality only; never serialize pointers/raw memory.
    std::vector<std::uint64_t> trace;
    bool operator==(const MainMenuSyncSnapshot&) const = default;
};

// Reader/Identify use the existing checked read<T>/LiveIdentity conventions.
// Follow first Engine.GamePlayers -> typed LocalPlayer -> Player.Actor -> typed
// R_EntryPlayerController.MainMenu, not the gameplay R_PlayerController global.
// MainMenu is exact ObjectProperty/ArrayDim1/ElementSize8 with R_GFx_MainMenu
// PropertyClass. Resolve Offset at+8c; never use cooked order or NetIndex.
// Require Controller.Player backlink and movie LocalPlayerOwnerIndex==0.
// Observe bMovieIsOpen independently; closed_menu is lifecycle-inconclusive.
// The 15 flags are exact BoolProperty/ArrayDim1/ElementSize4:
// account-info belongs to R_GFx_Options; the other 14 to R_GFx_MechMenu_Base.
// BoolProperty+a8 is uint32 BitMask (f89f0/f8ec0); read DWORD at reflected Offset.
// Shipped own-Children caps: Player10/PC575/Entry148/Movie147/Options712/
// MechBase1403, total2995; unknown owners fail closed. Count/cap4, class16,
// Offset65536 and Reader1MiB remain diagnostic policies, not UE3 allocation
// limits. Validate non-CDO/template
// instances with the existing mask600/CDO predicate; no instance names needed.
// Caller compares two complete snapshots/traces: matching reads are not atomic.
// Observed/closed_menu flags describe the associated typed movie; all_synced
// is their conjunction, not a successful UI callback. Closed is not active.
// Null roots/owner mismatch do not establish flags or universal absence. No
// state proves topmost focus, callback delivery or successful UI completion.
// Fixed errors: menu_invalid, menu_engine_absent, menu_array_invalid, menu_limit,
// menu_identity, menu_cycle, menu_property_missing, menu_property_mismatch,
// menu_backlink_mismatch. Reader/Identify failures propagate without logging.
template<class Reader, class Identify>
MainMenuSyncSnapshot read_main_menu_sync(Reader& reader, std::uint64_t module, Identify& identify) {
    const auto fail = [](const char* code) { throw std::runtime_error(code); };
    MainMenuSyncSnapshot out;
    const auto read = [&]<class T>(std::uint64_t base, std::uint64_t offset = 0) {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (!base || offset > maximum - base || sizeof(T) - 1 > maximum - (base + offset))
            fail("menu_invalid");
        const auto value = reader.template read<T>(base, offset);
        out.trace.insert(out.trace.end(), {base, offset, static_cast<std::uint64_t>(value)});
        return value;
    };
    const auto engine_object = read.template operator()<std::uint64_t>(module, 0x1cfb860);
    if (!engine_object) fail("menu_engine_absent");
    const auto class_kind = read.template operator()<std::uint64_t>(module, 0x1bda990);
    if (!class_kind) fail("menu_identity");
    const auto kind_id = identify(class_kind);
    if (kind_id.name != "Class" || kind_id.package != "Core" || kind_id.instance != 0)
        fail("menu_identity");
    struct ClassNode { std::uint64_t address; LiveIdentity identity; };
    const auto chain = [&](std::uint64_t first) {
        std::vector<ClassNode> nodes;
        auto current = first;
        while (current) {
            if (std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) { return node.address == current; }))
                fail("menu_cycle");
            if (nodes.size() == 16) fail("menu_limit");
            if (read.template operator()<std::uint64_t>(current, 0x50) != class_kind) fail("menu_identity");
            const auto id = identify(current);
            if (id.instance != 0) fail("menu_identity");
            nodes.push_back({current, id});
            current = read.template operator()<std::uint64_t>(current, 0x78);
        }
        if (nodes.empty() || nodes.back().identity.name != "Object" || nodes.back().identity.package != "Core")
            fail("menu_identity");
        return nodes;
    };
    const auto named = [&](const auto& nodes, std::string_view name, std::string_view package) {
        std::uint64_t found = 0;
        for (const auto& node : nodes) {
            if (node.identity.name == name && node.identity.package == package) {
                if (found) fail("menu_identity");
                found = node.address;
            }
        }
        if (!found) fail("menu_identity");
        return found;
    };
    const auto contains = [](const auto& nodes, std::uint64_t expected) {
        return std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) { return node.address == expected; });
    };
    const auto instance = [&](std::uint64_t instance_object) {
        if (!instance_object || (read.template operator()<std::uint64_t>(instance_object, 0x10) & 0x600) != 0)
            fail("menu_identity");
        const auto type = read.template operator()<std::uint64_t>(instance_object, 0x50);
        if (!type) fail("menu_identity");
        const auto nodes = chain(type);
        if (read.template operator()<std::uint64_t>(type, 0x1d4) == instance_object) fail("menu_identity");
        return nodes;
    };
    static_cast<void>(named(instance(engine_object), "Engine", "Engine"));
    const auto player_array = read.template operator()<std::uint64_t>(engine_object, 0x700);
    const auto count = read.template operator()<std::int32_t>(engine_object, 0x708);
    const auto capacity = read.template operator()<std::int32_t>(engine_object, 0x70c);
    if (count < 0 || capacity < 0 || capacity < count) fail("menu_array_invalid");
    if (count > 4 || capacity > 4) fail("menu_limit");
    if (count && !player_array) fail("menu_array_invalid");
    if (!count) return out;
    const auto local_object = read.template operator()<std::uint64_t>(player_array);
    const auto local_chain = instance(local_object);
    static_cast<void>(named(local_chain, "LocalPlayer", "Engine"));
    const auto player_type = named(local_chain, "Player", "Engine");
    if (read.template operator()<std::uint64_t>(local_object, 0x40) != engine_object) fail("menu_identity");

    struct Field { std::uint64_t address; LiveIdentity identity; };
    struct OwnerFields { std::uint64_t owner; std::vector<Field> fields; };
    std::vector<OwnerFields> owners;
    std::size_t total_children = 0;
    const auto fields = [&](std::uint64_t owner) -> const std::vector<Field>& {
        for (const auto& cached : owners) if (cached.owner == owner) return cached.fields;
        const auto owner_id = identify(owner);
        std::size_t owner_limit = 0;
        if (owner_id.instance == 0) {
            if (owner_id.package == "Engine" && owner_id.name == "Player") owner_limit = 10;
            else if (owner_id.package == "Engine" && owner_id.name == "PlayerController") owner_limit = 575;
            else if (owner_id.package == "HawkenGame" && owner_id.name == "R_EntryPlayerController") owner_limit = 148;
            else if (owner_id.package == "GFxUI" && owner_id.name == "GFxMoviePlayer") owner_limit = 147;
            else if (owner_id.package == "HawkenGame" && owner_id.name == "R_GFx_Options") owner_limit = 712;
            else if (owner_id.package == "HawkenGame" && owner_id.name == "R_GFx_MechMenu_Base") owner_limit = 1403;
        }
        if (!owner_limit) fail("menu_identity");
        std::vector<Field> list;
        auto child = read.template operator()<std::uint64_t>(owner, 0x80);
        while (child) {
            if (std::any_of(list.begin(), list.end(), [&](const auto& field) { return field.address == child; }))
                fail("menu_cycle");
            if (list.size() == owner_limit || total_children == 2995) fail("menu_limit");
            ++total_children;
            list.push_back({child, identify(child)});
            child = read.template operator()<std::uint64_t>(child, 0x60);
        }
        // Each class is walked once, so fifteen lookups do not multiply the
        // total-child budget or reread an owner's mutable Children/Next list.
        owners.push_back({owner, std::move(list)});
        return owners.back().fields;
    };
    struct Property { std::int32_t offset; std::uint64_t tail; };
    const auto property = [&](std::uint64_t owner, std::string_view field_name,
                              std::string_view package, std::string_view type_name,
                              std::string_view expected_name = {}, std::string_view expected_package = {}) {
        std::uint64_t field = 0;
        for (const auto& candidate : fields(owner)) {
            if (candidate.identity.name == field_name) {
                if (field || candidate.identity.instance != 0 || candidate.identity.package != package)
                    fail("menu_property_mismatch");
                field = candidate.address;
            }
        }
        if (!field) fail("menu_property_missing");
        if (read.template operator()<std::uint64_t>(field, 0x40) != owner) fail("menu_property_mismatch");
        const auto type = read.template operator()<std::uint64_t>(field, 0x50);
        if (!type || read.template operator()<std::uint64_t>(type, 0x50) != class_kind)
            fail("menu_property_mismatch");
        const auto id = identify(type);
        if (id.name != type_name || id.package != "Core" || id.instance != 0 ||
            read.template operator()<std::int32_t>(field, 0x68) != 1 ||
            read.template operator()<std::int32_t>(field, 0x6c) != (type_name == "ObjectProperty" ? 8 : 4))
            fail("menu_property_mismatch");
        Property result{read.template operator()<std::int32_t>(field, 0x8c), 0};
        if (result.offset < 0) fail("menu_property_mismatch");
        if (result.offset > 65536) fail("menu_limit");
        if (type_name == "ObjectProperty") {
            if ((read.template operator()<std::uint32_t>(type, 0x128) & 0x10000) == 0)
                fail("menu_property_mismatch");
            result.tail = read.template operator()<std::uint64_t>(field, 0xa8);
            if (!result.tail) fail("menu_property_mismatch");
            const auto expected = chain(result.tail);
            if (expected.front().identity.name != expected_name || expected.front().identity.package != expected_package)
                fail("menu_property_mismatch");
        } else if (type_name == "BoolProperty") {
            if ((read.template operator()<std::uint32_t>(type, 0x128) & 0x20000) == 0)
                fail("menu_property_mismatch");
            result.tail = read.template operator()<std::uint32_t>(field, 0xa8);
            if (!result.tail || (result.tail & (result.tail - 1)) != 0) fail("menu_property_mismatch");
        }
        return result;
    };
    const auto actor = property(player_type, "Actor", "Engine", "ObjectProperty", "PlayerController", "Engine");
    if (actor.offset != 0x68) fail("menu_property_mismatch");
    const auto controller_object = read.template operator()<std::uint64_t>(local_object, static_cast<std::uint64_t>(actor.offset));
    out.state = MainMenuSyncState::no_controller;
    if (!controller_object) return out;
    const auto controller_chain = instance(controller_object);
    const auto entry_class = named(controller_chain, "R_EntryPlayerController", "HawkenGame");
    const auto pc_class = named(controller_chain, "PlayerController", "Engine");
    if (!contains(controller_chain, actor.tail)) fail("menu_identity");
    const auto backlink = property(pc_class, "Player", "Engine", "ObjectProperty", "Player", "Engine");
    if (backlink.tail != player_type) fail("menu_property_mismatch");
    if (read.template operator()<std::uint64_t>(controller_object, static_cast<std::uint64_t>(backlink.offset)) != local_object)
        fail("menu_backlink_mismatch");
    const auto association = property(entry_class, "MainMenu", "HawkenGame", "ObjectProperty", "R_GFx_MainMenu", "HawkenGame");
    const auto menu_object = read.template operator()<std::uint64_t>(controller_object, static_cast<std::uint64_t>(association.offset));
    out.state = MainMenuSyncState::no_menu;
    if (!menu_object) return out;
    const auto menu_chain = instance(menu_object);
    if (!contains(menu_chain, association.tail)) fail("menu_identity");
    const auto movie_class = named(menu_chain, "GFxMoviePlayer", "GFxUI");
    const auto options_class = named(menu_chain, "R_GFx_Options", "HawkenGame");
    const auto base = named(menu_chain, "R_GFx_MechMenu_Base", "HawkenGame");
    const auto owner_index = property(movie_class, "LocalPlayerOwnerIndex", "GFxUI", "IntProperty");
    if (read.template operator()<std::int32_t>(menu_object, static_cast<std::uint64_t>(owner_index.offset)) != 0) {
        out.state = MainMenuSyncState::owner_mismatch;
        return out;
    }
    const auto open = property(movie_class, "bMovieIsOpen", "GFxUI", "BoolProperty");
    out.movie_open = (read.template operator()<std::uint32_t>(menu_object, static_cast<std::uint64_t>(open.offset)) & open.tail) != 0;
    out.all_synced = true;
    for (std::size_t index = 0; index < main_menu_sync_flag_names.size(); ++index) {
        const auto field = property(index == 3 ? options_class : base, main_menu_sync_flag_names[index],
                                    "HawkenGame", "BoolProperty");
        out.flags[index] = (read.template operator()<std::uint32_t>(menu_object, static_cast<std::uint64_t>(field.offset)) & field.tail) != 0;
        out.all_synced = out.all_synced && out.flags[index];
    }
    out.state = out.movie_open ? MainMenuSyncState::observed : MainMenuSyncState::closed_menu;
    return out;
}
} // namespace hawken::probe
