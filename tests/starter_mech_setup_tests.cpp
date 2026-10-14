#include "hawken/starter_mech_setup.hpp"
#include "hawken/mech_setup_wire.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace hawken;

inventory::StarterCatalogueIds expected_types() {
    return {"10000000-0000-0000-0000-000000000001",
        "10000000-0000-0000-0000-000000000002",
        "10000000-0000-0000-0000-000000000003",
        "10000000-0000-0000-0000-000000000004",
        "10000000-0000-0000-0000-000000000005",
        "10000000-0000-0000-0000-000000000006",
        "10000000-0000-0000-0000-000000000007",
        "10000000-0000-0000-0000-000000000008",
        "10000000-0000-0000-0000-000000000009"};
}

// Synthetic, complete caller-owned role fixture, not historical vendor IDs.
store::StarterMechSnapshot effective_starter() {
    const auto types = expected_types();
    const std::array<std::string, 9> role_types = {types.mech, types.primary,
        types.secondary, types.upper, types.middle, types.arms, types.lower,
        types.drone, types.ability};
    const std::array<std::string, 9> instances = {
        "20000000-0000-0000-0000-000000000001",
        "20000000-0000-0000-0000-000000000002",
        "20000000-0000-0000-0000-000000000003",
        "20000000-0000-0000-0000-000000000004",
        "20000000-0000-0000-0000-000000000005",
        "20000000-0000-0000-0000-000000000006",
        "20000000-0000-0000-0000-000000000007",
        "20000000-0000-0000-0000-000000000008",
        "20000000-0000-0000-0000-000000000009"};
    store::StarterMechSnapshot snapshot;
    for (std::size_t i = 0; i < instances.size(); ++i)
        snapshot.items[i] = {role_types[i], instances[i]};
    snapshot.attributes = {
        {"Preset.PrimaryWeapon_0", "0"},
        {"Preset.PrimaryWeapon_InstanceId_0", instances[1]},
        {"Preset.SecondaryWeapon", "1"},
        {"Preset.SecondaryWeapon_InstanceId", instances[2]},
        {"Preset.Style.UpperPart", "10"},
        {"Preset.Style.UpperPart_InstanceId", instances[3]},
        {"Preset.Style.MidPart", "10"},
        {"Preset.Style.MidPart_InstanceId", instances[4]},
        {"Preset.Style.ArmPart", "10"},
        {"Preset.Style.ArmPart_InstanceId", instances[5]},
        {"Preset.Style.LowerPart", "10"},
        {"Preset.Style.LowerPart_InstanceId", instances[6]},
        {"Preset.Style.DroneType", "0"},
        {"Preset.Style.DroneType_InstanceId", instances[7]},
        {"Preset.Ability", "3"},
        {"Preset.Ability_InstanceId", instances[8]}};
    snapshot.attributes["Preset.GarageOrder"] = "0";
    for (int slot = 1; slot <= 3; ++slot) {
        const auto number = std::to_string(slot);
        snapshot.attributes["Preset.Items.Item" + number] = "-1";
        snapshot.attributes["Preset.Items" + number + "_InstanceId"] = "";
        snapshot.attributes["Preset.bItem" + number + "IntentionallyEmpty"] = "1";
    }
    for (int slot = 1; slot <= 6; ++slot) {
        const auto number = std::to_string(slot);
        snapshot.attributes["Preset.MechInternals.Internal" + number] = "-1";
        snapshot.attributes["Preset.Internals" + number + "_InstanceId"] = "";
        snapshot.attributes["Preset.bInternal" + number + "IntentionallyEmpty"] = "1";
    }
    return snapshot;
}

inventory::StarterMechSelection selection() {
    return {0, 255, 0, std::nullopt, true};
}

std::vector<std::uint8_t> literal_bytes(std::string_view hex) {
    const auto digit = [](char c) {
        return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    return result;
}
}

