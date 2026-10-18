#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hawken::probe::native_host {
// Caller has independently verified the original EXE SHA256
// 4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2.
// Exact image size is 0x201e000; no configured target RVAs are accepted.
struct OriginalModule {
    std::uint64_t base=0;
    std::uint32_t size=0;
};
using ReadMemory=std::function<bool(std::uint64_t,std::span<std::byte>)>;
struct Target {
    // Internal research values, not a logging/serialization API. function_address
    // is checked through +0xf0 before return so the caller can derive that slot.
    std::uint64_t function_address=0;
    std::uint64_t original_native_address=0;
    std::uint32_t function_flags=0;
    std::uint16_t native_token=0;
    // Internal complete read trace for independent sample comparisons.
    std::vector<std::uint64_t> trace;
    bool operator==(const Target&) const=default;
};
// Read-only exact Engine.WorldInfo.IsClientOnlyBuild lookup. Reuses bounded
// FName/object hashes, requires Core.Function/UClass/package identities,
// FName number zero, uniqueness, no pending kill, native flag 0x400 and
// original target module+0x400560. Native token WORD +0xd4 must be zero as an
// EXPERIMENT SAFETY PREREQUISITE; the original runtime value is not yet proven.
// Owns a hard 1MiB attempted-read budget and 256-node object-chain limit;
// source memory is never written. Each invocation samples independently.
// Invalid/unavailable/ambiguous input throws std::runtime_error, never a target.
Target locate_is_client_only_build(const ReadMemory& read,OriginalModule module);
}
