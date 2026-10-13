#pragma once
#include "hawken/game_connection.hpp"
#include <filesystem>

namespace hawken::udp { struct UdpTrialConfig; }

namespace hawken::game {
// Read-only, explicit local trial for THIS pinned original installation.
// game_root must be absolute. Hash all eight fixed paths beneath
// HawkenGame/CookedPCConsole before returning: Core.upk, Engine.upk,
// HawkenGame.upk, VS-Wreckage.upk, HawkenDM_SF.upk, GameFramework.upk,
// UDKBase.upk and BaseAI.upk. No caller manifest,
// alternate GUID, file mutation, network, package loading or automatic closure.
// Returns roots Core/Engine/HawkenGame/VS-Wreckage/GameFramework/UDKBase
// then BaseAI in that order, then explicit
// contained HawkenGameContent (export180) in HawkenDM_SF, and Welcome strings
// VS-Wreckage / HawkenGameContent.HawkenDM. The contained GUID/generations
// differ from the physical SF header metadata. This selected plan does not
// prove local resolution, sufficient world packages, ACK/Join or gameplay.
// Missing/mismatched assets or invalid root throw std::runtime_error with fixed
// message "invalid Wreckage trial assets"; no path/file/hash contents in errors.
WorldPublication load_wreckage_trial(const std::filesystem::path& game_root);

// Explicit executable trial composition. Empty root leaves config unchanged.
// For a nonempty root, verify the same eight original pins before committing
// publication, Controller/Pawn/PRI plans and conditional initial property links.
// Preserve port/server GUID/region; failed asset verification leaves config intact.
// This selected metadata does not prove native possession or gameplay.
void configure_wreckage_trial(udp::UdpTrialConfig& config,
                             const std::filesystem::path& game_root);
}
