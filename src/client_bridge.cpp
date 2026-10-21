#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <intrin.h>
#include "hawken/client_routing.hpp"
#include "hawken/client_steam_identity_transport.hpp"
#include "hawken/sha256.hpp"
#include "hawken/win_callsite_hook.hpp"
#if defined(HAWKEN_NATIVE_HOST_RESEARCH)
#include "native_host_runtime.hpp"
#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
#error Native hosting research requires the verified original client.
#endif
#endif
#include <cstddef>
#include <cstdint>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#if !defined(_WIN64)
#error The client bridge supports Windows x64 only.
#endif

namespace {
using CommandLine = LPWSTR (WINAPI*)();
using InputCreate = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using SendRawBunch = int(__fastcall*)(void*, void*, std::uint32_t);
using RpcSender = void(__fastcall*)(void*, void*, void*, void*, void*, std::int32_t);
using RpcCacheLookup = void*(__fastcall*)(void*, void*);
using RpcActorLookup = int*(__fastcall*)(void*, int*, void*);

// Constant-initialized POD only: no C++ construction, IO, hashing or DLL loading
// in DllMain. The DLL and initialized buffers remain loaded until process exit.
constinit HMODULE proxy_module = nullptr;
constinit CommandLine original_command_line = nullptr;
constinit std::uintptr_t permitted_return = 0;
constinit volatile LONG initialization = 0; // 0 idle, 1 initializing, 2 ready.
constinit LPWSTR altered_command_line = nullptr;
constinit InputCreate real_create = nullptr;
constinit void* volatile identity_transport = nullptr;
constinit hawken::client::identity::WinHttpApi http_api{};
constinit std::uintptr_t connect_return = 0, request_return = 0;
constinit hawken::client::diagnostics::RelativeCallHook send_bunch_hook{};
constinit SendRawBunch original_send_raw_bunch = nullptr;
constinit HANDLE send_bunch_trace_file = nullptr;
constinit volatile LONG send_bunch_trace_sequence = 0;
constinit volatile LONG send_bunch_trace_enabled = 0;
SRWLOCK send_bunch_trace_lock = SRWLOCK_INIT;
constinit hawken::client::diagnostics::RelativeCallHook rpc_sender_hook{};
constinit hawken::client::diagnostics::RelativeCallHook rpc_class_cache_hook{};
constinit hawken::client::diagnostics::RelativeCallHook rpc_function_cache_hook{};
constinit hawken::client::diagnostics::RelativeCallHook rpc_actor_lookup_hook{};
constinit RpcSender original_rpc_sender = nullptr;
constinit RpcCacheLookup original_rpc_class_cache = nullptr, original_rpc_function_cache = nullptr;
constinit RpcActorLookup original_rpc_actor_lookup = nullptr;
constinit HANDLE rpc_gate_trace_file = nullptr;
constinit volatile LONG rpc_gate_sequence = 0, rpc_gate_enabled = 0;
SRWLOCK rpc_gate_lock = SRWLOCK_INIT;

constexpr std::size_t send_bunch_sample_limit = 64;
constexpr LONG send_bunch_record_limit = 4096;

struct SendBunchTraceRecord {
    std::uint32_t sequence = 0;
    std::uint64_t qpc = 0;
    DWORD thread_id = 0;
    std::int32_t channel_index = -1;
    std::uint32_t send_flags = 0;
    unsigned char bunch_cc = 0;
    unsigned char bunch_cd = 0;
    unsigned char bunch_ce = 0;
    std::uint32_t bunch_bc = 0;
    std::uint32_t bunch_c0 = 0;
    std::uint32_t bunch_c4 = 0;
    std::uint32_t num_bits = 0;
    std::array<unsigned char, send_bunch_sample_limit> sample{};
    std::uint32_t sample_size = 0;
};

struct RpcGateTraceRecord {
    std::uint32_t sequence = 0;
    std::uint64_t qpc = 0;
    DWORD thread_id = 0;
    std::int32_t class_cache = -1, function_cache = -1;
    // 0 not observed, 1 unreadable, 2 missing, 3 found.
    std::int32_t actor_lookup = 0;
    std::int32_t channel_index = -1, opening_packet_id = -1, field_id = -1;
};
struct RpcGateFrame;
constinit thread_local RpcGateFrame* rpc_gate_frame = nullptr;
struct RpcGateFrame {
    RpcGateFrame* previous = rpc_gate_frame;
    RpcGateTraceRecord record;
    void* actor = nullptr;
    void* function = nullptr;
    bool observe = false;
    RpcGateFrame() noexcept { rpc_gate_frame = this; }
    ~RpcGateFrame() { rpc_gate_frame = previous; }
};

struct Image {
    unsigned char* base;
    DWORD size;
    const IMAGE_NT_HEADERS64* nt;

    bool contains(DWORD rva, std::size_t length) const noexcept {
        return rva <= size && length <= static_cast<std::size_t>(size - rva);
    }
    template<class T> T* at(DWORD rva, std::size_t count = 1) const noexcept {
        if (count > static_cast<std::size_t>(size) / sizeof(T)
            || !contains(rva, count * sizeof(T))) return nullptr;
        return reinterpret_cast<T*>(base + rva);
    }
    bool name(DWORD rva, const char* expected) const noexcept {
        for (std::size_t i = 0; i < 512; ++i) {
            if (i > size || rva > size - i || !contains(rva + static_cast<DWORD>(i), 1))
                return false;
            unsigned char actual = base[rva + i];
            unsigned char wanted = static_cast<unsigned char>(expected[i]);
            if (actual >= 'A' && actual <= 'Z') actual = static_cast<unsigned char>(actual + 32);
            if (wanted >= 'A' && wanted <= 'Z') wanted = static_cast<unsigned char>(wanted + 32);
            if (actual != wanted) return false;
            if (wanted == 0) return true;
        }
        return false;
    }
};

bool image_of(HMODULE module, Image& image) noexcept {
    if (module == nullptr) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
        || dos->e_lfanew > 0x800) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.SizeOfImage < 0x1000
        || nt->OptionalHeader.SizeOfImage > 0x10000000
        || nt->OptionalHeader.NumberOfRvaAndSizes < IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        return false;
    image = {base, nt->OptionalHeader.SizeOfImage, nt};
    return true;
}

