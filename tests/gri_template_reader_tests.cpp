#include "gri_template_reader.hpp"
#include "fname_hash_lookup.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using namespace hawken::probe;
// Synthetic typed memory, not an attestation of any original client instance.
struct Memory {
    std::map<std::uint64_t,unsigned char> bytes;
    std::vector<std::pair<std::uint64_t,std::size_t>> reads;
    std::size_t used=0,budget=1024*1024;
    template<class T> void put(std::uint64_t address,const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data=reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i=0;i<sizeof(T);++i) bytes[address+i]=data[i];
    }
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        if (offset>maximum-base || sizeof(T)-1>maximum-(base+offset))
            throw std::runtime_error("reader_overflow");
        if (sizeof(T)>budget-used) throw std::runtime_error("read_limit");
        used+=sizeof(T);
        const auto address=base+offset;
        reads.emplace_back(address,sizeof(T));
        std::array<unsigned char,sizeof(T)> data{};
        for (std::size_t i=0;i<sizeof(T);++i) {
            const auto found=bytes.find(address+i);
            if (found==bytes.end()) throw std::runtime_error("reader_error");
            data[i]=found->second;
        }
        T result{};
        std::memcpy(&result,data.data(),sizeof(T));
        return result;
    }
    bool touched(std::uint64_t address) const {
        for (const auto& [base,width]:reads) if (base<=address && address-base<width) return true;
        return false;
    }
};
constexpr std::uint64_t module=0x140000000,klass=0x100000,cdo=0x110000,
    package=0x120000,kind=0x130000,property=0x400000,
    struct_type=0x300000,unique_struct=0x310000;
constexpr std::array<std::uint64_t,7> pri={0x200000,0x201000,0x202000,0x203000,0x204000,0x205000,0x206000};
constexpr std::array<const char*,7> pri_names={"R_PlayerReplicationInfo","HawkenPRI_Base",
    "PlayerReplicationInfo","ReplicationInfo","Info","Actor","Object"};

struct Fixture {
    Memory memory;
    std::map<std::uint64_t,GriIdentity> identities;
    Fixture() {
        identities[kind]={"Class","","Core","Class",0};
        identities[klass]={"R_GameReplicationInfo","","HawkenGame","Class",0};
        identities[cdo]={"Default__R_GameReplicationInfo","","HawkenGame","R_GameReplicationInfo",0};
        identities[package]={"HawkenGame","","HawkenGame","Package",0};
        memory.put(module+0x1bda990,kind);
        memory.put(module+0x1d3f988,pri[0]);
        memory.put(klass+0x50,kind); memory.put(klass+0x40,package);
        memory.put(klass+0x3c,std::int32_t{9438}); memory.put(klass+0x1d4,cdo);
        memory.put(package+0x40,std::uint64_t{0});
        memory.put(cdo,module+0x13cbf70); memory.put(cdo+0x50,klass);
        memory.put(cdo+0x40,package); memory.put(cdo+0x10,std::uint64_t{0x200});
        memory.put(cdo+0x3c,std::int32_t{9439}); memory.put(cdo+0xec,std::uint32_t{0xa0});
        memory.put(module+0x13cc5a8,module+0x379e20);
        for (std::size_t i=0;i<pri.size();++i) {
            identities[pri[i]]={pri_names[i],"",i<2 ? "HawkenGame" : i==6 ? "Core" : "Engine","Class",0};
            memory.put(pri[i]+0x50,kind);
            memory.put(pri[i]+0x78,i+1==pri.size() ? std::uint64_t{0} : pri[i+1]);
        }
        memory.put(pri[2]+0x80,property);
        identities[property]={"UniqueId","PlayerReplicationInfo","Engine","StructProperty",0};
        identities[struct_type]={"StructProperty","","Core","Class",0};
        identities[unique_struct]={"UniqueNetId","OnlineSubsystem","Engine","ScriptStruct",0};
        memory.put(property+0x60,std::uint64_t{0}); memory.put(property+0x40,pri[2]);
        memory.put(property+0x50,struct_type); memory.put(struct_type+0x50,kind);
        memory.put(property+0x68,std::int32_t{1}); memory.put(property+0x8c,std::int32_t{0x2d4});
        memory.put(property+0xa8,unique_struct);
        // No PRI object or 16-byte UniqueId value exists in this fixture.
    }
    GriTemplateSnapshot sample() {
        auto identify=[&](std::uint64_t address) { return identities.at(address); };
        return read_gri_template(memory,module,klass,identify);
    }
};
template<class F> void rejects(F&& action,const char* expected) {
    try { action(); FAIL("metadata must be rejected"); }
    catch (const std::runtime_error& error) { CHECK(std::string(error.what())==expected); }
}
}

