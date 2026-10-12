#include "hawken/starter_catalogue.hpp"
#include <doctest/doctest.h>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <string_view>

namespace {
using hawken::inventory::StarterBalanceProfile;
using hawken::inventory::StarterCatalogueError;
using hawken::inventory::StarterCatalogueErrorCode;
using hawken::inventory::StarterCatalogueIds;
using hawken::inventory::starter_catalogue_result;
using nlohmann::json;

StarterCatalogueIds fixture_ids() {
    // Synthetic persisted-ID fixtures; no historical GUID or Steam identity.
    return {"10000000-0000-0000-0000-000000000001",
            "10000000-0000-0000-0000-000000000002",
            "10000000-0000-0000-0000-000000000003",
            "10000000-0000-0000-0000-000000000004",
            "10000000-0000-0000-0000-000000000005",
            "10000000-0000-0000-0000-000000000006",
            "10000000-0000-0000-0000-000000000007",
            "10000000-0000-0000-0000-000000000008",
            "10000000-0000-0000-0000-000000000009"};
}

StarterBalanceProfile first_profile() {
    // NEW synthetic rules, not historical Hawken balance or a gameplay claim.
    return {"starter.test-1",
        {1.5f, 0.25f, 0.5f, 12000.0f, 40.0f, 100.0f, 32.0f, 45.0f, 55.0f, 70.0f, 80.0f},
        {8.0f, 0.0f, 2.0f, 16000.0f, 100.0f, 0.0f, 75.0f, 30.0f, 65.0f, 90.0f, 40.0f},
        {20.0f, 5.0f, 10.0f}, {2500, 3000, 100, 80, 300, 100, 20}};
}

StarterBalanceProfile second_profile() {
    return {"starter.test-2",
        {2.5f, 0.5f, 0.25f, 10000.0f, 35.0f, 75.0f, 35.0f, 40.0f, 60.0f, 65.0f, 70.0f},
        {6.0f, 0.125f, 1.5f, 18000.0f, 90.0f, 50.0f, 70.0f, 35.0f, 55.0f, 95.0f, 50.0f},
        {30.0f, 6.0f, 12.0f}, {4000, 5000, 200, 120, 400, 150, 30}};
}

json first_expected() {
    // Complete literal oracle independently transcribed from the approved wire.
    return json::parse(R"json([
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000001","ItemName":"Mech B - Rookie","Version":"starter.test-1","invType":"0","index":"15"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000002","ItemName":"Weap Prim - Point-D Vulcan","Version":"starter.test-1","invType":"20","index":"0","supportedMechClasses":"AllMechB",
       "HeatPerShot":"1.5","Spread":"0.25","RefireRate":"0.5","WeaponRange":"12000","InstantHitDamage":"40","AimHelpdist":"100","DamageForUI":"32","RefireRateForUI":"45","HeatForUI":"55","EffectiveRangeForUI":"70","AccuracyForUI":"80"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000003","ItemName":"Weap Sec - TOW Rocket","Version":"starter.test-1","invType":"21","index":"1","supportedMechClasses":"AllMechB",
       "HeatPerShot":"8","Spread":"0","RefireRate":"2","WeaponRange":"16000","InstantHitDamage":"100","AimHelpdist":"0","DamageForUI":"75","RefireRateForUI":"30","HeatForUI":"65","EffectiveRangeForUI":"90","AccuracyForUI":"40",
       "Proj.Speed":"2500","Proj.MaxSpeed":"3000","Proj.AccelRate":"100","Proj.Damage":"80","Proj.DamageRadius":"300","Proj.InnerDamageRadius":"100","Proj.MinDamage":"20"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000004","ItemName":"FRED UPPER","Version":"starter.test-1","invType":"8","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000005","ItemName":"FRED MIDDLE","Version":"starter.test-1","invType":"9","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000006","ItemName":"FRED ARMS","Version":"starter.test-1","invType":"10","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000007","ItemName":"FRED LOWER","Version":"starter.test-1","invType":"11","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000008","ItemName":"RD-TR1","Version":"starter.test-1","invType":"33","index":"0","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000009","ItemName":"Ability - Coolant","Version":"starter.test-1","invType":"34","index":"3","supportedMechClasses":"AllMechB","RegenerationSeconds":"20","ModeActiveTime":"5","HeatReductionRate":"10"}
    ])json");
}