void** import_slot(const Image& image, const char* library, const char* symbol) noexcept {
    const auto directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto count = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    auto* imports = image.at<IMAGE_IMPORT_DESCRIPTOR>(directory.VirtualAddress, count);
    if (directory.VirtualAddress == 0 || count > 512 || imports == nullptr) return nullptr;
    for (std::size_t i = 0; i < count && imports[i].Name != 0; ++i) {
        if (!image.name(imports[i].Name, library)) continue;
        const DWORD names_rva = imports[i].OriginalFirstThunk;
        const DWORD slots_rva = imports[i].FirstThunk;
        if (names_rva == 0 || slots_rva == 0 || slots_rva % sizeof(void*) != 0)
            return nullptr;
        for (DWORD j = 0; j < 4096; ++j) {
            const std::size_t delta = static_cast<std::size_t>(j) * sizeof(IMAGE_THUNK_DATA64);
            if (delta > image.size || names_rva > image.size - delta
                || slots_rva > image.size - delta) return nullptr;
            auto* name = image.at<IMAGE_THUNK_DATA64>(names_rva + static_cast<DWORD>(delta));
            auto* slot = image.at<void*>(slots_rva + static_cast<DWORD>(delta));
            if (name == nullptr || slot == nullptr || name->u1.AddressOfData == 0)
                break;
            if (IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal)) continue;
            if (name->u1.AddressOfData > image.size - sizeof(WORD)) return nullptr;
            const DWORD rva = static_cast<DWORD>(name->u1.AddressOfData) + static_cast<DWORD>(sizeof(WORD));
            if (image.name(rva, symbol)) return slot;
        }
    }
    return nullptr;
}

#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
// This path is compiled only into a separate fixture DLL. Production has no
// marker, environment flag, alternate build acceptance or test entry point.
struct HarnessMarker {
    std::uint64_t magic;
    CommandLine command_line;
};

const HarnessMarker* fixture_marker(const Image& image) noexcept {
    const auto directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* exports = image.at<IMAGE_EXPORT_DIRECTORY>(directory.VirtualAddress);
    if (directory.VirtualAddress == 0 || exports == nullptr || exports->NumberOfNames > 16384)
        return nullptr;
    const auto* names = image.at<DWORD>(exports->AddressOfNames, exports->NumberOfNames);
    const auto* ordinals = image.at<WORD>(exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto* functions = image.at<DWORD>(exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (names == nullptr || ordinals == nullptr || functions == nullptr) return nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (!image.name(names[i], "HawkenBridgeHarnessMarker")) continue;
        if (ordinals[i] >= exports->NumberOfFunctions) return nullptr;
        const DWORD rva = functions[ordinals[i]];
        if (rva >= directory.VirtualAddress
            && rva - directory.VirtualAddress < directory.Size) return nullptr;
        const auto* marker = image.at<HarnessMarker>(rva);
        if (marker == nullptr || marker->magic != 0x4841574b454e5445ULL) return nullptr;
        return marker;
    }
    return nullptr;
}

bool fixture_return(const Image& image, void** slot, std::uintptr_t start,
                    std::uintptr_t& result) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(image.base);
    if (start < base || start - base >= image.size) return false;
    const auto directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto count = directory.Size / sizeof(RUNTIME_FUNCTION);
    const auto* ranges = image.at<RUNTIME_FUNCTION>(directory.VirtualAddress, count);
    if (ranges == nullptr) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (ranges[i].BeginAddress != start - base) continue;
        const DWORD begin = ranges[i].BeginAddress, end = ranges[i].EndAddress;
        if (end <= begin || end - begin > 512 || !image.contains(begin, end - begin))
            return false;
        unsigned matches = 0;
        for (DWORD rva = begin; rva + 6 <= end; ++rva) {
            const auto* code = image.base + rva;
            if (code[0] != 0xff || code[1] != 0x15) continue;
            const auto displacement = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(code[2])
                | static_cast<std::uint32_t>(code[3]) << 8
                | static_cast<std::uint32_t>(code[4]) << 16
                | static_cast<std::uint32_t>(code[5]) << 24);
            const auto target = static_cast<std::int64_t>(base + rva + 6) + displacement;
            if (target == static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(slot))) {
                result = base + rva + 6;
                ++matches;
            }
        }
        return matches == 1;
    }
    return false;
}

bool fixture_callsite(const Image& image, void** slot) noexcept {
    const auto* marker = fixture_marker(image);
    return marker && fixture_return(image, slot,
        reinterpret_cast<std::uintptr_t>(marker->command_line), permitted_return);
}
#else
bool production_identity(const Image& image, void** slot) noexcept {
    if (image.nt->FileHeader.TimeDateStamp != 1499273047
        || image.size != 0x201e000 || image.nt->OptionalHeader.AddressOfEntryPoint != 0x104de30
        || reinterpret_cast<unsigned char*>(slot) != image.base + 0x128e498)
        return false;
    constexpr unsigned char call[] = {0xff,0x15,0xed,0x6f,0x61,0x00,0x48,0x8b,0xf8};
    constexpr unsigned char next[] = {0x48,0x8b,0xcf,0xe8,0x4f,0x85,0xff,0xff};
    if (!image.contains(0xc774a5, sizeof(call)) || !image.contains(0xc774e9, sizeof(next)))
        return false;
    for (std::size_t i = 0; i < sizeof(call); ++i)
        if (image.base[0xc774a5 + i] != call[i]) return false;
    for (std::size_t i = 0; i < sizeof(next); ++i)
        if (image.base[0xc774e9 + i] != next[i]) return false;
    permitted_return = reinterpret_cast<std::uintptr_t>(image.base) + 0xc774ab;
    return true;
}
#endif

