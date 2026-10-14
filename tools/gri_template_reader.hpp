#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hawken::probe {
// Fixed metadata identities only. No live account, actor UniqueId or FString value.
struct GriIdentity {
    std::string name,owner,package,type;
    std::uint32_t instance=0;
};
struct GriTemplateSnapshot {
    bool template_verified=false;
    bool getpc_null=false;
    bool rotation_required=false;
    bool unique_id_metadata_verified=false;
    std::int32_t unique_id_offset=0;
    // Internal equality trace; never serialize addresses or scalar words.
    std::vector<std::uint64_t> trace;
    bool operator==(const GriTemplateSnapshot&) const = default;
};

// Caller verifies the exact original process/path/SHA and locates the exact
// HawkenGame.R_GameReplicationInfo UClass using bounded fixed-name hash lookup.
// Reader charges all reads against the caller's shared 1MiB/two-sample budget.
// Identity guards: original class/CDO NetIndices9438/9439, CDO flag200/backref,
// native vtable13cbf70 and GetPC slot638 target379e20. Read actual CDO+ec bit0.
// RPRI global1d3f988 -> exact seven-class chain -> Engine.PlayerReplicationInfo
// own Children80/Next60 (cap512) -> UniqueId StructProperty, ArrayDim1,
// Offset8c<=65536, target Engine.OnlineSubsystem.UniqueNetId ScriptStruct.
// No UniqueId value is read. Two full snapshot traces must agree before output.
// Fixed errors: gri_identity, gri_property_mismatch, gri_property_missing,
// gri_cycle, gri_limit, gri_invalid; Reader errors propagate unchanged.
template<class Reader,class Identify>
GriTemplateSnapshot read_gri_template(Reader& gri_reader,std::uint64_t gri_module,
                                     std::uint64_t gri_klass,Identify& gri_identify) {
    const auto gri_fail=[](const char* gri_code) { throw std::runtime_error(gri_code); };
    GriTemplateSnapshot gri_out;
    const auto gri_read=[&]<class T>(std::uint64_t gri_base,std::uint64_t gri_offset=0) {
        constexpr auto gri_maximum=std::numeric_limits<std::uint64_t>::max();
        if (!gri_base || gri_offset>gri_maximum-gri_base || sizeof(T)-1>gri_maximum-(gri_base+gri_offset)) gri_fail("gri_invalid");
        const auto gri_value=gri_reader.template read<T>(gri_base,gri_offset);
        gri_out.trace.insert(gri_out.trace.end(),{gri_base,gri_offset,static_cast<std::uint64_t>(gri_value)});
        return gri_value;
    };
    const auto gri_matches=[](const GriIdentity& gri_id,std::string_view gri_expected_name,std::string_view gri_expected_package,
                         std::string_view gri_expected_type,std::string_view gri_expected_owner="") {
        return gri_id.instance==0 && gri_id.name==gri_expected_name && gri_id.package==gri_expected_package && gri_id.type==gri_expected_type && gri_id.owner==gri_expected_owner;
    };
    const auto gri_kind=gri_read.template operator()<std::uint64_t>(gri_module,0x1bda990);
    if (!gri_kind || !gri_klass || !gri_matches(gri_identify(gri_kind),"Class","Core","Class") ||
        gri_read.template operator()<std::uint64_t>(gri_klass,0x50)!=gri_kind ||
        !gri_matches(gri_identify(gri_klass),"R_GameReplicationInfo","HawkenGame","Class") ||
        gri_read.template operator()<std::int32_t>(gri_klass,0x3c)!=9438) gri_fail("gri_identity");
    const auto gri_package=gri_read.template operator()<std::uint64_t>(gri_klass,0x40);
    if (!gri_package || !gri_matches(gri_identify(gri_package),"HawkenGame","HawkenGame","Package") ||
        gri_read.template operator()<std::uint64_t>(gri_package,0x40)!=0) gri_fail("gri_identity");
    const auto gri_cdo=gri_read.template operator()<std::uint64_t>(gri_klass,0x1d4);
    if (!gri_cdo || !gri_matches(gri_identify(gri_cdo),"Default__R_GameReplicationInfo","HawkenGame","R_GameReplicationInfo") ||
        gri_read.template operator()<std::uint64_t>(gri_cdo,0x50)!=gri_klass ||
        gri_read.template operator()<std::uint64_t>(gri_cdo,0x40)!=gri_package ||
        gri_read.template operator()<std::int32_t>(gri_cdo,0x3c)!=9439) gri_fail("gri_identity");
    const auto gri_flags=gri_read.template operator()<std::uint64_t>(gri_cdo,0x10);
    if ((gri_flags&0x600)!=0x200 || (gri_flags&0x40000000000ULL)) gri_fail("gri_identity");
    constexpr std::uint64_t gri_native_vtable=0x13cbf70,gri_null_getter=0x379e20;
    if (gri_module>std::numeric_limits<std::uint64_t>::max()-gri_native_vtable ||
        gri_module>std::numeric_limits<std::uint64_t>::max()-gri_null_getter) gri_fail("gri_invalid");
    const auto gri_table=gri_read.template operator()<std::uint64_t>(gri_cdo);
    if (gri_table!=gri_module+gri_native_vtable ||
        gri_read.template operator()<std::uint64_t>(gri_table,0x638)!=gri_module+gri_null_getter) gri_fail("gri_identity");

    // Class metadata only. No CDO or instance of PlayerReplicationInfo is read.
    constexpr std::array<std::string_view,7> gri_pri_expected_classes={"R_PlayerReplicationInfo","HawkenPRI_Base",
        "PlayerReplicationInfo","ReplicationInfo","Info","Actor","Object"};
    constexpr std::array<std::string_view,7> gri_pri_expected_packages={"HawkenGame","HawkenGame",
        "Engine","Engine","Engine","Engine","Core"};
    std::array<std::uint64_t,7> gri_chain{};
    auto gri_current=gri_read.template operator()<std::uint64_t>(gri_module,0x1d3f988);
    for (std::size_t gri_i=0;gri_i<gri_chain.size();++gri_i) {
        if (std::find(gri_chain.begin(),gri_chain.begin()+static_cast<std::ptrdiff_t>(gri_i),gri_current)!=
            gri_chain.begin()+static_cast<std::ptrdiff_t>(gri_i)) gri_fail("gri_cycle");
        if (!gri_current || gri_read.template operator()<std::uint64_t>(gri_current,0x50)!=gri_kind ||
            !gri_matches(gri_identify(gri_current),gri_pri_expected_classes[gri_i],gri_pri_expected_packages[gri_i],"Class")) gri_fail("gri_property_mismatch");
        gri_chain[gri_i]=gri_current;
        gri_current=gri_read.template operator()<std::uint64_t>(gri_current,0x78);
    }
    if (gri_current) gri_fail("gri_property_mismatch");
    auto gri_child=gri_read.template operator()<std::uint64_t>(gri_chain[2],0x80);
    std::vector<std::uint64_t> gri_seen;
    std::uint64_t gri_property=0;
    while (gri_child) {
        if (std::find(gri_seen.begin(),gri_seen.end(),gri_child)!=gri_seen.end()) gri_fail("gri_cycle");
        if (gri_seen.size()==512) gri_fail("gri_limit");
        gri_seen.push_back(gri_child);
        const auto gri_id=gri_identify(gri_child);
        if (gri_id.name=="UniqueId") {
            if (gri_property || !gri_matches(gri_id,"UniqueId","Engine","StructProperty","PlayerReplicationInfo"))
                gri_fail("gri_property_mismatch");
            gri_property=gri_child;
        }
        gri_child=gri_read.template operator()<std::uint64_t>(gri_child,0x60);
    }
    if (!gri_property) gri_fail("gri_property_missing");
    const auto gri_type=gri_read.template operator()<std::uint64_t>(gri_property,0x50);
    if (!gri_type || gri_read.template operator()<std::uint64_t>(gri_type,0x50)!=gri_kind ||
        !gri_matches(gri_identify(gri_type),"StructProperty","Core","Class") ||
        gri_read.template operator()<std::uint64_t>(gri_property,0x40)!=gri_chain[2] ||
        gri_read.template operator()<std::int32_t>(gri_property,0x68)!=1) gri_fail("gri_property_mismatch");
    const auto gri_structure=gri_read.template operator()<std::uint64_t>(gri_property,0xa8);
    if (!gri_structure || !gri_matches(gri_identify(gri_structure),"UniqueNetId","Engine","ScriptStruct","OnlineSubsystem"))
        gri_fail("gri_property_mismatch");
    gri_out.unique_id_offset=gri_read.template operator()<std::int32_t>(gri_property,0x8c);
    if (gri_out.unique_id_offset<0 || gri_out.unique_id_offset>65536) gri_fail("gri_property_mismatch");

    // Only after both typed metadata guard sets succeed, inspect the scalar bit.
    gri_out.rotation_required=(gri_read.template operator()<std::uint32_t>(gri_cdo,0xec)&1)!=0;
    gri_out.template_verified=true;
    gri_out.getpc_null=true;
    gri_out.unique_id_metadata_verified=true;
    return gri_out;
}
}
