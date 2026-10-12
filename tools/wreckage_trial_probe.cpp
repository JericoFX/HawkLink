#include "hawken/wreckage_trial.hpp"
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
        const auto plan=hawken::game::load_wreckage_trial(game_root);
        if (!verify_plan(plan)) throw std::runtime_error("plan validation failed");
        std::cout<<"{\"plan_verified\":true,\"root_packages\":7,"
            "\"contained_packages\":1,\"contained_archive_bytes\":73,"
            "\"welcome_archive_bytes\":48,\"client_runtime_verified\":false}\n";
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