[[noreturn]] void stop_this_process(UINT code) noexcept {
    // Fixed text only; the exit code distinguishes identity81/config82/
    // forwarding83/internal84. Never emit paths, command lines or credentials.
    constexpr char text[] = "hawken-client bridge initialization failed.\r\n";
    DWORD written = 0;
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error != nullptr && error != INVALID_HANDLE_VALUE)
        (void)WriteFile(error, text, static_cast<DWORD>(sizeof(text) - 1), &written, nullptr);
    (void)TerminateProcess(GetCurrentProcess(), code);
    __fastfail(7); // Never continue to the original Meteor endpoint on failure.
}

std::filesystem::path module_file(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) throw std::runtime_error("Module path unavailable");
    path.resize(length);
    return std::filesystem::path(path);
}

hawken::client::identity::Transport* transport() noexcept {
    return static_cast<hawken::client::identity::Transport*>(
        InterlockedCompareExchangePointer(&identity_transport, nullptr, nullptr));
}

bool read_current_process_memory(const void* address, void* destination, SIZE_T bytes,
    SIZE_T* bytes_read = nullptr) noexcept {
    SIZE_T copied = 0;
    const bool complete = address != nullptr && destination != nullptr
        && ReadProcessMemory(GetCurrentProcess(), address, destination, bytes, &copied)
        && copied == bytes;
    if (bytes_read != nullptr) *bytes_read = copied;
    return complete;
}

bool read_send_bunch_field(const void* base, std::uintptr_t offset, void* destination,
    SIZE_T bytes) noexcept {
    if (base == nullptr || destination == nullptr) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    if (offset > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    return read_current_process_memory(reinterpret_cast<const void*>(address + offset),
        destination, bytes);
}

bool capture_send_bunch_fields(void* channel, void* bunch, std::uint32_t send_flags,
    SendBunchTraceRecord& record) noexcept {
    if (!read_send_bunch_field(channel, 0x6c, &record.channel_index,
            sizeof(record.channel_index))
        || record.channel_index < 2)
        return false;
    record.send_flags = send_flags;
    unsigned char bunch_flags[3]{};
    if (!read_send_bunch_field(bunch, 0xcc, bunch_flags, sizeof(bunch_flags))
        || !read_send_bunch_field(bunch, 0xbc, &record.bunch_bc, sizeof(record.bunch_bc))
        || !read_send_bunch_field(bunch, 0xc0, &record.bunch_c0, sizeof(record.bunch_c0))
        || !read_send_bunch_field(bunch, 0xc4, &record.bunch_c4, sizeof(record.bunch_c4))
        || !read_send_bunch_field(bunch, 0x98, &record.num_bits, sizeof(record.num_bits)))
        return false;
    record.bunch_cc = bunch_flags[0];
    record.bunch_cd = bunch_flags[1];
    record.bunch_ce = bunch_flags[2];

    const unsigned char* payload = nullptr;
    if (!read_send_bunch_field(bunch, 0x88, &payload, sizeof(payload))) return false;
    const std::uint32_t payload_bytes = record.num_bits / 8
        + (record.num_bits % 8 == 0 ? 0u : 1u);
    const SIZE_T requested = payload_bytes < send_bunch_sample_limit
        ? static_cast<SIZE_T>(payload_bytes) : static_cast<SIZE_T>(send_bunch_sample_limit);
    if (payload != nullptr && requested != 0) {
        SIZE_T copied = 0;
        (void)read_current_process_memory(payload, record.sample.data(), requested, &copied);
        record.sample_size = copied <= send_bunch_sample_limit
            ? static_cast<std::uint32_t>(copied)
            : static_cast<std::uint32_t>(send_bunch_sample_limit);
    }
    return true;
}

bool format_send_bunch_record(const SendBunchTraceRecord& record, char* output,
    std::size_t capacity) noexcept {
    if (output == nullptr || capacity == 0 || record.sample_size > record.sample.size())
        return false;
    const int prefix = std::snprintf(output, capacity,
        "%u,%llu,%lu,%d,%u,%u,%u,%u,%u,%u,%u,%u,",
        static_cast<unsigned>(record.sequence), static_cast<unsigned long long>(record.qpc),
        static_cast<unsigned long>(record.thread_id), static_cast<int>(record.channel_index),
        static_cast<unsigned>(record.send_flags), static_cast<unsigned>(record.bunch_cc),
        static_cast<unsigned>(record.bunch_cd), static_cast<unsigned>(record.bunch_ce),
        static_cast<unsigned>(record.bunch_bc), static_cast<unsigned>(record.bunch_c0),
        static_cast<unsigned>(record.bunch_c4), static_cast<unsigned>(record.num_bits));
    if (prefix < 0 || static_cast<std::size_t>(prefix) >= capacity) return false;
    std::size_t offset = static_cast<std::size_t>(prefix);
    constexpr char digits[] = "0123456789abcdef";
    for (std::uint32_t i = 0; i < record.sample_size; ++i) {
        if (offset + 2 >= capacity) return false;
        const unsigned char byte = record.sample[i];
        output[offset++] = digits[byte >> 4];
        output[offset++] = digits[byte & 0x0f];
    }
    if (offset + 2 >= capacity) return false;
    output[offset++] = '\r';
    output[offset++] = '\n';
    output[offset] = '\0';
    return true;
}

bool open_send_bunch_trace() noexcept {
    wchar_t local_app_data[32768]{};
    const DWORD local_length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data, static_cast<DWORD>(std::size(local_app_data)));
    if (local_length == 0 || local_length >= std::size(local_app_data)) return false;

    wchar_t directory[32768]{};
    const int directory_length = std::swprintf(directory, std::size(directory),
        L"%ls\\HawkLink", local_app_data);
    if (directory_length < 0 || static_cast<std::size_t>(directory_length) >= std::size(directory))
        return false;
    if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG stamp = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32)
        | static_cast<ULONGLONG>(now.dwLowDateTime);
    wchar_t path[32768]{};
    const int path_length = std::swprintf(path, std::size(path),
        L"%ls\\send-bunch-%lu-%llu.csv", directory,
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(stamp));
    if (path_length < 0 || static_cast<std::size_t>(path_length) >= std::size(path)) return false;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr char header[] =
        "sequence,qpc,thread_id,channel_index,send_flags,bunch_cc,bunch_cd,bunch_ce,"
        "bunch_bc,bunch_c0,bunch_c4,num_bits,sample_hex\r\n";
    DWORD written = 0;
    if (!WriteFile(file, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr)
        || written != sizeof(header) - 1) {
        CloseHandle(file);
        return false;
    }
    send_bunch_trace_file = file;
    InterlockedExchange(&send_bunch_trace_sequence, 0);
    InterlockedExchange(&send_bunch_trace_enabled, 1);
    return true;
}

