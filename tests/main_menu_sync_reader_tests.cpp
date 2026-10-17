#include "main_menu_sync_reader.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <type_traits>

namespace {
using hawken::probe::LiveIdentity;
using hawken::probe::MainMenuSyncState;
using hawken::probe::main_menu_sync_flag_names;

// Directed synthetic memory: missing bytes fail. No process, UI, SQL or game
// callbacks are mocked, and no live actor/movie instance names are supplied.
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
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
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
constexpr std::uint64_t module = 0x140000000;
constexpr std::uint64_t kind = 0x10000, object = 0x10400, engine_type = 0x10800;
constexpr std::uint64_t player = 0x10c00, local_type = 0x11000, pc = 0x11400;
constexpr std::uint64_t entry = 0x11800, movie = 0x11c00, options = 0x12000;
constexpr std::uint64_t mech_base = 0x12400, main_menu = 0x12800;
constexpr std::uint64_t object_property = 0x12c00, bool_property = 0x13000, int_property = 0x13400;
constexpr std::uint64_t engine = 0x20000, local = 0x24000, controller = 0x28000;
constexpr std::uint64_t menu = 0x2c000, array = 0x30000, decoy = 0x34000;
constexpr std::uint64_t actor_field = 0x40000, player_field = 0x40400, menu_field = 0x40800;
constexpr std::uint64_t owner_field = 0x40c00, open_field = 0x41000, flags_start = 0x50000;

struct Fixture {
    Memory memory;
    std::map<std::uint64_t, LiveIdentity> identities;
    Fixture() {
        memory.put(module + 0x1cfb860, engine);
        memory.put(module + 0x1bda990, kind);
        klass(kind, "Class", "Core", object);
        klass(object, "Object", "Core", 0);
        klass(engine_type, "Engine", "Engine", object);
        klass(player, "Player", "Engine", object);
        klass(local_type, "LocalPlayer", "Engine", player);
        klass(pc, "PlayerController", "Engine", object);
        klass(entry, "R_EntryPlayerController", "HawkenGame", pc);
        klass(movie, "GFxMoviePlayer", "GFxUI", object);
        klass(options, "R_GFx_Options", "HawkenGame", movie);
        klass(mech_base, "R_GFx_MechMenu_Base", "HawkenGame", options);
        klass(main_menu, "R_GFx_MainMenu", "HawkenGame", mech_base);
        klass(object_property, "ObjectProperty", "Core", object);
        klass(bool_property, "BoolProperty", "Core", object);
        klass(int_property, "IntProperty", "Core", object);
        memory.put(object_property + 0x128, std::uint32_t{0x10000});
        memory.put(bool_property + 0x128, std::uint32_t{0x20000});
        instance(engine, engine_type, 0);
        instance(local, local_type, engine);
        instance(controller, entry, 0);
        instance(menu, main_menu, 0);
        instance(decoy, main_menu, 0);
        memory.put(engine + 0x700, array);
        memory.put(engine + 0x708, std::int32_t{1});
        memory.put(engine + 0x70c, std::int32_t{1});
        memory.put(array, local);
        property(actor_field, player, "Actor", "Engine", object_property, 0x68, 8, pc);
        property(player_field, pc, "Player", "Engine", object_property, 0x500, 8, player);
        property(menu_field, entry, "MainMenu", "HawkenGame", object_property, 0x530, 8, main_menu);
        property(owner_field, movie, "LocalPlayerOwnerIndex", "GFxUI", int_property, 0x614, 4, 0);
        property(open_field, movie, "bMovieIsOpen", "GFxUI", bool_property, 0x610, 4, 0x80000000);
        memory.put(owner_field + 0x60, open_field);
        memory.put(player + 0x80, actor_field);
        memory.put(pc + 0x80, player_field);
        memory.put(entry + 0x80, menu_field);
        memory.put(movie + 0x80, owner_field);
        memory.put(local + 0x68, controller);
        memory.put(controller + 0x500, local);
        memory.put(controller + 0x530, menu);
        memory.put(menu + 0x600, std::uint32_t{0x7fff});
        memory.put(menu + 0x604, std::uint32_t{0x20});
        memory.put(menu + 0x610, std::uint32_t{0x80000000});
        memory.put(menu + 0x614, std::int32_t{0});
        std::uint64_t last = 0;
        for (std::size_t i = 0; i < main_menu_sync_flag_names.size(); ++i) {
            const auto field = flags_start + i * 0x100;
            const bool inherited = i == 3;
            property(field, inherited ? options : mech_base, main_menu_sync_flag_names[i], "HawkenGame",
                bool_property, inherited ? 0x604 : 0x600, 4,
                inherited ? std::uint64_t{0x20} : (std::uint64_t{1} << i));
            if (inherited) memory.put(options + 0x80, field);
            else {
                if (last) memory.put(last + 0x60, field);
                else memory.put(mech_base + 0x80, field);
                last = field;
            }
        }
    }
    void klass(std::uint64_t where, const char* name, const char* package, std::uint64_t parent) {
        identities[where] = {name, package, 0};
        memory.put(where + 0x50, kind);
        memory.put(where + 0x78, parent);
        memory.put(where + 0x80, std::uint64_t{0});
        memory.put(where + 0x1d4, where + 0x180);
    }
    void instance(std::uint64_t where, std::uint64_t type, std::uint64_t outer) {
        memory.put(where + 0x10, std::uint64_t{0});
        memory.put(where + 0x40, outer);
        memory.put(where + 0x50, type);
    }
    void property(std::uint64_t where, std::uint64_t owner, std::string_view name,
                  const char* package, std::uint64_t type, std::int32_t offset,
                  std::int32_t size, std::uint64_t tail) {
        identities[where] = {std::string(name), package, 0};
        memory.put(where + 0x40, owner);
        memory.put(where + 0x50, type);
        memory.put(where + 0x60, std::uint64_t{0});
        memory.put(where + 0x68, std::int32_t{1});
        memory.put(where + 0x6c, size);
        memory.put(where + 0x8c, offset);
        // ObjectProperty uses pointer width; BoolProperty uses uint32 mask.
        if (type == bool_property) memory.put(where + 0xa8, static_cast<std::uint32_t>(tail));
        else memory.put(where + 0xa8, tail);
    }
    void pad_owner_fields(std::uint64_t owner, std::size_t target, std::uint64_t padding_base) {
        auto child = memory.read<std::uint64_t>(owner, 0x80);
        std::uint64_t last = 0;
        std::size_t count = 0;
        while (child) {
            last = child;
            child = memory.read<std::uint64_t>(child, 0x60);
            ++count;
        }
        REQUIRE(count <= target);
        for (; count < target; ++count) {
            const auto padding = padding_base + count * 0x100;
            // Irrelevant metadata members still belong to the owner's full
            // Children/Next list, not just the requested properties/NET fields.
            identities[padding] = {"unrelated_" + std::to_string(count), identities.at(owner).package, 0};
            memory.put(padding + 0x40, owner);
            memory.put(padding + 0x60, std::uint64_t{0});
            if (last) memory.put(last + 0x60, padding);
            else memory.put(owner + 0x80, padding);
            last = padding;
        }
    }
    hawken::probe::MainMenuSyncSnapshot sample() {
        auto identify = [&](std::uint64_t where) {
            const auto found = identities.find(where);
            if (found == identities.end()) throw std::runtime_error("identity_unavailable");
            return found->second;
        };
        return hawken::probe::read_main_menu_sync(memory, module, identify);
    }
};
}