json second_expected() {
    // Entire second oracle is literal: no derivation from the first or builder.
    return json::parse(R"json([
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000001","ItemName":"Mech B - Rookie","Version":"starter.test-2","invType":"0","index":"15"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000002","ItemName":"Weap Prim - Point-D Vulcan","Version":"starter.test-2","invType":"20","index":"0","supportedMechClasses":"AllMechB",
       "HeatPerShot":"2.5","Spread":"0.5","RefireRate":"0.25","WeaponRange":"10000","InstantHitDamage":"35","AimHelpdist":"75","DamageForUI":"35","RefireRateForUI":"40","HeatForUI":"60","EffectiveRangeForUI":"65","AccuracyForUI":"70"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000003","ItemName":"Weap Sec - TOW Rocket","Version":"starter.test-2","invType":"21","index":"1","supportedMechClasses":"AllMechB",
       "HeatPerShot":"6","Spread":"0.125","RefireRate":"1.5","WeaponRange":"18000","InstantHitDamage":"90","AimHelpdist":"50","DamageForUI":"70","RefireRateForUI":"35","HeatForUI":"55","EffectiveRangeForUI":"95","AccuracyForUI":"50",
       "Proj.Speed":"4000","Proj.MaxSpeed":"5000","Proj.AccelRate":"200","Proj.Damage":"120","Proj.DamageRadius":"400","Proj.InnerDamageRadius":"150","Proj.MinDamage":"30"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000004","ItemName":"FRED UPPER","Version":"starter.test-2","invType":"8","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000005","ItemName":"FRED MIDDLE","Version":"starter.test-2","invType":"9","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000006","ItemName":"FRED ARMS","Version":"starter.test-2","invType":"10","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000007","ItemName":"FRED LOWER","Version":"starter.test-2","invType":"11","index":"10","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000008","ItemName":"RD-TR1","Version":"starter.test-2","invType":"33","index":"0","supportedMechClasses":"AllMechB"},
      {"GameItemsGuid":"10000000-0000-0000-0000-000000000009","ItemName":"Ability - Coolant","Version":"starter.test-2","invType":"34","index":"3","supportedMechClasses":"AllMechB","RegenerationSeconds":"30","ModeActiveTime":"6","HeatReductionRate":"12"}
    ])json");
}

std::array<std::string*, 9> id_fields(StarterCatalogueIds& ids) {
    return {&ids.mech, &ids.primary, &ids.secondary, &ids.upper, &ids.middle,
            &ids.arms, &ids.lower, &ids.drone, &ids.ability};
}

std::array<float*, 32> balance_fields(StarterBalanceProfile& p) {
    return {&p.primary.heat_per_shot, &p.primary.spread, &p.primary.refire_rate,
        &p.primary.weapon_range, &p.primary.instant_hit_damage, &p.primary.aim_help_distance,
        &p.primary.damage_for_ui, &p.primary.refire_rate_for_ui, &p.primary.heat_for_ui,
        &p.primary.effective_range_for_ui, &p.primary.accuracy_for_ui,
        &p.secondary.heat_per_shot, &p.secondary.spread, &p.secondary.refire_rate,
        &p.secondary.weapon_range, &p.secondary.instant_hit_damage, &p.secondary.aim_help_distance,
        &p.secondary.damage_for_ui, &p.secondary.refire_rate_for_ui, &p.secondary.heat_for_ui,
        &p.secondary.effective_range_for_ui, &p.secondary.accuracy_for_ui,
        &p.coolant.regeneration_seconds, &p.coolant.mode_active_time, &p.coolant.heat_reduction_rate,
        &p.secondary_projectile.speed, &p.secondary_projectile.max_speed,
        &p.secondary_projectile.acceleration, &p.secondary_projectile.damage,
        &p.secondary_projectile.damage_radius, &p.secondary_projectile.inner_damage_radius,
        &p.secondary_projectile.min_damage};
}

