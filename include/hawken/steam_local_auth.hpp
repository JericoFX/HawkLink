#pragma once
#include "hawken/steam_auth.hpp"
#include <functional>
#include <memory>

namespace hawken::auth {
enum class LocalSteamPhase { startup, begin, callback143, license, session_end, shutdown, context_lost, worker_finished };
enum class LocalSteamContextLoss { none, pipe, user, logged_off };
enum class LocalSteamWorkerFinish { none, normal, exception };
struct LocalSteamObservation {
    LocalSteamPhase phase = LocalSteamPhase::startup;
    bool available = false;
    bool begin_ok = false;
    bool packed20 = false;
    bool identity_match = false;
    bool response_ok = false;
    bool license_ok = false;
    bool gs_shutdown_returned = false;
    bool gs_pipe_zero = false;
    LocalSteamContextLoss context_loss = LocalSteamContextLoss::none;
    LocalSteamWorkerFinish worker_finish = LocalSteamWorkerFinish::none;
};
using LocalSteamObserver = std::function<void(const LocalSteamObservation&)>;

// Windows-only GameServer backend; it never initializes a desktop-user context.
// Ticket-only verify is unavailable. The two-argument overload requires a valid
// Individual identity claim, the matching native callback and AppID 271290 license.
// Returns nullptr when the verified original DLLs or owned context are unavailable.
// Owns one worker; destruction ends the session and its GameServer context.
// Temporary first-local-test policy: at most one BeginAuthSession attempt per
// process, even after rejection, timeout, success or logout. Restart the backend
// for another attempt; callback143 has no nonce to correlate reused identities.
// No concurrent destruction with interface calls. No remote SteamID discovery.
// Observer runs on the native worker, exposes no IDs/payloads and contains thrown
// exceptions. It must not call this verifier or block its bounded native work.
std::unique_ptr<SteamVerifier> make_local_steam_verifier(LocalSteamObserver observer = {});

namespace detail {
// Pure decoding of the adopted Win64 packed callback 143 contract, not auth.
// Callers must independently establish callback ID, pipe ownership and license.
bool callback143_is_ok(std::span<const std::uint8_t> payload,
                       std::uint64_t expected_local_hint);
}
}
