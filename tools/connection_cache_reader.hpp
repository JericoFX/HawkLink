#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hawken::probe {
struct ExpectedClass {
    std::uint64_t address;
    std::vector<std::uint64_t> fields;
};
struct CacheField {
    std::uint64_t address;
    std::int32_t id;
    std::int32_t secondary_value;
    bool operator==(const CacheField&) const = default;
};
struct CacheClass {
    std::int32_t base;
    std::int32_t capacity;
    std::vector<CacheField> fields;
    bool operator==(const CacheClass&) const = default;
};
struct CacheSnapshot {
    bool present = false;
    std::vector<std::uint64_t> trace;
    std::vector<CacheClass> classes;
    bool operator==(const CacheSnapshot&) const = default;
};
// Native 0x430d0: unsigned 32-bit wrap, full pointer equality only after hashing.
inline std::uint32_t class_hash(std::uint64_t key) {
    const auto k=static_cast<std::uint32_t>(key);
    auto a=(0x9e3779b9U-k)^(k<<8);
    auto b=(0U-a-k)^(a>>13);
    auto c=(k-b-a)^(b>>12);
    a=(a-c-b)^(c<<16);
    b=(b-a-c)^(a>>5);
    c=(c-a-b)^(b>>3);
    a=(a-c-b)^(c<<10);
    return (b-a-c)^(a>>15);
}
namespace cache_detail {
[[noreturn]] inline void fail(const char* code) { throw std::runtime_error(code); }
inline std::uint64_t address(std::uint64_t base,std::uint64_t offset,std::uint64_t width=1) {
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    if (!base || offset>maximum-base || width==0 || width-1>maximum-(base+offset))
        fail("cache_invalid");
    return base+offset;
}
template<class Reader> class TracedReader {
public:
    TracedReader(Reader& reader,std::vector<std::uint64_t>& trace):reader_(reader),trace_(trace) {}
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        const auto location=address(base,offset,sizeof(T));
        const auto value=reader_.template read<T>(location);
        trace_.push_back(location);
        trace_.push_back(static_cast<std::uint64_t>(value));
        return value;
    }
private:
    Reader& reader_;
    std::vector<std::uint64_t>& trace_;
};
}
// The caller supplies the existing bounded reader and verified class identities.
// No function invocation, allocation scan, or process write belongs in this tool.
// Limits below are conservative reader policy, not checks performed by the native lookup.
template<class Reader>
CacheSnapshot read_connection_cache(Reader& reader,std::uint64_t map,
                                   std::span<const ExpectedClass> expected) {
    using cache_detail::address;
    using cache_detail::fail;
    if (expected.empty()) fail("cache_invalid");
    if (expected.size()>8) fail("cache_limit");
    std::vector<std::uint64_t> class_ids,field_ids;
    for (const auto& klass:expected) {
        if (!klass.address || std::find(class_ids.begin(),class_ids.end(),klass.address)!=class_ids.end())
            fail("cache_invalid");
        class_ids.push_back(klass.address);
        if (klass.fields.size()>256) fail("cache_limit");
        for (const auto field:klass.fields) {
            if (!field || std::find(field_ids.begin(),field_ids.end(),field)!=field_ids.end())
                fail("cache_invalid");
            field_ids.push_back(field);
        }
    }
    CacheSnapshot out;
    cache_detail::TracedReader<Reader> view(reader,out.trace);
    const auto dictionary_address=address(map,0xb8);
    const auto count=view.template read<std::int32_t>(dictionary_address,0x44);
    if (count==0) return out;
    if (count<0) fail("cache_invalid");
    if (count>4096) fail("cache_limit");
    if ((count&(count-1))!=0) fail("cache_invalid");
    auto bucket_storage=view.template read<std::uint64_t>(dictionary_address,0x3c);
    if (!bucket_storage) {
        if (count!=1) fail("cache_invalid");
        bucket_storage=address(dictionary_address,0x38);
    }
    const auto bucket=class_hash(expected.front().address)&static_cast<std::uint32_t>(count-1);
    auto index=view.template read<std::int32_t>(bucket_storage,static_cast<std::uint64_t>(bucket)*4);
    if (index==-1) return out;
    if (index<0) fail("cache_invalid");
    if (index>4095) fail("cache_limit");
    const auto data=view.template read<std::uint64_t>(dictionary_address);
    if (!data) fail("cache_invalid");
    std::vector<std::int32_t> visited;
    std::uint64_t cache=0;
    while (index!=-1) {
        if (index<0) fail("cache_invalid");
        if (index>4095 || visited.size()>=256) fail("cache_limit");
        if (std::find(visited.begin(),visited.end(),index)!=visited.end()) fail("cache_cycle");
        visited.push_back(index);
        const auto node=address(data,static_cast<std::uint64_t>(index)*0x18,0x18);
        const auto key=view.template read<std::uint64_t>(node);
        if (key==expected.front().address) {
            cache=view.template read<std::uint64_t>(node,8);
            if (!cache) fail("cache_invalid");
            break;
        }
        index=view.template read<std::int32_t>(node,0x10);
    }
    if (!cache) return out;
    std::vector<std::uint64_t> caches;
    std::size_t total=0;
    for (std::size_t i=0;i<expected.size();++i) {
        if (!cache) fail("cache_class_mismatch");
        if (std::find(caches.begin(),caches.end(),cache)!=caches.end()) fail("cache_cycle");
        caches.push_back(cache);
        address(cache,0,0x38);
        if (view.template read<std::uint64_t>(cache,0x20)!=expected[i].address)
            fail("cache_class_mismatch");
        const auto parent=view.template read<std::uint64_t>(cache,0x14);
        CacheClass klass;
        klass.base=view.template read<std::int32_t>(cache,0x10);
        const auto array=view.template read<std::uint64_t>(cache,0x28);
        const auto own=view.template read<std::int32_t>(cache,0x30);
        klass.capacity=view.template read<std::int32_t>(cache,0x34);
        if (klass.base<0 || own<0 || klass.capacity<0) fail("cache_invalid");
        if (klass.base>2048 || own>256 || klass.capacity>256 || klass.base+own>2048)
            fail("cache_limit");
        if (own>klass.capacity || (own>0 && !array)) fail("cache_invalid");
        total+=static_cast<std::size_t>(own);
        if (total>2048) fail("cache_limit");
        for (std::int32_t j=0;j<own;++j) {
            const auto record=address(array,static_cast<std::uint64_t>(j)*16,16);
            CacheField field;
            field.address=view.template read<std::uint64_t>(record);
            field.id=view.template read<std::int32_t>(record,8);
            field.secondary_value=view.template read<std::int32_t>(record,12);
            if (field.id!=klass.base+j ||
                std::find(expected[i].fields.begin(),expected[i].fields.end(),field.address)==expected[i].fields.end() ||
                std::any_of(klass.fields.begin(),klass.fields.end(),[&](const auto& prior) {return prior.address==field.address;}))
                fail("cache_field_mismatch");
            klass.fields.push_back(field);
        }
        out.classes.push_back(std::move(klass));
        if (parent && std::find(caches.begin(),caches.end(),parent)!=caches.end()) fail("cache_cycle");
        if ((i+1==expected.size())!=(parent==0)) fail("cache_class_mismatch");
        cache=parent;
    }
    for (std::size_t i=0;i<out.classes.size();++i) {
        const auto inherited=i+1==out.classes.size() ? 0 : out.classes[i+1].base+
            static_cast<std::int32_t>(out.classes[i+1].fields.size());
        if (out.classes[i].base!=inherited) fail("cache_field_mismatch");
    }
    out.present=true;
    return out;
}
}
