#include "hawken/starter_inventory.hpp"
#include <doctest/doctest.h>
#include <array>
#include <string>
#include <string_view>

namespace {
using hawken::inventory::StarterInventoryError;
using hawken::inventory::StarterInventoryErrorCode;
using hawken::inventory::StarterInventoryIds;
using hawken::inventory::starter_inventory_result;
using nlohmann::json;

StarterInventoryIds fixture_ids() {
    // Synthetic fixture IDs only; no Steam/account identity or historical GUID.
    return {
        {"10000000-0000-0000-0000-000000000001", "20000000-0000-0000-0000-000000000001"},
        {"10000000-0000-0000-0000-000000000002", "20000000-0000-0000-0000-000000000002"},
        {"10000000-0000-0000-0000-000000000003", "20000000-0000-0000-0000-000000000003"},
        {"10000000-0000-0000-0000-000000000004", "20000000-0000-0000-0000-000000000004"},
        {"10000000-0000-0000-0000-000000000005", "20000000-0000-0000-0000-000000000005"},
        {"10000000-0000-0000-0000-000000000006", "20000000-0000-0000-0000-000000000006"},
        {"10000000-0000-0000-0000-000000000007", "20000000-0000-0000-0000-000000000007"},
        {"10000000-0000-0000-0000-000000000008", "20000000-0000-0000-0000-000000000008"},
        {"10000000-0000-0000-0000-000000000009", "20000000-0000-0000-0000-000000000009"}
    };
}

std::array<std::string*, 18> id_fields(StarterInventoryIds& ids) {
    return {&ids.mech.type_id, &ids.mech.instance_id,
            &ids.primary.type_id, &ids.primary.instance_id,
            &ids.secondary.type_id, &ids.secondary.instance_id,
            &ids.upper.type_id, &ids.upper.instance_id,
            &ids.middle.type_id, &ids.middle.instance_id,
            &ids.arms.type_id, &ids.arms.instance_id,
            &ids.lower.type_id, &ids.lower.instance_id,
            &ids.drone.type_id, &ids.drone.instance_id,
            &ids.ability.type_id, &ids.ability.instance_id};
}

json expected_result() {
    // Independently hand-written from the recovered keys and the NEW policy.
    return json::array({
        {
            {"GameItemsGuid", "10000000-0000-0000-0000-000000000001"},
            {"UserGameItemGuid", "20000000-0000-0000-0000-000000000001"},
            {"ReadOnly_Parent", ""}, {"ReadOnly_GlobalItem", "0"},
            {"ReadOnly_Children", json::array({
                "20000000-0000-0000-0000-000000000002",
                "20000000-0000-0000-0000-000000000003",
                "20000000-0000-0000-0000-000000000004",
                "20000000-0000-0000-0000-000000000005",
                "20000000-0000-0000-0000-000000000006",
                "20000000-0000-0000-0000-000000000007",
                "20000000-0000-0000-0000-000000000008",
                "20000000-0000-0000-0000-000000000009"})},
            {"Preset.PrimaryWeapon_0", "0"},
            {"Preset.PrimaryWeapon_InstanceId_0", "20000000-0000-0000-0000-000000000002"},
            {"Preset.SecondaryWeapon", "1"},
            {"Preset.SecondaryWeapon_InstanceId", "20000000-0000-0000-0000-000000000003"},
            {"Preset.Style.UpperPart", "10"},
            {"Preset.Style.UpperPart_InstanceId", "20000000-0000-0000-0000-000000000004"},
            {"Preset.Style.MidPart", "10"},
            {"Preset.Style.MidPart_InstanceId", "20000000-0000-0000-0000-000000000005"},
            {"Preset.Style.ArmPart", "10"},
            {"Preset.Style.ArmPart_InstanceId", "20000000-0000-0000-0000-000000000006"},
            {"Preset.Style.LowerPart", "10"},
            {"Preset.Style.LowerPart_InstanceId", "20000000-0000-0000-0000-000000000007"},
            {"Preset.Style.DroneType", "0"},
            {"Preset.Style.DroneType_InstanceId", "20000000-0000-0000-0000-000000000008"},
            {"Preset.Ability", "3"},
            {"Preset.Ability_InstanceId", "20000000-0000-0000-0000-000000000009"},
            {"ReadOnly_PrimaryWeapon", "20000000-0000-0000-0000-000000000002"},
            {"ReadOnly_DefaultUpperPart", "20000000-0000-0000-0000-000000000004"},
            {"ReadOnly_DefaultHealingDrone", "20000000-0000-0000-0000-000000000008"},
            {"Preset.GarageOrder", "0"},
            {"Preset.Items.Item1", "-1"}, {"Preset.Items1_InstanceId", ""},
            {"Preset.bItem1IntentionallyEmpty", "1"},
            {"Preset.Items.Item2", "-1"}, {"Preset.Items2_InstanceId", ""},
            {"Preset.bItem2IntentionallyEmpty", "1"},
            {"Preset.Items.Item3", "-1"}, {"Preset.Items3_InstanceId", ""},
            {"Preset.bItem3IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal1", "-1"}, {"Preset.Internals1_InstanceId", ""},
            {"Preset.bInternal1IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal2", "-1"}, {"Preset.Internals2_InstanceId", ""},
            {"Preset.bInternal2IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal3", "-1"}, {"Preset.Internals3_InstanceId", ""},
            {"Preset.bInternal3IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal4", "-1"}, {"Preset.Internals4_InstanceId", ""},
            {"Preset.bInternal4IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal5", "-1"}, {"Preset.Internals5_InstanceId", ""},
            {"Preset.bInternal5IntentionallyEmpty", "1"},
            {"Preset.MechInternals.Internal6", "-1"}, {"Preset.Internals6_InstanceId", ""},
            {"Preset.bInternal6IntentionallyEmpty", "1"}
        },
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000002"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000002"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000003"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000003"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000004"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000004"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000005"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000005"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000006"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000006"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000007"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000007"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000008"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000008"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}},
        {{"GameItemsGuid", "10000000-0000-0000-0000-000000000009"},
         {"UserGameItemGuid", "20000000-0000-0000-0000-000000000009"},
         {"ReadOnly_Parent", "20000000-0000-0000-0000-000000000001"},
         {"ReadOnly_Children", json::array()}, {"ReadOnly_GlobalItem", "0"}}
    });
}

