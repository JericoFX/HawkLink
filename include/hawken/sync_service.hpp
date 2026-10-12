#pragma once
#include "hawken/auth_service.hpp"
#include "hawken/local_store.hpp"
#include <cstdint>
#include <string_view>

namespace hawken::sync {
// Protected per-server initial sync data, separate from the Master directory.
// The references must outlive this service and refer to the same server store.
// Every get rechecks AuthService access. Wallet paths additionally bind the
// canonical lower-case account GUID; no request provisions an account or grant.
// overflow_interval is an explicit positive server rule, not a recovered price
// or historical balance. Empty definition arrays mean no active achievements
// or rewards; these five GET responses do not complete the full menu sync.
class SyncService {
public:
    SyncService(store::LocalStore& store, auth::AuthService& auth,
                std::int32_t overflow_interval);
    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    // Raw exact request target: no decoding, aliases, queries or slash repair.
    // Regions: empty or ASCII [A-Za-z0-9_.-], 1..64. Wallet GUIDs: exactly
    // lower-case 8-4-4-4-12 hexadecimal. Format does not authenticate ownership.
    bool supports_target(std::string_view raw_target) const;
    auth::AuthReply get(std::string_view raw_target, std::string_view authorization);
private:
    store::LocalStore& store_;
    auth::AuthService& auth_;
    std::int32_t overflow_interval_;
};
}
