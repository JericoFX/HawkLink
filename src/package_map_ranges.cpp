#include "hawken/package_map_ranges.hpp"
#include <algorithm>

namespace hawken::wire::packages {
namespace {
constexpr std::uint32_t signed_max=0x7fffffffU;
constexpr std::uint64_t coordinate_limit=0x80000000ULL;

// Only called after generation metadata has been validated.
std::uint32_t negotiated_count(const PackageGenerations& package) {
    const auto& counts=package.generation_net_counts;
    if (package.acknowledged_generation >= counts.size()) return counts.back();
    return std::min(counts.back(),counts[package.acknowledged_generation-1]);
}

void validate_ranges(std::span<const ObjectRange> ranges) {
    std::uint64_t total=0;
    for (const auto& range : ranges) {
        if (range.base != total || range.count > signed_max)
            throw WireError("invalid_ranges");
        total+=range.count;
        if (total > coordinate_limit) throw WireError("invalid_ranges");
    }
}
}

std::vector<ObjectRange> build_ranges(std::span<const PackageGenerations> packages) {
    // Complete validation and checked sizing precede output allocation.
    std::uint64_t total=0;
    for (const auto& package : packages) {
        const auto& counts=package.generation_net_counts;
        if (counts.empty() || counts.size() > signed_max)
            throw WireError("invalid_metadata");
        if (package.acknowledged_generation == 0 ||
            package.acknowledged_generation > signed_max)
            throw WireError("invalid_generation");
        for (const auto count : counts)
            if (count > signed_max) throw WireError("invalid_metadata");
        total+=negotiated_count(package);
        if (total > coordinate_limit) throw WireError("range_overflow");
    }
    std::vector<ObjectRange> result;
    if (packages.size() > result.max_size()) throw WireError("invalid_metadata");
    result.reserve(packages.size());
    std::uint32_t base=0;
    for (const auto& package : packages) {
        const auto count=negotiated_count(package);
        result.push_back({base,count});
        base+=count;
    }
    return result;
}

std::uint32_t encode_index(std::span<const ObjectRange> ranges,
                           std::size_t package_slot, std::int32_t local_net_index) {
    validate_ranges(ranges);
    if (package_slot >= ranges.size() || local_net_index < 0)
        throw WireError("index_unavailable");
    const auto& range=ranges[package_slot];
    const auto local=static_cast<std::uint32_t>(local_net_index);
    if (local >= range.count) throw WireError("index_unavailable");
    return range.base+local;
}

std::optional<Coordinate> decode_index(std::span<const ObjectRange> ranges,
                                       std::uint32_t flat_index) {
    validate_ranges(ranges);
    if (flat_index > signed_max) throw WireError("invalid_value");
    for (std::size_t slot=0;slot<ranges.size();++slot) {
        const auto& range=ranges[slot];
        if (flat_index >= range.base && flat_index-range.base < range.count)
            return Coordinate{slot,flat_index-range.base};
    }
    return std::nullopt;
}
}
