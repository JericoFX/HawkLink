#include "hawken/starter_inventory.hpp"
#include <array>
#include <string_view>

namespace hawken::inventory {
namespace {
bool valid_id(std::string_view id) {
    if (id.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            if (c != '0') nonzero = true;
        }
    }
    return nonzero;
}
}

StarterInventoryError::StarterInventoryError(StarterInventoryErrorCode code)
    : std::runtime_error("Starter inventory operation failed."), code_(code) {}

StarterInventoryErrorCode StarterInventoryError::code() const noexcept {
    return code_;
}

nlohmann::json starter_inventory_result(const StarterInventoryIds& ids) {
    const std::array<const ItemIds*, 9> items = {
        &ids.mech, &ids.primary, &ids.secondary, &ids.upper, &ids.middle,
        &ids.arms, &ids.lower, &ids.drone, &ids.ability};
    std::array<std::string_view, 18> all_ids;
    for (std::size_t i = 0; i < items.size(); ++i) {
        all_ids[i * 2] = items[i]->type_id;
        all_ids[i * 2 + 1] = items[i]->instance_id;
    }
    for (const auto id : all_ids) {
        if (!valid_id(id)) throw StarterInventoryError(StarterInventoryErrorCode::invalid_id);
    }
    for (std::size_t i = 0; i < all_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < all_ids.size(); ++j) {
            if (all_ids[i] == all_ids[j]) {
                throw StarterInventoryError(StarterInventoryErrorCode::duplicate_id);
            }
        }
    }

    auto children = nlohmann::json::array();
    for (std::size_t i = 1; i < items.size(); ++i) {
        children.push_back(items[i]->instance_id);
    }
    auto result = nlohmann::json::array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        result.push_back({
            {"GameItemsGuid", items[i]->type_id},
            {"UserGameItemGuid", items[i]->instance_id},
            {"ReadOnly_Parent", i == 0 ? std::string{} : ids.mech.instance_id},
            {"ReadOnly_Children", i == 0 ? children : nlohmann::json::array()},
            {"ReadOnly_GlobalItem", "0"}});
    }

    auto& mech = result.at(0);
    mech.update({
        {"Preset.PrimaryWeapon_0", "0"},
        {"Preset.PrimaryWeapon_InstanceId_0", ids.primary.instance_id},
        {"Preset.SecondaryWeapon", "1"},
        {"Preset.SecondaryWeapon_InstanceId", ids.secondary.instance_id},
        {"Preset.Style.UpperPart", "10"},
        {"Preset.Style.UpperPart_InstanceId", ids.upper.instance_id},
        {"Preset.Style.MidPart", "10"},
        {"Preset.Style.MidPart_InstanceId", ids.middle.instance_id},
        {"Preset.Style.ArmPart", "10"},
        {"Preset.Style.ArmPart_InstanceId", ids.arms.instance_id},
        {"Preset.Style.LowerPart", "10"},
        {"Preset.Style.LowerPart_InstanceId", ids.lower.instance_id},
        {"Preset.Style.DroneType", "0"},
        {"Preset.Style.DroneType_InstanceId", ids.drone.instance_id},
        {"Preset.Ability", "3"},
        {"Preset.Ability_InstanceId", ids.ability.instance_id},
        {"ReadOnly_PrimaryWeapon", ids.primary.instance_id},
        {"ReadOnly_DefaultUpperPart", ids.upper.instance_id},
        {"ReadOnly_DefaultHealingDrone", ids.drone.instance_id},
        {"Preset.GarageOrder", "0"}});
    for (int i = 1; i <= 3; ++i) {
        const auto slot = std::to_string(i);
        mech["Preset.Items.Item" + slot] = "-1";
        mech["Preset.Items" + slot + "_InstanceId"] = "";
        mech["Preset.bItem" + slot + "IntentionallyEmpty"] = "1";
    }
    for (int i = 1; i <= 6; ++i) {
        const auto slot = std::to_string(i);
        mech["Preset.MechInternals.Internal" + slot] = "-1";
        mech["Preset.Internals" + slot + "_InstanceId"] = "";
        mech["Preset.bInternal" + slot + "IntentionallyEmpty"] = "1";
    }
    return result;
}
}
