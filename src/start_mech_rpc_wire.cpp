#include "hawken/start_mech_rpc_wire.hpp"
#include <array>
#include <bit>
#include <new>
#include <stdexcept>
#include <utility>

namespace hawken::wire::rpc {
StartMechArgumentsResult read_start_mech_arguments(BitReader& reader,
    control::Endian integer_order, std::size_t max_argument_bits) {
    if (max_argument_bits==0) throw WireError("invalid_limit");
    if (integer_order!=control::Endian::little && integer_order!=control::Endian::big)
        throw WireError("invalid_value");

    auto cursor=reader;
    const auto start=cursor.position();
    const auto require_bits=[&](std::size_t count) {
        const auto used=cursor.position()-start;
        if (count>max_argument_bits-used) throw WireError("field_too_large");
    };
    const auto present=[&] {
        require_bits(1);
        return cursor.read_bit();
    };
    const auto scalar=[&] {
        require_bits(32);
        const auto bytes=cursor.read_bits(32);
        std::uint32_t value=0;
        for (std::size_t i=0;i<4;++i) {
            const auto shift=integer_order==control::Endian::little?i:3-i;
            value|=static_cast<std::uint32_t>(bytes[i])<<(shift*8);
        }
        return std::bit_cast<std::int32_t>(value);
    };

    try {
        StartMechArguments value;
        if (present()) {
            const auto count=scalar();
            if (count!=0 && count!=1 && count!=-1 && count!=37 && count!=-37)
                throw WireError("invalid_value");
            const bool wide=count<0;
            const std::size_t units=count==0?0:(count==1 || count==-1?1:37);
            const std::size_t unit_bytes=wide?2:1;
            require_bits(units*unit_bytes*8);
            const auto bytes=cursor.read_bits(units*unit_bytes*8);
            const auto unit=[&](std::size_t index) {
                auto result=static_cast<std::uint16_t>(bytes[index*unit_bytes]);
                if (wide) result=static_cast<std::uint16_t>(result |
                    (static_cast<std::uint16_t>(bytes[index*unit_bytes+1])<<8));
                return result;
            };
            if (units!=0 && unit(units-1)!=0) throw WireError("invalid_value");
            if (units==37) {
                std::array<char,36> normalized{};
                for (std::size_t i=0;i<normalized.size();++i) {
                    const auto character=unit(i);
                    if (i==8 || i==13 || i==18 || i==23) {
                        if (character!='-') throw WireError("invalid_value");
                    } else if (!((character>='0' && character<='9') ||
                                 (character>='a' && character<='f') ||
                                 (character>='A' && character<='F'))) {
                        throw WireError("invalid_value");
                    }
                    normalized[i]=static_cast<char>(character>='A' && character<='F'
                        ?character+('a'-'A'):character);
                }
                value.mech_instance_id.assign(normalized.data(),normalized.size());
            }
        }
        if (present()) value.primary_weapon=scalar();
        if (present()) value.holo_taunt_type=scalar();
        const auto consumed=cursor.position()-start;
        reader=cursor;
        return {std::move(value),consumed};
    } catch (const std::bad_alloc&) {
        throw WireError("resource_limit");
    } catch (const std::length_error&) {
        throw WireError("resource_limit");
    }
}
}
