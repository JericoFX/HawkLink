#include "hawken/wreckage_trial.hpp"
#include "hawken/sha256.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include <array>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hawken::game {
WorldPublication load_wreckage_trial(const std::filesystem::path& game_root) {
    try {
        if (!game_root.is_absolute() || !std::filesystem::is_directory(game_root))
            throw std::runtime_error("invalid Wreckage trial assets");
        struct Pin { const char* file; const char* sha; };
        static constexpr std::array<Pin,8> pins{{
            {"Core.upk","c6351b52aae3a78f0819963518be8bfcac93864fe73b8cdd5d07990662474375"},
            {"Engine.upk","8fff59b404ea4f9dd41330da618432fe50e4a26ce7487379efeb76ce0c4e859c"},
            {"HawkenGame.upk","d1a33a064de4451e5a39e13a28e4a8f2913ce6d9b54ca65bae47118d32849ed0"},
            {"VS-Wreckage.upk","4de4929cec5201efca5403fc264063b25bb8b507bb9befbbc67f2c8b5713d0b7"},
            {"HawkenDM_SF.upk","20d88294963654ad2cda3f3a94987d1428b7e0807f238faab5979cf23cb85155"},
            {"GameFramework.upk","d51275d3d52e4461feb98b34e3bc64264130efb0618f1fe6b7981730ad63ab8b"},
            {"UDKBase.upk","4e2e6caf4141f8f01d6d1f45c9bcb570991b63c37b3d89eb1e0ba79f55d14064"},
            {"BaseAI.upk","c163a8a388d85ad3bc4e78fd1a3b56eff974be7018315ebf7c599ff4df41128c"}
        }};
        const auto cooked=game_root/"HawkenGame"/"CookedPCConsole";
        // Read-only snapshot verification. No enduring TOCTOU guarantee or
        // package load: every pin matches before any publication is created.
        for (const auto& pin:pins) {
            const auto path=cooked/pin.file;
            if (!std::filesystem::is_regular_file(path) || sha256_file(path) != pin.sha)
                throw std::runtime_error("invalid Wreckage trial assets");
        }
        WorldPublication result;
        result.packages={
            {"Core",{0xd5e7f186,0x445d03bf,0x3d1bf0b3,0xcd119059},{1639}},
            {"Engine",{0x9355a20f,0x4633387e,0x428dd998,0x28422a5b},{36697}},
            {"HawkenGame",{0xbf5a6721,0x4933092b,0xe8522f80,0x238fc144},{49101}},
            {"VS-Wreckage",{0x2131cfb1,0x4b176c61,0x4025a787,0x8f798a26},{7468}},
            {"GameFramework",{0xe2817f91,0x467b459d,0x58d93694,0x4dc980f2},{3855}},
            {"UDKBase",{0x0ded9ed8,0x41917767,0x629475b2,0xc67624e2},{2900}},
            {"BaseAI",{0xee8f1f1e,0x4bfcc99c,0xdd64debb,0xb269ff11},{3126}}
        };
        result.contained_packages={
            {{"HawkenGameContent",{0x5cd139c7,0x4aa73e14,0xbd1c8593,0xf86056dd},{533}},
                "HawkenDM_SF"}
        };
        using control::posthello::StringForm;
        result.welcome={
            {StringForm::narrow,std::u16string(u"VS-Wreckage",12)},
            {StringForm::narrow,std::u16string(u"HawkenGameContent.HawkenDM",27)}
        };
        return result;
    } catch (...) {
        throw std::runtime_error("invalid Wreckage trial assets");
    }
}

void configure_wreckage_trial(udp::UdpTrialConfig& config,
                             const std::filesystem::path& game_root) {
    if (game_root.empty()) return;
    auto publication=load_wreckage_trial(game_root);
    auto prepared=config;
    prepared.publication=std::move(publication);
    // Explicit local trial policy, not historical spawn selection or collision
    // proof. The retained original Controller CDO rotation bit0 is clear.
    prepared.controller=udp::InitialControllerPlan{
        {1,0,0,false},
        {0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},
        18053,{-1472,2432,193},std::nullopt,0};
    // Original PRI CDO38826 and Pawn CDO233 have retained rotation-bit0-clear
    // observations; their native getter returns no Controller byte.
    prepared.pri=udp::InitialPriPlan{
        {3,0,0,false},
        {0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},
        38826,{0,0,0},std::nullopt};
    prepared.pawn=udp::InitialPawnPlan{
        {2,0,0,false},
        {0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU},
        233,{-1472,2432,193},std::nullopt};
    // Conditional static schema: normal startup Link0/primaryMap0 without
    // SEEKFREEPACKAGEMAP. See controller-effective-field-cache/
    // actor-link-plan-20261013/contract.json and selector-loader-20261013/
    // contract.json. These links do not prove native possession or gameplay.
    prepared.links=udp::InitialActorLinksPlan{21,22,294,49,30,150};
    // Explicit pinned local profile: these four CDOs use the observed no-PC,
    // no-initial-rotation prefix. Shared field IDs remain conditional on the
    // qualified original PRIMARY/normal Link0 caches, not native attestation.
    // Position is local trial policy copied from Pawn; no attachment inferred.
    const auto inventory_actor=[&](std::uint16_t actor_channel,std::int32_t cdo_index) {
        return udp::InitialPawnPlan{{actor_channel,0,0,false},
            {0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},
            cdo_index,prepared.pawn->position,std::nullopt};
    };
    prepared.inventory_bootstrap=udp::InventoryBootstrapPlan{
        inventory_actor(5,33398),inventory_actor(6,43715),
        inventory_actor(7,44117),inventory_actor(8,44157),
        25,80,150,15,14,21,21,23,24,31,23,35,29,32,34,
        {true,true,true,true,true,true,true,true}};
    // Supported trial policy: a newly admitted Recruit starts walking, with
    // zero heat and no active effects. Admission binds revision/alive/active;
    // inventory ACKs do not authorize combat. This is not full Pawn simulation.
    prepared.inventory_bootstrap->combat=udp::PawnCombatBootstrapPlan{
        30,true,{0,false,false,0,false,0,false,false,false,0,0,1.5F,1,false}};
    static_assert(std::is_nothrow_move_assignable_v<udp::UdpTrialConfig>);
    config=std::move(prepared);
}
}
