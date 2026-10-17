#include "hawken/wreckage_trial.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include "hawken/package_publication.hpp"
#include "hawken/package_map_ranges.hpp"
#include "hawken/posthello_control.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using hawken::control::packages::RootMetadata;
using Bytes=std::vector<std::uint8_t>;

Bytes literal_hex(std::string_view text) {
    const auto nibble=[](char c)->std::uint8_t {
        if (c>='0' && c<='9') return static_cast<std::uint8_t>(c-'0');
        if (c>='a' && c<='f') return static_cast<std::uint8_t>(c-'a'+10);
        throw std::logic_error("invalid probe literal");
    };
    if (text.size()%2!=0) throw std::logic_error("invalid probe literal");
    Bytes bytes;
    bytes.reserve(text.size()/2);
    for (std::size_t i=0;i<text.size();i+=2)
        bytes.push_back(static_cast<std::uint8_t>((nibble(text[i])<<4)|nibble(text[i+1])));
    return bytes;
}

bool verify_plan(const hawken::game::WorldPublication& plan) {
    // Original root header words/counts from original-plan-references.json.
    // This chosen order and descriptor shape are NEW local trial policy.
    const std::array<RootMetadata,7> roots{{
        {"Core",{0xd5e7f186U,0x445d03bfU,0x3d1bf0b3U,0xcd119059U},{1639}},
        {"Engine",{0x9355a20fU,0x4633387eU,0x428dd998U,0x28422a5bU},{36697}},
        {"HawkenGame",{0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},{49101}},
        {"VS-Wreckage",{0x2131cfb1U,0x4b176c61U,0x4025a787U,0x8f798a26U},{7468}},
        // Original summary words at 0x49, not export counts or candidate fieldIDs.
        // controller-effective-field-cache/parent-cache-prerequisite/contract.json
        {"GameFramework",{0xe2817f91U,0x467b459dU,0x58d93694U,0x4dc980f2U},{3855}},
        {"UDKBase",{0x0ded9ed8U,0x41917767U,0x629475b2U,0xc67624e2U},{2900}},
        // Original BaseAI summary, not export_count2859 or class NetIndex1188.
        // pawn-integration-readiness/baseai-original-metadata.json
        {"BaseAI",{0xee8f1f1eU,0x4bfcc99cU,0xdd64debbU,0xb269ff11U},{3126}}
    }};
    if (plan.packages.size()!=roots.size() || plan.contained_packages.size()!=1)
        return false;
    for (std::size_t i=0;i<roots.size();++i) {
        const auto& actual=plan.packages[i];
        if (actual.name!=roots[i].name || actual.identity!=roots[i].identity ||
            actual.generation_net_counts!=roots[i].generation_net_counts)
            return false;
    }
    const auto& contained=plan.contained_packages.front();
    // Export180, not the physical container header GUID. COUNT1 != netcount533.
    const std::array<std::uint32_t,4> hgc{
        0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU};
    if (contained.package.name!="HawkenGameContent" ||
        contained.package.identity!=hgc ||
        contained.package.generation_net_counts!=std::vector<std::uint32_t>{533} ||
        contained.container_name!="HawkenDM_SF")
        return false;
    std::vector<hawken::wire::packages::PackageGenerations> generations;
    for (const auto& root:plan.packages)
        generations.push_back({root.generation_net_counts,1});
    generations.push_back({contained.package.generation_net_counts,1});
    // Independent range oracle: old101660+BaseAI3126=104786; CDO local233.
    // Exercise the real range consumer with matched generation COUNT1 ACKs.
    // This is not concrete client PackageMap eligibility or actor creation.
    const auto ranges=hawken::wire::packages::build_ranges(generations);
    if (ranges.back().base!=104786 ||
        hawken::wire::packages::encode_index(ranges,plan.packages.size(),233)!=105019)
        return false;
    namespace post=hawken::control::posthello;
    constexpr post::Limits limits{512,256}; // Probe work budget, not native MTU.
    const auto archive=post::encode(hawken::control::packages::make_contained_package(
        contained,limits),hawken::control::Endian::little,limits);
    const auto expected_package=literal_hex(
        "07c739d15c143ea74a93851cbddd5660f8120000004861776b656e47616d65436f6e74656e7400"
        "01000000000000000000000000010000000c0000004861776b656e444d5f53460000");
    if (archive.size()!=73 || archive!=expected_package) return false;
    const auto welcome=post::encode(plan.welcome,hawken::control::Endian::little,limits);
    const auto expected_welcome=literal_hex(
        "010c00000056532d577265636b616765001b000000"
        "4861776b656e47616d65436f6e74656e742e4861776b656e444d00");
    return welcome.size()==48 && welcome==expected_welcome;
}