TEST_CASE("GRI template reads actual rotation bit and typed PRI field offset without identity value") {
    Fixture fixture;
    bool expected_rotation=false;
    SUBCASE("rotation absent") {}
    SUBCASE("rotation required") { fixture.memory.put(cdo+0xec,std::uint32_t{0xa1}); expected_rotation=true; }
    const auto result=fixture.sample();
    CHECK(result.template_verified);
    CHECK(result.getpc_null);
    CHECK(result.rotation_required==expected_rotation);
    CHECK(result.unique_id_metadata_verified);
    CHECK(result.unique_id_offset==0x2d4);
    CHECK(fixture.memory.touched(cdo+0xec));
    CHECK_FALSE(result.trace.empty());
    CHECK(fixture.memory.used<1024*1024);
}

TEST_CASE("GRI CDO identity must precede reading private rotation word") {
    Fixture fixture;
    SUBCASE("wrong class name") { fixture.identities[klass].name="Other"; }
    SUBCASE("wrong package") { fixture.identities[klass].package="Engine"; }
    SUBCASE("class FName instance") { fixture.identities[klass].instance=1; }
    SUBCASE("wrong class kind") { fixture.memory.put(klass+0x50,std::uint64_t{0}); }
    SUBCASE("wrong class NetIndex") { fixture.memory.put(klass+0x3c,std::int32_t{9439}); }
    SUBCASE("missing CDO") { fixture.memory.put(klass+0x1d4,std::uint64_t{0}); }
    SUBCASE("CDO wrong name") { fixture.identities[cdo].name="Default__Other"; }
    SUBCASE("CDO FName instance") { fixture.identities[cdo].instance=1; }
    SUBCASE("CDO archetype flags") { fixture.memory.put(cdo+0x10,std::uint64_t{0x600}); }
    SUBCASE("not CDO") { fixture.memory.put(cdo+0x10,std::uint64_t{0}); }
    SUBCASE("wrong backref") { fixture.memory.put(cdo+0x50,pri[0]); }
    SUBCASE("wrong outer") { fixture.memory.put(cdo+0x40,std::uint64_t{0}); }
    SUBCASE("wrong CDO NetIndex") { fixture.memory.put(cdo+0x3c,std::int32_t{47775}); }
    SUBCASE("foreign vtable") { fixture.memory.put(cdo,module+0x13b0220); }
    SUBCASE("wrong GetPC slot") { fixture.memory.put(module+0x13cc5a8,module+0x379e21); }
    rejects([&] { fixture.sample(); },"gri_identity");
    CHECK_FALSE(fixture.memory.touched(cdo+0xec));
}

TEST_CASE("GRI mode rejects PRI field metadata with another owner or struct") {
    Fixture fixture;
    SUBCASE("wrong PRI ancestor") { fixture.identities[pri[2]].name="Controller"; }
    SUBCASE("wrong property owner") { fixture.memory.put(property+0x40,pri[0]); }
    SUBCASE("wrong property type") { fixture.identities[struct_type].name="ObjectProperty"; }
    SUBCASE("wrong struct type") { fixture.identities[unique_struct].type="Class"; }
    SUBCASE("wrong struct owner") { fixture.identities[unique_struct].owner="PlayerReplicationInfo"; }
    SUBCASE("wrong struct package") { fixture.identities[unique_struct].package="Core"; }
    SUBCASE("wrong struct name") { fixture.identities[unique_struct].name="Vector"; }
    SUBCASE("property array") { fixture.memory.put(property+0x68,std::int32_t{2}); }
    SUBCASE("negative offset") { fixture.memory.put(property+0x8c,std::int32_t{-1}); }
    SUBCASE("excessive offset") { fixture.memory.put(property+0x8c,std::int32_t{65537}); }
    rejects([&] { fixture.sample(); },"gri_property_mismatch");
}

TEST_CASE("GRI mode requires a unique field and bounded acyclic metadata traversal") {
    Fixture fixture;
    SUBCASE("absent") {
        fixture.memory.put(pri[2]+0x80,std::uint64_t{0});
        rejects([&] { fixture.sample(); },"gri_property_missing");
    }
    SUBCASE("duplicate UniqueId") {
        constexpr std::uint64_t second=0x410000;
        fixture.identities[second]=fixture.identities[property];
        fixture.memory.put(property+0x60,second);
        fixture.memory.put(second+0x60,std::uint64_t{0});
        rejects([&] { fixture.sample(); },"gri_property_mismatch");
    }
    SUBCASE("cyclic children") {
        fixture.memory.put(property+0x60,property);
        rejects([&] { fixture.sample(); },"gri_cycle");
    }
    SUBCASE("truncated class chain") {
        fixture.memory.put(pri[2]+0x78,std::uint64_t{0});
        rejects([&] { fixture.sample(); },"gri_property_mismatch");
    }
    SUBCASE("over children cap") {
        for (std::uint64_t i=0;i<513;++i) {
            const auto child=0x500000+i*0x100;
            fixture.identities[child]={"Other","PlayerReplicationInfo","Engine","Function",0};
            fixture.memory.put(child+0x60,i==512 ? std::uint64_t{0} : child+0x100);
        }
        fixture.memory.put(pri[2]+0x80,std::uint64_t{0x500000});
        rejects([&] { fixture.sample(); },"gri_limit");
    }
}