void write_send_bunch_trace(const SendBunchTraceRecord& record) noexcept {
    if (InterlockedCompareExchange(&send_bunch_trace_enabled, 0, 0) == 0) return;
    char line[256]{};
    if (!format_send_bunch_record(record, line, sizeof(line))) return;
    const DWORD line_size = static_cast<DWORD>(std::strlen(line));
    AcquireSRWLockExclusive(&send_bunch_trace_lock);
    DWORD written = 0;
    const bool complete = send_bunch_trace_file != nullptr
        && WriteFile(send_bunch_trace_file, line, line_size, &written, nullptr)
        && written == line_size;
    if (!complete && send_bunch_trace_file != nullptr) {
        CloseHandle(send_bunch_trace_file);
        send_bunch_trace_file = nullptr;
        InterlockedExchange(&send_bunch_trace_enabled, 0);
    }
    ReleaseSRWLockExclusive(&send_bunch_trace_lock);
}

int __fastcall observed_send_raw_bunch(void* channel, void* connection,
    void* bunch, std::uint32_t send_flags) noexcept {
    const auto original = original_send_raw_bunch;
    if (original == nullptr) return 0;
    if (InterlockedCompareExchange(&send_bunch_trace_enabled, 0, 0) != 0) {
        SendBunchTraceRecord record{};
        if (capture_send_bunch_fields(channel, bunch, send_flags, record)) {
            const LONG sequence = InterlockedIncrement(&send_bunch_trace_sequence);
            if (sequence > 0 && sequence <= send_bunch_record_limit) {
                LARGE_INTEGER qpc{};
                (void)QueryPerformanceCounter(&qpc);
                record.sequence = static_cast<std::uint32_t>(sequence);
                record.qpc = static_cast<std::uint64_t>(qpc.QuadPart);
                record.thread_id = GetCurrentThreadId();
                write_send_bunch_trace(record);
            } else if (sequence > send_bunch_record_limit) {
                InterlockedExchange(&send_bunch_trace_enabled, 0);
            }
        }
    }
    return original(connection, bunch, send_flags);
}

bool install_local_send_bunch_trace(Image& image) noexcept {
    constexpr DWORD call_rva = 0x9f2b77;
    constexpr DWORD target_rva = 0x9f3410;
    constexpr unsigned char expected_call[] = {0xe8,0x94,0x08,0x00,0x00};
    if (!image.contains(call_rva, sizeof(expected_call)) || !image.contains(target_rva, 1)
        || std::memcmp(image.base + call_rva, expected_call, sizeof(expected_call)) != 0)
        return false;
    if (!open_send_bunch_trace()) return false;

    void* expected_target = image.base + target_rva;
    original_send_raw_bunch = reinterpret_cast<SendRawBunch>(expected_target);
    void* original = nullptr;
    if (!send_bunch_hook.install(image.base + call_rva, expected_target,
            reinterpret_cast<void*>(&observed_send_raw_bunch), &original,
            hawken::client::diagnostics::CallArgumentMode::prepend_rdi)
        || original != expected_target) {
        original_send_raw_bunch = nullptr;
        CloseHandle(send_bunch_trace_file);
        send_bunch_trace_file = nullptr;
        InterlockedExchange(&send_bunch_trace_enabled, 0);
        return false;
    }
    return true;
}

// The exact 0x687c91 callee returns a field-cache entry whose +8 value is
// consumed by the native bounded-field writer. No function name is inferred.
void capture_rpc_function_cache(void* result, RpcGateTraceRecord& record) noexcept {
    record.function_cache = result != nullptr ? 1 : 0;
    std::int32_t field = -1;
    if (result != nullptr && read_send_bunch_field(result, 8, &field, sizeof(field)))
        record.field_id = field;
}

void capture_rpc_actor_lookup(void* map, int* index, RpcGateTraceRecord& record) noexcept {
    record.actor_lookup = 1;
    std::int32_t slot = -1;
    if (!read_send_bunch_field(index, 0, &slot, sizeof(slot))) return;
    if (slot == -1) { record.actor_lookup = 2; return; }
    if (slot < 0) return;
    void* entries = nullptr;
    if (!read_send_bunch_field(map, 0, &entries, sizeof(entries))) return;
    if (entries == nullptr && slot == 0) { record.actor_lookup = 2; return; }
    void* channel = nullptr;
    // Exact native map stride and value offset at 0x687ccf..0x687cdc.
    const auto offset = static_cast<std::uintptr_t>(slot) * 0x18 + 8;
    if (!read_send_bunch_field(entries, offset, &channel, sizeof(channel))) return;
    if (channel == nullptr) { record.actor_lookup = 2; return; }
    std::int32_t channel_index = -1, opening_packet_id = -1;
    if (!read_send_bunch_field(channel, 0x6c, &channel_index, sizeof(channel_index))
        || !read_send_bunch_field(channel, 0x74, &opening_packet_id, sizeof(opening_packet_id)))
        return;
    record.channel_index = channel_index;
    record.opening_packet_id = opening_packet_id;
    record.actor_lookup = 3;
}

