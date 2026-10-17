#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include "hawken/sha256.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using Json=nlohmann::json;
using FName=std::array<std::uint32_t,2>;
constexpr wchar_t original_path[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\HawkenGame-Win64-Shipping.exe";
constexpr std::string_view original_sha="4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2";
constexpr std::uint64_t class_kind_rva=0x1bda990,name_table_rva=0x1cee4b8,name_count_rva=0x1cee4c0;
constexpr std::uint64_t root_hash_rva=0x1c6e4b0,outer_hash_rva=0x1cae4b0;
constexpr std::uint64_t pawn_vtable_rva=0x17de0c0,null_controller_getter_rva=0x379e20;
constexpr std::uint64_t exclusion_mask=0x40000000000;
constexpr std::int32_t name_count_cap=1048576;
constexpr std::size_t chain_cap=256,read_budget=128*1024*1024;
constexpr std::array<std::string_view,12> targets={"HawkenGameContent","R_PawnContent","Default__R_PawnContent",
    "HawkenGame","R_Weap_Machine","Default__R_Weap_Machine",
    "R_Weap_MissileLauncher","Default__R_Weap_MissileLauncher",
    "R_InventoryManager","Default__R_InventoryManager",
    "R_Weap_DualWield","Default__R_Weap_DualWield"};
constexpr bool required_target(std::size_t index,bool inventory) noexcept {
    return inventory ? index>=3 : index<3;
}
constexpr std::size_t target_units=[] {
    std::size_t maximum=0;
    for (const auto target:targets) maximum=std::max(maximum,target.size());
    return maximum+1; // Include the longest target's terminating NUL.
}();

class Handle {
public:
    explicit Handle(HANDLE value):value_(value) {}
    ~Handle() { if (value_ && value_!=INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_!=INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
// Only these locally fixed codes can reach the output, never API/file exceptions.
enum class Error { address,read_limit,read_unavailable,name_table_invalid,name_entry_null,
    target_names_absent,package_name_entry_null,name_limit,
    names_ambiguous,unstable,object_unavailable,object_ambiguous,chain_limit,identity,vtable };
class ProbeError final:public std::exception {
public:
    explicit ProbeError(Error value):value_(value) {}
    Error value() const noexcept { return value_; }
private:
    Error value_;
};
[[noreturn]] void fail(Error value) { throw ProbeError(value); }
const char* error_code(Error value) noexcept {
    switch (value) {
    case Error::address: return "invalid_address";
    case Error::read_limit: return "read_limit";
    case Error::read_unavailable: return "read_unavailable";
    case Error::name_table_invalid: return "name_table_invalid";
    case Error::name_entry_null: return "name_entry_null";
    case Error::target_names_absent: return "target_names_absent";
    case Error::package_name_entry_null: return "package_name_entry_null";
    case Error::name_limit: return "name_limit";
    case Error::names_ambiguous: return "names_ambiguous";
    case Error::unstable: return "unstable_sample";
    case Error::object_unavailable: return "object_unavailable";
    case Error::object_ambiguous: return "object_ambiguous";
    case Error::chain_limit: return "object_chain_limit";
    case Error::identity: return "object_identity_mismatch";
    case Error::vtable: return "cdo_vtable_mismatch";
    }
    return "observation_unavailable";
}
bool parse_pid(std::string_view text,DWORD& pid) noexcept {
    if (text.empty() || text.front()=='0') return false;
    std::uint64_t value=0;
    for (const char c:text) {
        if (c<'0' || c>'9') return false;
        const auto digit=static_cast<unsigned>(c-'0');
        if (value>(std::numeric_limits<DWORD>::max()-digit)/10) return false;
        value=value*10+digit;
    }
    pid=static_cast<DWORD>(value);
    return pid!=0;
}
bool same_original_path(std::wstring_view path) noexcept {
    constexpr std::wstring_view expected=original_path;
    return path.size()==expected.size() && CompareStringOrdinal(path.data(),static_cast<int>(path.size()),
        expected.data(),static_cast<int>(expected.size()),TRUE)==CSTR_EQUAL;
}
bool process_verified(HANDLE process) {
    std::array<wchar_t,32768> path{};
    DWORD length=static_cast<DWORD>(path.size());
    return QueryFullProcessImageNameW(process,0,path.data(),&length)!=0 &&
        same_original_path({path.data(),length});
}
std::uint64_t add(std::uint64_t base,std::uint64_t offset) {
    if (!base || offset>std::numeric_limits<std::uint64_t>::max()-base) fail(Error::address);
    const auto result=base+offset;
    if (result>std::numeric_limits<std::uintptr_t>::max()) fail(Error::address);
    return result;
}
class Reader {
public:
    explicit Reader(HANDLE process):process_(process) {}
    void read_into(std::uint64_t base,std::uint64_t offset,void* result,std::size_t size) {
        const auto location=add(base,offset);
        if (size>read_budget-used_ || size>std::numeric_limits<std::uintptr_t>::max()-location)
            fail(Error::read_limit);
        used_+=size; // Includes failed reads; one budget shared by both complete samples.
        SIZE_T received=0;
        if (!ReadProcessMemory(process_,reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(location)),
            result,size,&received) || received!=size) fail(Error::read_unavailable);
    }
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        T value{};
        read_into(base,offset,&value,sizeof(value));
        return value;
    }
private:
    HANDLE process_;
    std::size_t used_=0;
};
struct NameHit {
    std::uint32_t index=0;
    std::uint64_t entry=0;
    bool wide=false,found=false;
    bool operator==(const NameHit&) const = default;
};
struct Names {
    std::uint64_t table=0;
    std::int32_t count=0;
    std::array<NameHit,targets.size()> hits{};
    std::uint32_t null_entries=0;
    bool operator==(const Names&) const = default;
};
std::uint16_t name_unit(Reader& reader,std::uint64_t entry,bool wide,std::size_t position) {
    return wide ? reader.read<std::uint16_t>(entry,0x14+position*2) :
        static_cast<std::uint16_t>(reader.read<std::uint8_t>(entry,0x14+position));
}
bool exact_name(Reader& reader,std::uint64_t entry,bool wide,std::string_view target) {
    for (std::size_t i=0;i<=target.size();++i) {
        const auto expected=i==target.size() ? 0 : static_cast<unsigned char>(target[i]);
        if (name_unit(reader,entry,wide,i)!=expected) return false;
    }
    return true;
}
struct TargetNameMatcher {
    std::array<bool,targets.size()> possible{};
    constexpr explicit TargetNameMatcher(bool inventory=false) noexcept {
        for (std::size_t i=0;i<possible.size();++i) possible[i]=required_target(i,inventory);
    }
    std::size_t position=0;
    int target=-1;
    constexpr bool consume(std::uint16_t unit) noexcept {
        bool any=false;
        for (std::size_t i=0;i<targets.size();++i) {
            if (!possible[i]) continue;
            const auto expected=position==targets[i].size() ? 0 :
                static_cast<unsigned char>(targets[i][position]);
            possible[i]=unit==expected;
            if (possible[i] && position==targets[i].size()) {
                target=static_cast<int>(i);
                return true;
            }
            any=any || possible[i];
        }
        ++position;
        return any;
    }
};
template<std::size_t N>
constexpr int match_literal(const char16_t (&literal)[N],bool inventory=false) noexcept {
    TargetNameMatcher matcher(inventory);
    for (const auto unit:literal) {
        if (!matcher.consume(static_cast<std::uint16_t>(unit))) return -1;
        if (matcher.target>=0) return matcher.target;
    }
    return -1;
}
// Independent literals exercise the exact same matcher as remote narrow/wide reads.
// These are local string regressions, not a simulated client/process observation.
static_assert(targets[2].size()+1==23);
static_assert(target_units==32);
static_assert(match_literal(u"HawkenGameContent")==0);
static_assert(match_literal(u"R_PawnContent")==1);
static_assert(match_literal(u"Default__R_PawnContent")==2);
static_assert(match_literal(u"HawkenGameConten")==-1);
static_assert(match_literal(u"R_PawnConten")==-1);
static_assert(match_literal(u"Default__R_PawnConten")==-1);
static_assert(match_literal(u"HawkenGameContentX")==-1);
static_assert(match_literal(u"R_PawnContentX")==-1);
static_assert(match_literal(u"Default__R_PawnContentX")==-1);
static_assert(match_literal(u"HawkenGame",true)==3);
static_assert(match_literal(u"R_Weap_Machine",true)==4);
static_assert(match_literal(u"Default__R_Weap_Machine",true)==5);
static_assert(match_literal(u"R_Weap_MissileLauncher",true)==6);
static_assert(match_literal(u"Default__R_Weap_MissileLauncher",true)==7);
static_assert(match_literal(u"R_InventoryManager",true)==8);
static_assert(match_literal(u"Default__R_InventoryManager",true)==9);
static_assert(match_literal(u"HawkenGame")==-1);
static_assert(match_literal(u"HawkenGameContent",true)==-1);
static_assert(match_literal(u"R_Weap_Machin",true)==-1);
static_assert(match_literal(u"R_Weap_MachineX",true)==-1);
static_assert(match_literal(u"Default__R_Weap_MissileLaunche",true)==-1);
static_assert(match_literal(u"Default__R_Weap_MissileLauncherX",true)==-1);
static_assert(match_literal(u"Default__R_InventoryManage",true)==-1);
static_assert(match_literal(u"Default__R_InventoryManagerX",true)==-1);
static_assert(match_literal(u"R_Weap_MissileLaunche",true)==-1);
static_assert(match_literal(u"R_Weap_MissileLauncherX",true)==-1);
static_assert(match_literal(u"R_InventoryManage",true)==-1);
static_assert(match_literal(u"R_InventoryManagerX",true)==-1);
static_assert(match_literal(u"R_Weap_DualWield",true)==10);
static_assert(match_literal(u"Default__R_Weap_DualWield",true)==11);
static_assert(match_literal(u"R_Weap_DualWiel",true)==-1);
static_assert(match_literal(u"R_Weap_DualWieldX",true)==-1);
static_assert(match_literal(u"Default__R_Weap_DualWiel",true)==-1);
static_assert(match_literal(u"Default__R_Weap_DualWieldX",true)==-1);
int target_name(Reader& reader,std::uint64_t entry,bool wide,bool inventory) {
    TargetNameMatcher matcher(inventory);
    // Selective comparison only; the derived bound includes NUL. Never retain
    // or decode nonmatching names, and stop as soon as all candidates differ.
    for (std::size_t position=0;position<target_units;++position) {
        if (!matcher.consume(name_unit(reader,entry,wide,position))) return -1;
        if (matcher.target>=0) return matcher.target;
    }
    return -1;
}
Names find_names(Reader& reader,std::uint64_t module,bool inventory) {
    Names result;
    result.table=reader.read<std::uint64_t>(module,name_table_rva);
    result.count=reader.read<std::int32_t>(module,name_count_rva);
    if (!result.table || result.count<=0) fail(Error::name_table_invalid);
    if (result.count>name_count_cap) fail(Error::name_limit);
    std::array<std::uint64_t,1024> entries{};
    const auto count=static_cast<std::size_t>(result.count);
    for (std::size_t first=0;first<count;first+=entries.size()) {
        const auto amount=std::min(entries.size(),count-first);
        reader.read_into(result.table,static_cast<std::uint64_t>(first)*8,entries.data(),amount*8);
        for (std::size_t i=0;i<amount;++i) {
            const auto entry=entries[i];
            // Discovery policy: an empty slot has no candidate name. Count it
            // for two-sample equality; directly referenced entries stay strict.
            if (!entry) { ++result.null_entries; continue; }
            const bool wide=(reader.read<std::uint8_t>(entry,8)&1)!=0;
            const int target=target_name(reader,entry,wide,inventory);
            if (target<0) continue;
            auto& hit=result.hits[static_cast<std::size_t>(target)];
            if (hit.found) fail(Error::names_ambiguous);
            hit={static_cast<std::uint32_t>(first+i),entry,wide,true};
        }
    }
    for (std::size_t i=0;i<result.hits.size();++i)
        if (required_target(i,inventory) && !result.hits[i].found) fail(Error::target_names_absent);
    return result;
}
void check_names(Reader& reader,std::uint64_t module,const Names& names) {
    if (reader.read<std::uint64_t>(module,name_table_rva)!=names.table ||
        reader.read<std::int32_t>(module,name_count_rva)!=names.count) fail(Error::unstable);
    for (std::size_t i=0;i<names.hits.size();++i) {
        const auto& hit=names.hits[i];
        if (!hit.found) continue;
        if (reader.read<std::uint64_t>(names.table,static_cast<std::uint64_t>(hit.index)*8)!=hit.entry ||
            ((reader.read<std::uint8_t>(hit.entry,8)&1)!=0)!=hit.wide ||
            !exact_name(reader,hit.entry,hit.wide,targets[i])) fail(Error::unstable);
    }
}
struct Object {
    std::uint64_t pointer=0,flags=0,outer=0,klass=0;
    FName name{};
    bool operator==(const Object&) const = default;
};
struct Chain {
    std::array<std::uint64_t,chain_cap> nodes{};
    std::size_t count=0;
    bool operator==(const Chain&) const = default;
};
Object lookup(Reader& reader,std::uint64_t module,std::uint32_t name,std::uint64_t outer,
    std::uint64_t required_class,Chain& chain) {
    const auto hash=(name^static_cast<std::uint32_t>(outer>>4))&0x7fff;
    auto object=reader.read<std::uint64_t>(module,(outer ? outer_hash_rva : root_hash_rva)+
        static_cast<std::uint64_t>(hash)*8);
    Object found;
    while (object) {
        if (chain.count==chain.nodes.size()) fail(Error::chain_limit);
        for (std::size_t i=0;i<chain.count;++i) if (chain.nodes[i]==object) fail(Error::chain_limit);
        chain.nodes[chain.count++]=object;
        const auto next=reader.read<std::uint64_t>(object,outer ? 0x18 : 8);
        if (reader.read<std::int32_t>(object,0x38)!=-1) {
            const auto tuple=reader.read<FName>(object,0x48);
            if (tuple==FName{name,0}) {
                const auto flags=reader.read<std::uint64_t>(object,0x10);
                const auto actual_outer=reader.read<std::uint64_t>(object,0x40);
                if (!(flags&exclusion_mask) && actual_outer==outer) {
                    const auto klass=reader.read<std::uint64_t>(object,0x50);
                    if (!required_class || klass==required_class) {
                        if (found.pointer) fail(Error::object_ambiguous);
                        found={object,flags,actual_outer,klass,tuple};
                    }
                }
            }
        }
        object=next;
    }
    if (!found.pointer || !found.klass) fail(Error::object_unavailable);
    return found;
}
std::uint64_t allowed_flags(Reader& reader,std::uint64_t object) {
    const auto flags=reader.read<std::uint64_t>(object,0x10);
    // Conservative policy even when native global1bc7cec disables this exclusion.
    if (flags&exclusion_mask) fail(Error::identity);
    return flags;
}
struct InventoryTemplate {
    Object klass;
    Chain class_chain;
    std::uint64_t cdo=0,cdo_flags=0,vtable=0,slot=0;
    FName cdo_name{};
    bool bit0=false;
    bool operator==(const InventoryTemplate&) const = default;
};
void require_image_range(std::uint64_t module,std::uint64_t image_size,
    std::uint64_t location,std::uint64_t width) {
    (void)add(module,image_size); // Reject overflowing or unrepresentable image end.
    if (location<module || width>image_size || location-module>image_size-width) fail(Error::vtable);
}
struct Sample {
    Names names;
    std::array<InventoryTemplate,4> inventory_templates{};
    Chain root_chain,class_chain;
    Object package,pawn_class;
    std::uint64_t class_kind=0,package_class_flags=0,package_name_entry=0,cdo=0,cdo_flags=0,vtable=0;
    FName package_class_name{},cdo_name{};
    bool package_name_wide=false;
    std::int32_t net_index=0;
    std::uint32_t ec=0;
    bool operator==(const Sample&) const = default;
};
Sample sample(Reader& reader,std::uint64_t module,bool inventory,std::uint64_t image_size) {
    Sample out;
    out.class_kind=reader.read<std::uint64_t>(module,class_kind_rva);
    if (!out.class_kind) fail(Error::object_unavailable);
    out.names=find_names(reader,module,inventory);
    out.package=lookup(reader,module,out.names.hits[inventory ? 3 : 0].index,0,0,out.root_chain);
    const auto package_class=out.package.klass;
    out.package_class_flags=allowed_flags(reader,package_class);
    if (reader.read<std::int32_t>(package_class,0x38)==-1 ||
        reader.read<std::uint64_t>(package_class,0x50)!=out.class_kind) fail(Error::identity);
    out.package_class_name=reader.read<FName>(package_class,0x48);
    if (out.package_class_name[1]!=0 || out.package_class_name[0]>=static_cast<std::uint32_t>(out.names.count))
        fail(Error::identity);
    out.package_name_entry=reader.read<std::uint64_t>(out.names.table,
        static_cast<std::uint64_t>(out.package_class_name[0])*8);
    if (!out.package_name_entry) fail(Error::package_name_entry_null);
    out.package_name_wide=(reader.read<std::uint8_t>(out.package_name_entry,8)&1)!=0;
    if (!exact_name(reader,out.package_name_entry,out.package_name_wide,"Package")) fail(Error::identity);
    if (inventory) {
        for (std::size_t i=0;i<out.inventory_templates.size();++i) {
            auto& item=out.inventory_templates[i];
            const auto class_target=4+i*2,cdo_target=class_target+1;
            item.klass=lookup(reader,module,out.names.hits[class_target].index,out.package.pointer,
                out.class_kind,item.class_chain);
            item.cdo=reader.read<std::uint64_t>(item.klass.pointer,0x1d4);
            if (!item.cdo) fail(Error::object_unavailable);
            item.cdo_flags=allowed_flags(reader,item.cdo);
            item.cdo_name=reader.read<FName>(item.cdo,0x48);
            if (reader.read<std::int32_t>(item.cdo,0x38)==-1 ||
                (item.cdo_flags&0x600)!=0x200 ||
                item.cdo_name!=FName{out.names.hits[cdo_target].index,0} ||
                reader.read<std::uint64_t>(item.cdo,0x50)!=item.klass.pointer ||
                reader.read<std::uint64_t>(item.cdo,0x40)!=out.package.pointer) fail(Error::identity);
            item.vtable=reader.read<std::uint64_t>(item.cdo);
            require_image_range(module,image_size,item.vtable,0x640);
            item.slot=reader.read<std::uint64_t>(item.vtable,0x638);
            require_image_range(module,image_size,item.slot,1);
            item.bit0=(reader.read<std::uint8_t>(item.cdo,0xec)&1)!=0;
        }
        check_names(reader,module,out.names);
        if (reader.read<std::uint64_t>(module,class_kind_rva)!=out.class_kind) fail(Error::unstable);
        return out;
    }
    out.pawn_class=lookup(reader,module,out.names.hits[1].index,out.package.pointer,out.class_kind,out.class_chain);
    out.cdo=reader.read<std::uint64_t>(out.pawn_class.pointer,0x1d4);
    if (!out.cdo) fail(Error::object_unavailable); // Never invoke a getter to materialize a CDO.
    out.cdo_flags=allowed_flags(reader,out.cdo);
    out.cdo_name=reader.read<FName>(out.cdo,0x48);
    if ((out.cdo_flags&0x600)!=0x200 || out.cdo_name!=FName{out.names.hits[2].index,0} ||
        reader.read<std::uint64_t>(out.cdo,0x50)!=out.pawn_class.pointer ||
        reader.read<std::uint64_t>(out.cdo,0x40)!=out.package.pointer) fail(Error::identity);
    out.vtable=reader.read<std::uint64_t>(out.cdo);
    // Native inherited vtable is only PROBABLE for this content class: fail closed
    // on mismatch rather than observing a native R_Pawn CDO as a fallback.
    if (out.vtable!=add(module,pawn_vtable_rva) ||
        reader.read<std::uint64_t>(out.vtable,0x638)!=add(module,null_controller_getter_rva)) fail(Error::vtable);
    out.net_index=reader.read<std::int32_t>(out.cdo,0x3c);
    out.ec=reader.read<std::uint32_t>(out.cdo,0xec);
    check_names(reader,module,out.names);
    if (reader.read<std::uint64_t>(module,class_kind_rva)!=out.class_kind) fail(Error::unstable);
    return out;
}
int inconclusive(const char* error) {
    std::cout<<Json({{"status","inconclusive"},{"error",error},{"identity_verified",false}}).dump()<<'\n';
    return 1;
}
int observe(DWORD pid,bool inventory) {
    const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,pid));
    if (!process) return inconclusive("process_unavailable");
    if (!process_verified(process.get()) || hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)
        return inconclusive("original_identity_mismatch");
    const Handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid));
    if (!modules) return inconclusive("module_unavailable");
    MODULEENTRY32W module{}; module.dwSize=static_cast<DWORD>(sizeof(module));
    if (!Module32FirstW(modules.get(),&module) || !same_original_path(module.szExePath) ||
        module.modBaseSize<name_count_rva+sizeof(std::int32_t)) return inconclusive("module_unavailable");
    const auto base=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module.modBaseAddr));
    Reader reader(process.get());
    const auto first=sample(reader,base,inventory,module.modBaseSize),
        second=sample(reader,base,inventory,module.modBaseSize);
    if (first!=second) {
        if (!inventory) return inconclusive("unstable_sample");
        Json differences=Json::object();
        for (std::size_t i=0;i<second.inventory_templates.size();++i) {
            const auto& a=first.inventory_templates[i];
            const auto& b=second.inventory_templates[i];
            differences[targets[4+i*2].data()]={
                {"class_chain_equal",a.class_chain==b.class_chain},
                {"class_metadata_equal",a.klass==b.klass},{"cdo_pointer_equal",a.cdo==b.cdo},
                {"cdo_name_equal",a.cdo_name==b.cdo_name},{"cdo_flags_equal",a.cdo_flags==b.cdo_flags},
                {"vtable_equal",a.vtable==b.vtable},{"slot_equal",a.slot==b.slot},{"bit0_equal",a.bit0==b.bit0}};
        }
        std::cout<<Json({{"status","inconclusive"},{"error","unstable_sample"},{"identity_verified",false},
            {"diagnostics",{{"name_table_equal",first.names.table==second.names.table},
                {"name_count_equal",first.names.count==second.names.count},
                {"name_hits_equal",first.names.hits==second.names.hits},
                {"name_null_entries_equal",first.names.null_entries==second.names.null_entries},
                {"root_chain_equal",first.root_chain==second.root_chain},
                {"package_equal",first.package==second.package &&
                    first.package_class_flags==second.package_class_flags &&
                    first.package_name_entry==second.package_name_entry &&
                    first.package_class_name==second.package_class_name &&
                    first.package_name_wide==second.package_name_wide},
                {"class_kind_equal",first.class_kind==second.class_kind},{"templates",differences}}}}).dump()<<'\n';
        return 1;
    }
    if (!process_verified(process.get())) return inconclusive("original_identity_mismatch");
    if (inventory) {
        Json templates=Json::array();
        for (std::size_t i=0;i<second.inventory_templates.size();++i) {
            const auto& item=second.inventory_templates[i];
            templates.push_back({{"class",targets[4+i*2]},{"bit0",item.bit0},
                {"vtable_rva",item.vtable-base},{"slot_638_rva",item.slot-base}});
        }
        // Observed slot addresses have no inferred GetPC semantics; never invoke them.
        std::cout<<Json({{"status","observed"},{"error",nullptr},{"identity_verified",true},
            {"original_path_verified",true},{"original_hash_verified",true},{"module_verified",true},
            {"samples_equal",true},{"templates",templates}}).dump()<<'\n';
        return 0;
    }
    // Two matching samples are not atomic, loaded-memory attestation, or proof of
    // contained PackageMap remapping. No process pointers or unrelated names leave this tool.
    std::cout<<Json({{"status","observed"},{"error",nullptr},{"identity_verified",true},
        {"original_path_verified",true},{"original_hash_verified",true},{"module_verified",true},
        {"samples_equal",true},{"package_kind_verified",true},{"class_kind_verified",true},
        {"class_backref_verified",true},{"cdo_flags_verified",true},{"cdo_name_verified",true},
        {"cdo_vtable_verified",true},{"controller_getter_null_verified",true},
        {"net_index",second.net_index},{"fname_raw",second.cdo_name},{"cdo_flags",second.cdo_flags},
        {"observed_ec",second.ec},{"bit0",(second.ec&1)!=0}}).dump()<<'\n';
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Usage: hawken-pawn-content-probe PID [--inventory-templates]\n"
            "Read-only bounded original Pawn or fixed inventory CDO metadata; no process creation.\n";
        return 0;
    }
    DWORD pid=0;
    const bool inventory=argc==3 && std::string_view(argv[2])=="--inventory-templates";
    if ((argc!=2 && !inventory) || !parse_pid(argv[1],pid)) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"invalid_arguments\",\"identity_verified\":false}\n";
        return 2;
    }
    try { return observe(pid,inventory); }
    catch (const ProbeError& error) { return inconclusive(error_code(error.value())); }
    catch (...) { return inconclusive("observation_unavailable"); }
}