void expect_error(const StarterCatalogueIds& ids,
    const std::optional<StarterBalanceProfile>& profile, StarterCatalogueErrorCode code) {
    try {
        static_cast<void>(starter_catalogue_result(ids, profile));
        FAIL("Invalid catalogue input was accepted.");
    } catch (const StarterCatalogueError& error) {
        CHECK(error.code() == code);
        CHECK(std::string_view(error.what()) == "Starter catalogue operation failed.");
    }
}
}

TEST_CASE("Starter catalogue emits the complete first manual nine-type oracle") {
    CHECK(starter_catalogue_result(fixture_ids(), first_profile()) == first_expected());
}

TEST_CASE("TOW catalogue supplies the original projectile consumer attributes") {
    // Missing fields make the original SetProjectileDefaults use its CDO
    // fallbacks, not InstantHitDamage or the display-only DamageForUI.
    const auto row = starter_catalogue_result(fixture_ids(), first_profile()).at(2);
    for (const auto key : {"Proj.Speed", "Proj.MaxSpeed", "Proj.AccelRate",
             "Proj.Damage", "Proj.DamageRadius", "Proj.InnerDamageRadius", "Proj.MinDamage"}) {
        REQUIRE(row.contains(key));
        CHECK(row.at(key).is_string());
    }
    CHECK(row.at("Proj.Damage") == "80");
    CHECK(row.at("InstantHitDamage") == "100");
    CHECK(row.at("DamageForUI") == "75");
}

TEST_CASE("TOW catalogue rejects impossible projectile relationships") {
    auto p = first_profile();
    SUBCASE("maximum speed below launch speed") { p.secondary_projectile.max_speed = 2499; }
    SUBCASE("inner radius beyond outer radius") { p.secondary_projectile.inner_damage_radius = 301; }
    SUBCASE("minimum damage above peak damage") { p.secondary_projectile.min_damage = 81; }
    SUBCASE("missing projectile policy") { p.secondary_projectile = {}; }
    expect_error(fixture_ids(), p, StarterCatalogueErrorCode::invalid_balance);
}

TEST_CASE("TOW catalogue supports constant speed and zero minimum damage") {
    auto p = first_profile();
    p.secondary_projectile = {2500, 2500, 0, 80, 300, 0, 0};
    const auto row = starter_catalogue_result(fixture_ids(), p).at(2);
    CHECK(row.at("Proj.Speed") == "2500");
    CHECK(row.at("Proj.MaxSpeed") == "2500");
    CHECK(row.at("Proj.AccelRate") == "0");
    CHECK(row.at("Proj.InnerDamageRadius") == "0");
    CHECK(row.at("Proj.MinDamage") == "0");
}

TEST_CASE("Starter catalogue uses every caller balance value and new version") {
    CHECK(starter_catalogue_result(fixture_ids(), second_profile()) == second_expected());
}

TEST_CASE("Starter catalogue outputs string attributes without ownership or extra rows") {
    const auto result = starter_catalogue_result(fixture_ids(), first_profile());
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 9);
    for (const auto& row : result) {
        REQUIRE(row.is_object());
        for (const auto& [key, value] : row.items()) {
            CHECK(value.is_string());
            CHECK(key.find("ReadOnly_") != 0);
            CHECK(key.find("Preset.") != 0);
            CHECK(key != "UserGameItemGuid");
            CHECK(key != "UserGuid");
            CHECK(key != "primaryWeaponID");
            CHECK(key != "baseMechID");
        }
    }
    CHECK_FALSE(result.at(0).contains("XPBonus"));
    CHECK_FALSE(result.at(0).contains("HCBonus"));
    CHECK_FALSE(result.at(1).contains("WarmupTime"));
    CHECK_FALSE(result.at(1).contains("FallOffBeginDist"));
    CHECK_FALSE(result.at(1).contains("InstantHitDamageFar"));
    CHECK_FALSE(result.at(8).contains("OverheatReductionRate"));
}

TEST_CASE("Starter catalogue preserves each supplied type ID without generation") {
    auto ids = fixture_ids();
    ids.ability = "abcdef01-2345-6789-abcd-ef0123456789";
    const auto result = starter_catalogue_result(ids, first_profile());
    CHECK(result.at(8).at("GameItemsGuid") == "abcdef01-2345-6789-abcd-ef0123456789");
    CHECK(result.at(0).at("GameItemsGuid") == "10000000-0000-0000-0000-000000000001");
}

