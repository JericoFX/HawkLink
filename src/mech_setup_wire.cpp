#include "hawken/mech_setup_wire.hpp"

namespace hawken::wire::mech {
namespace {
template<std::size_t Count>
void write_bytes(BitWriter& writer, const std::array<std::uint8_t,Count>& bytes) {
    writer.write_bits(bytes,Count*8);
}
void write_byte(BitWriter& writer, std::uint8_t value) {
    write_bytes(writer,std::array{value});
}
void write_part(BitWriter& writer, const PartVisualInfo& value) {
    write_bytes(writer,std::array{value.camo_pattern,value.camo_saturation,value.camo_scale});
    writer.write_bit(value.b_accent);
}
void write_int(BitWriter& writer, std::int32_t value, control::Endian order) {
    const auto bits=static_cast<std::uint32_t>(value);
    std::array<std::uint8_t,4> bytes{};
    for (std::size_t index=0; index<bytes.size(); ++index) {
        const auto shift=(order==control::Endian::little ? index : 3-index)*8;
        bytes[index]=static_cast<std::uint8_t>(bits>>shift);
    }
    write_bytes(writer,bytes);
}
}

MechSetupPayload encode_mech_setup(const SMechSetupStruct& value,
    control::Endian integer_order, std::size_t max_payload_bits) {
    if (max_payload_bits==0) throw WireError("invalid_limit");
    if (integer_order!=control::Endian::little && integer_order!=control::Endian::big)
        throw WireError("invalid_value");
    if (value.ability_index>63 ||
        (value.template_reference && value.template_reference->index>0x7fffffffU))
        throw WireError("invalid_value");
    const std::size_t required_bits=value.template_reference ? 564 : 544;
    if (required_bits>max_payload_bits) throw WireError("field_too_large");

    BitWriter writer;
    const auto& opt=value.allocated_optimizations;
    write_bytes(writer,std::array{opt.damage,opt.fire_rate,opt.heal_rate,opt.heat_gen,
        opt.armor,opt.fuel_capacity,opt.boost_speed,opt.fuel_regen,opt.run_speed,
        opt.air_efficiency,opt.radar_range,opt.overheat_recovery,opt.dodge});

    const auto& visual=value.mech_appearance;
    write_bytes(writer,std::array{visual.accent_hue,visual.accent_saturation,visual.accent_value});
    write_part(writer,visual.upper);
    write_part(writer,visual.middle);
    write_part(writer,visual.lower);
    write_part(writer,visual.arm);
    write_bytes(writer,std::array{visual.lower_part,visual.mid_part,visual.upper_part,
        visual.arm_part,visual.drone_type,visual.holo_taunt_type,visual.anim_taunt_type,
        visual.booster_part,visual.armor_part});
    write_bytes(writer,visual.cockpit_decoration);
    write_bytes(writer,std::array{visual.primary_reticle,visual.secondary_reticle,
        visual.friend_hud_color,visual.foe_hud_color,visual.neutral_hud_color});
    writer.write_bit(visual.b_tint_reticle);

    const auto& internals=value.mech_internals;
    write_bytes(writer,std::array{internals.internal1,internals.internal2,internals.internal3,
        internals.internal4,internals.internal5,internals.internal6});
    const auto& weapons=value.mech_weapons;
    write_bytes(writer,std::array{weapons.primary_idx,weapons.secondary_idx,
        weapons.primary_upgrade_flags,weapons.secondary_upgrade_flags});

    writer.write_bits(std::array{value.ability_index},6);
    write_byte(writer,value.ability_upgrade_flags);
    write_int(writer,value.mech_type,integer_order);
    write_int(writer,value.mech_sub_type,integer_order);
    writer.write_bit(value.b_ready_for_setup);
    write_byte(writer,value.setup_id);

    const object::Reference reference=value.template_reference
        ? object::Reference{*value.template_reference}
        : object::Reference{object::ActorChannelReference{0}};
    object::write_reference(writer,reference,value.template_reference ? 32 : 12);
    return {writer.payload_bytes(),writer.bit_count()};
}
}
