#include "hawken/win_callsite_hook.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using Target = int(__fastcall*)(int, int, int);
using Caller = int(__fastcall*)();

Target original_target = nullptr;
unsigned observed_calls = 0;
int observed_arguments[3]{};
void* observed_channel = nullptr;

int __fastcall observe_call(int first, int second, int third) noexcept {
    ++observed_calls;
    observed_arguments[0] = first;
    observed_arguments[1] = second;
    observed_arguments[2] = third;
    return original_target(first, second, third);
}

int __fastcall observe_channel_call(void* channel, int first, int second, int third) noexcept {
    observed_channel = channel;
    return observe_call(first, second, third);
}

int failure(const char* reason) {
    std::fprintf(stderr, "%s\n", reason);
    return 1;
}

}

int main() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (page == nullptr) return failure("could not allocate callsite fixture");

    constexpr std::size_t call_offset = 20;
    constexpr std::size_t target_offset = 64;
    constexpr unsigned char caller_code[] = {
        0x48,0x83,0xec,0x28, 0xb9,0x2a,0x00,0x00,0x00,
        0xba,0x07,0x00,0x00,0x00, 0x41,0xb8,0x03,0x00,0x00,0x00,
        0xe8,0x00,0x00,0x00,0x00, 0x48,0x83,0xc4,0x28,0xc3};
    constexpr unsigned char target_code[] = {
        0x8b,0xc1, 0x03,0xc2, 0x44,0x01,0xc0, 0xc3};
    std::memcpy(page, caller_code, sizeof(caller_code));
    std::memcpy(page + target_offset, target_code, sizeof(target_code));
    auto* call_site = page + call_offset;
    auto* target = page + target_offset;
    const auto displacement = static_cast<std::int32_t>(
        reinterpret_cast<std::intptr_t>(target)
        - reinterpret_cast<std::intptr_t>(call_site + 5));
    std::memcpy(call_site + 1, &displacement, sizeof(displacement));

    DWORD previous = 0;
    if (!VirtualProtect(page, system.dwPageSize, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("could not mark callsite fixture executable");
    }
    FlushInstructionCache(GetCurrentProcess(), page, system.dwPageSize);

    const auto caller = reinterpret_cast<Caller>(page);
    if (caller() != 52) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("unpatched fixture did not call its target");
    }

    hawken::client::diagnostics::RelativeCallHook hook;
    void* original = nullptr;
    if (!hook.install(call_site, target, reinterpret_cast<void*>(&observe_call), &original)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("verified relative call was not intercepted");
    }
    original_target = reinterpret_cast<Target>(original);
    if (original != target || !hook.installed() || caller() != 52
        || observed_calls != 1 || observed_arguments[0] != 42
        || observed_arguments[1] != 7 || observed_arguments[2] != 3) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("hook did not observe and forward the original call");
    }
    if (!hook.restore() || hook.installed() || caller() != 52 || observed_calls != 1) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("restoring the callsite did not recover the original behavior");
    }

    if (hook.install(call_site, target + 1, reinterpret_cast<void*>(&observe_call), &original)
        || hook.installed() || caller() != 52 || observed_calls != 1) {
        VirtualFree(page, 0, MEM_RELEASE);
        return failure("callsite accepted a mismatched expected target");
    }

    VirtualFree(page, 0, MEM_RELEASE);

    auto* channel_page = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (channel_page == nullptr) return failure("could not allocate channel fixture");
    constexpr std::size_t channel_call_offset = 31;
    constexpr std::size_t channel_target_offset = 128;
    constexpr std::uintptr_t channel_value = 0x12345678;
    constexpr unsigned char channel_caller_code[] = {
        0x57, 0x48,0x83,0xec,0x20, 0x48,0xbf,
        0,0,0,0,0,0,0,0,
        0xb9,0x2a,0x00,0x00,0x00, 0xba,0x07,0x00,0x00,0x00,
        0x41,0xb8,0x03,0x00,0x00,0x00,
        0xe8,0x00,0x00,0x00,0x00, 0x48,0x83,0xc4,0x20,0x5f,0xc3};
    std::memcpy(channel_page, channel_caller_code, sizeof(channel_caller_code));
    std::memcpy(channel_page + 7, &channel_value, sizeof(channel_value));
    std::memcpy(channel_page + channel_target_offset, target_code, sizeof(target_code));
    auto* channel_call_site = channel_page + channel_call_offset;
    auto* channel_target = channel_page + channel_target_offset;
    const auto channel_displacement = static_cast<std::int32_t>(
        reinterpret_cast<std::intptr_t>(channel_target)
        - reinterpret_cast<std::intptr_t>(channel_call_site + 5));
    std::memcpy(channel_call_site + 1, &channel_displacement, sizeof(channel_displacement));
    if (!VirtualProtect(channel_page, system.dwPageSize, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(channel_page, 0, MEM_RELEASE);
        return failure("could not mark channel fixture executable");
    }
    FlushInstructionCache(GetCurrentProcess(), channel_page, system.dwPageSize);
    const auto channel_caller = reinterpret_cast<Caller>(channel_page);
    if (channel_caller() != 52) {
        VirtualFree(channel_page, 0, MEM_RELEASE);
        return failure("unpatched channel fixture did not call its target");
    }
    hawken::client::diagnostics::RelativeCallHook channel_hook;
    original = nullptr;
    if (!channel_hook.install(channel_call_site, channel_target,
            reinterpret_cast<void*>(&observe_channel_call), &original,
            hawken::client::diagnostics::CallArgumentMode::prepend_rdi)) {
        VirtualFree(channel_page, 0, MEM_RELEASE);
        return failure("callsite did not preserve the channel pointer");
    }
    original_target = reinterpret_cast<Target>(original);
    observed_channel = nullptr;
    if (original != channel_target || channel_caller() != 52
        || observed_channel != reinterpret_cast<void*>(channel_value)
        || observed_arguments[0] != 42 || observed_arguments[1] != 7
        || observed_arguments[2] != 3) {
        VirtualFree(channel_page, 0, MEM_RELEASE);
        return failure("channel adapter did not preserve all arguments and return value");
    }
    if (!channel_hook.restore() || channel_caller() != 52
        || observed_channel != reinterpret_cast<void*>(channel_value)) {
        VirtualFree(channel_page, 0, MEM_RELEASE);
        return failure("channel callsite did not restore its original behavior");
    }
    VirtualFree(channel_page, 0, MEM_RELEASE);
    std::puts("relative call hook observes, forwards, restores, and rejects mismatches.");
    return 0;
}
