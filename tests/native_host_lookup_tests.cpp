#include "native_host_lookup.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
namespace host=hawken::probe::native_host;
constexpr std::uint64_t kModule=0x140000000,kPool=0x50000000,kNameEntries=0x51000000;
constexpr std::uint64_t kCore=0x60000000,kEngine=0x60001000,kClass=0x60002000,
    kPackageClass=0x60003000,kFunctionClass=0x60004000,kWorldInfo=0x60005000,kTarget=0x60006000;
constexpr std::array<std::string_view,7> kNames={"Engine","WorldInfo","Core","Class",
    "Function","Package","IsClientOnlyBuild"};
// Independent XOR buckets for synthetic table[i]=i; UTF16 zero high bytes
// do not change this table's hash. No production hashing helper builds fixtures.
constexpr std::array<std::uint16_t,7> kNameBuckets={14,76,27,78,2,90,65};
struct Memory {
    std::map<std::uint64_t,std::byte> bytes;
    std::size_t used=0;
    template<class T> void put(std::uint64_t at,const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data=reinterpret_cast<const std::byte*>(&value);
        for (std::size_t i=0;i<sizeof(T);++i) bytes[at+i]=data[i];
    }
    bool load(std::uint64_t at,std::span<std::byte> output) {
        if (at>std::numeric_limits<std::uint64_t>::max()-(output.size()-1)) return false;
        used+=output.size();
        for (std::size_t i=0;i<output.size();++i) {
            const auto found=bytes.find(at+i);
            if (found==bytes.end()) return false;
            output[i]=found->second;
        }
        return true;
    }
};
struct Fixture {
    Memory memory;
    explicit Fixture(bool wide=false) {
        memory.put(kModule+0x1cee4b8,kPool);
        memory.put(kModule+0x1cee4c0,std::int32_t{8});
        std::array<std::uint32_t,256> table{};
        for (std::uint32_t i=0;i<table.size();++i) table[i]=i;
        memory.put(kModule+0x1bee030,table);
        for (std::size_t i=0;i<kNames.size();++i) {
            const auto entry=kNameEntries+i*0x100;
            memory.put(kModule+0x1bee430+8*kNameBuckets[i],entry);
            memory.put(entry+8,static_cast<std::uint32_t>((i+1)*2+(wide ? 1 : 0)));
            memory.put(entry+0xc,std::uint64_t{0});
            memory.put(kPool+8*(i+1),entry);
            for (std::size_t j=0;j<=kNames[i].size();++j) {
                const auto unit=j==kNames[i].size() ? 0 : static_cast<unsigned char>(kNames[i][j]);
                if (wide) memory.put(entry+0x14+2*j,static_cast<std::uint16_t>(unit));
                else memory.put(entry+0x14+j,static_cast<std::uint8_t>(unit));
            }
        }
        object(kCore,3,0,kPackageClass);
        object(kEngine,1,0,kPackageClass);
        object(kClass,4,kCore,kClass);
        object(kPackageClass,6,kCore,kClass);
        object(kFunctionClass,5,kCore,kClass);
        object(kWorldInfo,2,kEngine,kClass);
        object(kTarget,7,kWorldInfo,kFunctionClass);
        memory.put(kModule+0x1bda990,kClass);
        memory.put(kTarget+0xd0,std::uint32_t{0x22501});
        // Token zero is the experiment's safety prerequisite, not a claim
        // that this synthetic fixture proves the original runtime token.
        memory.put(kTarget+0xd4,std::uint16_t{0});
        memory.put(kTarget+0xf0,kModule+0x400560);
    }
    static std::uint64_t head(std::uint32_t name,std::uint64_t outer) {
        const auto bucket=(name^static_cast<std::uint32_t>(outer>>4))&0x7fff;
        return kModule+(outer ? 0x1cae4b0 : 0x1c6e4b0)+8*bucket;
    }
    void object(std::uint64_t at,std::uint32_t name,std::uint64_t outer,std::uint64_t type) {
        memory.put(at+8,std::uint64_t{0});memory.put(at+0x18,std::uint64_t{0});
        memory.put(at+0x10,std::uint64_t{0});memory.put(at+0x38,std::int32_t{1});
        memory.put(at+0x40,outer);memory.put(at+0x48,std::array<std::uint32_t,2>{name,0});
        memory.put(at+0x50,type);memory.put(head(name,outer),at);
    }
    host::Target sample(host::OriginalModule module={kModule,0x201e000}) {
        return host::locate_is_client_only_build(
            [&](std::uint64_t at,std::span<std::byte> output){return memory.load(at,output);},module);
    }
};
}

