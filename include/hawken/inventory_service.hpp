#pragma once
#include "hawken/auth_service.hpp"
#include "hawken/local_store.hpp"
#include "hawken/starter_catalogue.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace hawken::inventory {
// Fixed service outcome categories. HTTP framing failures happen before
// this service and are diagnosed by the HTTP owner separately.
enum class PutReason {
    success, target, authentication, ownership, catalogue, body_limit,
    json_parse, json_shape, duplicate_key, key_limit, preset_key, value_type,
    reference_unknown_key, reference_sentinel, reference_instance,
    store_validation, store_not_found, store_failure, internal
};
// Supplemental categories apply only to the first reference_unknown_key. They
// describe the rejected input through fixed categories. Only the five known
// visor keys may carry a bounded safe literal, excluding owned-instance matches.
enum class ReferenceFamily { none, camo, booster, armor, visor, helper_optional, other };
enum class ReferenceValueForm { none, matches_owned, zero_uuid, canonical_uuid, other };
enum class VisorKey {
    none, primary_reticle, secondary_reticle, friend_hud_color, foe_hud_color, neutral_hud_color
};
struct PutObservation {
    PutReason reason = PutReason::internal;
    ReferenceFamily reference_family = ReferenceFamily::none;
    ReferenceValueForm value_form = ReferenceValueForm::none;
    VisorKey visor_key = VisorKey::none;
    std::array<char, 65> visor_literal{};
    std::uint8_t visor_literal_size = 0;
};
using PutObserver = std::function<void(const PutObservation&)>;
std::string_view put_reason_name(PutReason reason) noexcept;
std::string_view reference_family_name(ReferenceFamily family) noexcept;
std::string_view reference_value_form_name(ReferenceValueForm form) noexcept;
// Protected per-server catalogue, free starter offer and owned inventory.
// The store/auth references must describe the same server and outlive this
// service. Every request rechecks its grant; owned paths bind the account GUID.
// The caller supplies stable type/offer IDs and an explicit NEW balance. Invalid
// configuration fails construction, without echoing its values. No historical
// catalogue prices/balances, paid currency or purchase route are reproduced.
// The first authorized owned-list GET grants exactly nine starter items
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
                     std::string starter_offer_id, PutObserver observer = {});
    InventoryService(const InventoryService&) = delete;
    InventoryService& operator=(const InventoryService&) = delete;

    // Raw exact targets only: /game-client/game-items, /game-client/game-offers,
    // /userGameItems/<canonical lower-case UUID36>. No slash/query/escaped alias.
    bool supports_target(std::string_view raw_target) const;
    auth::AuthReply get(std::string_view raw_target, std::string_view authorization);
    // Trusted internal caller must obtain/revalidate AccountRef from its active
    // authenticated session outside owner pins. Logical scope is no credential.
    // Reads one existing starter mech atomically through Store, checks all nine
    // configured type IDs, and returns saved attributes without GET/autogrant,
    // rendering, default seeding, PUT observation or gameplay authority.
    store::StarterMechSnapshot owned_starter_mech(const store::AccountRef& account, std::string_view instance_id);
    // Exact POST /userGameItemsBatch/<nonzero canonical lower-case UUID36>.
    // Read only the requested existing instances, in request order, with saved
    // Presets. Body UUIDs are ASCII case-insensitive; normalized duplicates fail.
    // The 8192-byte JSON array/null request never grants or enumerates all items.
    bool supports_batch_target(std::string_view raw_target) const noexcept;
    auth::AuthReply post_batch(std::string_view raw_target, std::string_view authorization,
                              std::string_view body);
    // Exact Broker PUT: two nonzero canonical lower-case UUID36 segments.
    // Only the account's already-owned mech may receive a flat Preset. string
    // map (64KiB/256 keys). Equipment references remain owned and slot-compatible.
    // Five exact built-in visor defaults are opaque configuration, not possession.
    // Stored scalars are never XP/unlock/economy authority.
    // Existing owned-list GET overlays only these attrs; no individual GET.
    bool supports_update_target(std::string_view raw_target) const;
    auth::AuthReply put(std::string_view raw_target, std::string_view authorization,
                        std::string_view body);
private:
    nlohmann::json owned_result(const store::AccountRef& account,
                               const store::StarterItems& rows) const;
    store::LocalStore& store_;
    auth::AuthService& auth_;
    StarterCatalogueIds ids_;
    StarterBalanceProfile balance_;
    std::string offer_id_;
    nlohmann::json catalogue_;
    nlohmann::json offers_;
    // Immutable sink: exactly once per PUT after locks release; exceptions are
    // contained. Only a rejected known visor key may expose its bounded safe literal;
    // credentials, headers and full bodies are excluded. Authority is unchanged.
    PutObserver put_observer_;
    std::mutex mutex_;
};
}