TEST_CASE("starter materializer emits the independent normal starter 544-bit fixture") {
    const auto value = inventory::materialize_starter_mech(
        effective_starter(), expected_types(), selection());
    const auto encoded = wire::mech::encode_mech_setup(value, control::Endian::little, 544);
    CHECK(encoded.logical_bits == 544);
    CHECK((encoded.payload.at(532 / 8) & (1u << (532 % 8))) != 0);
    std::string actual_hex;
    constexpr char hex_digits[] = "0123456789abcdef";
    for (const auto byte : encoded.payload) {
        actual_hex.push_back(hex_digits[byte >> 4]);
        actual_hex.push_back(hex_digits[byte & 15]);
    }
    const auto accent_hue = static_cast<unsigned>(value.mech_appearance.accent_hue);
    const auto accent_saturation = static_cast<unsigned>(value.mech_appearance.accent_saturation);
    const auto accent_value = static_cast<unsigned>(value.mech_appearance.accent_value);
    CAPTURE(actual_hex);
    CAPTURE(accent_hue);
    CAPTURE(accent_saturation);
    CAPTURE(accent_value);
    CHECK(encoded.payload == literal_bytes(
        "00000000000000000000000000383c19000703010e06021c0c043818a8a0a0a000f05ff0"
        "ffffffffffffffffffefffffffffff1fa00c0060000800000078000000181000"));
    CHECK(value.mech_type == 1);
    CHECK(value.mech_sub_type == 15);
    CHECK(value.mech_weapons.secondary_idx == 101);
    CHECK(value.mech_appearance.holo_taunt_type == 255);
    CHECK(value.mech_appearance.anim_taunt_type == 5);
    CHECK(value.b_ready_for_setup);
    CHECK(value.setup_id == 1);
    CHECK_FALSE(value.template_reference.has_value());
}

TEST_CASE("starter materializer rejects a raw snapshot without its effective equipment baseline") {
    auto snapshot = effective_starter();
    SUBCASE("raw persisted attributes alone") { snapshot.attributes.clear(); }
    SUBCASE("missing numeric role baseline") { snapshot.attributes.erase("Preset.SecondaryWeapon"); }
    SUBCASE("missing instance role baseline") { snapshot.attributes.erase("Preset.Ability_InstanceId"); }
    CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
        "Invalid starter mech setup.", std::invalid_argument);
}

TEST_CASE("starter materializer rejects corrupted or duplicated owned role identities") {
    auto snapshot = effective_starter();
    auto types = expected_types();
    SUBCASE("foreign role type") { snapshot.items[1].type_id = snapshot.items[2].type_id; }
    SUBCASE("invalid instance syntax") { snapshot.items[1].instance_id = "not-an-instance"; }
    SUBCASE("zero instance") { snapshot.items[1].instance_id = "00000000-0000-0000-0000-000000000000"; }
    SUBCASE("duplicated instance") { snapshot.items[1].instance_id = snapshot.items[2].instance_id; }
    SUBCASE("instance collides with type identity") { snapshot.items[1].instance_id = types.primary; }
    SUBCASE("duplicated expected catalogue roles") { types.primary = types.secondary; }
    CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, types, selection()),
        "Invalid starter mech setup.", std::invalid_argument);
}

TEST_CASE("starter materializer rejects references to another owned role or foreign instance") {
    const std::array<std::string_view, 8> keys = {
        "Preset.PrimaryWeapon_InstanceId_0", "Preset.SecondaryWeapon_InstanceId",
        "Preset.Style.UpperPart_InstanceId", "Preset.Style.MidPart_InstanceId",
        "Preset.Style.ArmPart_InstanceId", "Preset.Style.LowerPart_InstanceId",
        "Preset.Style.DroneType_InstanceId", "Preset.Ability_InstanceId"};
    for (const auto key : keys) {
        auto snapshot = effective_starter();
        snapshot.attributes[std::string(key)] = snapshot.items[0].instance_id;
        CAPTURE(key);
        CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
            "Invalid starter mech setup.", std::invalid_argument);
    }
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.PrimaryWeapon_InstanceId_0"] = "30000000-0000-0000-0000-000000000001";
    CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
        "Invalid starter mech setup.", std::invalid_argument);
}

TEST_CASE("starter materializer cannot equip indices outside the nine supplied roles") {
    const std::array<std::string_view, 8> keys = {"Preset.PrimaryWeapon_0",
        "Preset.SecondaryWeapon", "Preset.Style.UpperPart", "Preset.Style.MidPart",
        "Preset.Style.ArmPart", "Preset.Style.LowerPart", "Preset.Style.DroneType", "Preset.Ability"};
    for (const auto key : keys) {
        auto snapshot = effective_starter();
        snapshot.attributes[std::string(key)] = "42";
        CAPTURE(key);
        CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
            "Invalid starter mech setup.", std::invalid_argument);
    }
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.MechInternals.Internal1"] = "0";
    CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
        "Invalid starter mech setup.", std::invalid_argument);
}

