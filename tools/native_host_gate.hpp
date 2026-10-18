#pragma once

namespace hawken::probe::native_host {
// Win64 native exec ABI: context, FFrame, result in the same three arguments.
using NativeExec = void(*)(void*, void*, void*);

enum class InstallResult {
    installed, invalid_input, unexpected_original, already_installed
};

// Research experiment only, default OFF: no automatic installation or I/O.
// Caller must first validate Engine.WorldInfo.IsClientOnlyBuild, its exact
// UFunction +0xf0 slot and expected_original == original module +0x400560.
// The slot must be writable, mapped and suitably aligned for atomic_ref;
// its storage, original executable and statically imported hook module must
// remain alive until process exit. No protection changes, disk patches,
// uninstall, dynamic-unload guarantee, or generic native-function hook API.
// Only one gate may install per process. A rejected call preserves its slot.
// The installed gate calls original with unchanged arguments, then replaces
// the entire UBOOL32 result with zero if result is nonnull. No hosting claim.
InstallResult install_gate(NativeExec* validated_exec_slot,
                           NativeExec expected_original) noexcept;
}
