#pragma once
#include "hawken/control_wire.hpp"
#include "hawken/object_reference_wire.hpp"
#include <array>
#include <optional>

namespace hawken::wire::mech {
struct SAllocatedOptimization {
    std::uint8_t damage, fire_rate, heal_rate, heat_gen, armor, fuel_capacity;
    std::uint8_t boost_speed, fuel_regen, run_speed, air_efficiency, radar_range;
    std::uint8_t overheat_recovery, dodge;
};
struct PartVisualInfo {
    std::uint8_t camo_pattern, camo_saturation, camo_scale;
    bool b_accent;
};
struct VisualInfo {
    std::uint8_t accent_hue, accent_saturation, accent_value;
    PartVisualInfo upper, middle, lower, arm;
    std::uint8_t lower_part, mid_part, upper_part, arm_part, drone_type;
    std::uint8_t holo_taunt_type, anim_taunt_type, booster_part, armor_part;
    std::array<std::uint8_t,3> cockpit_decoration;
    std::uint8_t primary_reticle, secondary_reticle, friend_hud_color;
    std::uint8_t foe_hud_color, neutral_hud_color;
    bool b_tint_reticle;
};
struct InternalsInfo {
    std::uint8_t internal1, internal2, internal3, internal4, internal5, internal6;
};
struct WeaponInfo {
    std::uint8_t primary_idx, secondary_idx, primary_upgrade_flags, secondary_upgrade_flags;
};
struct SMechSetupStruct {
    SAllocatedOptimization allocated_optimizations;
    VisualInfo mech_appearance;
    InternalsInfo mech_internals;
    WeaponInfo mech_weapons;
    std::uint8_t ability_index, ability_upgrade_flags;
    std::int32_t mech_type, mech_sub_type;
    bool b_ready_for_setup;
    std::uint8_t setup_id;
    std::optional<object::PackageExportReference> template_reference;
};
struct MechSetupPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// Explicit ALL-MEMBERS branch of original SMechSetupStruct and its five child
// types: own Children/Next order, no ancestors, every member eligible, original
// ArrayDim/types unchanged, EAbilities count36. Caller must establish those
// preconditions; this codec performs no metadata/PackageMap/class lookup.
// Original f9020 delegates every array element without nested fieldIDs, RPC
// presence, alignment, lengths or terminators. Byte8, bool1, int32 explicit
// byte order, ability6. Four PartVisualInfo values and CockpitDecoration[3]
// expand61 unique properties to67 scalar elements. Template null is primary
// channel0 (12bits); nonnull is an already mapped package coordinate (32bits).
// Caller must ensure a nonnull coordinate resolves to an R_NPC_Template object,
// not its UClass; no actor-channel, GUID or inventory string input is accepted.
//
// Pure encoder: supplied values are not a generated preset or native defaults.
// No outer PRI fieldID/bound, actor/send/admission/ready policy or lifecycle is
// added. Entire output is544bits with null,564 with mapped Template. Positive
// budget includes all bits. Own wire policy rejects ability>63 (no invented
// <=35 enum-value check), coordinates>0x7fffffff and invalid endian. No partial
// payload is returned. Unused high bits are zero, with no packet marker.
// WireError.code: invalid_limit, invalid_value, field_too_large.
// Evidence: implementation/evidence/pri-mech-setup/headless-format-followup.
MechSetupPayload encode_mech_setup(const SMechSetupStruct& value,
    control::Endian integer_order, std::size_t max_payload_bits);
}