TEST_CASE("starter materializer requires explicit owned primary selection and completed human state") {
    auto choice = selection();
    SUBCASE("unowned requested primary") { choice.requested_primary_index = 1; }
    SUBCASE("negative requested primary") { choice.requested_primary_index = -1; }
    SUBCASE("not completed") { choice.loaded_completion = false; }
    SUBCASE("non-null NPC template") { choice.prior_template_reference = wire::object::PackageExportReference{0}; }
    CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(effective_starter(), expected_types(), choice),
        "Invalid starter mech setup.", std::invalid_argument);
}

TEST_CASE("starter materializer preserves present empty numeric values instead of fallback") {
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.Style.AccentSaturation"] = "";
    snapshot.attributes["Preset.Style.CamoSaturation"] = "";
    snapshot.attributes["Preset.VisorStyle.PrimaryReticle"] = "";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.mech_appearance.accent_saturation == 0);
    CHECK(value.mech_appearance.upper.camo_saturation == 0);
    CHECK(value.mech_appearance.primary_reticle == 0);
    CHECK(value.mech_appearance.middle.camo_saturation == 7);
    CHECK(value.mech_appearance.secondary_reticle == 255);
}

TEST_CASE("starter materializer folds known ASCII keys but rejects conflicting aliases") {
    auto snapshot = effective_starter();
    SUBCASE("one case variant and identical role aliases") {
        snapshot.attributes["Preset.style.accentsaturation"] = "0.5";
        snapshot.attributes["Preset.secondaryweapon"] = "1";
        const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
        CHECK(value.mech_appearance.accent_saturation == 50);
        CHECK(value.mech_weapons.secondary_idx == 101);
    }
    SUBCASE("conflicting cosmetic aliases") {
        snapshot.attributes["Preset.Style.CamoScale"] = "2";
        snapshot.attributes["Preset.style.camoscale"] = "3";
        CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
            "Invalid starter mech setup.", std::invalid_argument);
    }
    SUBCASE("conflicting equipment aliases") {
        snapshot.attributes["Preset.secondaryweapon"] = "2";
        CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
            "Invalid starter mech setup.", std::invalid_argument);
    }
}

TEST_CASE("starter materializer inherits the loaded upper camo and applies byte and bool conversion") {
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.Style.CamoPattern"] = "258";
    snapshot.attributes["Preset.Style.AccentUpper"] = "2";
    snapshot.attributes["Preset.Style.AccentArms"] = "3";
    snapshot.attributes["Preset.VisorStyle.bTintReticle"] = "3";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.mech_appearance.upper.camo_pattern == 2);
    CHECK(value.mech_appearance.middle.camo_pattern == 2);
    CHECK(value.mech_appearance.lower.camo_pattern == 2);
    CHECK(value.mech_appearance.arm.camo_pattern == 2);
    CHECK_FALSE(value.mech_appearance.upper.b_accent);
    CHECK(value.mech_appearance.arm.b_accent);
    CHECK(value.mech_appearance.b_tint_reticle);
}

TEST_CASE("starter materializer keeps native float32 order and clamps after byte conversion") {
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.Style.AccentSaturation"] = "0.53";
    snapshot.attributes["Preset.Style.AccentValue"] = "2.75";
    snapshot.attributes["Preset.Style.CamoSaturation"] = "-1";
    snapshot.attributes["Preset.Style.CamoScale"] = "-1";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.mech_appearance.accent_saturation == 52);
    CHECK(value.mech_appearance.accent_value == 19);
    CHECK(value.mech_appearance.upper.camo_saturation == 10);
    CHECK(value.mech_appearance.upper.camo_scale == 7);
}

