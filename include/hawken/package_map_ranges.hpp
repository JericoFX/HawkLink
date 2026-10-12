#pragma once
#include "hawken/game_wire.hpp"
#include <optional>

namespace hawken::wire::packages {
struct PackageGenerations {
    std::vector<std::uint32_t> generation_net_counts;
    std::uint32_t acknowledged_generation;
};
struct ObjectRange {
    std::uint32_t base;
    std::uint32_t count;
    bool operator==(const ObjectRange&) const = default;
};
struct Coordinate {
    std::size_t package_slot;
    std::uint32_t local_net_index;
    bool operator==(const Coordinate&) const = default;
};

// PRIMARY network-object coordinates, not UPK export ordinals. Native6f060
// uses descriptor+20 base/+24 negotiated count, +28 local generation and
// +2c peer generation. This API uses local generation L=counts.size(), as
// our Package7 publisher does. For positive R: count=G[L-1] if R>=L,
// otherwise min_signed(G[L-1],G[R-1]); no monotonicity assumption.
// See implementation/evidence/package-map-ranges/contract.json.
// Caller supplies published order and already identity-matched ACKs. No
// GUID/name lookup, UObject resolution, automatic map selection or world
// admission. A different caller may require ACK R==L; this arithmetic
// utility also represents the native-compatible R>L case explicitly.
//
// Own checked policy, stronger than native wrapping/log-only addition:
// L and R are1..INT32_MAX, every count is0..INT32_MAX; cumulative total
// is<=0x80000000. Validate all metadata before output allocation. Empty
// input is allowed as arithmetic, not a sufficient package-set claim.
// Public ranges must be canonical cumulative bases with the same count
// and total bounds; zero counts are retained and inverse lookup skips them.
// Negative local indices (including native sentinel-1), invalid slots and
// local>=count throw index_unavailable. Flat0 is valid when a nonzero range
// contains it; a valid31-bit flat outside the ranges returns nullopt.
// Errors never include package names, GUIDs or original values. Fixed codes:
// invalid_metadata, invalid_generation, range_overflow, invalid_ranges,
// index_unavailable, invalid_value; not_implemented only for RED scaffolding.
std::vector<ObjectRange> build_ranges(std::span<const PackageGenerations> packages);
std::uint32_t encode_index(std::span<const ObjectRange> ranges,
                           std::size_t package_slot, std::int32_t local_net_index);
std::optional<Coordinate> decode_index(std::span<const ObjectRange> ranges,
                                       std::uint32_t flat_index);
}
