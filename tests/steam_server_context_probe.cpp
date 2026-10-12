// Opt-in native diagnostic, deliberately not registered in CTest. It uses the
// installed, hash-pinned Steam libraries and never requests a user ticket.
#include "hawken/steam_local_auth.hpp"
#include <windows.h>
#include <array>
#include <bit>
#include <cstdio>
#include <io.h>

namespace {
bool contain_native_output() {
    FILE* null_stream = nullptr;
    if (fopen_s(&null_stream, "NUL", "w") != 0 || !null_stream) return false;
    const int descriptor = _fileno(null_stream);
    const bool redirected = _dup2(descriptor, _fileno(stdout)) == 0
        && _dup2(descriptor, _fileno(stderr)) == 0;
    fclose(null_stream);
    if (!redirected) return false;
    const HANDLE output = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
    const HANDLE error = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    return SetStdHandle(STD_OUTPUT_HANDLE, output) && SetStdHandle(STD_ERROR_HANDLE, error);
}
using HandleGetter = std::int32_t (__cdecl*)();
HandleGetter getter(HMODULE module, const char* name) {
    return std::bit_cast<HandleGetter>(GetProcAddress(module, name));
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) return 2;
    HANDLE report = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report == INVALID_HANDLE_VALUE) return 2;
    const auto record = [report](const char* name, bool result) {
        std::array<char, 128> line{};
        const int length = sprintf_s(line.data(), line.size(), "%s=%d\n", name, result ? 1 : 0);
        DWORD written = 0;
        return length > 0 && WriteFile(report, line.data(), static_cast<DWORD>(length), &written, nullptr)
            && written == static_cast<DWORD>(length);
    };
    bool pass = contain_native_output();
    pass = record("native_output_contained", pass) && pass;
    bool startup = false, shutdown = false;
    auto verifier = pass ? hawken::auth::make_local_steam_verifier([&](const auto& observation) {
        if (observation.phase == hawken::auth::LocalSteamPhase::startup)
            startup = observation.available;
        if (observation.phase == hawken::auth::LocalSteamPhase::shutdown)
            shutdown = observation.gs_shutdown_returned && observation.gs_pipe_zero;
    }) : nullptr;
    pass = record("provider_ready", verifier && startup) && verifier && startup && pass;
    HMODULE module = GetModuleHandleW(L"steam_api64.dll");
    const auto user = module ? getter(module, "SteamAPI_GetHSteamUser") : nullptr;
    const auto pipe = module ? getter(module, "SteamAPI_GetHSteamPipe") : nullptr;
    const auto gs_user = module ? getter(module, "SteamGameServer_GetHSteamUser") : nullptr;
    const auto gs_pipe = module ? getter(module, "SteamGameServer_GetHSteamPipe") : nullptr;
    const bool bound = user && pipe && gs_user && gs_pipe;
    pass = record("getter_exports_available", bound) && bound && pass;
    if (bound) {
        pass = record("desktop_user_context_absent_during", user() == 0 && pipe() == 0) && pass;
        pass = record("gameserver_context_present_during", gs_user() != 0 && gs_pipe() != 0) && pass;
    }
    if (verifier) {
        const auto result = verifier->verify(std::span<const std::uint8_t>{});
        pass = record("ticket_only_unavailable", result.status == hawken::auth::SteamAuthStatus::unavailable
            && result.steam_id == 0) && pass;
    }
    verifier.reset(); // joins the worker before inspecting observer state.
    pass = record("owned_gameserver_shutdown", shutdown) && pass;
    if (bound) {
        pass = record("desktop_user_context_absent_after", user() == 0 && pipe() == 0) && pass;
        // The original Shutdown releases the user but retains its numeric
        // getter value. Only the pipe is explicitly zeroed by this legacy DLL.
        pass = record("gameserver_context_absent_after", gs_pipe() == 0) && pass;
    }
    pass = record("probe_pass", pass) && pass;
    CloseHandle(report);
    return pass ? 0 : 1;
}