TEST_CASE("Native host lookup identifies exact typed target from ANSI or UTF16 hash entries") {
    bool wide=false;
    SUBCASE("ANSI") {}
    SUBCASE("UTF16") { wide=true; }
    Fixture fixture(wide);
    const auto before=fixture.memory.bytes;
    const auto result=fixture.sample();
    CHECK(result.function_address==kTarget);
    CHECK(result.original_native_address==kModule+0x400560);
    CHECK(result.function_flags==0x22501);
    CHECK(result.native_token==0);
    REQUIRE(result.function_address<=std::numeric_limits<std::uint64_t>::max()-0xf0);
    CHECK(result.function_address+0xf0==kTarget+0xf0);
    CHECK_FALSE(result.trace.empty());
    CHECK(fixture.memory.bytes==before);
    CHECK(fixture.memory.used<=1024*1024);
    CHECK(result==fixture.sample());
}

TEST_CASE("Native host lookup rejects invalid module contract before reading") {
    Fixture fixture;
    host::OriginalModule module{kModule,0x201e000};
    SUBCASE("zero base") { module.base=0; }
    SUBCASE("smaller image") { --module.size; }
    SUBCASE("larger image") { ++module.size; }
    SUBCASE("overflow image end") { module.base=std::numeric_limits<std::uint64_t>::max()-0x100; }
    CHECK_THROWS_AS(fixture.sample(module),std::runtime_error);
    CHECK(fixture.memory.used==0);
}

TEST_CASE("Native host lookup rejects missing memory callback") {
    CHECK_THROWS_AS(host::locate_is_client_only_build({}, {kModule,0x201e000}),std::runtime_error);
}

TEST_CASE("Native host lookup rejects wrong target identity owner and lifecycle") {
    Fixture fixture;
    SUBCASE("FName number") { fixture.memory.put(kTarget+0x48,std::array<std::uint32_t,2>{7,1}); }
    SUBCASE("wrong name") { fixture.memory.put(kTarget+0x48,std::array<std::uint32_t,2>{2,0}); }
    SUBCASE("wrong owner") { fixture.memory.put(kTarget+0x40,kEngine); }
    SUBCASE("wrong Function class") { fixture.memory.put(kTarget+0x50,kClass); }
    SUBCASE("Function metadata wrong package") { fixture.memory.put(kFunctionClass+0x40,kEngine); }
    SUBCASE("WorldInfo wrong package") { fixture.memory.put(kWorldInfo+0x40,kCore); }
    SUBCASE("WorldInfo not UClass") { fixture.memory.put(kWorldInfo+0x50,kFunctionClass); }
    SUBCASE("Engine not package") { fixture.memory.put(kEngine+0x50,kClass); }
    SUBCASE("metaclass not self typed") { fixture.memory.put(kClass+0x50,kFunctionClass); }
    SUBCASE("pending kill") { fixture.memory.put(kTarget+0x10,std::uint64_t{0x40000000000}); }
    SUBCASE("removed object") { fixture.memory.put(kTarget+0x38,std::int32_t{-1}); }
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
}

TEST_CASE("Native host lookup requires exact original native target and experiment token") {
    Fixture fixture;
    SUBCASE("not native") { fixture.memory.put(kTarget+0xd0,std::uint32_t{0x22101}); }
    SUBCASE("null target") { fixture.memory.put(kTarget+0xf0,std::uint64_t{0}); }
    SUBCASE("other original function") { fixture.memory.put(kTarget+0xf0,kModule+0x400561); }
    SUBCASE("outside original image") { fixture.memory.put(kTarget+0xf0,kModule+0x201e000); }
    SUBCASE("nonzero token fails experiment") { fixture.memory.put(kTarget+0xd4,std::uint16_t{1}); }
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
}

