#include "native_host_lookup.hpp"
#include "fname_hash_lookup.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace hawken::probe::native_host {
namespace {
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }
class MemoryReader {
public:
    MemoryReader(const ReadMemory& source,std::vector<std::uint64_t>& trace)
        : source_(source),trace_(trace) {}
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        static_assert(std::is_trivially_copyable_v<T>);
        constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
        if (!base || offset>maximum-base || sizeof(T)-1>maximum-(base+offset))
            fail("native_host_invalid");
        if (sizeof(T)>budget-used_) fail("native_host_limit");
        used_+=sizeof(T); // Charge attempted reads, including an unavailable read.
        const auto address=base+offset;
        std::array<std::byte,sizeof(T)> bytes{};
        if (!source_(address,bytes)) fail("native_host_unavailable");
        trace_.insert(trace_.end(),{address,sizeof(T)});
        for (std::size_t i=0;i<bytes.size();i+=8) {
            std::uint64_t word=0;
            for (std::size_t j=0;j<8 && i+j<bytes.size();++j)
                word|=std::to_integer<std::uint64_t>(bytes[i+j])<<(j*8);
            trace_.push_back(word);
        }
        T value{};
        std::memcpy(&value,bytes.data(),sizeof(T));
        return value;
    }
private:
    static constexpr std::size_t budget=1024*1024;
    const ReadMemory& source_;
    std::vector<std::uint64_t>& trace_;
    std::size_t used_=0;
};
// The original hash selects a bounded chain by name and optional Outer, never
// a whole object-table scan. Inspect the complete chain to prove uniqueness.
std::uint64_t object_lookup(MemoryReader& reader,std::uint64_t module,
    std::uint32_t name,std::uint64_t outer,std::uint64_t required_type) {
    const auto bucket=(name^static_cast<std::uint32_t>(outer>>4))&0x7fff;
    auto object=reader.read<std::uint64_t>(module,
        (outer ? 0x1cae4b0ULL : 0x1c6e4b0ULL)+static_cast<std::uint64_t>(bucket)*8);
    std::array<std::uint64_t,256> visited{};
    std::size_t count=0;
    std::uint64_t found=0;
    while (object) {
        if (std::find(visited.begin(),visited.begin()+count,object)!=visited.begin()+count)
            fail("native_host_cycle");
        if (count==visited.size()) fail("native_host_limit");
        visited[count++]=object;
        const auto next=reader.read<std::uint64_t>(object,outer ? 0x18 : 8);
        if (reader.read<std::int32_t>(object,0x38)>=0 &&
            reader.read<std::array<std::uint32_t,2>>(object,0x48)==
                std::array<std::uint32_t,2>{name,0}) {
            if ((reader.read<std::uint64_t>(object,0x10)&0x40000000000ULL)!=0 ||
                reader.read<std::uint64_t>(object,0x40)!=outer)
                fail("native_host_identity");
            const auto type=reader.read<std::uint64_t>(object,0x50);
            if (!type || (required_type && type!=required_type)) fail("native_host_identity");
            if (found) fail("native_host_ambiguous");
            found=object;
        }
        object=next;
    }
    if (!found) fail("native_host_missing");
    return found;
}
}

Target locate_is_client_only_build(const ReadMemory& read,OriginalModule module) {
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    if (!read || !module.base || module.size!=0x201e000 || module.base>maximum-module.size)
        fail("native_host_invalid");
    Target result;
    MemoryReader reader(read,result.trace);
    const auto pool=reader.read<std::uint64_t>(module.base,0x1cee4b8);
    const auto name_count=reader.read<std::int32_t>(module.base,0x1cee4c0);
    const auto names=[&](std::string_view first,std::string_view second) {
        const auto value=read_fixed_fname_targets(reader,module.base,
            std::array<std::string_view,2>{first,second});
        if (value.name_count!=name_count) fail("unstable_sample");
        return value.indices;
    };
    const auto engine_world=names("Engine","WorldInfo");
    const auto core_class=names("Core","Class");
    const auto function_package=names("Function","Package");
    const auto target_engine=names("IsClientOnlyBuild","Engine");
    if (target_engine[1]!=engine_world[0]) fail("unstable_sample");

    const auto kind=reader.read<std::uint64_t>(module.base,0x1bda990);
    if (!kind) fail("native_host_identity");
    const auto core=object_lookup(reader,module.base,core_class[0],0,0);
    if (object_lookup(reader,module.base,core_class[1],core,kind)!=kind ||
        reader.read<std::uint64_t>(kind,0x50)!=kind)
        fail("native_host_identity");
    const auto package_type=object_lookup(reader,module.base,function_package[1],core,kind);
    if (reader.read<std::uint64_t>(core,0x50)!=package_type) fail("native_host_identity");
    const auto function_type=object_lookup(reader,module.base,function_package[0],core,kind);
    const auto engine=object_lookup(reader,module.base,engine_world[0],0,package_type);
    const auto world=object_lookup(reader,module.base,engine_world[1],engine,kind);
    result.function_address=object_lookup(reader,module.base,target_engine[0],world,function_type);
    result.function_flags=reader.read<std::uint32_t>(result.function_address,0xd0);
    result.native_token=reader.read<std::uint16_t>(result.function_address,0xd4);
    result.original_native_address=reader.read<std::uint64_t>(result.function_address,0xf0);
    // Token zero is an experimental safety prerequisite, not original-token proof.
    if (!(result.function_flags&0x400) || result.native_token!=0 ||
        result.original_native_address!=module.base+0x400560)
        fail("native_host_native_mismatch");
    if (reader.read<std::uint64_t>(module.base,0x1cee4b8)!=pool ||
        reader.read<std::int32_t>(module.base,0x1cee4c0)!=name_count ||
        reader.read<std::uint64_t>(module.base,0x1bda990)!=kind)
        fail("unstable_sample");
    return result;
}
}
