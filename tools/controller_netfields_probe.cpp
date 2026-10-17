#include <windows.h>
#include <tlhelp32.h>
#include "hawken/sha256.hpp"
#include "connection_cache_reader.hpp"
#include "fname_hash_lookup.hpp"
#include "mech_setup_reader.hpp"
#include "controller_sample_comparison.hpp"
#include "live_player_reader.hpp"
#include "main_menu_sync_reader.hpp"
#include "gri_template_reader.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json=nlohmann::json;
constexpr wchar_t original_path[]=L"F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\HawkenGame-Win64-Shipping.exe";
constexpr std::string_view original_sha="4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2";
constexpr std::uint64_t class_slot=0x1d3fa68,controller_vtable=0x17b9c50;
constexpr std::size_t read_budget=1024*1024,name_limit=128,own_limit=256,total_limit=2048;
constexpr std::array<std::string_view,8> packages={"HawkenGame","HawkenGame","UDKBase","GameFramework","Engine","Engine","Engine","Core"};
constexpr std::array<std::string_view,8> classes={"R_PlayerController","R_PlayerController_Base","UDKPlayerController","GamePlayerController","PlayerController","Controller","Actor","Object"};

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
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }
class NameInstanceError final:public std::runtime_error {
public:
    explicit NameInstanceError(std::array<std::uint32_t,2> raw):
        std::runtime_error("nonoriginal_name_instance"),raw_(raw) {}
    const auto& raw() const noexcept { return raw_; }
private:
    std::array<std::uint32_t,2> raw_;
};
bool parse_pid(std::string_view text,DWORD& pid) noexcept {
    if (text.empty() || text.front()=='0') return false;
    std::uint64_t value=0;
    for (char c:text) {
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
    std::array<wchar_t,32768> buffer{};
    DWORD length=static_cast<DWORD>(buffer.size());
    return QueryFullProcessImageNameW(process,0,buffer.data(),&length)!=0 &&
        same_original_path({buffer.data(),length});
}
std::uint64_t add(std::uint64_t base,std::uint64_t offset) {
    if (base==0 || offset>std::numeric_limits<std::uint64_t>::max()-base)
        fail("invalid_address");
    const auto result=base+offset;
    if (result>std::numeric_limits<std::uintptr_t>::max()) fail("invalid_address");
    return result;
}
class Reader {
public:
    explicit Reader(HANDLE process):process_(process) {}
    template<class T> T read(std::uint64_t base,std::uint64_t offset=0) {
        const auto location=add(base,offset);
        if (sizeof(T)>read_budget-used_ || location>std::numeric_limits<std::uintptr_t>::max()-sizeof(T))
            fail("read_limit");
        used_+=sizeof(T); // Charge attempted reads, including failures, across BOTH snapshots.
        T result{};
        SIZE_T received=0;
        if (!ReadProcessMemory(process_,reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(location)),
            &result,sizeof(T),&received) || received!=sizeof(T)) fail("read_unavailable");
        if (trace_) {
            trace_->insert(trace_->end(),{location,sizeof(T)});
            const auto* bytes=reinterpret_cast<const unsigned char*>(&result);
            for (std::size_t i=0;i<sizeof(T);i+=8) {
                std::uint64_t word=0;
                for (std::size_t j=0;j<8 && i+j<sizeof(T);++j)
                    word|=static_cast<std::uint64_t>(bytes[i+j])<<(j*8);
                trace_->push_back(word);
            }
        }
        return result;
    }
    std::size_t used() const noexcept { return used_; }
    std::vector<std::uint64_t>* trace_to(std::vector<std::uint64_t>* sink) noexcept {
        const auto previous=trace_;
        trace_=sink;
        return previous;
    }
private:
    HANDLE process_;
    std::size_t used_=0;
    std::vector<std::uint64_t>* trace_=nullptr;
};
struct Name {
    std::string text;
    std::array<std::uint32_t,2> raw{};
    bool operator==(const Name&) const = default;
};
struct Sample {
    Json metadata;
    std::vector<std::uint64_t> pointers;
    std::vector<std::uint64_t> class_addresses,field_addresses;
    bool operator==(const Sample&) const = default;
};
bool identifier(std::uint16_t unit,bool first) noexcept {
    return (unit>='A' && unit<='Z') || (unit>='a' && unit<='z') || unit=='_' ||
        (!first && unit>='0' && unit<='9');
}
class Names {
public:
    Names(Reader& reader,std::uint64_t module,Sample& sample):reader_(reader),sample_(sample) {
        table_=reader_.read<std::uint64_t>(module,0x1cee4b8);
        count_=reader_.read<std::int32_t>(module,0x1cee4c0);
        if (!table_ || count_<=0) fail("names_unavailable");
        sample_.pointers.push_back(table_);
        sample_.metadata["name_count"]=count_;
    }
    Name get(std::uint64_t object) {
        return get_raw(reader_.read<std::array<std::uint32_t,2>>(object,0x48));
    }
    Name get_raw(std::array<std::uint32_t,2> raw,bool package_name=false) {
        Name result;
        result.raw=raw;
        sample_.pointers.insert(sample_.pointers.end(),{raw[0],raw[1]});
        if (result.raw[0]>=static_cast<std::uint32_t>(count_)) fail("invalid_name_index");
        const auto entry=reader_.read<std::uint64_t>(table_,static_cast<std::uint64_t>(result.raw[0])*8);
        if (!entry) fail("names_unavailable");
        sample_.pointers.push_back(entry);
        const bool wide=(reader_.read<std::uint8_t>(entry,8)&1)!=0;
        for (std::size_t i=0;i<=name_limit;++i) {
            const auto unit=wide ? reader_.read<std::uint16_t>(entry,0x14+i*2) :
                static_cast<std::uint16_t>(reader_.read<std::uint8_t>(entry,0x14+i));
            if (!unit) {
                if (result.text.empty()) fail("invalid_name");
                return result;
            }
            if (i==name_limit || (!identifier(unit,i==0) && !(package_name && i!=0 && unit=='-')))
                fail("invalid_name");
            result.text.push_back(static_cast<char>(unit));
        }
        fail("invalid_name");
    }
private:
    Reader& reader_;
    Sample& sample_;
    std::uint64_t table_=0;
    std::int32_t count_=0;
};
void instance_zero(const Name& name) {
    // Raw instance is retained, never silently equated with an original instance-zero identity.
    if (name.raw[1]!=0) throw NameInstanceError(name.raw);
}
Json name_json(const Name& name) { return {{"name",name.text},{"fname_raw",name.raw}}; }
Json class_identity(Reader& reader,Names& names,Sample& out,std::uint64_t klass,
    std::string_view expected_name,std::string_view expected_package,const char* error) {
    if (!klass) fail(error);
    const auto name=names.get(klass);
    instance_zero(name);
    const auto outer=reader.read<std::uint64_t>(klass,0x40);
    if (!outer) fail(error);
    const auto package=names.get(outer);
    instance_zero(package);
    if (name.text!=expected_name || package.text!=expected_package || reader.read<std::uint64_t>(outer,0x40)!=0)
        fail(error);
    out.pointers.insert(out.pointers.end(),{klass,outer});
    return {{"class",name_json(name)},{"package",name_json(package)}};
}
std::uint64_t connection_cache_sample(Reader& reader,std::uint64_t module,Names& names,Sample& out,
    std::span<const hawken::probe::ExpectedClass> expected,bool read_cache=true) {
    const auto world=reader.read<std::uint64_t>(module,0x1cfffb0);
    out.pointers.push_back(world);
    if (!world) fail("connection_absent");
    const auto world_class=reader.read<std::uint64_t>(module,0x1cfff88);
    const auto world_identity=class_identity(reader,names,out,world_class,"World","Engine","world_identity_mismatch");
    const auto world_cdo=reader.read<std::uint64_t>(world_class,0x1d4);
    const auto world_vtable=reader.read<std::uint64_t>(world);
    const auto world_secondary=reader.read<std::uint64_t>(world,0x60);
    const auto world_backref=reader.read<std::uint64_t>(world,0x50);
    const auto world_flags=reader.read<std::uint64_t>(world,0x10);
    out.pointers.insert(out.pointers.end(),{world_class,world_cdo,world_vtable,world_secondary,world_backref,world_flags});
    if (world==world_cdo || world_vtable!=add(module,0x14e4d40) ||
        world_secondary!=add(module,0x14e4fd0) || world_backref!=world_class ||
        (world_flags&0x600)!=0) fail("world_identity_mismatch");
    // TheWorld is a constructor-path name, not an invariant of every loaded map.
    const auto driver=reader.read<std::uint64_t>(world,0x120);
    out.pointers.push_back(driver);
    if (!driver) fail("connection_absent");
    // Only this driver constructor-vtable is positively bound; no UObject name/flags reads.
    const auto driver_vtable=reader.read<std::uint64_t>(driver);
    if (driver_vtable!=add(module,0x15cd310)) fail("driver_identity_mismatch");
    const auto connection=reader.read<std::uint64_t>(driver,0x80);
    out.pointers.insert(out.pointers.end(),{driver_vtable,connection});
    if (!connection) fail("connection_absent");
    const auto connection_class=reader.read<std::uint64_t>(module,0x1d37a98);
    const auto connection_identity=class_identity(reader,names,out,connection_class,"TcpipConnection","IpDrv","connection_identity_mismatch");
    const auto connection_vtable=reader.read<std::uint64_t>(connection);
    const auto connection_backref=reader.read<std::uint64_t>(connection,0x50);
    const auto connection_flags=reader.read<std::uint64_t>(connection,0x10);
    out.pointers.insert(out.pointers.end(),{connection_class,connection_vtable,connection_backref,connection_flags});
    if (connection_vtable!=add(module,0x15ccfe0) || connection_backref!=connection_class ||
        (connection_flags&0x600)!=0) fail("connection_identity_mismatch");
    const auto archive_byte_swap=reader.read<std::uint32_t>(connection,0x164);
    out.pointers.insert(out.pointers.end(),{connection,0x164,archive_byte_swap});
    const auto map=reader.read<std::uint64_t>(connection,0x15c);
    out.pointers.push_back(map);
    if (!map) fail("connection_absent");
    const auto map_vtable=reader.read<std::uint64_t>(map);
    const auto map_backref=reader.read<std::uint64_t>(map,0x100);
    const bool primary=map_vtable==add(module,0x15885c0);
    if ((!primary && map_vtable!=add(module,0x15888b0)) ||
        map_backref!=connection) fail("map_identity_mismatch");
    // Map is not assumed to be a UObject; never inspect its +48/+50 as names/class.
    out.pointers.insert(out.pointers.end(),{world,world_class,world_cdo,world_vtable,world_secondary,world_backref,
        driver,driver_vtable,connection,connection_class,connection_vtable,connection_backref,map,map_vtable,map_backref});
    Json metadata={{"variant",primary ? "primary" : "alternative"},{"present",false},
        {"requested",read_cache},
        {"world_identity",world_identity},{"world_flags",world_flags},
        {"connection_identity",connection_identity},{"connection_flags",connection_flags},
        {"archive_byte_swap_raw",archive_byte_swap},
        {"derived_int32_byte_order",archive_byte_swap==0 ? "little" : "big"},
        {"selector_raw",reader.read<std::uint32_t>(module,0x1bc7d14)},
        {"status",primary ? (read_cache ? "cache_absent" : "not_requested") : "unsupported_variant"},
        {"classes",Json::array()}};
    if (primary && read_cache) {
        const auto cache=hawken::probe::read_connection_cache(reader,map,expected);
        out.pointers.insert(out.pointers.end(),cache.trace.begin(),cache.trace.end());
        if (cache.present) {
            if (cache.classes.size()!=expected.size()) fail("cache_identity_mismatch");
            for (std::size_t i=0;i<cache.classes.size();++i) {
                const auto& actual=cache.classes[i];
                Json klass={{"package",out.metadata["classes"][i]["package"]},{"class",out.metadata["classes"][i]["class"]},
                    {"base",actual.base},{"capacity",actual.capacity},{"fields",Json::array()}};
                for (const auto& field:actual.fields) {
                    const auto found=std::find(expected[i].fields.begin(),expected[i].fields.end(),field.address);
                    if (found==expected[i].fields.end()) fail("cache_field_mismatch");
                    const auto index=static_cast<std::size_t>(found-expected[i].fields.begin());
                    const auto& original=out.metadata["classes"][i]["fields"][index];
                    klass["fields"].push_back({{"name",original["name"]},{"fname_raw",original["fname_raw"]},
                        {"id",field.id},{"secondary_value",field.secondary_value}});
                }
                metadata["classes"].push_back(std::move(klass));
            }
            metadata["present"]=true;
            metadata["status"]="cache_observed";
        }
    }
    out.metadata["connection_cache"]=std::move(metadata);
    return primary ? map : 0;
}
Sample mech_sample(Reader& reader,std::uint64_t module) {
    Sample out;
    out.metadata={{"mode","mech_setup"},{"field_ids_accepted",false}};
    Names names(reader,module,out);
    const auto read=[&]<class T>(std::uint64_t address,std::uint64_t offset) {
        const auto value=reader.read<T>(address,offset);
        out.pointers.insert(out.pointers.end(),{address,offset,static_cast<std::uint64_t>(value)});
        return value;
    };
    const auto klass=read.template operator()<std::uint64_t>(module,0x1d3f988);
    out.metadata["pri_identity"]=class_identity(reader,names,out,klass,"R_PlayerReplicationInfo","HawkenGame","mech_identity");
    const auto cdo=read.template operator()<std::uint64_t>(klass,0x1d4);
    if (!cdo || read.template operator()<std::uint64_t>(cdo,0)!=add(module,0x17daa60) ||
        read.template operator()<std::uint64_t>(cdo,0x50)!=klass ||
        (read.template operator()<std::uint64_t>(cdo,0x10)&0x600)!=0x200) fail("mech_identity");
    // Inspect class metadata only; no MechSetupStruct value in the CDO or actor.
    std::vector<std::uint64_t> seen;
    auto child=read.template operator()<std::uint64_t>(klass,0x80);
    std::uint64_t property=0;
    while (child) {
        // pri-children-static.json verifies 291 own PRI children; retain bounded duplicate detection.
        if (seen.size()==512) fail("mech_pri_children_limit");
        if (std::find(seen.begin(),seen.end(),child)!=seen.end()) fail("mech_cycle");
        seen.push_back(child);
        const auto name=names.get(child); instance_zero(name);
        if (name.text=="MechSetupStruct") {
            if (property) fail("mech_identity");
            property=child;
        }
        child=read.template operator()<std::uint64_t>(child,0x60);
    }
    if (!property || read.template operator()<std::uint64_t>(property,0x40)!=klass) fail("mech_identity");
    std::optional<hawken::probe::MechPackageRange> range;
    try {
        const auto map=connection_cache_sample(reader,module,names,out,{},false);
        if (map) {
            const auto array=read.template operator()<std::uint64_t>(map,0x60);
            const auto count=read.template operator()<std::int32_t>(map,0x68);
            if (count<0) fail("mech_invalid");
            if (count>256) fail("mech_package_count_limit");
            if (count && !array) fail("mech_invalid");
            for (std::int32_t i=0;i<count;++i) {
                const auto entry=add(array,static_cast<std::uint64_t>(i)*0x58);
                const auto raw=reader.read<std::array<std::uint32_t,2>>(entry);
                const auto name=names.get_raw(raw,true); instance_zero(name);
                if (name.text!="HawkenGame") continue;
                if (range) fail("mech_identity");
                const auto package=read.template operator()<std::uint64_t>(entry,8);
                if (!package) fail("mech_identity");
                const auto package_name=names.get(package); instance_zero(package_name);
                const auto package_class=read.template operator()<std::uint64_t>(package,0x50);
                if (package_name.text!="HawkenGame" || read.template operator()<std::uint64_t>(package,0x40)!=0)
                    fail("mech_identity");
                class_identity(reader,names,out,package_class,"Package","Core","mech_identity");
                range=hawken::probe::MechPackageRange{
                    read.template operator()<std::int32_t>(entry,0x20),read.template operator()<std::int32_t>(entry,0x24)};
            }
            out.metadata["package_range_status"]=range ? "descriptor_observed" : "package_absent";
        } else out.metadata["package_range_status"]="unsupported_variant";
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what())!="connection_absent") throw;
        out.metadata["package_range_status"]="connection_absent";
    }
    auto identify=[&](std::uint64_t address) {
        hawken::probe::MechIdentity result;
        const auto name=names.get(address); instance_zero(name);
        result.name=name.text; result.instance=name.raw[1];
        const auto type=read.template operator()<std::uint64_t>(address,0x50);
        if (!type) fail("mech_identity");
        const auto type_name=names.get(type); instance_zero(type_name);
        result.type=type_name.text;
        class_identity(reader,names,out,type,type_name.text,"Core","mech_identity");
        auto outer=read.template operator()<std::uint64_t>(address,0x40);
        std::array<std::uint64_t,4> ancestors{};
        bool found_package=false;
        for (std::size_t i=0;i<ancestors.size() && outer;++i) {
            for (std::size_t j=0;j<i;++j) if (ancestors[j]==outer) fail("mech_cycle");
            ancestors[i]=outer;
            const auto outer_name=names.get(outer); instance_zero(outer_name);
            const auto parent=read.template operator()<std::uint64_t>(outer,0x40);
            if (!parent) { result.package=outer_name.text; found_package=true; break; }
            if (i==0) result.owner=outer_name.text;
            outer=parent;
        }
        if (!found_package) fail("mech_identity");
        return result;
    };
    const auto metadata=hawken::probe::read_mech_setup(reader,property,identify,range,module);
    out.pointers.insert(out.pointers.end(),metadata.trace.begin(),metadata.trace.end());
    out.metadata["ancestor_binding_verified"]=metadata.ancestor_binding_verified;
    out.metadata["complete_wire_metadata"]=metadata.complete_wire_metadata;
    out.metadata["virtual_eligibility_invoked"]=metadata.virtual_eligibility_invoked;
    out.metadata["eligibility_basis"]="reconstructed_primary_package_range_predicate";
    out.metadata["structs"]=Json::array();
    if (range) out.metadata["package_range"]={{"base",range->base},{"count",range->count}};
    const auto identity_json=[](const hawken::probe::MechIdentity& id) {
        return Json{{"name",id.name},{"owner",id.owner},{"package",id.package},{"type",id.type},{"instance",id.instance}};
    };
    for (const auto& item:metadata.structs) {
        Json entry={{"identity",identity_json(item.identity)},{"members",Json::array()},
            {"ancestor_binding_verified",item.ancestor_binding_verified},
            {"ancestor_chain_terminated",item.ancestor_chain_terminated},{"ancestor_present",nullptr}};
        if (item.ancestor_present) entry["ancestor_present"]=*item.ancestor_present;
        for (const auto& member:item.members) {
            Json field={{"identity",identity_json(member.identity)},{"net_index",member.net_index},
                {"array_dim",member.array_dim},{"element_size",member.element_size},{"offset",member.offset},
                {"referenced_type",nullptr},{"enum_count",nullptr},{"eligible",nullptr},{"mapped_index",nullptr}};
            if (member.referenced_type) field["referenced_type"]=identity_json(*member.referenced_type);
            if (member.enum_count) field["enum_count"]=*member.enum_count;
            if (member.eligible) field["eligible"]=*member.eligible;
            if (member.mapped_index) field["mapped_index"]=*member.mapped_index;
            entry["members"].push_back(std::move(field));
        }
        out.metadata["structs"].push_back(std::move(entry));
    }
    return out;
}
Sample sample(Reader& reader,std::uint64_t module,bool connection_cache,
    std::uint64_t supplied_root=0,std::span<const std::string_view> expected_packages=packages,
    std::span<const std::string_view> expected_names=classes,std::uint64_t expected_vtable=controller_vtable,
    std::uint64_t class_kind=0,std::uint64_t expected_map=0) {
    Sample out;
    out.metadata={{"selector_raw",reader.read<std::uint32_t>(module,0x1bb640c)},{"classes",Json::array()}};
    const auto root=supplied_root ? supplied_root : reader.read<std::uint64_t>(module,class_slot);
    if (!root) fail("cdo_unavailable_or_mismatch");
    const auto cdo=reader.read<std::uint64_t>(root,0x1d4);
    if (!cdo) fail("cdo_unavailable_or_mismatch");
    const auto cdo_flags=reader.read<std::uint64_t>(cdo,0x10);
    if (reader.read<std::uint64_t>(cdo)!=add(module,expected_vtable) ||
        (cdo_flags&0x600)!=0x200 ||
        reader.read<std::uint64_t>(cdo,0x50)!=root) fail("cdo_unavailable_or_mismatch");
    out.metadata["cdo_flags"]=cdo_flags;
    out.pointers={root,cdo};
    Names names(reader,module,out);
    if (expected_names.empty() || expected_names.size()>10 || expected_packages.size()!=expected_names.size())
        fail("class_chain_mismatch");
    std::vector<std::uint64_t> chain(expected_names.size()),arrays(expected_names.size());
    std::vector<std::int32_t> counts(expected_names.size());
    std::vector<hawken::probe::ExpectedClass> expected(expected_names.size());
    auto klass=root;
    std::size_t total=0;
    for (std::size_t i=0;i<chain.size();++i) {
        if (!klass) fail("class_chain_mismatch");
        for (std::size_t j=0;j<i;++j) if (chain[j]==klass) fail("class_cycle");
        chain[i]=klass;
        expected[i].address=klass;
        if (class_kind) {
            const auto type=reader.read<std::uint64_t>(klass,0x50);
            out.pointers.push_back(type);
            if (type!=class_kind) fail("player_class_identity");
        }
        const auto name=names.get(klass);
        instance_zero(name);
        const auto outer=reader.read<std::uint64_t>(klass,0x40);
        if (!outer) fail("class_chain_mismatch");
        const auto package=names.get(outer);
        instance_zero(package);
        if (name.text!=expected_names[i] || package.text!=expected_packages[i] || reader.read<std::uint64_t>(outer,0x40)!=0)
            fail("class_chain_mismatch");
        arrays[i]=reader.read<std::uint64_t>(klass,0x150);
        counts[i]=reader.read<std::int32_t>(klass,0x158);
        const auto capacity=reader.read<std::int32_t>(klass,0x15c);
        if (counts[i]<0 || capacity<counts[i] || capacity>static_cast<std::int32_t>(own_limit) ||
            (counts[i]>0 && !arrays[i])) fail("field_array_limit");
        total+=static_cast<std::size_t>(counts[i]);
        if (total>total_limit) fail("field_array_limit");
        const auto parent=reader.read<std::uint64_t>(klass,0x78);
        out.pointers.insert(out.pointers.end(),{klass,outer,arrays[i],parent});
        out.metadata["classes"].push_back({{"package",package.text},{"class",name.text},
            {"fname_raw",name.raw},{"package_fname_raw",package.raw},{"own_count",counts[i]},
            {"net_index",reader.read<std::int32_t>(klass,0x3c)},
            {"capacity",capacity},{"fields",Json::array()}});
        klass=parent;
    }
    if (klass!=0) fail("class_chain_mismatch");
    std::vector<std::uint64_t> fields;
    fields.reserve(total);
    for (std::size_t i=0;i<chain.size();++i) {
        for (std::int32_t j=0;j<counts[i];++j) {
            const auto field=reader.read<std::uint64_t>(arrays[i],static_cast<std::uint64_t>(j)*8);
            if (!field) fail("field_identity_mismatch");
            for (const auto prior:fields) if (prior==field) fail("field_cycle");
            fields.push_back(field);
            expected[i].fields.push_back(field);
            const auto outer=reader.read<std::uint64_t>(field,0x40);
            const auto type=reader.read<std::uint64_t>(field,0x50);
            if (outer!=chain[i] || !type) fail("field_identity_mismatch");
            const auto name=names.get(field),type_name=names.get(type);
            instance_zero(name); instance_zero(type_name);
            if (class_kind) class_identity(reader,names,out,type,type_name.text,"Core","field_type_mismatch");
            const auto cast=reader.read<std::uint32_t>(type,0x128);
            Json entry=name_json(name);
            entry["type"]=name_json(type_name);
            entry["net_index"]=reader.read<std::int32_t>(field,0x3c);
            entry["cast_flags"]=cast;
            out.pointers.insert(out.pointers.end(),{field,outer,type});
            if ((cast&0x8000)!=0 && (cast&0x80000)==0) {
                entry["kind"]="property";
                entry["property_flags"]=reader.read<std::uint64_t>(field,0x70);
                entry["array_dim"]=reader.read<std::int32_t>(field,0x68);
            } else if ((cast&0x80000)!=0 && (cast&0x8000)==0) {
                entry["kind"]="function";
                entry["function_flags"]=reader.read<std::uint32_t>(field,0xd0);
                const auto super=reader.read<std::uint64_t>(field,0x78);
                out.pointers.push_back(super);
                entry["super"]=nullptr;
                if (super) {
                    const auto super_owner=reader.read<std::uint64_t>(super,0x40);
                    std::size_t owner=0;
                    while (owner<chain.size() && chain[owner]!=super_owner) ++owner;
                    if (owner==chain.size()) fail("field_identity_mismatch");
                    const auto super_name=names.get(super);
                    instance_zero(super_name);
                    entry["super"]={{"package",expected_packages[owner]},{"class",expected_names[owner]},
                        {"field",name_json(super_name)}};
                    out.pointers.push_back(super_owner);
                }
            } else fail("field_type_mismatch");
            out.metadata["classes"][i]["fields"].push_back(std::move(entry));
        }
    }
    out.class_addresses=chain;
    out.field_addresses=fields;
    if (connection_cache) {
        const auto observed_map=connection_cache_sample(reader,module,names,out,expected);
        if (expected_map && observed_map!=expected_map) fail("unstable_sample");
    }
    return out;
}
struct PlayerLocator {
    std::int32_t name_count=0;
    std::size_t visited_nodes=0,selected_heads=0,lookups=0,discovery_bytes=0;
};
std::array<std::uint32_t,2> find_pawn_names(Reader& reader,std::uint64_t module,PlayerLocator& locator,Sample& out) {
    const auto initial=reader.used();
    hawken::probe::FNameLookupSnapshot progress;
    ++locator.lookups;
    try {
        const auto result=hawken::probe::read_player_fname_targets(reader,module,&progress);
        if (result.name_count!=out.metadata["name_count"].get<std::int32_t>()) fail("unstable_sample");
        out.pointers.insert(out.pointers.end(),result.trace.begin(),result.trace.end());
        out.metadata["name_lookup"]={{"selected_heads",result.head_count},{"visited_nodes",result.scanned_entries}};
        locator.name_count=result.name_count;
        locator.visited_nodes+=result.scanned_entries;
        locator.selected_heads+=result.head_count;
        locator.discovery_bytes+=reader.used()-initial;
        return result.indices;
    } catch (...) {
        locator.name_count=progress.name_count;
        locator.visited_nodes+=progress.scanned_entries;
        locator.selected_heads+=progress.head_count;
        locator.discovery_bytes+=reader.used()-initial;
        throw;
    }
}
std::uint64_t player_object_lookup(Reader& reader,std::uint64_t module,Sample& out,
    std::uint32_t name,std::uint64_t outer,std::uint64_t required_class) {
    const auto hash=(name^static_cast<std::uint32_t>(outer>>4))&0x7fff;
    auto object=reader.read<std::uint64_t>(module,(outer ? 0x1cae4b0 : 0x1c6e4b0)+
        static_cast<std::uint64_t>(hash)*8);
    std::vector<std::uint64_t> seen;
    std::uint64_t found=0;
    out.pointers.push_back(object);
    while (object) {
        if (seen.size()==256 || std::find(seen.begin(),seen.end(),object)!=seen.end()) fail("player_hash_limit");
        seen.push_back(object);
        const auto next=reader.read<std::uint64_t>(object,outer ? 0x18 : 8);
        const auto index=reader.read<std::int32_t>(object,0x38);
        out.pointers.insert(out.pointers.end(),{object,next,static_cast<std::uint64_t>(index)});
        if (index!=-1) {
            const auto tuple=reader.read<std::array<std::uint32_t,2>>(object,0x48);
            out.pointers.insert(out.pointers.end(),{tuple[0],tuple[1]});
            if (tuple==std::array<std::uint32_t,2>{name,0}) {
                const auto flags=reader.read<std::uint64_t>(object,0x10);
                const auto actual_outer=reader.read<std::uint64_t>(object,0x40);
                const auto type=reader.read<std::uint64_t>(object,0x50);
                out.pointers.insert(out.pointers.end(),{flags,actual_outer,type});
                // Same conservative exclusion as the existing content locator.
                if (!(flags&0x40000000000ULL) && actual_outer==outer && (!required_class || type==required_class)) {
                    if (found) fail("player_object_ambiguous");
                    found=object;
                }
            }
        }
        object=next;
    }
    if (!found) fail("player_object_absent");
    return found;
}
struct PlayerRange { std::string name; std::uint64_t package; std::int32_t base,count; };
std::vector<PlayerRange> player_ranges(Reader& reader,std::uint64_t map,Names& names,Sample& out) {
    std::vector<PlayerRange> ranges;
    const auto array=reader.read<std::uint64_t>(map,0x60);
    const auto count=reader.read<std::int32_t>(map,0x68);
    out.pointers.insert(out.pointers.end(),{array,static_cast<std::uint64_t>(count)});
    if (count<0 || count>256 || (count && !array)) fail("player_package_limit");
    constexpr std::array<std::string_view,8> wanted={"Core","Engine","HawkenGame","VS-Wreckage",
        "GameFramework","UDKBase","BaseAI","HawkenGameContent"};
    out.metadata["package_ranges"]=Json::array();
    for (std::int32_t i=0;i<count;++i) {
        const auto entry=add(array,static_cast<std::uint64_t>(i)*0x58);
        const auto descriptor_name=names.get_raw(reader.read<std::array<std::uint32_t,2>>(entry),true);
        instance_zero(descriptor_name);
        if (std::find(wanted.begin(),wanted.end(),descriptor_name.text)==wanted.end()) continue;
        if (std::any_of(ranges.begin(),ranges.end(),[&](const auto& item){return item.name==descriptor_name.text;}))
            fail("player_package_identity");
        const auto package=reader.read<std::uint64_t>(entry,8);
        const auto guid=reader.read<std::array<std::uint32_t,4>>(entry,0x10);
        const auto base=reader.read<std::int32_t>(entry,0x20),objects=reader.read<std::int32_t>(entry,0x24);
        out.pointers.insert(out.pointers.end(),{package,static_cast<std::uint64_t>(base),static_cast<std::uint64_t>(objects)});
        out.pointers.insert(out.pointers.end(),guid.begin(),guid.end());
        if (!package || base<0 || objects<0 || static_cast<std::uint64_t>(base)+static_cast<std::uint64_t>(objects)>0x80000000ULL)
            fail("player_package_identity");
        const auto actual_name=names.get(package); instance_zero(actual_name);
        const auto type=reader.read<std::uint64_t>(package,0x50);
        const auto outer=reader.read<std::uint64_t>(package,0x40);
        out.pointers.insert(out.pointers.end(),{type,outer});
        if (actual_name!=descriptor_name || outer!=0) fail("player_package_identity");
        class_identity(reader,names,out,type,"Package","Core","player_package_identity");
        ranges.push_back({descriptor_name.text,package,base,objects});
        out.metadata["package_ranges"].push_back({{"package",descriptor_name.text},{"guid",guid},
            {"base",base},{"count",objects}});
    }
    return ranges;
}
void player_eligibility(Reader& reader,Sample& item,const std::vector<PlayerRange>& ranges) {
    // Reconstruction of primary 7ace0/86ac0 only; no remote virtual call.
    for (std::size_t i=0;i<item.class_addresses.size();++i) {
        auto& klass=item.metadata["classes"][i];
        const auto range=std::find_if(ranges.begin(),ranges.end(),[&](const auto& value){return value.name==klass["package"].get<std::string>();});
        klass["eligible"]=nullptr;
        for (auto& field:klass["fields"]) field["eligible"]=nullptr;
        if (range==ranges.end()) continue;
        const auto package=reader.read<std::uint64_t>(item.class_addresses[i],0x40);
        item.pointers.push_back(package);
        if (package!=range->package) fail("player_package_identity");
        const auto eligible=[&](const Json& index){const auto n=index.get<std::int32_t>();return n>=0 && n<range->count;};
        klass["eligible"]=eligible(klass["net_index"]);
        for (auto& field:klass["fields"]) field["eligible"]=eligible(field["net_index"]);
    }
    item.metadata["eligibility_basis"]="reconstructed_primary_package_range_predicate";
    item.metadata["virtual_eligibility_invoked"]=false;
}
Sample player_links_sample(Reader& reader,std::uint64_t module,PlayerLocator& locator) {
    Sample out; out.metadata={{"mode","player_links"},{"field_ids_accepted",false},{"roots",Json::array()}};
    Names names(reader,module,out);
    const auto map=connection_cache_sample(reader,module,names,out,{},false);
    if (!map) fail("player_map_unsupported");
    const auto ranges=player_ranges(reader,map,names,out);
    const auto kind=reader.read<std::uint64_t>(module,0x1bda990);
    class_identity(reader,names,out,kind,"Class","Core","player_class_identity");
    const auto controller=reader.read<std::uint64_t>(module,class_slot);
    const auto pri=reader.read<std::uint64_t>(module,0x1d3f988);
    out.pointers.insert(out.pointers.end(),{kind,controller,pri});
    if (!controller || !pri) fail("player_class_identity");
    const auto target_names=find_pawn_names(reader,module,locator,out);
    const auto package_name=names.get_raw({target_names[0],0}),pawn_name=names.get_raw({target_names[1],0});
    if (package_name.text!="HawkenGameContent" || pawn_name.text!="R_PawnContent") fail("player_class_identity");
    const auto package=player_object_lookup(reader,module,out,target_names[0],0,0);
    const auto package_type=reader.read<std::uint64_t>(package,0x50);
    class_identity(reader,names,out,package_type,"Package","Core","player_class_identity");
    const auto pawn=player_object_lookup(reader,module,out,target_names[1],package,kind);
    const auto pawn_cdo=reader.read<std::uint64_t>(pawn,0x1d4);
    if (!pawn_cdo) fail("cdo_unavailable_or_mismatch");
    const auto pawn_cdo_name=names.get(pawn_cdo); instance_zero(pawn_cdo_name);
    const auto pawn_cdo_outer=reader.read<std::uint64_t>(pawn_cdo,0x40);
    const auto pawn_getter=reader.read<std::uint64_t>(add(module,0x17de0c0),0x638);
    out.pointers.insert(out.pointers.end(),{pawn_cdo,pawn_cdo_outer,pawn_getter});
    if (pawn_cdo_name.text!="Default__R_PawnContent" || pawn_cdo_outer!=package || pawn_getter!=add(module,0x379e20))
        fail("cdo_unavailable_or_mismatch");
    constexpr std::array<std::string_view,7> pri_packages={"HawkenGame","HawkenGame","Engine","Engine","Engine","Engine","Core"};
    constexpr std::array<std::string_view,7> pri_classes={"R_PlayerReplicationInfo","HawkenPRI_Base","PlayerReplicationInfo","ReplicationInfo","Info","Actor","Object"};
    constexpr std::array<std::string_view,10> pawn_packages={"HawkenGameContent","HawkenGame","HawkenGame","HawkenGame","BaseAI","UDKBase","GameFramework","Engine","Engine","Core"};
    constexpr std::array<std::string_view,10> pawn_classes={"R_PawnContent","R_Pawn","R_PawnNative","R_PawnBase","BaseAIPawn","UDKPawn","GamePawn","Pawn","Actor","Object"};
    const std::array<std::uint64_t,3> roots={controller,pri,pawn};
    std::vector<std::uint64_t> unique_fields;
    for (std::size_t i=0;i<roots.size();++i) {
        Sample item=i==0 ? sample(reader,module,true,roots[i],packages,classes,controller_vtable,kind,map) : i==1 ?
            sample(reader,module,true,roots[i],pri_packages,pri_classes,0x17daa60,kind,map) :
            sample(reader,module,true,roots[i],pawn_packages,pawn_classes,0x17de0c0,kind,map);
        for (const auto field:item.field_addresses) {
            if (std::find(unique_fields.begin(),unique_fields.end(),field)==unique_fields.end()) {
                if (unique_fields.size()==total_limit) fail("field_array_limit");
                unique_fields.push_back(field);
            }
        }
        player_eligibility(reader,item,ranges);
        const auto& cache=item.metadata["connection_cache"];
        if (!cache["present"].get<bool>()) fail("player_cache_absent");
        if (cache["classes"].empty()) fail("cache_class_mismatch");
        const auto& leaf=cache["classes"][0];
        item.metadata["exclusive_bound"]=leaf["base"].get<std::int32_t>()+static_cast<std::int32_t>(leaf["fields"].size());
        item.metadata["field_ids_accepted"]=false;
        out.pointers.insert(out.pointers.end(),item.pointers.begin(),item.pointers.end());
        out.metadata["roots"].push_back(std::move(item.metadata));
    }
    out.metadata["locator_name_count"]=locator.name_count;
    return out;
}

