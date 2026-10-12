#pragma once
#include "hawken/auth_service.hpp"
#include "hawken/local_store.hpp"
#include "hawken/starter_catalogue.hpp"
#include <mutex>
#include <string>
#include <string_view>

namespace hawken::inventory {
// Protected per-server catalogue, free starter offer and owned inventory.
// The store/auth references must describe the same server and outlive this
// service. Every request rechecks its grant; owned paths bind the account GUID.
// The caller supplies stable type/offer IDs and an explicit NEW balance. Invalid
// configuration fails construction, without echoing its values. No historical
// catalogue prices/balances, paid currency or purchase route are reproduced.
// The first authorized owned-inventory read grants exactly nine starter items
// atomically. Subsequent reads use SQL and never repair or replace a grant.
// Instance IDs are a NEW deterministic policy: first 128 SHA-256 bits of ASCII
// "hawken-owned-starter-v1/<UserGuid>/<slot>", lower-case 8-4-4-4-12, slots 0..8.
// They are identifiers, not credentials. Preserve database/server identity.
// Catalogue/offers reads do not grant items. One HP=0 offer describes the same
// nine type IDs/version; it is not a public entitlement/purchase operation.
class InventoryService {
public:
    InventoryService(store::LocalStore& store, auth::AuthService& auth,
                     StarterCatalogueIds ids, StarterBalanceProfile balance,
                     std::string starter_offer_id);
    InventoryService(const InventoryService&) = delete;
    InventoryService& operator=(const InventoryService&) = delete;

    // Raw exact targets only: /game-client/game-items, /game-client/game-offers,
    // /userGameItems/<canonical lower-case UUID36>. No slash/query/escaped alias.
    bool supports_target(std::string_view raw_target) const;
    auth::AuthReply get(std::string_view raw_target, std::string_view authorization);
    // Exact Broker PUT: two nonzero canonical lower-case UUID36 segments.
    // Only the account's already-owned mech may receive a flat Preset. string
    // map (64KiB/256 keys). References remain owned and slot-compatible. Stored
    // scalars are opaque UI configuration, never XP/unlock/economy authority.
    // Existing owned-list GET overlays only these attrs; no individual GET.
    bool supports_update_target(std::string_view raw_target) const;
    auth::AuthReply put(std::string_view raw_target, std::string_view authorization,
                        std::string_view body);
private:
    store::LocalStore& store_;
    auth::AuthService& auth_;
    StarterCatalogueIds ids_;
    StarterBalanceProfile balance_;
    std::string offer_id_;
    nlohmann::json catalogue_;
    nlohmann::json offers_;
    std::mutex mutex_;
};
}