TEST_CASE("Main menu sync follows typed EntryPC association and inherited packed flags") {
    Fixture f;
    const auto result = f.sample();
    CHECK(result.state == MainMenuSyncState::observed);
    for (const bool value : result.flags) CHECK(value);
    CHECK(result.all_synced);
    CHECK(result.movie_open);
    CHECK_FALSE(result.trace.empty());
    // A second movie does not select itself by class identity or object order.
    f.memory.put(decoy + 0x600, std::uint32_t{0});
    CHECK(f.sample() == result);
}

TEST_CASE("Main menu sync derives conjunction from every one of fifteen flags") {
    for (std::size_t index = 0; index < main_menu_sync_flag_names.size(); ++index) {
        Fixture f;
        CAPTURE(index);
        if (index == 3) f.memory.put(menu + 0x604, std::uint32_t{0});
        else f.memory.put(menu + 0x600, std::uint32_t{0x7fff} & ~(std::uint32_t{1} << index));
        const auto result = f.sample();
        REQUIRE(result.state == MainMenuSyncState::observed);
        CHECK_FALSE(result.all_synced);
        for (std::size_t other = 0; other < result.flags.size(); ++other)
            CHECK(result.flags[other] == (index != other));
    }
}

TEST_CASE("Main menu sync distinguishes null roots without universal absence") {
    Fixture f;
    SUBCASE("no local player") { f.memory.put(engine + 0x708, std::int32_t{0});
        CHECK(f.sample().state == MainMenuSyncState::no_local_player); }
    SUBCASE("null Actor") { f.memory.put(local + 0x68, std::uint64_t{0});
        CHECK(f.sample().state == MainMenuSyncState::no_controller); }
    SUBCASE("null associated menu ignores decoy") { f.memory.put(controller + 0x530, std::uint64_t{0});
        CHECK(f.sample().state == MainMenuSyncState::no_menu); }
}