bool format_rpc_gate_record(const RpcGateTraceRecord& record, char* output,
    std::size_t capacity) noexcept {
    if (output == nullptr || capacity == 0) return false;
    const char* result = "not_observed";
    if (record.class_cache == 0) result = "class_cache_miss";
    else if (record.function_cache == 0) result = "function_cache_miss";
    else if (record.actor_lookup == 1) result = "unreadable";
    else if (record.actor_lookup == 2) result = "actor_channel_miss";
    else if (record.actor_lookup == 3)
        result = record.opening_packet_id == -1 ? "channel_open_id_minus_one" : "gates_passed";
    const int length = std::snprintf(output, capacity,
        "%u,%llu,%lu,%d,%d,%d,%d,%d,%d,%s\r\n",
        static_cast<unsigned>(record.sequence), static_cast<unsigned long long>(record.qpc),
        static_cast<unsigned long>(record.thread_id), record.class_cache, record.function_cache,
        record.actor_lookup, record.channel_index, record.opening_packet_id, record.field_id, result);
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

void disable_rpc_gate_trace() noexcept {
    AcquireSRWLockExclusive(&rpc_gate_lock);
    InterlockedExchange(&rpc_gate_enabled, 0);
    if (rpc_gate_trace_file != nullptr) {
        CloseHandle(rpc_gate_trace_file);
        rpc_gate_trace_file = nullptr;
    }
    ReleaseSRWLockExclusive(&rpc_gate_lock);
}

void write_rpc_gate_trace(const RpcGateTraceRecord& record) noexcept {
    char line[192]{};
    if (!format_rpc_gate_record(record, line, sizeof(line))) return;
    AcquireSRWLockExclusive(&rpc_gate_lock);
    if (rpc_gate_trace_file != nullptr && InterlockedCompareExchange(&rpc_gate_enabled, 0, 0) != 0) {
        const auto size = static_cast<DWORD>(std::strlen(line));
        DWORD written = 0;
        if (!WriteFile(rpc_gate_trace_file, line, size, &written, nullptr) || written != size) {
            CloseHandle(rpc_gate_trace_file);
            rpc_gate_trace_file = nullptr;
            InterlockedExchange(&rpc_gate_enabled, 0);
        }
    }
    ReleaseSRWLockExclusive(&rpc_gate_lock);
}

void* __fastcall observed_rpc_class_cache(void* map, void* actor_class) {
    auto* frame = rpc_gate_frame;
    void* result = original_rpc_class_cache(map, actor_class);
    if (frame != nullptr && frame->observe) frame->record.class_cache = result != nullptr ? 1 : 0;
    return result;
}

void* __fastcall observed_rpc_function_cache(void* cache, void* function) {
    auto* frame = rpc_gate_frame;
    void* result = original_rpc_function_cache(cache, function);
    const DWORD error = GetLastError();
    if (frame != nullptr && frame->observe && frame->function == function)
        capture_rpc_function_cache(result, frame->record);
    SetLastError(error);
    return result;
}

int* __fastcall observed_rpc_actor_lookup(void* map, int* index, void* actor) {
    auto* frame = rpc_gate_frame;
    int* result = original_rpc_actor_lookup(map, index, actor);
    const DWORD error = GetLastError();
    if (frame != nullptr && frame->observe && frame->actor == actor)
        capture_rpc_actor_lookup(map, index, frame->record);
    SetLastError(error);
    return result;
}

// Six Win64 arguments, including the untouched stack arguments at the caller's
// +20/+28. Exceptions propagate; the frame only restores our TLS observation.
void __fastcall observed_rpc_sender(void* actor, void* connection, void* function,
    void* arguments, void* script_frame, std::int32_t server_side) {
    const DWORD incoming_error = GetLastError();
    RpcGateFrame frame;
    frame.actor = actor;
    frame.function = function;
    if (server_side == 0 && InterlockedCompareExchange(&rpc_gate_enabled, 0, 0) != 0) {
        const LONG sequence = InterlockedIncrement(&rpc_gate_sequence);
        if (sequence > 0 && sequence <= send_bunch_record_limit) {
            frame.observe = true;
            frame.record.sequence = static_cast<std::uint32_t>(sequence);
            LARGE_INTEGER qpc{};
            (void)QueryPerformanceCounter(&qpc);
            frame.record.qpc = static_cast<std::uint64_t>(qpc.QuadPart);
            frame.record.thread_id = GetCurrentThreadId();
        } else disable_rpc_gate_trace();
    }
    SetLastError(incoming_error);
    original_rpc_sender(actor, connection, function, arguments, script_frame, server_side);
    const DWORD outgoing_error = GetLastError();
    if (frame.observe) write_rpc_gate_trace(frame.record);
    SetLastError(outgoing_error);
}

bool open_rpc_gate_trace() noexcept {
    wchar_t root[32768]{}, directory[32768]{}, path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    if (length == 0 || length >= std::size(root)) return false;
    const int directory_length = std::swprintf(directory, std::size(directory), L"%ls\\HawkLink", root);
    if (directory_length < 0 || static_cast<std::size_t>(directory_length) >= std::size(directory)) return false;
    if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG stamp = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const int path_length = std::swprintf(path, std::size(path), L"%ls\\rpc-gates-%lu-%llu.csv", directory,
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(stamp));
    if (path_length < 0 || static_cast<std::size_t>(path_length) >= std::size(path)) return false;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr char header[] = "sequence,qpc,thread_id,class_cache,function_cache,actor_lookup,"
        "channel_index,opening_packet_id,field_id,result\r\n";
    DWORD written = 0;
    if (!WriteFile(file, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr)
        || written != sizeof(header) - 1) { CloseHandle(file); return false; }
    rpc_gate_trace_file = file;
    return true;
}

bool install_local_rpc_gate_trace(Image& image) noexcept {
    // Exact SHA is checked by initialize(). These four complete CALL rel32
    // instructions are independently verified against that same original EXE.
    struct Site { DWORD call, target; std::array<unsigned char, 5> bytes; };
    constexpr std::array<Site, 4> sites{{
        {0x687c7a,0x72c50,{0xe8,0xd1,0xaf,0x9e,0xff}},
        {0x687c91,0x739e0,{0xe8,0x4a,0xbd,0x9e,0xff}},
        {0x687cb2,0x430d0,{0xe8,0x19,0xb4,0x9b,0xff}},
        {0x690566,0x687c10,{0xe8,0xa5,0x76,0xff,0xff}}}};
    for (const auto& site : sites)
        if (!image.contains(site.call, site.bytes.size()) || !image.contains(site.target, 1)
            || std::memcmp(image.base + site.call, site.bytes.data(), site.bytes.size()) != 0)
            return false;
    if (!open_rpc_gate_trace()) return false;
    original_rpc_class_cache = reinterpret_cast<RpcCacheLookup>(image.base + sites[0].target);
    original_rpc_function_cache = reinterpret_cast<RpcCacheLookup>(image.base + sites[1].target);
    original_rpc_actor_lookup = reinterpret_cast<RpcActorLookup>(image.base + sites[2].target);
    original_rpc_sender = reinterpret_cast<RpcSender>(image.base + sites[3].target);
    const std::array hooks{&rpc_class_cache_hook,&rpc_function_cache_hook,&rpc_actor_lookup_hook,&rpc_sender_hook};
    const std::array replacements{reinterpret_cast<void*>(&observed_rpc_class_cache),
        reinterpret_cast<void*>(&observed_rpc_function_cache), reinterpret_cast<void*>(&observed_rpc_actor_lookup),
        reinterpret_cast<void*>(&observed_rpc_sender)};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        void* original = nullptr;
        if (!hooks[i]->install(image.base + sites[i].call, image.base + sites[i].target, replacements[i], &original)
            || original != image.base + sites[i].target) {
            // Any hook whose restoration fails remains a silent passthrough:
            // original targets remain valid for the process-lifetime DLL.
            for (auto* hook : hooks) if (hook->installed()) (void)hook->restore();
            disable_rpc_gate_trace();
            return false;
        }
    }
    InterlockedExchange(&rpc_gate_enabled, 1);
    return true;
}

