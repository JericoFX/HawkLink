#pragma once
#include "native_host_lookup.hpp"
#include <cstdint>

namespace hawken::probe::native_host {
enum class RuntimeStatus : std::uint32_t {
    idle, pending, installed, rejected, timed_out, not_writable, failed
};

// Win64 research adapter, default OFF. Caller independently verifies the
// original EXE SHA256 4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2
// and calls outside DllMain. The module and statically imported DLL must stay
// loaded until process exit; there is no dynamic unload/reset contract.
// Only one exact -HawkLinkNativeHostGate argument enables a one-shot worker.
// Null input, invalid original module, absent/duplicate flag do not start it.
// Worker limits: 30 seconds, 60 attempts, 250ms retry sleep, 16MiB total
// attempted self-process reads. Independent matching samples, writable-slot
// validation and a final matching lookup precede CAS; no VirtualProtect,
// arbitrary invocation, executable-byte patch, Steam or routing change.
bool start_runtime(OriginalModule module, const wchar_t* original_command_line) noexcept;
RuntimeStatus runtime_status() noexcept;
}
