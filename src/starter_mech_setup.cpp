#include "hawken/starter_mech_setup.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace hawken::inventory {
namespace {
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid starter mech setup.");
}

char lower_ascii(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool same_key(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](char a, char b) { return lower_ascii(a) == lower_ascii(b); });
}

const std::string* attribute(const store::ItemAttributes& attributes, std::string_view key) {
    const std::string* found = nullptr;
    for (const auto& [name, value] : attributes) {
        if (!same_key(name, key)) continue;
        if (found && *found != value) invalid();
        found = &value;
    }
    return found;
}

std::string_view required(const store::ItemAttributes& attributes, std::string_view key) {
    const auto* value = attribute(attributes, key);
    if (!value) invalid();
    return *value;
}

std::string_view skip_space(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
        value.front() == '\n' || value.front() == '\r' || value.front() == '\f' ||
        value.front() == '\v')) value.remove_prefix(1);
    return value;
}

// Native normal decimal-prefix semantics, with checked int32 overflow as local policy.
std::int32_t integer(std::string_view text) {
    text = skip_space(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const std::uint32_t limit = negative ? 2147483648u : 2147483647u;
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') break;
        const auto digit = static_cast<std::uint32_t>(c - '0');
        if (value > (limit - digit) / 10u) invalid();
        value = value * 10u + digit;
    }
    if (negative && value == 2147483648u) return std::numeric_limits<std::int32_t>::min();
    const auto result = static_cast<std::int32_t>(value);
    return negative ? -result : result;
}

std::int32_t integer_attribute(const store::ItemAttributes& attributes,
    std::string_view key, std::int32_t fallback) {
    const auto* value = attribute(attributes, key);
    return value ? integer(*value) : fallback;
}

float decimal_attribute(const store::ItemAttributes& attributes,
    std::string_view key, std::string_view fallback) {
    const auto* value = attribute(attributes, key);
    auto text = skip_space(value ? std::string_view(*value) : fallback);
    if (!text.empty() && text.front() == '+' &&
        (text.size() == 1 || (text[1] != '+' && text[1] != '-'))) text.remove_prefix(1);
    double number = 0;
    if (!text.empty()) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), number,
            std::chars_format::general);
        if (result.ec == std::errc::result_out_of_range) invalid();
        if (result.ec == std::errc::invalid_argument) number = 0;
    }
    if (!std::isfinite(number) || number > std::numeric_limits<float>::max() ||
        number < -std::numeric_limits<float>::max()) invalid();
    const auto converted = static_cast<float>(number);
    if (!std::isfinite(converted)) invalid();
    return converted;
}

std::uint8_t byte(std::int32_t value) {
    return static_cast<std::uint8_t>(static_cast<std::uint32_t>(value));
}

std::uint8_t float_byte(float value) {
    // Check the truncated value before conversion, so negative fractions remain safe.
    const auto truncated = std::trunc(static_cast<double>(value));
    if (!std::isfinite(truncated) || truncated < std::numeric_limits<std::int32_t>::min() ||
        truncated > std::numeric_limits<std::int32_t>::max()) invalid();
    return byte(static_cast<std::int32_t>(truncated));
}

std::uint8_t capped_byte(std::int32_t value, unsigned maximum) {
    return static_cast<std::uint8_t>(std::min(static_cast<unsigned>(byte(value)), maximum));
}

bool bit(std::int32_t value) {
    return (static_cast<std::uint32_t>(value) & 1u) != 0;
}

bool canonical_guid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}

void validate_roles(const store::StarterMechSnapshot& effective,
    const StarterCatalogueIds& expected) {
    const std::array<std::string_view, 9> types = {expected.mech, expected.primary,
        expected.secondary, expected.upper, expected.middle, expected.arms,
        expected.lower, expected.drone, expected.ability};
    std::array<std::string_view, 18> identities;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (!canonical_guid(types[i]) || effective.items[i].type_id != types[i] ||
            !canonical_guid(effective.items[i].instance_id)) invalid();
        identities[i] = types[i];
        identities[i + types.size()] = effective.items[i].instance_id;
    }
    for (std::size_t i = 0; i < identities.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (identities[i] == identities[j]) invalid();

    const std::array<std::string_view, 8> index_keys = {"Preset.PrimaryWeapon_0",
        "Preset.SecondaryWeapon", "Preset.Style.UpperPart", "Preset.Style.MidPart",
        "Preset.Style.ArmPart", "Preset.Style.LowerPart", "Preset.Style.DroneType", "Preset.Ability"};
    const std::array<std::int32_t, 8> indices = {0, 1, 10, 10, 10, 10, 0, 3};
    const std::array<std::string_view, 8> reference_keys = {"Preset.PrimaryWeapon_InstanceId_0",
        "Preset.SecondaryWeapon_InstanceId", "Preset.Style.UpperPart_InstanceId",
        "Preset.Style.MidPart_InstanceId", "Preset.Style.ArmPart_InstanceId",
        "Preset.Style.LowerPart_InstanceId", "Preset.Style.DroneType_InstanceId", "Preset.Ability_InstanceId"};
    for (std::size_t i = 0; i < index_keys.size(); ++i) {
        if (integer(required(effective.attributes, index_keys[i])) != indices[i] ||
            required(effective.attributes, reference_keys[i]) != effective.items[i + 1].instance_id)
            invalid();
    }
}