TEST_CASE("Starter catalogue rejects absent profiles instead of filling historical guesses") {
    expect_error(fixture_ids(), std::nullopt, StarterCatalogueErrorCode::missing_balance);
}

TEST_CASE("Starter catalogue rejects a default empty balance profile") {
    auto profile = StarterBalanceProfile{};
    profile.version = "own-v1";
    expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
}

TEST_CASE("Starter catalogue rejects invalid canonical IDs in every role") {
    const std::array<std::string, 7> invalid = {"", "0",
        "00000000-0000-0000-0000-000000000000",
        "ABCDEF01-2345-6789-abcd-ef0123456789",
        "10000000000000000000000000000001",
        "10000000_0000-0000-0000-000000000001",
        "10000000-0000-0000-0000-00000000000g"};
    for (std::size_t i = 0; i < 9; ++i) {
        for (const auto& value : invalid) {
            auto ids = fixture_ids();
            *id_fields(ids)[i] = value;
            expect_error(ids, first_profile(), StarterCatalogueErrorCode::invalid_id);
        }
    }
}

TEST_CASE("Starter catalogue rejects duplicate type IDs for every pair") {
    for (std::size_t i = 0; i < 9; ++i) {
        for (std::size_t j = i + 1; j < 9; ++j) {
            auto ids = fixture_ids();
            auto fields = id_fields(ids);
            *fields[j] = *fields[i];
            expect_error(ids, first_profile(), StarterCatalogueErrorCode::duplicate_id);
        }
    }
}

TEST_CASE("Starter catalogue rejects ID whitespace NUL or non-ASCII suffix") {
    for (const auto& value : {std::string("10000000-0000-0000-0000-000000000001 "),
         std::string("10000000-0000-0000-0000-000000000001\0", 37),
         std::string("10000000-0000-0000-0000-00000000000\xc3\xa9")}) {
        auto ids = fixture_ids();
        ids.mech = value;
        expect_error(ids, first_profile(), StarterCatalogueErrorCode::invalid_id);
    }
}

TEST_CASE("Starter catalogue rejects invalid version tokens with fixed errors") {
    const std::array<std::string, 8> invalid = {"", std::string(65, 'a'), "a b", "a/b",
        "a\n", std::string("a\0b", 3), "v\xc3\xa9", "v=1"};
    for (const auto& version : invalid) {
        auto profile = first_profile();
        profile.version = version;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_version);
    }
}

TEST_CASE("Starter catalogue accepts the one and sixty-four byte version boundaries") {
    for (const auto& version : {std::string("1"), std::string(64, 'a')}) {
        auto profile = first_profile();
        profile.version = version;
        const auto result = starter_catalogue_result(fixture_ids(), profile);
        for (const auto& row : result) CHECK(row.at("Version") == version);
    }
}