void expect_error(const StarterInventoryIds& ids, StarterInventoryErrorCode code) {
    bool caught = false;
    try { static_cast<void>(starter_inventory_result(ids)); }
    catch (const StarterInventoryError& error) {
        caught = true;
        CHECK(error.code() == code);
        CHECK(std::string(error.what()) == "Starter inventory operation failed.");
    }
    CHECK(caught);
}
}

TEST_CASE("Starter Result equals all nine independently specified instance records") {
    const auto result = starter_inventory_result(fixture_ids());
    CHECK(result == expected_result());
}

TEST_CASE("Starter wire keeps attributes as strings and child references as an array") {
    const auto result = starter_inventory_result(fixture_ids());
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 9);
    for (const auto& record : result) {
        REQUIRE(record.is_object());
        for (const auto& [name, value] : record.items()) {
            if (name == "ReadOnly_Children") {
                REQUIRE(value.is_array());
                for (const auto& child : value) CHECK(child.is_string());
            } else {
                CHECK(value.is_string());
            }
        }
    }
    CHECK_FALSE(result.front().contains("Status"));
    CHECK_FALSE(result.front().contains("Guid"));
    CHECK_FALSE(result.front().contains("UserGuid"));
    CHECK_FALSE(result.front().contains("Version"));
    CHECK_FALSE(result.front().contains("ItemName"));
}

