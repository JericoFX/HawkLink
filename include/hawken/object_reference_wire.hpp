#pragma once
#include "hawken/game_wire.hpp"
#include <variant>

namespace hawken::wire::object {
struct PackageExportReference {
    std::uint32_t index;
    bool operator==(const PackageExportReference&) const = default;
};
struct ActorChannelReference {
    std::uint16_t channel;
    bool operator==(const ActorChannelReference&) const = default;
};
using Reference = std::variant<PackageExportReference,ActorChannelReference>;
struct ReferenceResult {
    Reference value;
    std::size_t consumed_bits;
};

// Explicit PRIMARY network-map representation, native9f4080/raw9f3480:
// selector0 + bounded0x80000000 (31 LSB bits), index0..0x7fffffff;
// selector1 + bounded0x800 (11 LSB bits), channel0..2047. Package index0
// is a valid export coordinate; channel0 is null. No alignment/terminator.
// See implementation/evidence/actor-initial-reference/contract.json.
// No automatic selection of the alternative u32/FString map, object lookup,
// expected-class checking, actor creation, world admission or packet wrapping.
//
// Own safety policy: positive caller max_field_bits, with no artificial upper
// ceiling. Write preflights variant/range, complete field32/12bits and absolute
// bit_count overflow before mutation; allocation failure may leave partial
// output. Read consumes ONE prefix at the current cursor, preserving suffix.
// Budget1 is checked BEFORE selector; full32/12 budget BEFORE bounded read.
// A short physical bounded field advances partially, as BitReader does; a
// budget failure after selector leaves that single bit consumed. No rollback.
// Fixed WireError.code: invalid_limit, invalid_value, field_too_large,
// invalid_length, truncated; not_implemented is only the RED scaffold.
std::size_t write_reference(BitWriter& writer, const Reference& value,
                            std::size_t max_field_bits);
ReferenceResult read_reference(BitReader& reader, std::size_t max_field_bits);
}