__declspec(noinline) HINTERNET WINAPI identity_connect(HINTERNET session, LPCWSTR host,
    INTERNET_PORT port, DWORD reserved) noexcept {
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != connect_return)
        return http_api.connect(session, host, port, reserved);
    return transport()->connect(session, host, port, reserved);
}

__declspec(noinline) HINTERNET WINAPI identity_request(HINTERNET connection, LPCWSTR verb,
    LPCWSTR object, LPCWSTR version, LPCWSTR referrer, LPCWSTR* types, DWORD flags) noexcept {
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != request_return)
        return http_api.open_request(connection, verb, object, version, referrer, types, flags);
    return transport()->open_request(connection, verb, object, version, referrer, types, flags);
}

BOOL WINAPI identity_close(HINTERNET handle) noexcept {
    return transport()->close_handle(handle);
}

#if !defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
struct SteamUserAccess {
    std::int32_t (__cdecl* user)();
    std::int32_t (__cdecl* pipe)();
    void* (__cdecl* factory)(const char*);
    void* (__cdecl* interface_user)(void*, std::int32_t, std::int32_t, const char*);
    std::uint64_t (__cdecl* id)(void*);
};
constinit SteamUserAccess steam_user{};

// Only read the session the original game already initialized. No Init,
// Shutdown, pipe/user creation, ticket request or callback pump here.
bool read_client_identity(void*, std::uint64_t* value) noexcept {
    *value = 0;
    const auto user = steam_user.user(), pipe = steam_user.pipe();
    if (!user || !pipe) return false;
    void* client = steam_user.factory("SteamClient017");
    if (!client) return false;
    void* interface_user = steam_user.interface_user(client, user, pipe, "SteamUser019");
    if (!interface_user) return false;
    const auto id = steam_user.id(interface_user);
    const auto universe = id >> 56, type = (id >> 52) & 0xfU, instance = (id >> 32) & 0xfffffU;
    if (universe == 0 || universe >= 5 || type != 1 || instance > 4 || (id & 0xffffffffU) == 0)
        return false;
    *value = id;
    return true;
}

template<class T> T steam_export(HMODULE module, const Image& image, const char* name, DWORD rva) {
    const auto address = GetProcAddress(module, name);
    if (!image.contains(rva, 1) || reinterpret_cast<unsigned char*>(address) != image.base + rva)
        stop_this_process(85);
    return reinterpret_cast<T>(address);
}

void bind_client_identity() {
    const HMODULE module = GetModuleHandleW(L"steam_api64.dll");
    Image image{};
    const auto expected = module_file(nullptr).parent_path() / L"steam_api64.dll";
    if (!module || !image_of(module, image)
        || image.nt->FileHeader.TimeDateStamp != 1482200241 || image.size != 0x40000
        || image.nt->OptionalHeader.AddressOfEntryPoint != 0xaf48
        || !std::filesystem::equivalent(module_file(module), expected)
        || hawken::sha256_file(expected) != "38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29")
        stop_this_process(85);
    steam_user.user = steam_export<decltype(steam_user.user)>(module, image, "SteamAPI_GetHSteamUser", 0x7ea0);
    steam_user.pipe = steam_export<decltype(steam_user.pipe)>(module, image, "SteamAPI_GetHSteamPipe", 0x7eb0);
    steam_user.factory = steam_export<decltype(steam_user.factory)>(module, image, "SteamInternal_CreateInterface", 0x8cc0);
    steam_user.interface_user = steam_export<decltype(steam_user.interface_user)>(module, image, "SteamAPI_ISteamClient_GetISteamUser", 0x4e20);
    steam_user.id = steam_export<decltype(steam_user.id)>(module, image, "SteamAPI_ISteamUser_GetSteamID", 0x52f0);
}

