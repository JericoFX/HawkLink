#include "hawken/inventory_service.hpp"
#include "hawken/sha256.hpp"
#include "hawken/starter_inventory.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hawken::inventory {
namespace {
constexpr std::string_view owned_prefix = "/userGameItems/";
constexpr std::string_view batch_prefix = "/userGameItemsBatch/";
constexpr std::string_view broker_prefix = "/userGameItemsBroker/";
struct UpdateTarget { std::string_view guid, instance; };
enum class Kind { unknown, catalogue, offers, owned };
struct Target {
    Kind kind = Kind::unknown;
    std::optional<std::string_view> guid;
};
bool valid_guid(std::string_view value, bool require_nonzero = false) noexcept {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero || !require_nonzero;
}
Target parse_target(std::string_view raw_target) noexcept {
    if (raw_target == "/game-client/game-items") return {Kind::catalogue, std::nullopt};
    if (raw_target == "/game-client/game-offers") return {Kind::offers, std::nullopt};
    if (raw_target.starts_with(owned_prefix)) {
        const auto guid = raw_target.substr(owned_prefix.size());
        if (valid_guid(guid)) return {Kind::owned, guid};
    }
    return {};
}
std::optional<UpdateTarget> parse_update_target(std::string_view raw_target) noexcept {
    if (!raw_target.starts_with(broker_prefix)) return std::nullopt;
    const auto ids = raw_target.substr(broker_prefix.size());
    if (ids.size() != 73 || ids[36] != '/') return std::nullopt;
    const auto guid = ids.substr(0, 36), instance = ids.substr(37);
    if (!valid_guid(guid, true) || !valid_guid(instance, true)) return std::nullopt;
    return UpdateTarget{guid, instance};
}
std::optional<std::string_view> parse_batch_target(std::string_view raw_target) noexcept {
    if (!raw_target.starts_with(batch_prefix)) return std::nullopt;
    const auto guid = raw_target.substr(batch_prefix.size());
    if (!valid_guid(guid, true)) return std::nullopt;
    return guid;
}
// References require a known owned domain or an exact native absence sentinel.
// Unknown _InstanceId names (including indexed forms) reject nonempty values.
bool reference_valid(std::string_view key, std::string_view value,
                     const store::StarterItems& items, PutReason& reason) {
    if (value.empty() || key.find("_InstanceId") == std::string_view::npos) return true;
    // Restrictive local compatibility for built-in visor defaults, not owned items.
    // Foe is observed natively; the other four pairs use static role correspondence.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> visor_defaults{{
        {"Preset.VisorStyle.FriendHUDColor_InstanceId", "FAKE-COLOR-INSTANCE-GUID"},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId", "FAKE-COLOR-INSTANCE-GUID2"},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId", "FAKE-COLOR-INSTANCE-GUID3"},
        {"Preset.VisorStyle.PrimaryReticle_InstanceId", "FAKE-PRIM-RETICLE-INSTANCE-GUID"},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId", "FAKE-SEC-RETICLE-INSTANCE-GUID"}}};
    for (const auto& [name, literal] : visor_defaults)
        if (key == name && value == literal) return true;
    if (value == "empty") {
        // dcad70->db7c20 emits this opaque absence string for these exact
        // helper-produced fields only. No new instance or deletion is implied.
        constexpr std::array<std::string_view, 13> helper_keys{
            "Preset.Items1_InstanceId", "Preset.Items2_InstanceId", "Preset.Items3_InstanceId",
            "Preset.Internals1_InstanceId", "Preset.Internals2_InstanceId", "Preset.Internals3_InstanceId",
            "Preset.Internals4_InstanceId", "Preset.Internals5_InstanceId", "Preset.Internals6_InstanceId",
            "Preset.Style.AnimTauntType_InstanceId", "Preset.Style.CockpitDecoration1_InstanceId",
            "Preset.Style.CockpitDecoration2_InstanceId", "Preset.Style.CockpitDecoration3_InstanceId"};
        for (const auto name : helper_keys)
            if (key == name) return true;
        reason = PutReason::reference_sentinel;
        return false;
    }
    constexpr std::array<std::pair<std::string_view, std::size_t>, 8> roles{{
        {"Preset.PrimaryWeapon_InstanceId_0", 1}, {"Preset.SecondaryWeapon_InstanceId", 2},
        {"Preset.Style.UpperPart_InstanceId", 3}, {"Preset.Style.MidPart_InstanceId", 4},
        {"Preset.Style.ArmPart_InstanceId", 5}, {"Preset.Style.LowerPart_InstanceId", 6},
        {"Preset.Style.DroneType_InstanceId", 7}, {"Preset.Ability_InstanceId", 8}}};
    for (const auto& [name, slot] : roles)
        if (key == name) {
            if (value == items[slot].instance_id) return true;
            reason = PutReason::reference_instance;
            return false;
        }
    reason = PutReason::reference_unknown_key;
    return false;
}
auth::AuthReply failure(int status) {
    switch (status) {
    case 400: return {400, "Invalid inventory request.", nullptr};
    case 401: return {401, "Authentication required.", nullptr};
    case 403: return {403, "Access was rejected.", nullptr};
    case 404: return {404, "Resource not found.", nullptr};
    default: return {500, "Request could not be processed.", nullptr};
    }
}
// Diagnostic categories only: the reference validator remains authoritative.
PutObservation reference_observation(std::string_view key, std::string_view value,
                                     const store::StarterItems& items) noexcept {
    constexpr std::array<std::pair<std::string_view, ReferenceFamily>, 24> families{{
        {"Preset.Style.CamoPattern_InstanceId", ReferenceFamily::camo},
        {"Preset.Style.CamoPatternMiddle_InstanceId", ReferenceFamily::camo},
        {"Preset.Style.CamoPatternLower_InstanceId", ReferenceFamily::camo},
        {"Preset.Style.CamoPatternArm_InstanceId", ReferenceFamily::camo},
        {"Preset.Style.BoosterPart_InstanceId", ReferenceFamily::booster},
        {"Preset.Style.ArmorPart_InstanceId", ReferenceFamily::armor},
        {"Preset.VisorStyle.PrimaryReticle_InstanceId", ReferenceFamily::visor},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId", ReferenceFamily::visor},
        {"Preset.VisorStyle.FriendHUDColor_InstanceId", ReferenceFamily::visor},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId", ReferenceFamily::visor},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId", ReferenceFamily::visor},
        {"Preset.Items1_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Items2_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Items3_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals1_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals2_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals3_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals4_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals5_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Internals6_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Style.AnimTauntType_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration1_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration2_InstanceId", ReferenceFamily::helper_optional},
        {"Preset.Style.CockpitDecoration3_InstanceId", ReferenceFamily::helper_optional}}};
    PutObservation observation{PutReason::reference_unknown_key,
        ReferenceFamily::other, ReferenceValueForm::other};
    for (const auto& [name, family] : families)
        if (key == name) { observation.reference_family = family; break; }
    constexpr std::array<std::pair<std::string_view, VisorKey>, 5> visor_keys{{
        {"Preset.VisorStyle.PrimaryReticle_InstanceId", VisorKey::primary_reticle},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId", VisorKey::secondary_reticle},
        {"Preset.VisorStyle.FriendHUDColor_InstanceId", VisorKey::friend_hud_color},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId", VisorKey::foe_hud_color},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId", VisorKey::neutral_hud_color}}};
    for (const auto& [name, visor_key] : visor_keys)
        if (key == name) { observation.visor_key = visor_key; break; }
    for (const auto& item : items)
        if (value == item.instance_id) {
            observation.value_form = ReferenceValueForm::matches_owned;
            return observation;
        }
    if (valid_guid(value))
        observation.value_form = valid_guid(value, true)
            ? ReferenceValueForm::canonical_uuid : ReferenceValueForm::zero_uuid;
    if (observation.visor_key == VisorKey::none || value.empty() || value.size() > 64)
        return observation;
    bool decimal_identity_shape = value.size() == 17;
    for (const char byte : value) {
        const bool allowed = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == ':'
            || byte == '#' || byte == '-';
        if (!allowed) return observation;
        if (byte < '0' || byte > '9') decimal_identity_shape = false;
    }
    if (decimal_identity_shape) return observation;
    for (std::size_t i = 0; i < value.size(); ++i) observation.visor_literal[i] = value[i];
    observation.visor_literal_size = static_cast<std::uint8_t>(value.size());
    return observation;
}
struct PutReport {
    const PutObserver& observer;
    PutObservation observation{};
    ~PutReport() noexcept {
        if (observer) {
            try { observer(observation); } catch (...) {}
        }
    }
    auth::AuthReply reject(int status, PutReason why) {
        observation.reason = why;
        return failure(status);
    }
};
std::array<std::string_view, 9> types(const StarterCatalogueIds& ids) {
    return {ids.mech, ids.primary, ids.secondary, ids.upper, ids.middle,
            ids.arms, ids.lower, ids.drone, ids.ability};
}
std::string instance_id(std::string_view user_guid, std::size_t slot) {
    const auto text = "hawken-owned-starter-v1/" + std::string(user_guid) + "/" + std::to_string(slot);
    const auto hash = hawken::sha256(std::span(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-" + hash.substr(12, 4)
        + "-" + hash.substr(16, 4) + "-" + hash.substr(20, 12);
}
StarterInventoryIds graph(const store::StarterItems& rows) {
    return {{rows[0].type_id, rows[0].instance_id}, {rows[1].type_id, rows[1].instance_id},
            {rows[2].type_id, rows[2].instance_id}, {rows[3].type_id, rows[3].instance_id},
            {rows[4].type_id, rows[4].instance_id}, {rows[5].type_id, rows[5].instance_id},
            {rows[6].type_id, rows[6].instance_id}, {rows[7].type_id, rows[7].instance_id},
            {rows[8].type_id, rows[8].instance_id}};
}
}
std::string_view put_reason_name(PutReason reason) noexcept {
    switch (reason) {
    case PutReason::success: return "success";
    case PutReason::target: return "target";
    case PutReason::authentication: return "authentication";
    case PutReason::ownership: return "ownership";
    case PutReason::catalogue: return "catalogue";
    case PutReason::body_limit: return "body_limit";
    case PutReason::json_parse: return "json_parse";
    case PutReason::json_shape: return "json_shape";
    case PutReason::duplicate_key: return "duplicate_key";
    case PutReason::key_limit: return "key_limit";
    case PutReason::preset_key: return "preset_key";
    case PutReason::value_type: return "value_type";
    case PutReason::reference_unknown_key: return "reference_unknown_key";
    case PutReason::reference_sentinel: return "reference_sentinel";
    case PutReason::reference_instance: return "reference_instance";
    case PutReason::store_validation: return "store_validation";
    case PutReason::store_not_found: return "store_not_found";
    case PutReason::store_failure: return "store_failure";
    case PutReason::internal: return "internal";
    }
    return "internal";
}
std::string_view reference_family_name(ReferenceFamily family) noexcept {
    switch (family) {
    case ReferenceFamily::none: return "none";
    case ReferenceFamily::camo: return "camo";
    case ReferenceFamily::booster: return "booster";
    case ReferenceFamily::armor: return "armor";
    case ReferenceFamily::visor: return "visor";
    case ReferenceFamily::helper_optional: return "helper_optional";
    case ReferenceFamily::other: return "other";
    }
    return "none";
}
std::string_view reference_value_form_name(ReferenceValueForm form) noexcept {
    switch (form) {
    case ReferenceValueForm::none: return "none";
    case ReferenceValueForm::matches_owned: return "matches_owned";
    case ReferenceValueForm::zero_uuid: return "zero_uuid";
    case ReferenceValueForm::canonical_uuid: return "canonical_uuid";
    case ReferenceValueForm::other: return "other";
    }
    return "none";
}
InventoryService::InventoryService(store::LocalStore& store, auth::AuthService& auth,
                                   StarterCatalogueIds ids, StarterBalanceProfile balance,
                                   std::string starter_offer_id, PutObserver observer)
    : store_(store), auth_(auth), ids_(std::move(ids)), balance_(std::move(balance)),
      offer_id_(std::move(starter_offer_id)), put_observer_(std::move(observer)) {
    try { catalogue_ = starter_catalogue_result(ids_, balance_); }
    catch (const StarterCatalogueError&) {
        throw std::invalid_argument("Invalid inventory configuration.");
    }
    if (!valid_guid(offer_id_, true)) throw std::invalid_argument("Invalid inventory configuration.");
    auto offered_items = nlohmann::json::array();
    for (const auto type : types(ids_)) {
        if (type == offer_id_) throw std::invalid_argument("Invalid inventory configuration.");
        offered_items.push_back({{"GameItemsGuid", std::string(type)}, {"Quantity", 1},
                                 {"GameItemsVersion", balance_.version}});
    }
    offers_ = nlohmann::json::array({{{"GameOfferGuid", offer_id_},
        {"Description", "Free starter equipment"}, {"Fees", {{"HP", 0}}},
        {"GameItems", std::move(offered_items)}}});
}
bool InventoryService::supports_target(std::string_view raw_target) const {
    return parse_target(raw_target).kind != Kind::unknown;
}
bool InventoryService::supports_update_target(std::string_view raw_target) const {
    return parse_update_target(raw_target).has_value();
}
auth::AuthReply InventoryService::put(std::string_view raw_target,
                                     std::string_view authorization, std::string_view body) {
    // Created before the inner lock: every return/exception releases all service
    // and store locks before this reporter invokes the optional immutable sink.
    PutReport report{put_observer_};
    const auto target = parse_update_target(raw_target);
    if (!target) return report.reject(404, PutReason::target);
    try {
        std::lock_guard lock(mutex_);
        const auto access = auth_.access(authorization, target->guid);
        if (access.status != 200) return report.reject(access.status, PutReason::authentication);
        if (!access.account) return report.reject(500, PutReason::internal);
        const auto& account = access.account->account;
        const auto stored = store_.starter_items(account);
        // PUT neither grants nor repairs equipment. Only the existing mech is mutable.
        if (!stored || stored->at(0).instance_id != target->instance)
            return report.reject(404, PutReason::ownership);
        const auto type_ids = types(ids_);
        for (std::size_t i = 0; i < stored->size(); ++i)
            if (stored->at(i).type_id != type_ids[i]) return report.reject(500, PutReason::catalogue);
        if (body.size() > 65536) return report.reject(400, PutReason::body_limit);
        store::ItemAttributes update;
        try {
            report.observation.reason = PutReason::json_parse;
            std::set<std::string, std::less<>> keys;
            const auto parsed = nlohmann::json::parse(body, [&keys, &report](int depth,
                nlohmann::json::parse_event_t event, nlohmann::json& value) {
                using Event = nlohmann::json::parse_event_t;
                if (event == Event::array_start || (event == Event::object_start && depth != 0)) {
                    report.observation.reason = PutReason::json_shape;
                    throw std::invalid_argument("Invalid inventory request.");
                }
                if (event == Event::key) {
                    if (!keys.insert(value.get<std::string>()).second) {
                        report.observation.reason = PutReason::duplicate_key;
                        throw std::invalid_argument("Invalid inventory request.");
                    }
                    if (keys.size() > 256) {
                        report.observation.reason = PutReason::key_limit;
                        throw std::invalid_argument("Invalid inventory request.");
                    }
                }
                return true;
            });
            if (!parsed.is_object()) return report.reject(400, PutReason::json_shape);
            for (const auto& [key, value] : parsed.items()) {
                if (!key.starts_with("Preset.")) return report.reject(400, PutReason::preset_key);
                if (!value.is_string()) return report.reject(400, PutReason::value_type);
                auto text = value.get<std::string>();
                if (!reference_valid(key, text, *stored, report.observation.reason)) {
                    if (report.observation.reason == PutReason::reference_unknown_key)
                        report.observation = reference_observation(key, text, *stored);
                    return failure(400);
                }
                update.emplace(key, std::move(text));
            }
        } catch (const nlohmann::json::exception&) { return failure(400); }
          catch (const std::invalid_argument&) { return failure(400); }
        // Atomic SQL merge validates byte/UTF8 budgets before commit. Empty and
        // identical requests are valid replays. Opaque scalars grant no authority.
        store_.merge_owned_item_attributes(account, target->instance, update);
        report.observation.reason = PutReason::success;
        return {200, "", nullptr};
    } catch (const store::StoreError& error) {
        if (error.code() == store::StoreErrorCode::invalid_input)
            return report.reject(400, PutReason::store_validation);
        if (error.code() == store::StoreErrorCode::not_found)
            return report.reject(404, PutReason::store_not_found);
        return report.reject(500, PutReason::store_failure);
    } catch (...) { return report.reject(500, PutReason::internal); }
}

auth::AuthReply InventoryService::get(std::string_view raw_target, std::string_view authorization) {
    const auto target = parse_target(raw_target);
    if (target.kind == Kind::unknown) return failure(404);
    try {
        std::lock_guard lock(mutex_);
        const auto access = auth_.access(authorization, target.guid);
        if (access.status != 200) return failure(access.status);
        if (!access.account) return failure(500);
        if (target.kind == Kind::catalogue) return {200, "", catalogue_};
        if (target.kind == Kind::offers) return {200, "", offers_};
        const auto& account = access.account->account;
        auto stored = store_.starter_items(account);
        const auto type_ids = types(ids_);
        if (!stored) {
            store::StarterItems items;
            for (std::size_t i = 0; i < items.size(); ++i)
                items[i] = {std::string(type_ids[i]), instance_id(access.account->user_guid, i)};
            // SQL validates collisions and commits all nine items and the marker
            // atomically. An identical concurrent grant is an idempotent replay.
            store_.grant_starter_items(account, items);
            stored = store_.starter_items(account);
        }
        if (!stored) return failure(500);
        return {200, "", owned_result(account, *stored)};
    } catch (...) { return failure(500); }
}
nlohmann::json InventoryService::owned_result(const store::AccountRef& account,
                                             const store::StarterItems& rows) const {
    const auto type_ids = types(ids_);
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows.at(i).type_id != type_ids[i]) throw std::runtime_error("Invalid owned inventory.");
    auto result = starter_inventory_result(graph(rows));
    for (std::size_t i = 0; i < result.size(); ++i) {
        result.at(i)["ItemName"] = catalogue_.at(i).at("ItemName");
        result.at(i)["Version"] = catalogue_.at(i).at("Version");
        const auto attributes = store_.owned_item_attributes(account, rows.at(i).instance_id);
        for (const auto& [key, value] : attributes) {
            // Root identity and ReadOnly graph always come from the owned tree.
            if (!key.starts_with("Preset.")) throw std::runtime_error("Invalid owned inventory.");
            result.at(i)[key] = value;
        }
    }
    return result;
}
bool InventoryService::supports_batch_target(std::string_view raw_target) const noexcept {
    return parse_batch_target(raw_target).has_value();
}
auth::AuthReply InventoryService::post_batch(std::string_view raw_target,
                                            std::string_view authorization, std::string_view body) {
    const auto target = parse_batch_target(raw_target);
    if (!target) return failure(404);
    try {
        std::lock_guard lock(mutex_);
        const auto access = auth_.access(authorization, *target);
        if (access.status != 200) return failure(access.status);
        if (!access.account) return failure(500);
        if (body.size() > 8192) return failure(400);
        std::vector<std::string> requested;
        try {
            const auto parsed = nlohmann::json::parse(body, [](int depth,
                nlohmann::json::parse_event_t event, nlohmann::json&) {
                using Event = nlohmann::json::parse_event_t;
                if (event == Event::object_start || (event == Event::array_start && depth != 0))
                    throw std::invalid_argument("Invalid inventory request.");
                return true;
            });
            if (!parsed.is_null() && !parsed.is_array()) return failure(400);
            std::set<std::string, std::less<>> unique;
            if (parsed.is_array()) for (const auto& value : parsed) {
                if (!value.is_string()) return failure(400);
                auto id = value.get<std::string>();
                // Only ASCII hex changes case; valid_guid retains exact UUID shape.
                for (auto& c : id) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
                if (!valid_guid(id, true) || !unique.insert(id).second) return failure(400);
                requested.push_back(std::move(id));
            }
        } catch (const nlohmann::json::exception&) { return failure(400); }
          catch (const std::invalid_argument&) { return failure(400); }
        auto result = nlohmann::json::array();
        if (requested.empty()) return {200, "", std::move(result)};
        const auto& account = access.account->account;
        const auto stored = store_.starter_items(account);
        // This read never creates or repairs a starter tree, even on a missing ID.
        if (!stored) return failure(404);
        std::vector<std::size_t> slots;
        for (const auto& id : requested) {
            const auto match = std::find_if(stored->begin(), stored->end(), [&id](const auto& row) {
                return row.instance_id == id;
            });
            if (match == stored->end()) return failure(404);
            slots.push_back(static_cast<std::size_t>(match - stored->begin()));
        }
        const auto owned = owned_result(account, *stored);
        for (const auto slot : slots) result.push_back(owned.at(slot));
        return {200, "", std::move(result)};
    } catch (...) { return failure(500); }
}
}
