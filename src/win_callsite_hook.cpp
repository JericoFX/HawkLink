#include "hawken/win_callsite_hook.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hawken::client::diagnostics {
namespace {

std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept {
    const std::uintptr_t remainder = value % alignment;
    if (remainder == 0) return value;
    const std::uintptr_t increment = alignment - remainder;
    if (value > std::numeric_limits<std::uintptr_t>::max() - increment) return 0;
    return value + increment;
}

void* allocate_relay_near(const unsigned char* call_site) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
    const auto page_size = static_cast<std::uintptr_t>(system.dwPageSize);
    const auto next_instruction = reinterpret_cast<std::uintptr_t>(call_site) + 5;
    if (granularity == 0 || page_size == 0
        || next_instruction > std::numeric_limits<std::uintptr_t>::max() - granularity
        || next_instruction > std::numeric_limits<std::uintptr_t>::max()
            - static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max()))
        return nullptr;

    const auto limit = next_instruction
        + static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
    auto cursor = align_up(next_instruction + granularity, granularity);
    while (cursor != 0 && cursor <= limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) == 0)
            return nullptr;
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin)
            return nullptr;
        const auto end = begin + region.RegionSize;
        if (region.State == MEM_FREE && cursor < end) {
            const auto candidate = align_up(cursor, granularity);
            if (candidate != 0 && candidate <= limit && candidate < end
                && page_size <= end - candidate) {
                void* relay = VirtualAlloc(reinterpret_cast<void*>(candidate),
                    static_cast<SIZE_T>(page_size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (relay != nullptr) {
                    if (reinterpret_cast<std::uintptr_t>(relay) == candidate) return relay;
                    (void)VirtualFree(relay, 0, MEM_RELEASE);
                }
            }
        }
        const auto next = align_up(end, granularity);
        if (end <= cursor || next <= cursor) return nullptr;
        cursor = next;
    }
    return nullptr;
}

}

bool RelativeCallHook::install(void* call_site, void* expected_target,
    void* replacement, void** original_target, CallArgumentMode argument_mode) noexcept {
    if (original_target != nullptr) *original_target = nullptr;
    if (installed() || call_site == nullptr || expected_target == nullptr
        || replacement == nullptr || original_target == nullptr)
        return false;

    auto* site = static_cast<unsigned char*>(call_site);
    unsigned char original[5]{};
    std::memcpy(original, site, sizeof(original));
    if (original[0] != 0xe8) return false;

    std::int32_t old_displacement = 0;
    std::memcpy(&old_displacement, original + 1, sizeof(old_displacement));
    const auto next_instruction = reinterpret_cast<std::uintptr_t>(site) + sizeof(original);
    const auto decoded_target = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(next_instruction) + old_displacement);
    if (decoded_target != reinterpret_cast<std::uintptr_t>(expected_target)) return false;

    void* relay = allocate_relay_near(site);
    if (relay == nullptr) return false;
    const auto replacement_address = reinterpret_cast<std::uintptr_t>(replacement);
    std::array<unsigned char, 24> relay_code{};
    std::size_t relay_size = 0;
    if (argument_mode == CallArgumentMode::unchanged) {
        relay_code[0] = 0xff;
        relay_code[1] = 0x25;
        std::memcpy(relay_code.data() + 6, &replacement_address, sizeof(replacement_address));
        relay_size = 14;
    } else if (argument_mode == CallArgumentMode::prepend_rdi) {
        constexpr unsigned char adapter[] = {
            0x45,0x89,0xc1, 0x49,0x89,0xd0, 0x48,0x89,0xca,
            0x48,0x89,0xf9, 0x48,0xb8};
        std::memcpy(relay_code.data(), adapter, sizeof(adapter));
        std::memcpy(relay_code.data() + sizeof(adapter),
            &replacement_address, sizeof(replacement_address));
        relay_code[22] = 0xff;
        relay_code[23] = 0xe0;
        relay_size = sizeof(relay_code);
    } else {
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(relay, relay_code.data(), relay_size);
    DWORD relay_protection = 0;
    if (!VirtualProtect(relay, relay_size, PAGE_EXECUTE_READ, &relay_protection)) {
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    (void)relay_protection;
    FlushInstructionCache(GetCurrentProcess(), relay, relay_size);

    const auto displacement64 = static_cast<std::intptr_t>(
        reinterpret_cast<std::uintptr_t>(relay)) - static_cast<std::intptr_t>(next_instruction);
    if (displacement64 < std::numeric_limits<std::int32_t>::min()
        || displacement64 > std::numeric_limits<std::int32_t>::max()) {
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    DWORD site_protection = 0;
    if (!VirtualProtect(site, sizeof(original), PAGE_EXECUTE_READWRITE, &site_protection)) {
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    if (std::memcmp(site, original, sizeof(original)) != 0) {
        DWORD ignored = 0;
        (void)VirtualProtect(site, sizeof(original), site_protection, &ignored);
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    unsigned char replacement_call[5]{0xe8};
    const auto new_displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(replacement_call + 1, &new_displacement, sizeof(new_displacement));
    std::memcpy(site, replacement_call, sizeof(replacement_call));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(replacement_call));
    DWORD ignored = 0;
    if (!VirtualProtect(site, sizeof(original), site_protection, &ignored)) {
        std::memcpy(site, original, sizeof(original));
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(original));
        (void)VirtualProtect(site, sizeof(original), site_protection, &ignored);
        (void)VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    call_site_ = site;
    relay_ = relay;
    std::memcpy(original_, original, sizeof(original_));
    *original_target = expected_target;
    return true;
}

bool RelativeCallHook::restore() noexcept {
    if (!installed() || relay_ == nullptr) return false;
    DWORD protection = 0;
    if (!VirtualProtect(call_site_, sizeof(original_), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    std::memcpy(call_site_, original_, sizeof(original_));
    FlushInstructionCache(GetCurrentProcess(), call_site_, sizeof(original_));
    DWORD ignored = 0;
    const bool protection_restored = VirtualProtect(
        call_site_, sizeof(original_), protection, &ignored) != FALSE;
    const bool relay_released = VirtualFree(relay_, 0, MEM_RELEASE) != FALSE;
    call_site_ = nullptr;
    relay_ = nullptr;
    std::memset(original_, 0, sizeof(original_));
    return protection_restored && relay_released;
}

}
