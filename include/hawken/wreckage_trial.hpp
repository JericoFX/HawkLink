#pragma once
#include "hawken/game_connection.hpp"
#include <filesystem>

namespace hawken::game {
// Read-only, explicit local trial for THIS pinned original installation.
// game_root must be absolute. Hash all seven fixed paths beneath
// HawkenGame/CookedPCConsole before returning: Core.upk, Engine.upk,
// HawkenGame.upk, VS-Wreckage.upk, HawkenDM_SF.upk, GameFramework.upk and
// UDKBase.upk. No caller manifest,
// alternate GUID, file mutation, network, package loading or automatic closure.
// Returns roots Core/Engine/HawkenGame/VS-Wreckage/GameFramework/UDKBase
// in that order, then explicit
// contained HawkenGameContent (export180) in HawkenDM_SF, and Welcome strings
// VS-Wreckage / HawkenGameContent.HawkenDM. The contained GUID/generations
// differ from the physical SF header metadata. This selected plan does not
// prove local resolution, sufficient world packages, ACK/Join or gameplay.
// Missing/mismatched assets or invalid root throw std::runtime_error with fixed
// message "invalid Wreckage trial assets"; no path/file/hash contents in errors.
WorldPublication load_wreckage_trial(const std::filesystem::path& game_root);
}