int run(const std::filesystem::path& game_root) {
    if (!game_root.is_absolute()) {
        std::cout<<"{\"argument_valid\":false}\n";
        return 2;
    }
    try {
        // Real original-asset loader; no injected checker or production hook.
        hawken::udp::UdpTrialConfig config{
            0,"11111111-1111-4111-8111-111111111111","probe"};
        // Exercise the same real SHA-gated configuration used by Main, including
        // its actor relationships. No sockets, account data or client execution.
        hawken::game::configure_wreckage_trial(config,game_root);
        if (!config.publication || !verify_plan(*config.publication) ||
            !config.controller || !config.pawn || !config.pri || !config.links ||
            !config.inventory_bootstrap)
            throw std::runtime_error("trial configuration missing");
        const auto& controller=*config.controller;
        const auto& pawn=*config.pawn;
        const auto& pri=*config.pri;
        const auto& links=*config.links;
        const auto& inventory=*config.inventory_bootstrap;
        const std::array<std::uint32_t,4> hg{
            0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U};
        const std::array<std::uint32_t,4> hgc{
            0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU};
        const auto fresh_channel=[](const auto& actor,std::uint16_t index) {
            return actor.channel.channel==index &&
                actor.channel.outgoing_sequence==0 &&
                actor.channel.incoming_sequence==0 && !actor.channel.open_confirmed;
        };
        // Independent original metadata oracle; trial channels/positions remain
        // explicit local policy. Dense IDs assume normal Link0 and PRIMARY map0.
        if (config.port!=0 || config.server_guid!="11111111-1111-4111-8111-111111111111" ||
            config.region!="probe" || !fresh_channel(controller,1) ||
            !fresh_channel(pawn,2) || !fresh_channel(pri,3) ||
            controller.archetype_package_guid!=hg || controller.archetype_local_net_index!=18053 ||
            pawn.archetype_package_guid!=hgc || pawn.archetype_local_net_index!=233 ||
            pri.archetype_package_guid!=hg || pri.archetype_local_net_index!=38826 ||
            controller.position!=hawken::wire::actor::QuantizedPosition{-1472,2432,193} ||
            pawn.position!=controller.position || pri.position!=hawken::wire::actor::QuantizedPosition{0,0,0} ||
            controller.rotation || pawn.rotation || pri.rotation || controller.net_player_index!=0 ||
            links.controller_pri_field_id!=21 || links.controller_pawn_field_id!=22 ||
            links.controller_exclusive_bound!=294 || links.pawn_controller_field_id!=49 ||
            links.pawn_pri_field_id!=30 || links.pawn_exclusive_bound!=150)
            throw std::runtime_error("trial configuration mismatch");
        const std::array inventory_actors{&inventory.manager,&inventory.dual_weapon,
            &inventory.primary_weapon,&inventory.secondary_weapon};
        constexpr std::array<std::int32_t,4> inventory_cdos{33398,43715,44117,44157};
        for (std::size_t index=0;index<inventory_actors.size();++index) {
            const auto& actor=*inventory_actors[index];
            if (!fresh_channel(actor,static_cast<std::uint16_t>(5+index)) ||
                actor.archetype_package_guid!=hg ||
                actor.archetype_local_net_index!=inventory_cdos[index] ||
                actor.position!=pawn.position || actor.rotation)
                throw std::runtime_error("trial inventory archetype mismatch");
        }
        const auto& eligible=inventory.eligibility;
        if (inventory.pawn_inv_manager_field_id!=25 || inventory.pawn_dual_weapon_field_id!=80 ||
            inventory.pawn_exclusive_bound!=150 || inventory.owner_field_id!=15 ||
            inventory.instigator_field_id!=14 || inventory.inventory_manager_field_id!=21 ||
            inventory.manager_chain_field_id!=21 ||
            inventory.manager_server_set_current_weapon_field_id!=23 ||
            inventory.manager_exclusive_bound!=24 || inventory.dual_hand_weap_field_id!=31 ||
            inventory.dual_client_given_to_field_id!=23 || inventory.dual_exclusive_bound!=35 ||
            inventory.hand_dual_weapon_field_id!=29 || inventory.primary_exclusive_bound!=32 ||
            inventory.secondary_exclusive_bound!=34 || !eligible.primary_package_map ||
            !eligible.actor_prefixes_correct || !eligible.archetypes_correct ||
            !eligible.actor_caches_correct || !eligible.scalar_links_correct ||
            !eligible.hand_weap_array_correct || !eligible.client_given_to_correct ||
            !eligible.server_set_current_weapon_correct)
            throw std::runtime_error("trial inventory schema mismatch");
        std::cout<<"{\"plan_verified\":true,\"root_packages\":7,"
            "\"contained_packages\":1,\"contained_archive_bytes\":73,"
            "\"welcome_archive_bytes\":48,\"actor_links_policy_verified\":true,"
            "\"inventory_policy_verified\":true,"
            "\"client_runtime_verified\":false}\n";
        return 0;
    } catch (...) {
        // Never expose supplied path, loader error text or original file data.
        std::cout<<"{\"plan_verified\":false}\n";
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc,wchar_t* argv[]) {
#else
int main(int argc,char* argv[]) {
#endif
    if (argc!=2) {
        std::cout<<"{\"argument_valid\":false}\n";
        return 2;
    }
    try {
        return run(std::filesystem::path(argv[1]));
    } catch (...) {
        std::cout<<"{\"argument_valid\":false}\n";
        return 2;
    }
}