TEST_CASE("Main menu sync refuses closed stale or foreign local movie ownership") {
    Fixture f;
    SUBCASE("closed movie") { f.memory.put(menu + 0x610, std::uint32_t{0});
        const auto result = f.sample(); CHECK(result.state == MainMenuSyncState::closed_menu);
        CHECK_FALSE(result.movie_open);
        for (const bool value : result.flags) CHECK(value);
        CHECK(result.all_synced); }
    SUBCASE("other local player") { f.memory.put(menu + 0x614, std::int32_t{1});
        CHECK(f.sample().state == MainMenuSyncState::owner_mismatch); }
    SUBCASE("negative index is not a verified zero owner") { f.memory.put(menu + 0x614, std::int32_t{-1});
        CHECK(f.sample().state == MainMenuSyncState::owner_mismatch); }
    SUBCASE("Controller.Player mismatch") { f.memory.put(controller + 0x500, decoy);
        CHECK_THROWS_WITH_AS(f.sample(), "menu_backlink_mismatch", std::runtime_error); }
}

TEST_CASE("Main menu sync validates exact reflected owner type dimension size and mask") {
    Fixture f;
    SUBCASE("MainMenu expected class") { f.memory.put(menu_field + 0xa8, movie); }
    SUBCASE("MainMenu dimension") { f.memory.put(menu_field + 0x68, std::int32_t{2}); }
    SUBCASE("MainMenu size") { f.memory.put(menu_field + 0x6c, std::int32_t{4}); }
    SUBCASE("Bool size") { f.memory.put(flags_start + 0x6c, std::int32_t{1}); }
    SUBCASE("Bool type") { f.memory.put(flags_start + 0x50, int_property); }
    SUBCASE("Bool dimension") { f.memory.put(flags_start + 0x68, std::int32_t{2}); }
    SUBCASE("Bool owner") { f.memory.put(flags_start + 0x40, options); }
    SUBCASE("Inherited account flag owner") { f.memory.put(flags_start + 3 * 0x100 + 0x40, mech_base); }
    SUBCASE("zero mask") { f.memory.put(flags_start + 0xa8, std::uint32_t{0}); }
    SUBCASE("multiple bit mask") { f.memory.put(flags_start + 0xa8, std::uint32_t{3}); }
    SUBCASE("negative offset") { f.memory.put(flags_start + 0x8c, std::int32_t{-1}); }
    CHECK_THROWS_WITH_AS(f.sample(), "menu_property_mismatch", std::runtime_error);
}

TEST_CASE("Main menu sync rejects wrong controller movie class and template instances") {
    Fixture f;
    SUBCASE("gameplay PC is not EntryPC") { f.identities[entry].name = "R_PlayerController"; }
    SUBCASE("wrong movie") { f.memory.put(menu + 0x50, movie); }
    SUBCASE("CDO movie") { f.memory.put(main_menu + 0x1d4, menu); }
    SUBCASE("template movie") { f.memory.put(menu + 0x10, std::uint64_t{0x400}); }
    CHECK_THROWS_WITH_AS(f.sample(), "menu_identity", std::runtime_error);
}