wire::mech::PartVisualInfo part(const store::ItemAttributes& attributes,
    std::string_view pattern, std::string_view saturation, std::string_view scale,
    std::string_view accent, std::uint8_t inherited_pattern) {
    return {capped_byte(integer_attribute(attributes, pattern, inherited_pattern), 500),
        capped_byte(integer_attribute(attributes, saturation, 7), 10),
        capped_byte(integer_attribute(attributes, scale, 3), 7),
        bit(integer_attribute(attributes, accent, 1))};
}
}

wire::mech::SMechSetupStruct materialize_starter_mech(
    const store::StarterMechSnapshot& effective,
    const StarterCatalogueIds& expected,
    const StarterMechSelection& selection) {
    if (!selection.loaded_completion || selection.prior_template_reference ||
        selection.requested_primary_index != 0) invalid();
    validate_roles(effective, expected);
    const auto& attributes = effective.attributes;
    wire::mech::SMechSetupStruct result{};
    // Completed nonlocal XP normalization and the concrete bound upgrade getter yield zero.
    result.allocated_optimizations = {};
    auto& appearance = result.mech_appearance;
    const float hue = decimal_attribute(attributes, "Preset.Style.AccentHue", "80.0") * 0.0027777778f;
    appearance.accent_hue = float_byte(hue * 255.0f);
    appearance.accent_saturation = capped_byte(float_byte(decimal_attribute(attributes,
        "Preset.Style.AccentSaturation", "0.6") * 100.0f), 100);
    appearance.accent_value = capped_byte(float_byte(decimal_attribute(attributes,
        "Preset.Style.AccentValue", "0.25") * 100.0f), 100);
    appearance.upper = part(attributes, "Preset.Style.CamoPattern", "Preset.Style.CamoSaturation",
        "Preset.Style.CamoScale", "Preset.Style.AccentUpper", 0);
    appearance.middle = part(attributes, "Preset.Style.Mid.CamoPattern", "Preset.Style.Mid.CamoSaturation",
        "Preset.Style.Mid.CamoScale", "Preset.Style.AccentMid", appearance.upper.camo_pattern);
    appearance.lower = part(attributes, "Preset.Style.Low.CamoPattern", "Preset.Style.Low.CamoSaturation",
        "Preset.Style.Low.CamoScale", "Preset.Style.AccentLower", appearance.upper.camo_pattern);
    appearance.arm = part(attributes, "Preset.Style.Arm.CamoPattern", "Preset.Style.Arm.CamoSaturation",
        "Preset.Style.Arm.CamoScale", "Preset.Style.AccentArms", appearance.upper.camo_pattern);
    appearance.lower_part = 10;
    appearance.mid_part = 10;
    appearance.upper_part = 10;
    appearance.arm_part = 10;
    appearance.drone_type = 0;
    appearance.holo_taunt_type = selection.holo_taunt_index;
    appearance.anim_taunt_type = byte(integer_attribute(attributes, "Preset.Style.AnimTauntType", -1));
    if (appearance.anim_taunt_type == 255) appearance.anim_taunt_type = 5;
    appearance.booster_part = byte(integer_attribute(attributes, "Preset.Style.BoosterPart", -1));
    appearance.armor_part = byte(integer_attribute(attributes, "Preset.Style.ArmorPart", -1));
    appearance.cockpit_decoration = {
        byte(integer_attribute(attributes, "Preset.Style.CockpitDecoration0", -1)),
        byte(integer_attribute(attributes, "Preset.Style.CockpitDecoration1", -1)),
        byte(integer_attribute(attributes, "Preset.Style.CockpitDecoration2", -1))};
    appearance.primary_reticle = byte(integer_attribute(attributes, "Preset.VisorStyle.PrimaryReticle", -1));
    appearance.secondary_reticle = byte(integer_attribute(attributes, "Preset.VisorStyle.SecondaryReticle", -1));
    appearance.friend_hud_color = byte(integer_attribute(attributes, "Preset.VisorStyle.FriendHUDColor", -1));
    appearance.foe_hud_color = byte(integer_attribute(attributes, "Preset.VisorStyle.FoeHUDColor", -1));
    appearance.neutral_hud_color = byte(integer_attribute(attributes, "Preset.VisorStyle.NeutralHUDColor", -1));
    appearance.b_tint_reticle = bit(integer_attribute(attributes, "Preset.VisorStyle.bTintReticle", 0));

    const std::array<std::string_view, 6> internal_keys = {"Preset.MechInternals.Internal1",
        "Preset.MechInternals.Internal2", "Preset.MechInternals.Internal3",
        "Preset.MechInternals.Internal4", "Preset.MechInternals.Internal5", "Preset.MechInternals.Internal6"};
    for (const auto key : internal_keys) {
        const auto index = integer_attribute(attributes, key, -1);
        if (index != -1 && index != 255) invalid();
    }
    result.mech_internals = {255, 255, 255, 255, 255, 255};
    result.mech_weapons = {0, 101, 0, 0};
    result.ability_index = 3;
    result.ability_upgrade_flags = 0;
    result.mech_type = 1;
    result.mech_sub_type = 15;
    result.b_ready_for_setup = true;
    result.setup_id = byte(static_cast<std::int32_t>(selection.previous_setup_id) + 1);
    if (result.setup_id == 255) result.setup_id = 0;
    result.template_reference = std::nullopt;
    return result;
}
}
