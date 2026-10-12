#pragma once
#include "hawken/auth_service.hpp"
#include <string>
#include <string_view>

namespace hawken::sync {
struct MenuSyncReply {
    int http_status = 500;
    int api_status = 500;
    std::string message;
    nlohmann::json result = nullptr;
};

// Persist only received settings; other initial-sync resources remain absent.
// AuthService and its SAME LocalStore must outlive this service. Each GET/PUT
// rechecks access; settings are not authoritative gameplay metrics or consent.
// All user targets bind their GUID to the authenticated account. Unknown raw
// targets are rejected before auth. Settings alone write SQL; this service grants
// no inventory, authenticates no Steam identity and claims no native menu sync.
// Targets use canonical lower-case GUIDs, without alias/escape decoding; the
// sole countryCode value is 1..64 ASCII [A-Za-z0-9_.-] (new server policy).
// Unlocked-resource absence deliberately returns HTTP200/API404/Result[].
class MenuSyncService {
public:
    MenuSyncService(auth::AuthService& auth, store::LocalStore& store);
    MenuSyncService(const MenuSyncService&) = delete;
    MenuSyncService& operator=(const MenuSyncService&) = delete;

    bool supports_target(std::string_view raw_target) const;
    bool supports_settings_target(std::string_view raw_target) const;
    MenuSyncReply get(std::string_view raw_target, std::string_view authorization);
    // Exact PUT settings object<string,string>; no duplicate keys. New policy:
    // <=65536 body bytes, <=256 pairs, Store limits above. HTTP/API200 Result:null
    // only after commit; malformed input400, access401/403, fixed internal500.
    MenuSyncReply put(std::string_view raw_target, std::string_view authorization,
                      std::string_view body);
private:
    auth::AuthService& auth_;
    store::LocalStore& store_;
};
}