TEST_CASE("starter materializer rejects nonfinite and overflowing consumed numbers") {
    const std::array<std::pair<std::string_view, std::string_view>, 5> bad = {{
        {"Preset.Style.AccentHue", "nan"},
        {"Preset.Style.AccentSaturation", "inf"},
        {"Preset.Style.AccentValue", "1e30"},
        {"Preset.Style.CamoPattern", "2147483648"},
        {"Preset.Style.CamoScale", "-2147483649"}}};
    for (const auto& [key, text] : bad) {
        auto snapshot = effective_starter();
        snapshot.attributes[std::string(key)] = text;
        CAPTURE(key);
        CHECK_THROWS_WITH_AS(inventory::materialize_starter_mech(snapshot, expected_types(), selection()),
            "Invalid starter mech setup.", std::invalid_argument);
    }
}

TEST_CASE("starter materializer normalizes the nonlocal allocation and upgrade route without opaque aliases") {
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.ProgressionUnlockFlags"] = "2147483647";
    snapshot.attributes["Preset.AllocOpt.Damage"] = "255";
    snapshot.attributes["Preset.Unknown_InstanceId"] = "not-an-owned-reference";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.allocated_optimizations.damage == 0);
    CHECK(value.allocated_optimizations.fire_rate == 0);
    CHECK(value.allocated_optimizations.dodge == 0);
    CHECK(value.mech_weapons.primary_upgrade_flags == 0);
    CHECK(value.mech_weapons.secondary_upgrade_flags == 0);
    CHECK(value.ability_upgrade_flags == 0);
}

TEST_CASE("starter materializer applies B animation fallback and explicit revision wrap") {
    const std::array<std::pair<std::uint8_t, std::uint8_t>, 5> revisions = {{
        {0, 1}, {1, 2}, {253, 254}, {254, 0}, {255, 0}}};
    for (const auto& [previous, expected] : revisions) {
        auto choice = selection();
        choice.previous_setup_id = previous;
        choice.holo_taunt_index = 7;
        const auto value = inventory::materialize_starter_mech(effective_starter(), expected_types(), choice);
        CAPTURE(previous);
        CHECK(value.setup_id == expected);
        CHECK(value.mech_appearance.holo_taunt_type == 7);
        CHECK(value.mech_appearance.anim_taunt_type == 5);
    }
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.Style.AnimTauntType"] = "9";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.mech_appearance.anim_taunt_type == 9);
}

TEST_CASE("starter materializer consumes separate part and decoration override keys") {
    auto snapshot = effective_starter();
    snapshot.attributes["Preset.Style.Mid.CamoPattern"] = "9";
    snapshot.attributes["Preset.Style.Mid.CamoSaturation"] = "2";
    snapshot.attributes["Preset.Style.Mid.CamoScale"] = "1";
    snapshot.attributes["Preset.Style.Low.CamoPattern"] = "8";
    snapshot.attributes["Preset.Style.Low.CamoSaturation"] = "4";
    snapshot.attributes["Preset.Style.Low.CamoScale"] = "5";
    snapshot.attributes["Preset.Style.Arm.CamoPattern"] = "7";
    snapshot.attributes["Preset.Style.Arm.CamoSaturation"] = "6";
    snapshot.attributes["Preset.Style.Arm.CamoScale"] = "3";
    snapshot.attributes["Preset.Style.CockpitDecoration0"] = "12";
    snapshot.attributes["Preset.Style.CockpitDecoration1"] = "13";
    snapshot.attributes["Preset.Style.CockpitDecoration2"] = "14";
    const auto value = inventory::materialize_starter_mech(snapshot, expected_types(), selection());
    CHECK(value.mech_appearance.middle.camo_pattern == 9);
    CHECK(value.mech_appearance.middle.camo_saturation == 2);
    CHECK(value.mech_appearance.middle.camo_scale == 1);
    CHECK(value.mech_appearance.lower.camo_pattern == 8);
    CHECK(value.mech_appearance.lower.camo_saturation == 4);
    CHECK(value.mech_appearance.lower.camo_scale == 5);
    CHECK(value.mech_appearance.arm.camo_pattern == 7);
    CHECK(value.mech_appearance.arm.camo_saturation == 6);
    CHECK(value.mech_appearance.arm.camo_scale == 3);
    CHECK(value.mech_appearance.cockpit_decoration[0] == 12);
    CHECK(value.mech_appearance.cockpit_decoration[1] == 13);
    CHECK(value.mech_appearance.cockpit_decoration[2] == 14);
}