TEST_CASE("Starter references each equipped instance rather than its catalogue type") {
    const auto result = starter_inventory_result(fixture_ids());
    const auto& mech = result.at(0);
    CHECK(mech.at("ReadOnly_PrimaryWeapon") == "20000000-0000-0000-0000-000000000002");
    CHECK(mech.at("Preset.PrimaryWeapon_InstanceId_0") == "20000000-0000-0000-0000-000000000002");
    CHECK(mech.at("ReadOnly_DefaultUpperPart") == "20000000-0000-0000-0000-000000000004");
    CHECK(mech.at("ReadOnly_DefaultHealingDrone") == "20000000-0000-0000-0000-000000000008");
    CHECK(mech.at("Preset.Ability_InstanceId") == "20000000-0000-0000-0000-000000000009");
    CHECK(result.at(1).at("GameItemsGuid") == "10000000-0000-0000-0000-000000000002");
    CHECK(result.at(1).at("UserGameItemGuid") == "20000000-0000-0000-0000-000000000002");
}

TEST_CASE("Starter leaves all optional item and internal slots intentionally empty") {
    const auto result = starter_inventory_result(fixture_ids());
    const auto& mech = result.at(0);
    for (const auto* key : {"Preset.Items.Item1", "Preset.Items.Item2", "Preset.Items.Item3",
                           "Preset.MechInternals.Internal1", "Preset.MechInternals.Internal2",
                           "Preset.MechInternals.Internal3", "Preset.MechInternals.Internal4",
                           "Preset.MechInternals.Internal5", "Preset.MechInternals.Internal6"}) {
        CHECK(mech.at(key) == "-1");
    }
    for (const auto* key : {"Preset.Items1_InstanceId", "Preset.Items2_InstanceId", "Preset.Items3_InstanceId",
                           "Preset.Internals1_InstanceId", "Preset.Internals2_InstanceId", "Preset.Internals3_InstanceId",
                           "Preset.Internals4_InstanceId", "Preset.Internals5_InstanceId", "Preset.Internals6_InstanceId"}) {
        CHECK(mech.at(key) == "");
    }
    for (const auto* key : {"Preset.bItem1IntentionallyEmpty", "Preset.bItem2IntentionallyEmpty",
                           "Preset.bItem3IntentionallyEmpty", "Preset.bInternal1IntentionallyEmpty",
                           "Preset.bInternal2IntentionallyEmpty", "Preset.bInternal3IntentionallyEmpty",
                           "Preset.bInternal4IntentionallyEmpty", "Preset.bInternal5IntentionallyEmpty",
                           "Preset.bInternal6IntentionallyEmpty"}) {
        CHECK(mech.at(key) == "1");
    }
}

TEST_CASE("Starter output is deterministic and leaves caller IDs unchanged") {
    auto ids = fixture_ids();
    auto before = fixture_ids();
    const auto first = starter_inventory_result(ids);
    const auto second = starter_inventory_result(ids);
    CHECK(first == second);
    const auto original_fields = id_fields(before);
    const auto after_fields = id_fields(ids);
    for (std::size_t i = 0; i < original_fields.size(); ++i) {
        CHECK(*after_fields[i] == *original_fields[i]);
    }
}

TEST_CASE("Starter uses changed persisted IDs throughout the graph without retaining old references") {
    auto ids = fixture_ids();
    ids.mech.instance_id = "abcdefab-cdef-abcd-efab-cdefabcdefab";
    ids.primary.type_id = "abcdefab-cdef-abcd-efab-cdefabcdefac";
    ids.primary.instance_id = "abcdefab-cdef-abcd-efab-cdefabcdefad";
    const auto result = starter_inventory_result(ids);
    CHECK(result.at(0).at("UserGameItemGuid") == "abcdefab-cdef-abcd-efab-cdefabcdefab");
    CHECK(result.at(0).at("ReadOnly_Children").at(0) == "abcdefab-cdef-abcd-efab-cdefabcdefad");
    CHECK(result.at(0).at("ReadOnly_PrimaryWeapon") == "abcdefab-cdef-abcd-efab-cdefabcdefad");
    CHECK(result.at(0).at("Preset.PrimaryWeapon_InstanceId_0") == "abcdefab-cdef-abcd-efab-cdefabcdefad");
    CHECK(result.at(1).at("GameItemsGuid") == "abcdefab-cdef-abcd-efab-cdefabcdefac");
    CHECK(result.at(1).at("UserGameItemGuid") == "abcdefab-cdef-abcd-efab-cdefabcdefad");
    for (std::size_t i = 1; i < result.size(); ++i) {
        CHECK(result.at(i).at("ReadOnly_Parent") == "abcdefab-cdef-abcd-efab-cdefabcdefab");
    }
    CHECK(result.dump().find("20000000-0000-0000-0000-000000000001") == std::string::npos);
    CHECK(result.dump().find("20000000-0000-0000-0000-000000000002") == std::string::npos);
}

