#include "../tools/native_host_gate.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace {
using hawken::probe::native_host::InstallResult;
using hawken::probe::native_host::NativeExec;
using hawken::probe::native_host::install_gate;

struct ArgumentState { std::uint32_t value; };
struct Observation {
    void* context = nullptr;
    void* frame = nullptr;
    void* result = nullptr;
    std::uint32_t calls = 0;
    std::uint32_t result_before = 0;
    std::uint32_t original_return = 1;
};
Observation observation;

void original_exec(void* context, void* frame, void* result) {
    observation.context = context;
    observation.frame = frame;
    observation.result = result;
    ++observation.calls;
    ++static_cast<ArgumentState*>(context)->value;
    static_cast<ArgumentState*>(frame)->value += 3;
    if (result) {
        observation.result_before = *static_cast<std::uint32_t*>(result);
        *static_cast<std::uint32_t*>(result) = observation.original_return;
    }
}
void other_exec(void*, void*, void*) {}

// The successful slot and both native functions outlive the process-scoped gate.
alignas(std::atomic_ref<NativeExec>::required_alignment) NativeExec process_slot = &original_exec;
static_assert(noexcept(install_gate(nullptr, nullptr)));
}

TEST_CASE("Native host gate validates then intercepts one process lifetime slot") {
    // Ordered intentionally: a real process singleton has no test-only reset.
    NativeExec untouched = &original_exec;
    CHECK(install_gate(nullptr, &original_exec) == InstallResult::invalid_input);
    CHECK(install_gate(&untouched, nullptr) == InstallResult::invalid_input);
    CHECK(untouched == &original_exec);

    constexpr auto alignment = std::atomic_ref<NativeExec>::required_alignment;
    alignas(alignment) std::array<std::byte, sizeof(NativeExec) + alignment> storage{};
    if constexpr (alignment > 1) {
        const auto before = storage;
        auto* misaligned = reinterpret_cast<NativeExec*>(storage.data() + 1);
        CHECK(install_gate(misaligned, &original_exec) == InstallResult::invalid_input);
        CHECK(storage == before);
    }

    NativeExec wrong_slot = &other_exec;
    CHECK(install_gate(&wrong_slot, &original_exec) == InstallResult::unexpected_original);
    CHECK(wrong_slot == &other_exec);
    NativeExec empty_slot = nullptr;
    CHECK(install_gate(&empty_slot, &original_exec) == InstallResult::unexpected_original);
    CHECK(empty_slot == nullptr);

    REQUIRE(install_gate(&process_slot, &original_exec) == InstallResult::installed);
    REQUIRE(process_slot != nullptr);
    CHECK(process_slot != &original_exec);
    ArgumentState context{7};
    ArgumentState frame{20};
    std::uint32_t result = 0xaabbccdd;
    process_slot(&context, &frame, &result);
    CHECK(observation.calls == 1);
    CHECK(observation.context == &context);
    CHECK(observation.frame == &frame);
    CHECK(observation.result == &result);
    CHECK(observation.result_before == 0xaabbccdd);
    CHECK(context.value == 8);
    CHECK(frame.value == 23);
    CHECK(result == 0);

    observation.original_return = 0x12345678;
    result = 0xdeadbeef;
    process_slot(&context, &frame, &result);
    CHECK(observation.calls == 2);
    CHECK(observation.result_before == 0xdeadbeef);
    CHECK(context.value == 9);
    CHECK(frame.value == 26);
    CHECK(result == 0); // Clear the entire UBOOL32, not just its low byte.

    process_slot(&context, &frame, nullptr);
    CHECK(observation.calls == 3);
    CHECK(observation.result == nullptr);
    CHECK(context.value == 10);
    CHECK(frame.value == 29);

    const NativeExec installed = process_slot;
    CHECK(install_gate(&process_slot, &original_exec) == InstallResult::already_installed);
    CHECK(process_slot == installed);
    CHECK(install_gate(&untouched, &original_exec) == InstallResult::already_installed);
    CHECK(untouched == &original_exec);
    NativeExec hook_slot = installed;
    CHECK(install_gate(&hook_slot, installed) == InstallResult::invalid_input);
    CHECK(hook_slot == installed);
    CHECK(observation.calls == 3); // Installation never invokes the native exec.
}
