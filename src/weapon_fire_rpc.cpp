#include "hawken/weapon_fire_rpc.hpp"

namespace hawken::game::weapon_fire {
Result read_rpc(wire::BitReader& reader,const Schema& schema,std::size_t max_rpc_bits) {
    if (schema.exclusive_bound==0 || max_rpc_bits==0)
        throw wire::WireError("invalid_limit");
    if (!schema.primary_package_map || !schema.weapon_cache_correct ||
        !schema.parameter_order_correct || !schema.all_parameters_eligible ||
        schema.start_field_id>=schema.exclusive_bound ||
        schema.stop_field_id>=schema.exclusive_bound ||
        schema.start_field_id==schema.stop_field_id ||
        (schema.extended_stop_field_id &&
         (*schema.extended_stop_field_id>=schema.exclusive_bound ||
          *schema.extended_stop_field_id==schema.start_field_id ||
          *schema.extended_stop_field_id==schema.stop_field_id)))
        throw wire::WireError("invalid_value");

    auto cursor=reader;
    const auto start=cursor.position();
    const auto require_bits=[&](std::size_t count) {
        const auto used=cursor.position()-start;
        if (used>max_rpc_bits || count>max_rpc_bits-used)
            throw wire::WireError("field_too_large");
    };
    const auto field=cursor.read_bounded(schema.exclusive_bound);
    require_bits(0);
    Result result{};
    if (field==schema.start_field_id) result.kind=Kind::start_fire;
    else if (field==schema.stop_field_id) result.kind=Kind::stop_fire;
    else if (schema.extended_stop_field_id && field==*schema.extended_stop_field_id)
        result.kind=Kind::stop_fire_extended;
    else throw wire::WireError("invalid_value");

    require_bits(1);
    if (cursor.read_bit()) {
        require_bits(8);
        for (unsigned bit=0;bit<8;++bit)
            if (cursor.read_bit()) result.fire_mode|=static_cast<std::uint8_t>(1U<<bit);
    }
    if (result.kind==Kind::stop_fire_extended) {
        require_bits(1);
        result.recent_fire=cursor.read_bit();
    }
    result.consumed_bits=cursor.position()-start;
    reader=cursor;
    return result;
}
}