TEST_CASE("Starter rejects an empty ID in every named type and instance role") {
    for (std::size_t i = 0; i < 18; ++i) {
        auto ids = fixture_ids();
        *id_fields(ids).at(i) = "";
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
    }
}

TEST_CASE("Starter rejects unset zero UUIDs without generating replacement IDs") {
    for (std::size_t i = 0; i < 18; ++i) {
        auto ids = fixture_ids();
        *id_fields(ids).at(i) = "00000000-0000-0000-0000-000000000000";
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
    }
}

TEST_CASE("Starter rejects wrong length and hyphen positions instead of normalizing IDs") {
    const std::array<std::string_view, 6> malformed = {
        "10000000000000000000000000000001",
        "10000000-0000-0000-0000-00000000001",
        "10000000-0000-0000-0000-0000000000010",
        "1000000-00000-0000-0000-000000000001",
        "10000000_0000-0000-0000-000000000001",
        "10000000-0000-0000-0000-00000000000g"};
    for (const auto value : malformed) {
        auto ids = fixture_ids();
        ids.mech.type_id = value;
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
        ids = fixture_ids();
        ids.ability.instance_id = value;
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
    }
}

TEST_CASE("Starter rejects upper-case hex whitespace and symbolic IDs") {
    for (const auto* value : {"ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB",
                             " abcdefab-cdef-abcd-efab-cdefabcdefab",
                             "abcdefab-cdef-abcd-efab-cdefabcdefab ",
                             "I_mech", "T_primary", "76561198000000000"}) {
        auto ids = fixture_ids();
        ids.primary.instance_id = value;
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
    }
}

TEST_CASE("Starter rejects NUL control and nonASCII bytes even at valid string length") {
    for (const auto byte : {char{0}, char{10}, static_cast<char>(0x80), static_cast<char>(0xff)}) {
        auto ids = fixture_ids();
        ids.drone.type_id[0] = byte;
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
        ids = fixture_ids();
        ids.secondary.instance_id[35] = byte;
        expect_error(ids, StarterInventoryErrorCode::invalid_id);
    }
}

TEST_CASE("Starter rejects duplicate type IDs without substituting another class") {
    auto ids = fixture_ids();
    ids.primary.type_id = ids.mech.type_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
    ids = fixture_ids();
    ids.ability.type_id = ids.drone.type_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
}

TEST_CASE("Starter rejects duplicate instance IDs instead of collapsing children") {
    auto ids = fixture_ids();
    ids.primary.instance_id = ids.mech.instance_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
    ids = fixture_ids();
    ids.ability.instance_id = ids.drone.instance_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
}

TEST_CASE("Starter rejects collisions between type and instance ID domains") {
    auto ids = fixture_ids();
    ids.mech.type_id = ids.mech.instance_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
    ids = fixture_ids();
    ids.ability.instance_id = ids.primary.type_id;
    expect_error(ids, StarterInventoryErrorCode::duplicate_id);
}

TEST_CASE("Starter invalid input error contains no submitted identifier") {
    auto ids = fixture_ids();
    ids.ability.instance_id = "fixture-private-invalid-id";
    expect_error(ids, StarterInventoryErrorCode::invalid_id);
}
