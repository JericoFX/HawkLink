#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

namespace hawken::probe {
struct ControllerSampleComparison {
    bool full_samples_equal=false;
    bool inspected_metadata_equal=false;
    bool name_count_changed=false;
};
// Pure comparison of previously read samples. Only mech mode may accept
// increasing name_count: positive signed32-representable integers, with ALL
// other metadata and the typed-read trace identical. Missing/malformed/count
// decreases reject. Other modes retain strict complete equality.
// Inspection agreement is not atomicity or acceptance of effective field IDs.
inline ControllerSampleComparison compare_controller_samples(
    const nlohmann::json& first, std::span<const std::uint64_t> first_trace,
    const nlohmann::json& second, std::span<const std::uint64_t> second_trace, bool mech_setup) {
    const bool trace_equal=first_trace.size()==second_trace.size() &&
        std::equal(first_trace.begin(),first_trace.end(),second_trace.begin());
    const auto first_count=first.find("name_count");
    const auto second_count=second.find("name_count");
    const bool counts_equal=first_count==first.end() ? second_count==second.end() :
        second_count!=second.end() && *first_count==*second_count;
    ControllerSampleComparison result{first==second && trace_equal,false,!counts_equal};
    if (!mech_setup) {
        result.inspected_metadata_equal=result.full_samples_equal;
        return result;
    }
    const auto valid_count=[](const nlohmann::json& value) {
        if (value.is_number_unsigned()) {
            const auto count=value.get<std::uint64_t>();
            return count>0 && count<=static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
        }
        if (!value.is_number_integer()) return false;
        const auto count=value.get<std::int64_t>();
        return count>0 && count<=std::numeric_limits<std::int32_t>::max();
    };
    if (first_count==first.end() || second_count==second.end() ||
        !valid_count(*first_count) || !valid_count(*second_count) ||
        second_count->get<std::int64_t>()<first_count->get<std::int64_t>()) return result;
    auto first_metadata=first;
    auto second_metadata=second;
    first_metadata.erase("name_count");
    second_metadata.erase("name_count");
    result.inspected_metadata_equal=trace_equal && first_metadata==second_metadata;
    return result;
}
}