TEST_CASE("Native host lookup rejects absent duplicate and cyclic object hash targets") {
    Fixture fixture;
    SUBCASE("absent") { fixture.memory.put(Fixture::head(7,kWorldInfo),std::uint64_t{0}); }
    SUBCASE("duplicate exact target") {
        constexpr auto duplicate=kTarget+0x1000;
        fixture.object(duplicate,7,kWorldInfo,kFunctionClass);
        fixture.memory.put(Fixture::head(7,kWorldInfo),kTarget);
        fixture.memory.put(kTarget+0x18,duplicate);
        fixture.memory.put(duplicate+0xd0,std::uint32_t{0x22501});
        fixture.memory.put(duplicate+0xd4,std::uint16_t{0});
        fixture.memory.put(duplicate+0xf0,kModule+0x400560);
    }
    SUBCASE("cycle after valid target") { fixture.memory.put(kTarget+0x18,kTarget); }
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
}

TEST_CASE("Native host lookup inspects bounded hash collisions without scanning object table") {
    Fixture fixture;
    // 255 collision entries then the exact target: 256 nodes are allowed.
    constexpr std::uint64_t first=0x70000000;
    for (std::size_t i=0;i<255;++i) {
        const auto at=first+i*0x100;
        fixture.object(at,7,kWorldInfo,kFunctionClass);
        fixture.memory.put(at+0x48,std::array<std::uint32_t,2>{7,1});
        fixture.memory.put(at+0x18,i==254 ? kTarget : at+0x100);
    }
    fixture.memory.put(Fixture::head(7,kWorldInfo),first);
    const auto result=fixture.sample();
    CHECK(result.function_address==kTarget);
    CHECK(fixture.memory.used<=1024*1024);
}

TEST_CASE("Native host lookup rejects a hash chain past the fixed 256 node bound") {
    Fixture fixture;
    constexpr std::uint64_t first=0x70000000;
    for (std::size_t i=0;i<256;++i) {
        const auto at=first+i*0x100;
        fixture.object(at,7,kWorldInfo,kFunctionClass);
        fixture.memory.put(at+0x48,std::array<std::uint32_t,2>{7,1});
        fixture.memory.put(at+0x18,i==255 ? kTarget : at+0x100);
    }
    fixture.memory.put(Fixture::head(7,kWorldInfo),first);
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
    CHECK(fixture.memory.used<=1024*1024);
}

TEST_CASE("Native host lookup rejects name hash cycle duplicate and invalid backlinks") {
    Fixture fixture;
    SUBCASE("cycle") { fixture.memory.put(kNameEntries+0xc,kNameEntries); }
    SUBCASE("invalid directed pool entry") { fixture.memory.put(kPool+8,std::uint64_t{0}); }
    SUBCASE("duplicate exact name") {
        const auto duplicate=kNameEntries+0x1000;
        fixture.memory.put(kNameEntries+0xc,duplicate);
        fixture.memory.put(duplicate+8,std::uint32_t{16});
        fixture.memory.put(duplicate+0xc,std::uint64_t{0});
        fixture.memory.put(kModule+0x1cee4c0,std::int32_t{9});
        fixture.memory.put(kPool+8*8,duplicate);
        for (std::size_t i=0;i<=kNames[0].size();++i)
            fixture.memory.put(duplicate+0x14+i,i==kNames[0].size() ? std::uint8_t{0} : static_cast<std::uint8_t>(kNames[0][i]));
    }
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
}

TEST_CASE("Native host lookup propagates unreadable memory without accepting a target") {
    Fixture fixture;
    fixture.memory.bytes.erase(kTarget+0xf0);
    CHECK_THROWS_AS(fixture.sample(),std::runtime_error);
}