TEST_CASE("GRI complete snapshots retain scalar and field metadata changes") {
    Fixture fixture;
    const auto first=fixture.sample();
    REQUIRE(first.template_verified);
    CHECK(first==fixture.sample());
    SUBCASE("rotation bit changed") { fixture.memory.put(cdo+0xec,std::uint32_t{0xa1}); }
    SUBCASE("other rotation word bits changed") { fixture.memory.put(cdo+0xec,std::uint32_t{0xa2}); }
    SUBCASE("reflected offset changed") { fixture.memory.put(property+0x8c,std::int32_t{0x2d8}); }
    CHECK_FALSE(first==fixture.sample());
}

TEST_CASE("GRI reader rejects module address overflow before any memory read") {
    Fixture fixture;
    auto identify=[&](std::uint64_t address) { return fixture.identities.at(address); };
    rejects([&] { read_gri_template(fixture.memory,std::numeric_limits<std::uint64_t>::max(),klass,identify); },"gri_invalid");
    CHECK(fixture.memory.reads.empty());
}

TEST_CASE("GRI reader propagates shared budget exhaustion") {
    Fixture fixture;
    fixture.memory.budget=1;
    rejects([&] { fixture.sample(); },"read_limit");
}

namespace {
struct FixedNameFixture {
    Memory memory;
    static constexpr std::uint64_t base=0x50000000,pool=0x60000000;
    static constexpr std::array<std::uint64_t,2> entries={0x70000000,0x70000100};
    static constexpr std::array<std::string_view,2> targets={"HawkenGame","R_GameReplicationInfo"};
    FixedNameFixture() {
        memory.put(base+0x1cee4b8,pool); memory.put(base+0x1cee4c0,std::int32_t{200000});
        std::array<std::uint32_t,256> table{};
        for (std::uint32_t i=0;i<256;++i) table[i]=0x10000U|i;
        memory.put(base+0x1bee030,table);
        // Synthetic table's independent hash values are stored in fixture-proof.json.
        for (const auto bucket:{0x110,0x150,0x151})
            memory.put(base+0x1bee430+8*static_cast<std::uint64_t>(bucket),std::uint64_t{0});
        memory.put(base+0x1bee430+8*0x110,entries[0]);
        memory.put(base+0x1bee430+8*0x150,entries[1]);
        for (std::size_t i=0;i<2;++i) {
            memory.put(entries[i]+8,static_cast<std::uint32_t>((i==0 ? 3 : 7)*2));
            memory.put(entries[i]+0xc,std::uint64_t{0});
            memory.put(pool+(i==0 ? 3 : 7)*8,entries[i]);
            for (std::size_t j=0;j<=targets[i].size();++j)
                memory.put(entries[i]+0x14+j,j==targets[i].size() ? std::uint8_t{0} : static_cast<std::uint8_t>(targets[i][j]));
        }
    }
    FNameLookupSnapshot sample() { return read_fixed_fname_targets(memory,base,targets); }
};
}

TEST_CASE("Fixed GRI names lookup reads target buckets without scanning large NamePool") {
    FixedNameFixture fixture;
    const auto first=fixture.sample();
    CHECK((first.indices==std::array<std::uint32_t,2>{3,7}));
    CHECK(first.name_count==200000);
    CHECK(first.head_count<=4);
    CHECK(fixture.memory.used<1024*1024);
    for (const auto& [address,width]:fixture.memory.reads)
        if (address>=FixedNameFixture::pool && address<FixedNameFixture::pool+200000*8) {
            CHECK((address==FixedNameFixture::pool+3*8 || address==FixedNameFixture::pool+7*8));
            CHECK(width==8);
        }
    CHECK(first.trace==fixture.sample().trace);
}

TEST_CASE("Fixed names reject invalid or duplicate observer targets before memory reads") {
    FixedNameFixture fixture;
    std::array<std::string_view,2> targets={"HawkenGame","R_GameReplicationInfo"};
    SUBCASE("empty") { targets[0]=""; }
    SUBCASE("duplicate") { targets[1]=targets[0]; }
    SUBCASE("nonidentifier") { targets[0]="Hawken Game"; }
    SUBCASE("over128") { static const std::string too_long(129,'A'); targets[0]=too_long; }
    rejects([&] { read_fixed_fname_targets(fixture.memory,FixedNameFixture::base,targets); },"player_name_invalid");
    CHECK(fixture.memory.reads.empty());
}

TEST_CASE("Fixed GRI lookup preserves cycle guard and shared one MiB budget") {
    FixedNameFixture fixture;
    SUBCASE("cycle") {
        fixture.memory.put(FixedNameFixture::entries[0]+0xc,FixedNameFixture::entries[0]);
        rejects([&] { fixture.sample(); },"player_name_cycle");
    }
    SUBCASE("budget") {
        fixture.memory.budget=1024;
        rejects([&] { fixture.sample(); },"read_limit");
    }
}
