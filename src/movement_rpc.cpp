#include "hawken/movement_rpc.hpp"
#include <bit>
#include <cmath>

namespace hawken::wire::movement {
Result read_rpc(BitReader& reader,control::Endian integer_order,
    const Eligibility& eligibility,std::size_t max_rpc_bits) {
    if (max_rpc_bits==0) throw WireError("invalid_limit");
    if ((integer_order!=control::Endian::little && integer_order!=control::Endian::big) ||
        !eligibility.primary_package_map || !eligibility.controller_cache_correct ||
        !eligibility.parameter_order_correct || !eligibility.all_parameters_eligible)
        throw WireError("invalid_value");

    auto cursor=reader;
    const auto start=cursor.position();
    const auto require_bits=[&](std::size_t count) {
        const auto used=cursor.position()-start;
        if (used>max_rpc_bits || count>max_rpc_bits-used) throw WireError("field_too_large");
    };
    const auto presence=[&] {
        require_bits(1);
        return cursor.read_bit();
    };
    const auto raw32=[&] {
        require_bits(32);
        std::uint32_t storage=0;
        for (std::size_t bit=0;bit<32;++bit) {
            const auto shift=integer_order==control::Endian::little
                ? bit : (3-bit/8)*8+bit%8;
            if (cursor.read_bit()) storage|=std::uint32_t{1}<<shift;
        }
        return storage;
    };
    const auto timestamp=[&] {
        if (!presence()) return 0.0F;
        const auto value=std::bit_cast<float>(raw32());
        if (!std::isfinite(value)) throw WireError("invalid_value");
        return value;
    };
    const auto byte=[&]() -> std::uint8_t {
        if (!presence()) return 0;
        require_bits(8);
        std::uint8_t value=0;
        for (unsigned bit=0;bit<8;++bit)
            if (cursor.read_bit()) value|=static_cast<std::uint8_t>(1U<<bit);
        return value;
    };
    const auto integer=[&]() -> std::int32_t {
        return presence()?std::bit_cast<std::int32_t>(raw32()):0;
    };
    const auto vector=[&] {
        if (!presence()) return actor::QuantizedPosition{};
        require_bits(1);
        return actor::read_position(cursor,max_rpc_bits-(cursor.position()-start)).value;
    };

    const auto field=cursor.read_bounded(294);
    require_bits(0);
    Result result{};
    if (field==59) {
        result.kind=Kind::dual_server_move;
        Move pending;
        pending.time_stamp=timestamp();pending.packed_acceleration=vector();
        pending.move_flags=byte();pending.view=integer();
        pending.jump=(pending.move_flags&0x20U)!=0;
        result.pending=pending;
    } else if (field==60) result.kind=Kind::old_server_move;
    else if (field==61) result.kind=Kind::server_move;
    else throw WireError("invalid_value");

    result.current.time_stamp=timestamp();
    if (result.kind==Kind::old_server_move) {
        for (auto& component:result.old_acceleration) component=byte();
        result.current.move_flags=byte();
    } else {
        result.current.packed_acceleration=vector();result.current.client_location=vector();
        result.current.move_flags=byte();result.current.client_roll=byte();result.current.view=integer();
    }
    result.current.jump=(result.current.move_flags&0x20U)!=0;
    result.consumed_bits=cursor.position()-start;
    reader=cursor;
    return result;
}
}