TEST_CASE("Starter catalogue rejects nonfinite values in all balance fields") {
    for (const float value : {std::numeric_limits<float>::quiet_NaN(),
         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        for (std::size_t i = 0; i < 32; ++i) {
            auto profile = first_profile();
            *balance_fields(profile)[i] = value;
            expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
        }
    }
}

TEST_CASE("Starter catalogue rejects negative values in all balance fields") {
    for (std::size_t i = 0; i < 32; ++i) {
        auto profile = first_profile();
        *balance_fields(profile)[i] = -0.25f;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
    }
}

TEST_CASE("Starter catalogue rejects zero in every strictly positive rule") {
    for (std::size_t i = 0; i < 32; ++i) {
        if (i == 1 || i == 5 || i == 12 || i == 16 || i == 27 || i == 30 || i == 31) continue;
        for (const float value : {0.0f, -0.0f}) {
            auto profile = first_profile();
            *balance_fields(profile)[i] = value;
            expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
        }
    }
}

TEST_CASE("Starter catalogue permits zero spread and zero aim assistance as new policy") {
    auto profile = first_profile();
    profile.primary.spread = 0.0f;
    profile.primary.aim_help_distance = -0.0f;
    profile.secondary.spread = -0.0f;
    profile.secondary.aim_help_distance = 0.0f;
    const auto result = starter_catalogue_result(fixture_ids(), profile);
    CHECK(result.at(1).at("Spread") == "0");
    CHECK(result.at(1).at("AimHelpdist") == "0");
    CHECK(result.at(2).at("Spread") == "0");
    CHECK(result.at(2).at("AimHelpdist") == "0");
}

TEST_CASE("Starter catalogue emits plain decimal float32 that survives a separate parser") {
    const std::array<std::string_view, 11> numeric_keys = {"HeatPerShot", "Spread", "RefireRate",
        "WeaponRange", "InstantHitDamage", "AimHelpdist", "DamageForUI", "RefireRateForUI",
        "HeatForUI", "EffectiveRangeForUI", "AccuracyForUI"};
    for (const float value : {std::numeric_limits<float>::denorm_min(),
         std::numeric_limits<float>::max(), 0.1f}) {
        auto profile = first_profile();
        for (auto* field : balance_fields(profile)) *field = value;
        const auto result = starter_catalogue_result(fixture_ids(), profile);
        for (const auto& key : numeric_keys) {
            for (const std::size_t row : {std::size_t{1}, std::size_t{2}}) {
                const auto text = result.at(row).at(std::string(key)).get<std::string>();
                CHECK(text.find_first_of("eEL,%+ \t\r\n") == std::string::npos);
                float parsed{};
                const auto converted = std::from_chars(text.data(), text.data() + text.size(),
                                                       parsed, std::chars_format::fixed);
                CHECK(converted.ec == std::errc{});
                CHECK(converted.ptr == text.data() + text.size());
                CHECK(parsed == value);
            }
        }
        for (const auto& key : {"RegenerationSeconds", "ModeActiveTime", "HeatReductionRate"}) {
            const auto text = result.at(8).at(key).get<std::string>();
            CHECK(text.find_first_of("eEL,%+ \t\r\n") == std::string::npos);
            float parsed{};
            const auto converted = std::from_chars(text.data(), text.data() + text.size(),
                                                   parsed, std::chars_format::fixed);
            CHECK(converted.ec == std::errc{});
            CHECK(converted.ptr == text.data() + text.size());
            CHECK(parsed == value);
        }
    }
}

namespace {
hawken::inventory::FtuxTest ftux_fixture() {
    return {"40000000-0000-0000-0000-000000000001", "trial.ftux-1", 1, 20};
}
json ftux_section() {
    return {{"GlobalsGuid", "40000000-0000-0000-0000-000000000001"},
        {"Version", "trial.ftux-1"}, {"MatchTimeMinutes", 1}, {"MatchGoal", 20}};
}
void expect_ftux_parse_error(const json& section, StarterCatalogueErrorCode code) {
    bool caught = false;
    try { static_cast<void>(hawken::inventory::parse_ftux_test(section)); }
    catch (const StarterCatalogueError& error) {
        caught = true; CHECK(error.code() == code);
        CHECK(std::string(error.what()) == "Starter catalogue operation failed.");
    }
    CHECK(caught);
}
}
TEST_CASE("Optional FTUX absent retains both complete original nine-record policy oracles") {
    CHECK_FALSE(first_profile().ftux_test);
    CHECK_FALSE(second_profile().ftux_test);
    CHECK(starter_catalogue_result(fixture_ids(), first_profile()) == first_expected());
    CHECK(starter_catalogue_result(fixture_ids(), second_profile()) == second_expected());
}
TEST_CASE("Explicit FTUX adds only the exact unowned Globals row after unchanged nine types") {
    auto profile = first_profile(); profile.ftux_test = ftux_fixture();
    auto expected = first_expected();
    expected.push_back(json::parse(R"({"GameItemsGuid":"40000000-0000-0000-0000-000000000001","ItemName":"Globals","Version":"trial.ftux-1","invType":"41","FTUXMatchTime":"1","FTUXMatchGoal":"20"})"));
    const auto result = starter_catalogue_result(fixture_ids(), profile);
    CHECK(result == expected);
    REQUIRE(result.size() == 10);
    const auto& globals = result.at(9);
    REQUIRE(globals.size() == 6);
    for (const auto* absent : {"Guid", "UserGameItemGuid", "Parent", "Children", "Preset", "index", "supportedMechClasses"})
        CHECK_FALSE(globals.contains(absent));
    for (const auto& [key, value] : globals.items()) { CAPTURE(key); CHECK(value.is_string()); }
}
TEST_CASE("FTUX minute and goal boundaries are positive bounded integer trial policies") {
    for (const auto minutes : {1, 60}) {
        for (const auto goal : {1, 1000}) {
            auto profile = second_profile(); profile.ftux_test = ftux_fixture();
            profile.ftux_test->match_time_minutes = minutes; profile.ftux_test->match_goal = goal;
            const auto result = starter_catalogue_result(fixture_ids(), profile);
            REQUIRE(result.size() == 10);
            CHECK(result.at(9).at("FTUXMatchTime") == std::to_string(minutes));
            CHECK(result.at(9).at("FTUXMatchGoal") == std::to_string(goal));
            for (std::size_t i = 0; i < 9; ++i) CHECK(result.at(i) == second_expected().at(i));
        }
    }
    for (const auto value : {-1, 0, 61, std::numeric_limits<std::int32_t>::max()}) {
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->match_time_minutes = value;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
    }
    for (const auto value : {-1, 0, 1001, std::numeric_limits<std::int32_t>::max()}) {
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->match_goal = value;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_balance);
    }
}
TEST_CASE("FTUX Globals identity is canonical nonzero unique across all nine types") {
    for (const auto* value : {"", "private-invalid", "00000000-0000-0000-0000-000000000000",
         "40000000-0000-0000-0000-00000000000A", "40000000-0000-0000-0000-000000000001/"}) {
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->globals_guid = value;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_id);
    }
    auto ids = fixture_ids();
    for (const auto* field : id_fields(ids)) {
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->globals_guid = *field;
        expect_error(ids, profile, StarterCatalogueErrorCode::duplicate_id);
    }
}
TEST_CASE("FTUX Globals has its own explicit bounded version token") {
    for (const auto& value : {std::string{}, std::string(65, 'v'), std::string("private version"),
         std::string("bad/option"), std::string("bad\nversion"), std::string("\xc3\xa9")}) {
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->version = value;
        expect_error(fixture_ids(), profile, StarterCatalogueErrorCode::invalid_version);
    }
    auto profile = first_profile(); profile.ftux_test = ftux_fixture();
    profile.ftux_test->version = std::string(64, 'v');
    const auto result = starter_catalogue_result(fixture_ids(), profile);
    REQUIRE(result.size() == 10); CHECK(result.at(9).at("Version") == std::string(64, 'v'));
    CHECK(result.at(0).at("Version") == "starter.test-1");
}
TEST_CASE("FTUX section parser reads the explicit four-field integer DOM only") {
    const auto parsed = hawken::inventory::parse_ftux_test(ftux_section());
    CHECK(parsed.globals_guid == "40000000-0000-0000-0000-000000000001");
    CHECK(parsed.version == "trial.ftux-1"); CHECK(parsed.match_time_minutes == 1); CHECK(parsed.match_goal == 20);
    CHECK_FALSE(parsed.enable_new_user_dialog.has_value());
    for (const auto* key : {"GlobalsGuid", "Version", "MatchTimeMinutes", "MatchGoal"}) {
        auto section = ftux_section(); section.erase(key);
        expect_ftux_parse_error(section, StarterCatalogueErrorCode::invalid_balance);
    }
    auto extra = ftux_section(); extra["Unknown"] = "private-value";
    expect_ftux_parse_error(extra, StarterCatalogueErrorCode::invalid_balance);
    for (const json& bad : {json(nullptr), json::array(), json("private-value")})
        expect_ftux_parse_error(bad, StarterCatalogueErrorCode::invalid_balance);
}
TEST_CASE("FTUX section parser rejects coercion fractions and integer overflow") {
    for (const auto* field : {"MatchTimeMinutes", "MatchGoal"}) {
        for (const json& bad : {json(nullptr), json(true), json("1"), json(1.0), json(0), json(-1),
             json(std::numeric_limits<std::uint64_t>::max()), json::array(), json::object()}) {
            auto section = ftux_section(); section[field] = bad;
            expect_ftux_parse_error(section, StarterCatalogueErrorCode::invalid_balance);
        }
    }
    for (const auto* field : {"GlobalsGuid", "Version"}) {
        for (const json& bad : {json(nullptr), json(true), json(1), json::array(), json::object()}) {
            auto section = ftux_section(); section[field] = bad;
            expect_ftux_parse_error(section, StarterCatalogueErrorCode::invalid_balance);
        }
    }
    auto minutes = ftux_section(); minutes["MatchTimeMinutes"] = 61;
    expect_ftux_parse_error(minutes, StarterCatalogueErrorCode::invalid_balance);
    auto goal = ftux_section(); goal["MatchGoal"] = 1001;
    expect_ftux_parse_error(goal, StarterCatalogueErrorCode::invalid_balance);
    auto identity = ftux_section(); identity["GlobalsGuid"] = "private-invalid";
    expect_ftux_parse_error(identity, StarterCatalogueErrorCode::invalid_id);
    auto version = ftux_section(); version["Version"] = "private version";
    expect_ftux_parse_error(version, StarterCatalogueErrorCode::invalid_version);
}

