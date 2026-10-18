#include "native_host_gate.hpp"
#include <atomic>
#include <cstdint>

namespace hawken::probe::native_host {
namespace {
constinit std::atomic<bool> gate_reserved{false};
constinit std::atomic<NativeExec> original_exec{nullptr};

void hooked_exec(void* context, void* frame, void* result) {
    // Published before the slot can expose this function; immutable on success.
    const auto original = original_exec.load(std::memory_order_acquire);
    original(context, frame, result);
    if (result) *static_cast<std::uint32_t*>(result) = 0;
}
}

InstallResult install_gate(NativeExec* validated_exec_slot,
                           NativeExec expected_original) noexcept {
    if (!validated_exec_slot || !expected_original || expected_original == &hooked_exec ||
        reinterpret_cast<std::uintptr_t>(validated_exec_slot) %
            std::atomic_ref<NativeExec>::required_alignment != 0)
        return InstallResult::invalid_input;

    bool expected_reservation = false;
    if (!gate_reserved.compare_exchange_strong(expected_reservation, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return InstallResult::already_installed;

    original_exec.store(expected_original, std::memory_order_release);
    std::atomic_ref<NativeExec> slot(*validated_exec_slot);
    auto expected = expected_original;
    if (!slot.compare_exchange_strong(expected, &hooked_exec,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        original_exec.store(nullptr, std::memory_order_release);
        gate_reserved.store(false, std::memory_order_release);
        return InstallResult::unexpected_original;
    }
    return InstallResult::installed;
}
}