TEST_CASE("Main menu sync bounds metadata walks arithmetic and read budget") {
    Fixture f;
    SUBCASE("oversized local array") { f.memory.put(engine + 0x70c, std::int32_t{5});
        CHECK_THROWS_WITH_AS(f.sample(), "menu_limit", std::runtime_error); }
    SUBCASE("invalid count") { f.memory.put(engine + 0x708, std::int32_t{-1});
        CHECK_THROWS_WITH_AS(f.sample(), "menu_array_invalid", std::runtime_error); }
    SUBCASE("large property offset") { f.memory.put(flags_start + 0x8c, std::int32_t{65537});
        CHECK_THROWS_WITH_AS(f.sample(), "menu_limit", std::runtime_error); }
    SUBCASE("parent cycle") { f.memory.put(options + 0x78, main_menu);
        CHECK_THROWS_WITH_AS(f.sample(), "menu_cycle", std::runtime_error); }
    SUBCASE("property cycle") { f.memory.put(flags_start + 0x60, flags_start);
        CHECK_THROWS_WITH_AS(f.sample(), "menu_cycle", std::runtime_error); }
    SUBCASE("missing flag") { f.identities[flags_start].name = "unrelated";
        CHECK_THROWS_WITH_AS(f.sample(), "menu_property_missing", std::runtime_error); }
    SUBCASE("missing bytes") { f.memory.bytes.erase(menu + 0x600);
        CHECK_THROWS_WITH_AS(f.sample(), "reader_error", std::runtime_error); }
    SUBCASE("overflowing engine pointer") {
        f.memory.put(module + 0x1cfb860, std::numeric_limits<std::uint64_t>::max() - 4);
        CHECK_THROWS_WITH_AS(f.sample(), "menu_invalid", std::runtime_error); }
    SUBCASE("budget") { f.memory.budget = 1;
        CHECK_THROWS_WITH_AS(f.sample(), "read_limit", std::runtime_error); }
}

TEST_CASE("Main menu sync retains full internal equality trace for two non atomic samples") {
    Fixture f;
    const auto first = f.sample();
    CHECK(f.sample() == first);
    // A sibling bit changes even though the fifteen selected values do not.
    f.memory.put(menu + 0x600, std::uint32_t{0x80007fff});
    const auto second = f.sample();
    CHECK(second.flags == first.flags);
    CHECK(second.all_synced == first.all_synced);
    CHECK_FALSE(second == first);
}

// Independent counts of ALL own Children/Next members in the shipped packages,
// not NET-field/cache bounds or UE3 allocation limits. Full original proof pins
// are recorded in stats-diagnosis.json/main_menu_owner_caps_red_20261017.
TEST_CASE("Main menu sync supports the six shipped owner Children counts") {
    Fixture f;
    const std::array<std::uint64_t, 6> owners = {player, pc, entry, movie, options, mech_base};
    const std::array<std::size_t, 6> counts = {10, 575, 148, 147, 712, 1403};
    for (std::size_t index = 0; index < owners.size(); ++index)
        f.pad_owner_fields(owners[index], counts[index], 0x100000 + index * 0x100000);
    const auto result = f.sample();
    REQUIRE(result.state == MainMenuSyncState::observed);
    CHECK(result.movie_open);
    CHECK(result.all_synced);
    for (const bool flag : result.flags) CHECK(flag);
    CHECK_FALSE(result.trace.empty());
}

TEST_CASE("Main menu sync rejects each shipped owner cap plus one") {
    const std::array<std::uint64_t, 6> owners = {player, pc, entry, movie, options, mech_base};
    const std::array<std::size_t, 6> counts = {10, 575, 148, 147, 712, 1403};
    for (std::size_t index = 0; index < owners.size(); ++index) {
        Fixture f;
        CAPTURE(index);
        f.pad_owner_fields(owners[index], counts[index] + 1, 0x100000);
        CHECK_THROWS_WITH_AS(f.sample(), "menu_limit", std::runtime_error);
    }
}

TEST_CASE("Main menu sync detects a cycle after the shipped large owner walk") {
    Fixture f;
    f.pad_owner_fields(mech_base, 1403, 0x100000);
    const auto tail = std::uint64_t{0x100000} + 1402 * 0x100;
    f.memory.put(tail + 0x60, flags_start);
    CHECK_THROWS_WITH_AS(f.sample(), "menu_cycle", std::runtime_error);
}
