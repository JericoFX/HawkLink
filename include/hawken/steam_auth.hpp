#pragma once
#include <cstdint>
#include <span>

namespace hawken::auth {
enum class SteamAuthStatus {
    authenticated, unavailable, invalid_ticket, rejected, license_missing, timed_out
};

struct SteamAuthResult {
    SteamAuthStatus status = SteamAuthStatus::unavailable;
    std::uint64_t steam_id = 0;
};

// A successful result requires the native ticket callback and app license check.
// Interface identities are private account data; do not include them in diagnostics.
class SteamVerifier {
public:
    virtual ~SteamVerifier() = default;
    virtual SteamAuthResult verify(std::span<const std::uint8_t> ticket) = 0;
    // A supplied identity is an untrusted claim, not evidence of authentication.
    // Unsupported implementations fail closed; never fall back to ticket-only.
    virtual SteamAuthResult verify(std::span<const std::uint8_t>, std::uint64_t) {
        return {};
    }
    // A prior result may be revoked by a later Steam callback or context loss.
    virtual bool is_authenticated(std::uint64_t steam_id) const = 0;
    virtual void end_session(std::uint64_t steam_id) = 0;
};
}
