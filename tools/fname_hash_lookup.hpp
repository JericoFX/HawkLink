#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hawken::probe {
struct FNameLookupSnapshot {
    std::array<std::uint32_t,2> indices{};
    std::int32_t name_count=0;
    std::size_t scanned_entries=0,head_count=0;
    std::vector<std::uint64_t> trace;
};
namespace fname_detail {
[[noreturn]] inline void fail(const char* code) { throw std::runtime_error(code); }
inline std::uint64_t address(std::uint64_t base,std::uint64_t offset,std::uint64_t width) {
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    if (!base || offset>maximum-base || !width || width-1>maximum-(base+offset))
        fail("player_name_invalid");
    return base+offset;
}
// Native eab70/9e90 for ASCII only. The table comes from the live module, not a
// guessed CRC polynomial. UTF16 includes a zero high byte for EACH ASCII unit.
inline std::uint32_t ascii_hash(const std::array<std::uint32_t,256>& table,
                               std::string_view text,bool wide) {
    std::uint32_t hash=0;
    for (const auto character:text) {
        auto unit=static_cast<unsigned char>(character);
        if (!unit || unit>127) fail("player_name_invalid");
        if (unit>='a' && unit<='z') unit=static_cast<unsigned char>(unit-32);
        hash=table[(hash^unit)&255]^(hash>>8);
        if (wide) hash=table[hash&255]^(hash>>8);
    }
    return hash;
}
}
// Read-only observer policy: exact spelling of TWO fixed ASCII names, both
// stored encodings. Native ASCII construction itself chooses ANSI. At most
// four direct heads, 256 nodes/head; no whole GNames pool or remote invocation.
// Trace includes the FULL table, selected heads, headers, directed index slots
// and inspected comparison units. Caller compares two complete lookups and owns
// the shared attempted-read budget; no cache skips the second lookup.
template<class Reader>
FNameLookupSnapshot read_player_fname_targets(Reader& reader,std::uint64_t module,
                                             FNameLookupSnapshot* progress=nullptr) {
    FNameLookupSnapshot local;
    auto& result=progress ? *progress : local;
    result={};
    const auto read=[&]<class T>(std::uint64_t base,std::uint64_t offset=0) {
        const auto location=fname_detail::address(base,offset,sizeof(T));
        const auto value=reader.template read<T>(location);
        result.trace.push_back(location);
        result.trace.push_back(static_cast<std::uint64_t>(value));
        return value;
    };
    const auto pool=read.template operator()<std::uint64_t>(module,0x1cee4b8);
    result.name_count=read.template operator()<std::int32_t>(module,0x1cee4c0);
    if (!pool || result.name_count<=0 || result.name_count>1048576)
        fname_detail::fail("names_unavailable");
    const auto table_address=fname_detail::address(module,0x1bee030,1024);
    const auto table=reader.template read<std::array<std::uint32_t,256>>(table_address);
    result.trace.push_back(table_address);
    result.trace.insert(result.trace.end(),table.begin(),table.end());
    if (std::all_of(table.begin(),table.end(),[](auto value){return value==0;}))
        fname_detail::fail("player_names_uninitialized");
    constexpr std::array<std::string_view,2> targets={"HawkenGameContent","R_PawnContent"};
    std::array<std::array<std::uint16_t,2>,2> target_buckets{};
    std::vector<std::uint16_t> unique_buckets;
    for (std::size_t target=0;target<targets.size();++target)
        for (std::size_t encoding=0;encoding<2;++encoding) {
            const auto bucket=static_cast<std::uint16_t>(fname_detail::ascii_hash(table,targets[target],encoding!=0));
            target_buckets[target][encoding]=bucket;
            if (std::find(unique_buckets.begin(),unique_buckets.end(),bucket)==unique_buckets.end())
                unique_buckets.push_back(bucket);
        }
    result.head_count=unique_buckets.size();
    std::array<std::optional<std::uint32_t>,2> found{};
    for (const auto bucket:unique_buckets) {
        auto entry=read.template operator()<std::uint64_t>(module,0x1bee430+static_cast<std::uint64_t>(bucket)*8);
        std::vector<std::uint64_t> seen;
        while (entry) {
            if (std::find(seen.begin(),seen.end(),entry)!=seen.end()) fname_detail::fail("player_name_cycle");
            if (seen.size()==256) fname_detail::fail("player_name_limit");
            seen.push_back(entry);
            ++result.scanned_entries;
            const auto packed=read.template operator()<std::uint32_t>(entry,8);
            const auto next=read.template operator()<std::uint64_t>(entry,0xc);
            // Native signed arithmetic >>1: reject the negative range first,
            // then unsigned division reproduces the nonnegative index exactly.
            if (packed&0x80000000U) fname_detail::fail("player_name_invalid");
            const auto index=packed>>1;
            if (index>=static_cast<std::uint32_t>(result.name_count) ||
                read.template operator()<std::uint64_t>(pool,static_cast<std::uint64_t>(index)*8)!=entry)
                fname_detail::fail("player_name_invalid");
            const bool wide=(packed&1)!=0;
            for (std::size_t target=0;target<targets.size();++target) {
                if (target_buckets[target][wide ? 1 : 0]!=bucket) continue;
                bool exact=true;
                for (std::size_t j=0;j<=targets[target].size();++j) {
                    const auto unit=wide ? read.template operator()<std::uint16_t>(entry,0x14+j*2) :
                        static_cast<std::uint16_t>(read.template operator()<std::uint8_t>(entry,0x14+j));
                    const auto expected=j==targets[target].size() ? 0 : static_cast<unsigned char>(targets[target][j]);
                    if (unit!=expected) { exact=false; break; }
                }
                if (exact) {
                    if (found[target]) fname_detail::fail("player_names_ambiguous");
                    found[target]=index;
                }
            }
            entry=next;
        }
    }
    if (!found[0] || !found[1]) fname_detail::fail("player_names_absent");
    if (read.template operator()<std::uint64_t>(module,0x1cee4b8)!=pool ||
        read.template operator()<std::int32_t>(module,0x1cee4c0)!=result.name_count)
        fname_detail::fail("unstable_sample");
    result.indices={*found[0],*found[1]};
    return result;
}
}