bool original_call(const Image& image, DWORD rva, DWORD slot) noexcept {
    const auto* bytes = image.at<unsigned char>(rva, 6);
    if (!bytes || bytes[0] != 0xff || bytes[1] != 0x15) return false;
    std::int32_t displacement = 0;
    for (unsigned i = 0; i < 4; ++i)
        reinterpret_cast<unsigned char*>(&displacement)[i] = bytes[i + 2];
    return static_cast<std::int64_t>(rva) + 6 + displacement == slot;
}
#endif

void replace_http_slot(void** slot, void* expected, void* replacement) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) stop_this_process(85);
    const bool installed = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), previous, &ignored) || !installed)
        stop_this_process(85); // Process-local fail closed, even after a partial installation.
}

void install_identity_transport(const hawken::client::RoutingConfig& config) {
    if (config.meteor_uri != "http://127.0.0.1") return;
#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    const HMODULE sdk = GetModuleHandleW(nullptr);
#else
    const HMODULE sdk = GetModuleHandleW(L"Meteor.Sdk.Cpp.Win32.Client64.dll");
#endif
    const HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    Image image{};
    if (!sdk || !winhttp || !image_of(sdk, image)) stop_this_process(85);
    void** connect = import_slot(image, "winhttp.dll", "WinHttpConnect");
    void** request = import_slot(image, "winhttp.dll", "WinHttpOpenRequest");
    void** close = import_slot(image, "winhttp.dll", "WinHttpCloseHandle");
    http_api = {reinterpret_cast<decltype(http_api.connect)>(GetProcAddress(winhttp, "WinHttpConnect")),
        reinterpret_cast<decltype(http_api.open_request)>(GetProcAddress(winhttp, "WinHttpOpenRequest")),
        reinterpret_cast<decltype(http_api.close_handle)>(GetProcAddress(winhttp, "WinHttpCloseHandle")),
        reinterpret_cast<decltype(http_api.add_headers)>(GetProcAddress(winhttp, "WinHttpAddRequestHeaders")),
        reinterpret_cast<decltype(http_api.set_option)>(GetProcAddress(winhttp, "WinHttpSetOption"))};
    if (!connect || !request || !close || !http_api.connect || !http_api.open_request
        || !http_api.close_handle || !http_api.add_headers || !http_api.set_option
        || *connect != reinterpret_cast<void*>(http_api.connect)
        || *request != reinterpret_cast<void*>(http_api.open_request)
        || *close != reinterpret_cast<void*>(http_api.close_handle)) stop_this_process(85);
    hawken::client::identity::IdentityProvider provider{};
#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    provider.read = reinterpret_cast<decltype(provider.read)>(GetProcAddress(sdk, "HarnessReadIdentity"));
    if (!provider.read || !fixture_return(image, connect,
            reinterpret_cast<std::uintptr_t>(GetProcAddress(sdk, "HarnessIdentityConnect")), connect_return)
        || !fixture_return(image, request,
            reinterpret_cast<std::uintptr_t>(GetProcAddress(sdk, "HarnessIdentityRequest")), request_return))
        stop_this_process(85);
#else
    const auto expected = module_file(nullptr).parent_path() / L"Meteor.Sdk.Cpp.Win32.Client64.dll";
    if (image.nt->FileHeader.TimeDateStamp != 1498518423 || image.size != 0x28f000
        || image.nt->OptionalHeader.AddressOfEntryPoint != 0x165354
        || !std::filesystem::equivalent(module_file(sdk), expected)
        || hawken::sha256_file(expected) != "f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57"
        || reinterpret_cast<unsigned char*>(connect) != image.base + 0x18fc38
        || reinterpret_cast<unsigned char*>(request) != image.base + 0x18fc10
        || reinterpret_cast<unsigned char*>(close) != image.base + 0x18fc30
        || !original_call(image, 0x6f0b3, 0x18fc38) || !original_call(image, 0x6f2cf, 0x18fc10)
        || !original_call(image, 0x6fcbc, 0x18fc30) || !original_call(image, 0x6fcc9, 0x18fc30)
        || !original_call(image, 0x6fce6, 0x18fc30)) stop_this_process(85);
    connect_return = reinterpret_cast<std::uintptr_t>(image.base) + 0x6f0b9;
    request_return = reinterpret_cast<std::uintptr_t>(image.base) + 0x6f2d5;
    bind_client_identity();
    provider.read = &read_client_identity;
#endif
    // Construct and publish fully before any import can dispatch here. This
    // static-import bridge and its adapter intentionally have process lifetime.
    auto* adapter = new hawken::client::identity::Transport({config.meteor_uri, http_api, provider});
    InterlockedExchangePointer(&identity_transport, adapter);
    replace_http_slot(close, reinterpret_cast<void*>(http_api.close_handle), reinterpret_cast<void*>(&identity_close));
    replace_http_slot(request, reinterpret_cast<void*>(http_api.open_request), reinterpret_cast<void*>(&identity_request));
    replace_http_slot(connect, reinterpret_cast<void*>(http_api.connect), reinterpret_cast<void*>(&identity_connect));
}

void initialize() {
#if !defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    try {
        if (hawken::sha256_file(module_file(nullptr))
            != "4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2")
            stop_this_process(81);
    } catch (...) {
        stop_this_process(81);
    }
