#include "hawken/wreckage_trial.hpp"
#include "hawken/sha256.hpp"
#include <array>
#include <stdexcept>

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
}
