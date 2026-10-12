#include <windows.h>
#include <tlhelp32.h>
#include "hawken/sha256.hpp"
#include "connection_cache_reader.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
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
        return result;
    }
private:
    HANDLE process_;
    std::size_t used_=0;
};
struct Name {
    std::string text;
    std::array<std::uint32_t,2> raw{};
    bool operator==(const Name&) const = default;
};
struct Sample {
    Json metadata;
    std::vector<std::uint64_t> pointers;
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
        Name result;
        result.raw=reader_.read<std::array<std::uint32_t,2>>(object,0x48);
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
            if (i==name_limit || !identifier(unit,i==0)) fail("invalid_name");
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
void connection_cache_sample(Reader& reader,std::uint64_t module,Names& names,Sample& out,
    std::span<const hawken::probe::ExpectedClass> expected) {
    const auto world=reader.read<std::uint64_t>(module,0x1cfffb0);
    if (!world) fail("connection_absent");
    const auto world_class=reader.read<std::uint64_t>(module,0x1cfff88);
    const auto world_identity=class_identity(reader,names,out,world_class,"World","Engine","world_identity_mismatch");
    const auto world_cdo=reader.read<std::uint64_t>(world_class,0x1d4);
    const auto world_vtable=reader.read<std::uint64_t>(world);
    const auto world_secondary=reader.read<std::uint64_t>(world,0x60);
    const auto world_backref=reader.read<std::uint64_t>(world,0x50);
    const auto world_flags=reader.read<std::uint64_t>(world,0x10);
    if (world==world_cdo || world_vtable!=add(module,0x14e4d40) ||
        world_secondary!=add(module,0x14e4fd0) || world_backref!=world_class ||
        (world_flags&0x600)!=0) fail("world_identity_mismatch");
    // TheWorld is a constructor-path name, not an invariant of every loaded map.
    const auto driver=reader.read<std::uint64_t>(world,0x120);
    if (!driver) fail("connection_absent");
    // Only this driver constructor-vtable is positively bound; no UObject name/flags reads.
    const auto driver_vtable=reader.read<std::uint64_t>(driver);
    if (driver_vtable!=add(module,0x15cd310)) fail("driver_identity_mismatch");
    const auto connection=reader.read<std::uint64_t>(driver,0x80);
    if (!connection) fail("connection_absent");
    const auto connection_class=reader.read<std::uint64_t>(module,0x1d37a98);
    const auto connection_identity=class_identity(reader,names,out,connection_class,"TcpipConnection","IpDrv","connection_identity_mismatch");
    const auto connection_vtable=reader.read<std::uint64_t>(connection);
    const auto connection_backref=reader.read<std::uint64_t>(connection,0x50);
    const auto connection_flags=reader.read<std::uint64_t>(connection,0x10);
    if (connection_vtable!=add(module,0x15ccfe0) || connection_backref!=connection_class ||
        (connection_flags&0x600)!=0) fail("connection_identity_mismatch");
    const auto map=reader.read<std::uint64_t>(connection,0x15c);
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
        {"world_identity",world_identity},{"world_flags",world_flags},
        {"connection_identity",connection_identity},{"connection_flags",connection_flags},
        {"selector_raw",reader.read<std::uint32_t>(module,0x1bc7d14)},
        {"status",primary ? "cache_absent" : "unsupported_variant"},{"classes",Json::array()}};
    if (primary) {
        const auto cache=hawken::probe::read_connection_cache(reader,map,expected);
        out.pointers.insert(out.pointers.end(),cache.trace.begin(),cache.trace.end());
        if (cache.present) {
            if (cache.classes.size()!=expected.size()) fail("cache_identity_mismatch");
            for (std::size_t i=0;i<cache.classes.size();++i) {
                const auto& actual=cache.classes[i];
                Json klass={{"package",packages[i]},{"class",classes[i]},
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
}
Sample sample(Reader& reader,std::uint64_t module,bool connection_cache) {
    Sample out;
    out.metadata={{"selector_raw",reader.read<std::uint32_t>(module,0x1bb640c)},{"classes",Json::array()}};
    const auto root=reader.read<std::uint64_t>(module,class_slot);
    if (!root) fail("cdo_unavailable_or_mismatch");
    const auto cdo=reader.read<std::uint64_t>(root,0x1d4);
    if (!cdo) fail("cdo_unavailable_or_mismatch");
    const auto cdo_flags=reader.read<std::uint64_t>(cdo,0x10);
    if (reader.read<std::uint64_t>(cdo)!=add(module,controller_vtable) ||
        (cdo_flags&0x600)!=0x200 ||
        reader.read<std::uint64_t>(cdo,0x50)!=root) fail("cdo_unavailable_or_mismatch");
    out.metadata["cdo_flags"]=cdo_flags;
    out.pointers={root,cdo};
    Names names(reader,module,out);
    std::array<std::uint64_t,8> chain{},arrays{};
    std::array<std::int32_t,8> counts{};
    std::array<hawken::probe::ExpectedClass,8> expected{};
    auto klass=root;
    std::size_t total=0;
    for (std::size_t i=0;i<chain.size();++i) {
        if (!klass) fail("class_chain_mismatch");
        for (std::size_t j=0;j<i;++j) if (chain[j]==klass) fail("class_cycle");
        chain[i]=klass;
        expected[i].address=klass;
        const auto name=names.get(klass);
        instance_zero(name);
        const auto outer=reader.read<std::uint64_t>(klass,0x40);
        if (!outer) fail("class_chain_mismatch");
        const auto package=names.get(outer);
        instance_zero(package);
        if (name.text!=classes[i] || package.text!=packages[i] || reader.read<std::uint64_t>(outer,0x40)!=0)
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
                    entry["super"]={{"package",packages[owner]},{"class",classes[owner]},
                        {"field",name_json(super_name)}};
                    out.pointers.push_back(super_owner);
                }
            } else fail("field_type_mismatch");
            out.metadata["classes"][i]["fields"].push_back(std::move(entry));
        }
    }
    if (connection_cache) connection_cache_sample(reader,module,names,out,expected);
    return out;
}
int observe(DWORD pid,bool connection_cache) {
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
    const auto first=sample(reader,base,connection_cache),second=sample(reader,base,connection_cache);
    if (first!=second) fail("unstable_sample");
    if (!process_verified(process.get())) fail("original_identity_mismatch");
    // Agreement is not atomicity; the disk hash does not attest all loaded bytes.
    Json report={{"status","observed"},{"identity_verified",true},{"samples_equal",true},
        {"field_ids_accepted",false},{"metadata",second.metadata}};
    std::cout<<report.dump()<<'\n';
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Usage: hawken-controller-netfields-probe PID [--connection-cache]\n"
            "Read-only bounded original-controller NetFields metadata; no process creation.\n";
        return 0;
    }
    DWORD pid=0;
    const bool connection_cache=argc==3 && std::string_view(argv[2])=="--connection-cache";
    if ((argc!=2 && !connection_cache) || !parse_pid(argv[1],pid)) {
        std::cout<<"{\"status\":\"inconclusive\",\"error\":\"invalid_arguments\",\"identity_verified\":false}\n";
        return 2;
    }
    try { return observe(pid,connection_cache); }
    catch (const NameInstanceError& error) {
        std::cout<<Json({{"status","inconclusive"},{"error","nonoriginal_name_instance"},
            {"identity_verified",false},{"rejected_fname_raw",error.raw()}}).dump()<<'\n';
        return 1;
    }
    catch (const std::runtime_error& error) {
        // Only locally fixed codes are allowed to escape: file hashing may throw its own errors.
        constexpr std::array<std::string_view,27> codes={"invalid_address","read_limit","read_unavailable",
            "names_unavailable","invalid_name_index","invalid_name","nonoriginal_name_instance",
            "cdo_unavailable_or_mismatch","class_chain_mismatch","class_cycle","field_array_limit",
            "field_identity_mismatch","field_cycle","field_type_mismatch","process_unavailable","unstable_sample",
            "connection_absent","world_identity_mismatch","driver_identity_mismatch","connection_identity_mismatch",
            "map_identity_mismatch","cache_identity_mismatch","cache_field_mismatch",
            "cache_invalid","cache_limit","cache_cycle","cache_class_mismatch"};
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
