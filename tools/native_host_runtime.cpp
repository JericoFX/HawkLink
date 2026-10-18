#include "native_host_runtime.hpp"
#include "native_host_gate.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>

#if !defined(_WIN64)
#error The native host runtime supports Windows x64 only.
#endif

namespace hawken::probe::native_host {
namespace {
constinit std::atomic<bool> started{false};
constinit std::atomic<RuntimeStatus> status{RuntimeStatus::idle};
constinit OriginalModule original_module{};

bool explicit_request(const wchar_t* command_line) noexcept {
    if (!command_line) return false;
    constexpr std::size_t limit = 32767;
    std::size_t length = 0;
    while (length <= limit && command_line[length] != L'\0') ++length;
    if (!length || length > limit) return false;

    int count = 0;
    struct Arguments {
        LPWSTR* values;
        ~Arguments() { if (values) LocalFree(values); }
    } arguments{CommandLineToArgvW(command_line, &count)};
    if (!arguments.values || count < 1 || count > 512) return false;
    unsigned matches = 0;
    for (int index = 1; index < count; ++index) {
        if (std::wstring_view(arguments.values[index]) == L"-HawkLinkNativeHostGate") {
            if (++matches > 1) return false;
        }
    }
    return matches == 1;
}

bool writable_slot(std::uint64_t address) noexcept {
    if (!address || address % std::atomic_ref<NativeExec>::required_alignment != 0)
        return false;
    MEMORY_BASIC_INFORMATION region{};
    const auto pointer = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
    if (VirtualQuery(pointer, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Protect != PAGE_READWRITE)
        return false;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (address < base || address - base > region.RegionSize) return false;
    return sizeof(NativeExec) <= region.RegionSize - (address - base);
}

bool retryable(std::string_view code) noexcept {
    return code == "native_host_unavailable" || code == "native_host_missing" ||
        code == "names_unavailable" || code == "player_names_uninitialized" ||
        code == "player_names_absent" || code == "unstable_sample";
}

DWORD WINAPI worker(void*) noexcept {
    // This acquire publishes the caller's complete module before any read.
    if (status.load(std::memory_order_acquire) != RuntimeStatus::pending) return 0;
    const auto module = original_module;
    const auto begin = GetTickCount64();
    constexpr ULONGLONG duration = 30000;
    constexpr std::size_t budget = 16 * 1024 * 1024;
    std::size_t used = 0;
    bool limit_reached = false;
    try {
        const ReadMemory read = [&](std::uint64_t address, std::span<std::byte> bytes) {
            if (GetTickCount64() - begin >= duration || bytes.size() > budget - used) {
                limit_reached = true;
                return false;
            }
            used += bytes.size(); // Failed attempts consume the same shared budget.
            if (!address || bytes.empty() || bytes.size() - 1 >
                std::numeric_limits<std::uint64_t>::max() - address) return false;
            SIZE_T copied = 0;
            return ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
                bytes.data(), bytes.size(), &copied) != FALSE && copied == bytes.size();
        };
        for (unsigned attempt = 0; attempt < 60; ++attempt) {
            if (GetTickCount64() - begin >= duration || used == budget) break;
            try {
                const auto first = locate_is_client_only_build(read, module);
                const auto second = locate_is_client_only_build(read, module);
                if (first == second) {
                    if (first.function_address > std::numeric_limits<std::uint64_t>::max() -
                        0xf0 - (sizeof(NativeExec) - 1)) {
                        status.store(RuntimeStatus::rejected, std::memory_order_release);
                        return 0;
                    }
                    const auto address = first.function_address + 0xf0;
                    if (!writable_slot(address)) {
                        status.store(RuntimeStatus::not_writable, std::memory_order_release);
                        return 0;
                    }
                    const auto final = locate_is_client_only_build(read, module);
                    if (final == second) {
                        if (GetTickCount64() - begin >= duration) break;
                        if (!writable_slot(address)) {
                            status.store(RuntimeStatus::not_writable, std::memory_order_release);
                            return 0;
                        }
                        auto* slot = reinterpret_cast<NativeExec*>(
                            static_cast<std::uintptr_t>(address));
                        const auto expected = reinterpret_cast<NativeExec>(
                            static_cast<std::uintptr_t>(module.base + 0x400560));
                        const auto result = install_gate(slot, expected);
                        status.store(result == InstallResult::installed ? RuntimeStatus::installed :
                            RuntimeStatus::rejected, std::memory_order_release);
                        return 0;
                    }
                }
            } catch (const std::runtime_error& error) {
                if (limit_reached) break;
                if (!retryable(error.what())) {
                    // A terminal rejection is a safety decision, not evidence
                    // that the function can never finish binding later.
                    status.store(RuntimeStatus::rejected, std::memory_order_release);
                    return 0;
                }
            }
            const auto elapsed = GetTickCount64() - begin;
            if (limit_reached || used == budget || attempt == 59 || elapsed >= duration) break;
            const auto remaining = duration - elapsed;
            Sleep(static_cast<DWORD>(remaining < 250 ? remaining : 250));
        }
        status.store(RuntimeStatus::timed_out, std::memory_order_release);
    } catch (...) {
        status.store(RuntimeStatus::failed, std::memory_order_release);
    }
    return 0;
}
}

bool start_runtime(OriginalModule module, const wchar_t* original_command_line) noexcept {
    if (!module.base || module.size != 0x201e000 ||
        module.base > std::numeric_limits<std::uint64_t>::max() - module.size ||
        !explicit_request(original_command_line)) return false;
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;

    original_module = module;
    status.store(RuntimeStatus::pending, std::memory_order_release);
    const auto thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
    if (!thread) {
        status.store(RuntimeStatus::failed, std::memory_order_release);
        return false;
    }
    CloseHandle(thread); // Worker/module/DLL lifetime is the process, not this handle.
    return true;
}

RuntimeStatus runtime_status() noexcept {
    return status.load(std::memory_order_acquire);
}
}