TEST_CASE("FTUX original menu choice emits only an optional boolean string in Globals") {
    for (const bool enabled : {true, false}) {
        CAPTURE(enabled);
        auto profile = first_profile(); profile.ftux_test = ftux_fixture();
        profile.ftux_test->enable_new_user_dialog = enabled;
        auto expected = first_expected();
        expected.push_back(enabled
            ? json::parse(R"({"GameItemsGuid":"40000000-0000-0000-0000-000000000001","ItemName":"Globals","Version":"trial.ftux-1","invType":"41","FTUXMatchTime":"1","FTUXMatchGoal":"20","EnableNewUserDialog":"true"})")
            : json::parse(R"({"GameItemsGuid":"40000000-0000-0000-0000-000000000001","ItemName":"Globals","Version":"trial.ftux-1","invType":"41","FTUXMatchTime":"1","FTUXMatchGoal":"20","EnableNewUserDialog":"false"})"));
        CHECK(starter_catalogue_result(fixture_ids(), profile) == expected);
    }
}

TEST_CASE("FTUX parser retains both explicit original menu choices without defaulting absence") {
    CHECK_FALSE(hawken::inventory::parse_ftux_test(ftux_section()).enable_new_user_dialog);
    for (const bool enabled : {true, false}) {
        CAPTURE(enabled);
        auto section = ftux_section(); section["EnableNewUserDialog"] = enabled;
        hawken::inventory::FtuxTest parsed;
        CHECK_NOTHROW(parsed = hawken::inventory::parse_ftux_test(section));
        REQUIRE(parsed.enable_new_user_dialog.has_value());
        CHECK(parsed.enable_new_user_dialog.value() == enabled);
        CHECK(parsed.globals_guid == "40000000-0000-0000-0000-000000000001");
        CHECK(parsed.version == "trial.ftux-1");
        CHECK(parsed.match_time_minutes == 1); CHECK(parsed.match_goal == 20);
    }
}

TEST_CASE("FTUX optional menu choice rejects coercion and cannot hide unknown or missing fields") {
    for (const json& invalid : {json(nullptr), json(0), json(1), json(1.0), json("true"),
         json("false"), json::array(), json::object()}) {
        auto section = ftux_section(); section["EnableNewUserDialog"] = invalid;
        expect_ftux_parse_error(section, StarterCatalogueErrorCode::invalid_balance);
    }
    auto extra = ftux_section(); extra["EnableNewUserDialog"] = true; extra["Unknown"] = false;
    expect_ftux_parse_error(extra, StarterCatalogueErrorCode::invalid_balance);
    for (const auto* key : {"GlobalsGuid", "Version", "MatchTimeMinutes", "MatchGoal"}) {
        auto missing = ftux_section(); missing.erase(key); missing["EnableNewUserDialog"] = false;
        expect_ftux_parse_error(missing, StarterCatalogueErrorCode::invalid_balance);
    }
    auto wrong_key = ftux_section(); wrong_key["enableNewUserDialog"] = true;
    expect_ftux_parse_error(wrong_key, StarterCatalogueErrorCode::invalid_balance);
}
