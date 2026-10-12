#include "hawken/inventory_service.hpp"
#include "hawken/sha256.hpp"
#include "hawken/starter_inventory.hpp"
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hawken::inventory {
namespace {
constexpr std::string_view owned_prefix = "/userGameItems/";
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
auth::AuthReply failure(int status) {
    switch (status) {
    case 401: return {401, "Authentication required.", nullptr};
    case 403: return {403, "Access was rejected.", nullptr};
    case 404: return {404, "Resource not found.", nullptr};
    default: return {500, "Request could not be processed.", nullptr};
    }
}
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
InventoryService::InventoryService(store::LocalStore& store, auth::AuthService& auth,
                                   StarterCatalogueIds ids, StarterBalanceProfile balance,
                                   std::string starter_offer_id)
    : store_(store), auth_(auth), ids_(std::move(ids)), balance_(std::move(balance)),
      offer_id_(std::move(starter_offer_id)) {
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
        for (std::size_t i = 0; i < stored->size(); ++i)
            if (stored->at(i).type_id != type_ids[i]) return failure(500);
        auto result = starter_inventory_result(graph(*stored));
        for (std::size_t i = 0; i < result.size(); ++i) {
            result.at(i)["ItemName"] = catalogue_.at(i).at("ItemName");
            result.at(i)["Version"] = catalogue_.at(i).at("Version");
        }
        return {200, "", std::move(result)};
    } catch (...) { return failure(500); }
}
}