Sample live_player_sample(Reader& reader,std::uint64_t module) {
    Sample out;
    // Successful reads only, without additional remote reads. The shared 1MiB
    // read budget bounds this internal trace; no addresses/bytes are serialized.
    struct TraceScope {
        Reader& reader;
        std::vector<std::uint64_t>* previous;
        ~TraceScope() { reader.trace_to(previous); }
    } trace_scope{reader,reader.trace_to(&out.pointers)};
    Names names(reader,module,out);
    const auto kind=reader.read<std::uint64_t>(module,0x1bda990);
    if (!kind) fail("live_identity");
    struct LiveValidatedPackage { std::uint64_t address; Name identity; };
    std::vector<LiveValidatedPackage> live_validated_packages;
    const auto identify=[&](std::uint64_t object) {
        // Called only for class/property metadata, never live actor names.
        const auto name=names.get(object);
        std::vector<std::uint64_t> seen;
        auto outer=reader.read<std::uint64_t>(object,0x40);
        while (outer) {
            if (std::find(seen.begin(),seen.end(),outer)!=seen.end()) fail("live_cycle");
            if (seen.size()==4) fail("live_limit");
            seen.push_back(outer);
            const auto parent=reader.read<std::uint64_t>(outer,0x40);
            if (!parent) {
                const auto cached=std::find_if(live_validated_packages.begin(),live_validated_packages.end(),
                    [&](const auto& item) { return item.address==outer; });
                if (cached!=live_validated_packages.end())
                    return hawken::probe::LiveIdentity{name.text,cached->identity.text,name.raw[1]};
                const auto package=names.get(outer);
                const auto package_type=reader.read<std::uint64_t>(outer,0x50);
                if (!package_type || reader.read<std::uint64_t>(package_type,0x50)!=kind || package.raw[1]!=0)
                    fail("live_identity");
                class_identity(reader,names,out,package_type,"Package","Core","live_identity");
                live_validated_packages.push_back({outer,package});
                return hawken::probe::LiveIdentity{name.text,package.text,name.raw[1]};
            }
            outer=parent;
        }
        fail("live_identity");
    };
    const auto value=hawken::probe::read_live_player(reader,module,identify,true);
    out.pointers.insert(out.pointers.end(),value.trace.begin(),value.trace.end());
    // Replace Names metadata: neither raw FNames nor metadata names escape this mode.
    out.metadata={{"local_player_present",value.local_player_present},
        {"controller_present",value.controller_present},{"pawn_present",value.pawn_present},
        {"pri_present",value.pri_present},{"controller_player_matches",value.controller_player_matches},
        {"first_entry_only",true},{"world_membership_verified",false}};
    if (value.pawn_offset) out.metadata["pawn_offset"]=*value.pawn_offset;
    if (value.pri_offset) out.metadata["pri_offset"]=*value.pri_offset;
    if (value.inventory) {
        const auto& inventory_snapshot=*value.inventory;
        out.metadata["inventory"]={
            {"pawn_invmanager_present",inventory_snapshot.pawn_invmanager_present},
            {"pawn_dual_weapon_present",inventory_snapshot.pawn_dual_weapon_present},
            {"pawn_weapon_present",inventory_snapshot.pawn_weapon_present},
            {"pawn_weapon_is_dual",inventory_snapshot.pawn_weapon_is_dual},
            {"manager_instigator_is_pawn",inventory_snapshot.manager_instigator_is_pawn},
            {"manager_chain_is_dual",inventory_snapshot.manager_chain_is_dual},
            {"dual_instigator_is_pawn",inventory_snapshot.dual_instigator_is_pawn},
            {"dual_invmanager_matches",inventory_snapshot.dual_invmanager_matches}};
        if (inventory_snapshot.manager_role)
            out.metadata["inventory"]["manager_role"]=*inventory_snapshot.manager_role;
        if (inventory_snapshot.pawn_role)
            out.metadata["inventory"]["pawn_role"]=*inventory_snapshot.pawn_role;
        if (inventory_snapshot.controller_role)
            out.metadata["inventory"]["controller_role"]=*inventory_snapshot.controller_role;
        if (inventory_snapshot.pawn_physics)
            out.metadata["inventory"]["pawn_physics"]=*inventory_snapshot.pawn_physics;
        if (inventory_snapshot.dual_role)
            out.metadata["inventory"]["dual_role"]=*inventory_snapshot.dual_role;
        const auto inventory_bool=[&](const char* label, const std::optional<hawken::probe::LiveBoolSnapshot>& field) {
            if (field) out.metadata["inventory"][label]={{"value",field->value},
                {"metadata_offset",field->metadata_offset},{"bit_mask",field->bit_mask}};
        };
        inventory_bool("dual_server_can_fire",inventory_snapshot.dual_server_can_fire);
        inventory_bool("dual_shutdown",inventory_snapshot.dual_shutdown);
        inventory_bool("dual_client_shutdown",inventory_snapshot.dual_client_shutdown);
        inventory_bool("pawn_jump_capable",inventory_snapshot.pawn_jump_capable);
        inventory_bool("pawn_can_move",inventory_snapshot.pawn_can_move);
        inventory_bool("pawn_no_weapon_firing",inventory_snapshot.pawn_no_weapon_firing);
        if (inventory_snapshot.pawn_jump_z)
            out.metadata["inventory"]["pawn_jump_z"]=*inventory_snapshot.pawn_jump_z;
        if (inventory_snapshot.pawn_thruster_energy)
            out.metadata["inventory"]["pawn_thruster_energy"]=*inventory_snapshot.pawn_thruster_energy;
        if (inventory_snapshot.pawn_thruster_jump_energy)
            out.metadata["inventory"]["pawn_thruster_jump_energy"]=*inventory_snapshot.pawn_thruster_jump_energy;
        if (inventory_snapshot.pawn_move_state)
            out.metadata["inventory"]["pawn_move_state"]=*inventory_snapshot.pawn_move_state;
    }
    return out;
}
Sample main_menu_sync_sample(Reader& reader,std::uint64_t module) {
    Sample out;
    struct TraceScope {
        Reader& reader;
        std::vector<std::uint64_t>* previous;
        ~TraceScope() { reader.trace_to(previous); }
    } trace_scope{reader,reader.trace_to(&out.pointers)};
    Names names(reader,module,out);
    const auto kind=reader.read<std::uint64_t>(module,0x1bda990);
    if (!kind) fail("menu_identity");
    struct ValidatedPackage { std::uint64_t address; Name identity; };
    // Snapshot-local only: every child name and Outer chain is still read.
    // Package identity/Package-class validation is repeated in the next sample.
    std::vector<ValidatedPackage> validated_packages;
    const auto identify=[&](std::uint64_t object) {
        // Metadata identities only; never inspect live movie or account names.
        const auto name=names.get(object);
        std::vector<std::uint64_t> seen;
        auto outer=reader.read<std::uint64_t>(object,0x40);
        while (outer) {
            if (std::find(seen.begin(),seen.end(),outer)!=seen.end()) fail("menu_cycle");
            if (seen.size()==4) fail("menu_limit");
            seen.push_back(outer);
            const auto parent=reader.read<std::uint64_t>(outer,0x40);
            if (!parent) {
                const auto cached=std::find_if(validated_packages.begin(),validated_packages.end(),
                    [&](const auto& package_entry) { return package_entry.address==outer; });
                if (cached!=validated_packages.end())
                    return hawken::probe::LiveIdentity{name.text,cached->identity.text,name.raw[1]};
                const auto package=names.get(outer);
                const auto package_type=reader.read<std::uint64_t>(outer,0x50);
                if (!package_type || reader.read<std::uint64_t>(package_type,0x50)!=kind || package.raw[1]!=0)
                    fail("menu_identity");
                class_identity(reader,names,out,package_type,"Package","Core","menu_identity");
                validated_packages.push_back({outer,package});
                return hawken::probe::LiveIdentity{name.text,package.text,name.raw[1]};
            }
            outer=parent;
        }
        fail("menu_identity");
    };
    const auto value=hawken::probe::read_main_menu_sync(reader,module,identify);
    out.pointers.insert(out.pointers.end(),value.trace.begin(),value.trace.end());
    using State=hawken::probe::MainMenuSyncState;
    const char* state="no_local_player";
    switch (value.state) {
    case State::no_local_player: break;
    case State::no_controller: state="no_controller"; break;
    case State::no_menu: state="no_menu"; break;
    case State::closed_menu: state="closed_menu"; break;
    case State::owner_mismatch: state="owner_mismatch"; break;
    case State::observed: state="observed"; break;
    }
    // Only fixed names and booleans escape. A closed movie is not active UI;
    // all flags true does not establish callback delivery or UI completion.
    out.metadata={{"menu_state",state},{"first_entry_only",true},{"ui_completion_verified",false}};
    if (value.state==State::observed || value.state==State::closed_menu) {
        out.metadata["movie_open"]=value.movie_open;
        out.metadata["all_synced"]=value.all_synced;
        auto flags=Json::object();
        for (std::size_t i=0;i<value.flags.size();++i)
            flags[std::string(hawken::probe::main_menu_sync_flag_names[i])]=value.flags[i];
        out.metadata["flags"]=std::move(flags);
    }
    return out;
}
Sample gri_template_sample(Reader& reader,std::uint64_t module,PlayerLocator& locator) {
    Sample out;
    struct TraceScope {
        Reader& reader;
        std::vector<std::uint64_t>* previous;
        ~TraceScope() { reader.trace_to(previous); }
    } trace_scope{reader,reader.trace_to(&out.pointers)};
    Names names(reader,module,out);
    const auto kind=reader.read<std::uint64_t>(module,0x1bda990);
    if (!kind || reader.read<std::uint64_t>(kind,0x50)!=kind) fail("gri_identity");
    class_identity(reader,names,out,kind,"Class","Core","gri_identity");
    constexpr std::array<std::string_view,2> targets={"HawkenGame","R_GameReplicationInfo"};
    hawken::probe::FNameLookupSnapshot progress;
    const auto initial=reader.used();
    ++locator.lookups;
    const auto record_lookup=[&] {
        locator.name_count=progress.name_count;
        locator.visited_nodes+=progress.scanned_entries;
        locator.selected_heads+=progress.head_count;
        locator.discovery_bytes+=reader.used()-initial;
    };
    try { hawken::probe::read_fixed_fname_targets(reader,module,targets,&progress); }
    catch (...) { record_lookup(); throw; }
    record_lookup();
    if (progress.name_count!=out.metadata["name_count"].get<std::int32_t>()) fail("unstable_sample");
    out.pointers.insert(out.pointers.end(),progress.trace.begin(),progress.trace.end());
    const auto package=player_object_lookup(reader,module,out,progress.indices[0],0,0);
    const auto package_type=reader.read<std::uint64_t>(package,0x50);
    if (!package_type || reader.read<std::uint64_t>(package_type,0x50)!=kind) fail("gri_identity");
    class_identity(reader,names,out,package_type,"Package","Core","gri_identity");
    const auto klass=player_object_lookup(reader,module,out,progress.indices[1],package,kind);
    auto identify=[&](std::uint64_t object) {
        hawken::probe::GriIdentity id;
        const auto name=names.get(object); instance_zero(name);
        id.name=name.text; id.instance=name.raw[1];
        const auto type=reader.read<std::uint64_t>(object,0x50);
        if (!type || reader.read<std::uint64_t>(type,0x50)!=kind) fail("gri_identity");
        const auto type_name=names.get(type); instance_zero(type_name);
        id.type=type_name.text;
        class_identity(reader,names,out,type,type_name.text,type==klass ? "HawkenGame" : "Core","gri_identity");
        auto outer=reader.read<std::uint64_t>(object,0x40);
        if (!outer) {
            if (id.type!="Package") fail("gri_identity");
            id.package=id.name;
            return id;
        }
        std::vector<std::uint64_t> seen;
        while (outer) {
            if (std::find(seen.begin(),seen.end(),outer)!=seen.end()) fail("gri_cycle");
            if (seen.size()==4) fail("gri_limit");
            seen.push_back(outer);
            const auto outer_name=names.get(outer); instance_zero(outer_name);
            const auto parent=reader.read<std::uint64_t>(outer,0x40);
            if (!parent) {
                const auto outer_type=reader.read<std::uint64_t>(outer,0x50);
                if (!outer_type || reader.read<std::uint64_t>(outer_type,0x50)!=kind) fail("gri_identity");
                class_identity(reader,names,out,outer_type,"Package","Core","gri_identity");
                id.package=outer_name.text;
                return id;
            }
            if (seen.size()==1) id.owner=outer_name.text;
            outer=parent;
        }
        fail("gri_identity");
    };
    const auto result=hawken::probe::read_gri_template(reader,module,klass,identify);
    if (!result.template_verified || !result.getpc_null || !result.unique_id_metadata_verified) fail("gri_identity");
    out.pointers.insert(out.pointers.end(),result.trace.begin(),result.trace.end());
    // Only fixed schema names, booleans and the reflected offset escape this mode.
    // No raw FNames, NetIndices, addresses, UniqueId value or ec word is serialized.
    out.metadata={{"mode","gri_template"},{"gri_class","HawkenGame.R_GameReplicationInfo"},
        {"template_verified",true},{"getpc_null",result.getpc_null},{"rotation_required",result.rotation_required},
        {"unique_id_metadata_verified",true},{"unique_id_property","Engine.PlayerReplicationInfo.UniqueId"},
        {"unique_id_type","Engine.OnlineSubsystem.UniqueNetId"},{"unique_id_offset",result.unique_id_offset}};
    return out;
}
int observe(DWORD pid,bool connection_cache,bool mech_setup,bool player_links,bool live_player,bool gri_template,bool menu_sync) {
    const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,pid));
    if (!process) fail("process_unavailable");
    if (!process_verified(process.get()) || hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)
        fail("original_identity_mismatch");
    const Handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid));
    if (!modules) fail("module_unavailable");
    MODULEENTRY32W module{}; module.dwSize=static_cast<DWORD>(sizeof(module));
    if (!Module32FirstW(modules.get(),&module) || !same_original_path(module.szExePath) ||
        module.modBaseSize<class_slot+sizeof(std::uint64_t)) fail("module_unavailable");
    const auto base=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module.modBaseAddr));
    Reader reader(process.get());
    PlayerLocator locator;
    Sample first,second;
    unsigned live_sample_phase=1;
    try {
        first=menu_sync ? main_menu_sync_sample(reader,base) : gri_template ? gri_template_sample(reader,base,locator) : live_player ? live_player_sample(reader,base) : player_links ? player_links_sample(reader,base,locator) :
            mech_setup ? mech_sample(reader,base) : sample(reader,base,connection_cache);
        live_sample_phase=2;
        second=menu_sync ? main_menu_sync_sample(reader,base) : gri_template ? gri_template_sample(reader,base,locator) : live_player ? live_player_sample(reader,base) : player_links ? player_links_sample(reader,base,locator) :
            mech_setup ? mech_sample(reader,base) : sample(reader,base,connection_cache);
    } catch (const std::runtime_error& error) {
        if (live_player && std::string_view(error.what())=="read_limit") {
            std::cout<<Json({{"status","inconclusive"},{"error","read_limit"},{"identity_verified",false},
                {"diagnostics",{{"sample_phase",live_sample_phase},{"read_bytes",reader.used()},
                    {"read_budget_bytes",read_budget}}}}).dump()<<'\n';
            return 1;
        }
        if ((!player_links && !gri_template) || std::string_view(error.what())!="read_limit") throw;
        std::cout<<Json({{"status","inconclusive"},{"error","read_limit"},{"identity_verified",false},
            {"field_ids_accepted",false},{"diagnostics",{{"locator_name_count",locator.name_count},
                {"locator_visited_nodes",locator.visited_nodes},{"locator_selected_heads",locator.selected_heads},
                {"locator_lookups",locator.lookups},{"locator_read_bytes",locator.discovery_bytes},
                {"total_read_bytes",reader.used()}}}}).dump()<<'\n';
        return 1;
    }
    const auto comparison=hawken::probe::compare_controller_samples(
        first.metadata,first.pointers,second.metadata,second.pointers,mech_setup);
    if (menu_sync) {
        if (!comparison.full_samples_equal) fail("unstable_sample");
        if (!process_verified(process.get()) || hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)
            fail("original_identity_mismatch");
        const bool observed=second.metadata["menu_state"]=="observed";
        std::cout<<Json({{"status",observed ? "observed" : "inconclusive"},
            {"identity_verified",true},{"samples_equal",true},{"atomic_snapshot",false},
            {"metadata",second.metadata}}).dump()<<'\n';
        return observed ? 0 : 1;
    }
    if (live_player) {
        if (!comparison.full_samples_equal) fail("unstable_sample");
        if (!process_verified(process.get()) || hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)
            fail("original_identity_mismatch");
        const bool local=second.metadata["local_player_present"].get<bool>();
        const bool controller=second.metadata["controller_present"].get<bool>();
        Json report={{"status",controller ? "observed" : "inconclusive"},
            {"identity_verified",true},{"samples_equal",true},{"metadata",second.metadata}};
        if (!controller) report["error"]=local ? "live_controller_absent" : "live_local_player_absent";
        std::cout<<report.dump()<<'\n';
        return controller ? 0 : 1;
    }
    if (!comparison.inspected_metadata_equal) {
        const auto section_equal=[&](const char* key) {
            const auto left=first.metadata.find(key);
            const auto right=second.metadata.find(key);
            return left==first.metadata.end() ? right==second.metadata.end() :
                right!=second.metadata.end() && *left==*right;
        };
        auto first_metadata=first.metadata;
        auto second_metadata=second.metadata;
        first_metadata.erase("name_count");
        second_metadata.erase("name_count");
        const Json diagnostics={{"name_count_equal",section_equal("name_count")},
            {"metadata_without_name_count_equal",first_metadata==second_metadata},
            {"connection_metadata_equal",section_equal("connection_cache")},
            {"structs_metadata_equal",section_equal("structs")},
            {"trace_equal",first.pointers==second.pointers},
            {"trace_size_equal",first.pointers.size()==second.pointers.size()}};
        std::cout<<Json({{"status","inconclusive"},{"error","unstable_sample"},
            {"identity_verified",false},{"diagnostics",diagnostics}}).dump()<<'\n';
        return 1;
    }
    if (!process_verified(process.get()) || (gri_template &&
        hawken::sha256_file(std::filesystem::path(original_path))!=original_sha)) fail("original_identity_mismatch");
    // Agreement is not atomicity; the disk hash does not attest all loaded bytes.
    Json report={{"status","observed"},{"identity_verified",true},
        {"samples_equal",comparison.full_samples_equal},{"full_samples_equal",comparison.full_samples_equal},
        {"inspected_metadata_equal",comparison.inspected_metadata_equal},
        {"name_count_changed",comparison.name_count_changed},
        {"field_ids_accepted",false},{"metadata",second.metadata}};
    if (gri_template) report["original_sha256"]=original_sha;
    if (player_links || gri_template) report["diagnostics"]={{"locator_name_count",locator.name_count},
        {"locator_visited_nodes",locator.visited_nodes},{"locator_selected_heads",locator.selected_heads},
        {"locator_lookups",locator.lookups},{"locator_read_bytes",locator.discovery_bytes},
        {"total_read_bytes",reader.used()}};
    std::cout<<report.dump()<<'\n';
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Usage: hawken-controller-netfields-probe PID [--connection-cache|--mech-setup|--player-links|--live-player|--gri-template|--menu-sync]\n"
            "Read-only bounded original NetFields metadata or menu sync flags; no process creation.\n";
        return 0;
    }
    DWORD pid=0;
    const bool connection_cache=argc==3 && std::string_view(argv[2])=="--connection-cache";
    const bool mech_setup=argc==3 && std::string_view(argv[2])=="--mech-setup";
    const bool player_links=argc==3 && std::string_view(argv[2])=="--player-links";
    const bool live_player=argc==3 && std::string_view(argv[2])=="--live-player";
    const bool gri_template=argc==3 && std::string_view(argv[2])=="--gri-template";
    const bool menu_sync=argc==3 && std::string_view(argv[2])=="--menu-sync";
    if ((argc!=2 && !connection_cache && !mech_setup && !player_links && !live_player && !gri_template && !menu_sync) || !parse_pid(argv[1],pid)) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"invalid_arguments\",\"identity_verified\":false}\n";
        return 2;
    }
    try { return observe(pid,connection_cache,mech_setup,player_links,live_player,gri_template,menu_sync); }
    catch (const NameInstanceError& error) {
        Json report={{"status","inconclusive"},{"error","nonoriginal_name_instance"},{"identity_verified",false}};
        if (!live_player && !gri_template && !menu_sync) report["rejected_fname_raw"]=error.raw();
        std::cout<<report.dump()<<'\n';
        return 1;
    }
    catch (const std::runtime_error& error) {
        // Only locally fixed codes are allowed to escape: file hashing may throw its own errors.
        constexpr std::array<std::string_view,78> codes={"invalid_address","read_limit","read_unavailable",
            "names_unavailable","invalid_name_index","invalid_name","nonoriginal_name_instance",
            "cdo_unavailable_or_mismatch","class_chain_mismatch","class_cycle","field_array_limit",
            "field_identity_mismatch","field_cycle","field_type_mismatch","process_unavailable","unstable_sample",
            "connection_absent","world_identity_mismatch","driver_identity_mismatch","connection_identity_mismatch",
            "map_identity_mismatch","cache_identity_mismatch","cache_field_mismatch",
            "cache_invalid","cache_limit","cache_cycle","cache_class_mismatch",
            "mech_invalid","mech_cycle","mech_identity",
            "mech_range_limit","mech_struct_limit","mech_children_limit","mech_total_children_limit",
            "mech_array_dim_limit","mech_struct_target_null","mech_pending_limit","mech_enum_limit",
            "mech_pri_children_limit","mech_package_count_limit",
            "player_names_ambiguous","player_names_absent","player_hash_limit","player_object_ambiguous",
            "player_object_absent","player_package_limit","player_package_identity","player_class_identity",
            "player_map_unsupported","player_cache_absent","player_names_uninitialized",
            "player_name_invalid","player_name_limit","player_name_cycle",
            "live_invalid","live_engine_absent","live_array_invalid","live_limit","live_identity",
            "live_cycle","live_property_missing","live_property_mismatch","live_backlink_mismatch",
            "gri_identity","gri_property_mismatch","gri_property_missing","gri_cycle","gri_limit","gri_invalid",
            "menu_invalid","menu_engine_absent","menu_array_invalid","menu_limit","menu_identity",
            "menu_cycle","menu_property_missing","menu_property_mismatch","menu_backlink_mismatch"};
        std::string_view code="observation_unavailable";
        for (const auto allowed:codes) if (allowed==error.what()) code=allowed;
        if (std::string_view(error.what())=="original_identity_mismatch" ||
            std::string_view(error.what())=="module_unavailable") code=error.what();
        std::cout<<Json({{"status","inconclusive"},{"error",code},{"identity_verified",false}}).dump()<<'\n';
        return 1;
    } catch (...) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"observation_unavailable\",\"identity_verified\":false}\n";
        return 1;
    }
}