#endif
    std::wstring command;
    hawken::client::RoutingConfig config;
    try {
        config = hawken::client::read_routing_toml(
            module_file(proxy_module).parent_path() / L"hawken-client.toml");
        const LPWSTR original = original_command_line();
        if (original == nullptr) stop_this_process(82);
        command = hawken::client::meteor_command_line(original, config);
    } catch (...) {
        stop_this_process(82);
    }

    wchar_t system[MAX_PATH + 1]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH + 1);
    if (length == 0 || length >= MAX_PATH + 1) stop_this_process(83);
    const std::wstring path = std::wstring(system, length) + L"\\dinput8.dll";
    const HMODULE real = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (real == nullptr || real == proxy_module) stop_this_process(83);
    const auto create = reinterpret_cast<InputCreate>(GetProcAddress(real, "DirectInput8Create"));
    if (create == nullptr) stop_this_process(83);

    if (command.size() >= (std::numeric_limits<std::size_t>::max() / sizeof(wchar_t)) - 1)
        stop_this_process(84);
    auto* buffer = static_cast<LPWSTR>(HeapAlloc(GetProcessHeap(), 0,
        (command.size() + 1) * sizeof(wchar_t)));
    if (buffer == nullptr) stop_this_process(84);
    for (std::size_t i = 0; i <= command.size(); ++i) buffer[i] = command.c_str()[i];
    altered_command_line = buffer;
    real_create = create;
    install_identity_transport(config);
#if !defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    if (config.meteor_uri == "http://127.0.0.1") {
        Image game_image{};
        if (!image_of(GetModuleHandleW(nullptr), game_image)
            || !install_local_send_bunch_trace(game_image))
            OutputDebugStringA("HawkLink SendBunch trace was not installed; startup continues.\r\n");
        if (game_image.base != nullptr && !install_local_rpc_gate_trace(game_image))
            OutputDebugStringA("HawkLink RPC gate trace was not installed; startup continues.\r\n");
    }
#endif
    // Interlocked publish occurs after all data; exports never read partial state.
    InterlockedExchange(&initialization, 2);
#if defined(HAWKEN_NATIVE_HOST_RESEARCH)
    // Opt-in worker starts after normal initialization, outside loader lock.
    // This separate research build retains the original EXE/SDK/Steam checks.
    Image original_image{};
    if (!image_of(GetModuleHandleW(nullptr), original_image)) stop_this_process(86);
    const bool research_started = hawken::probe::native_host::start_runtime(
        {reinterpret_cast<std::uint64_t>(original_image.base), original_image.size},
        original_command_line());
    if (!research_started && hawken::probe::native_host::runtime_status()
            == hawken::probe::native_host::RuntimeStatus::failed)
        stop_this_process(86); // An enabled research request must be contained.
#endif
}

__declspec(noinline) LPWSTR WINAPI hooked_command_line() noexcept {
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != permitted_return)
        return original_command_line();
    const LONG state = InterlockedCompareExchange(&initialization, 1, 0);
    if (state == 2) return altered_command_line;
    if (state != 0) stop_this_process(84); // No wait/reentrant initialization.
    try {
        initialize();
        return altered_command_line;
    } catch (...) {
        stop_this_process(84);
    }
}

bool attach(HMODULE module) noexcept {
    Image image{};
    if (!image_of(GetModuleHandleW(nullptr), image)) return false;
#if !defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    // Reject a different executable before scanning any of its import tables.
    if (image.nt->FileHeader.TimeDateStamp != 1499273047
        || image.size != 0x201e000 || image.nt->OptionalHeader.AddressOfEntryPoint != 0x104de30)
        return false;
#endif
    void** slot = import_slot(image, "kernel32.dll", "GetCommandLineW");
    if (slot == nullptr || *slot != reinterpret_cast<void*>(&GetCommandLineW)) return false;
#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
    if (!fixture_callsite(image, slot)) return false;
#else
    if (!production_identity(image, slot)) return false;
#endif
    proxy_module = module;
    original_command_line = reinterpret_cast<CommandLine>(*slot);
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    void* expected = reinterpret_cast<void*>(original_command_line);
    void* replacement = reinterpret_cast<void*>(&hooked_command_line);
    const bool installed = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), previous, &ignored)) {
        // The page is still writable. Never let loader unload a DLL whose hook
        // remains installed after an attach failure.
        if (installed) (void)InterlockedCompareExchangePointer(slot, expected, replacement);
        (void)VirtualProtect(slot, sizeof(void*), previous, &ignored);
        return false;
    }
    return installed;
}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version,
    REFIID iid, LPVOID* result, LPUNKNOWN outer) {
    if (InterlockedCompareExchange(&initialization, 2, 2) != 2) {
        if (result != nullptr) *result = nullptr;
        return DIERR_NOTINITIALIZED;
    }
    return real_create(instance, version, iid, result, outer);
}

#if defined(HAWKEN_CLIENT_BRIDGE_HARNESS)
extern "C" __declspec(dllexport) BOOL WINAPI HawkLinkHarnessFormatRpcGateTrace(
    void* class_cache, void* function_cache, void* actor_map, int* index,
    char* output, DWORD capacity) noexcept {
    RpcGateTraceRecord record{};
    record.sequence = 1;
    record.qpc = 2;
    record.thread_id = 3;
    record.class_cache = class_cache != nullptr ? 1 : 0;
    if (class_cache != nullptr) {
        capture_rpc_function_cache(function_cache, record);
        if (function_cache != nullptr) capture_rpc_actor_lookup(actor_map, index, record);
    }
    return format_rpc_gate_record(record, output, capacity) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI HawkLinkHarnessFormatBunchTrace(
    void* channel, void* bunch, DWORD send_flags, char* output, DWORD capacity) noexcept {
    SendBunchTraceRecord record{};
    record.sequence = 1;
    record.qpc = 2;
    record.thread_id = 3;
    return capture_send_bunch_fields(channel, bunch, send_flags, record)
        && format_send_bunch_record(record, output, capacity) ? TRUE : FALSE;
}
#endif

#if defined(HAWKEN_NATIVE_HOST_RESEARCH)
// Read-only observation of our worker; never an entry point into game code.
extern "C" __declspec(dllexport) std::uint32_t HawkLinkNativeHostState() noexcept {
    return static_cast<std::uint32_t>(hawken::probe::native_host::runtime_status());
}
#endif

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) return attach(module) ? TRUE : FALSE;
    // No DisableThreadLibraryCalls: this project uses a static CRT.
    // No unload cleanup: static-import proxy has process lifetime only.
    return TRUE;
}
