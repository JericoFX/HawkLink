#include "hawken/starter_catalogue.hpp"
#include <array>
#include <charconv>
#include <cmath>
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

bool valid_version(std::string_view version) {
    if (version.empty() || version.size() > 64) return false;
    for (const char c : version) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

void validate_ftux(const FtuxTest& trial) {
    if (!valid_id(trial.globals_guid)) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_id);
    }
    if (!valid_version(trial.version)) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_version);
    }
    if (trial.match_time_minutes < 1 || trial.match_time_minutes > 60 ||
        trial.match_goal < 1 || trial.match_goal > 1000) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    }
}

void require_number(float value, bool allow_zero = false) {
    if (!std::isfinite(value) || (allow_zero ? value < 0.0f : value <= 0.0f)) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    }
}

void validate_weapon(const WeaponBalance& weapon) {
    for (const float value : {weapon.heat_per_shot, weapon.refire_rate,
         weapon.weapon_range, weapon.instant_hit_damage, weapon.damage_for_ui,
         weapon.refire_rate_for_ui, weapon.heat_for_ui, weapon.effective_range_for_ui,
         weapon.accuracy_for_ui}) {
        require_number(value);
    }
    require_number(weapon.spread, true);
    require_number(weapon.aim_help_distance, true);
}

std::string decimal(float value) {
    if (value == 0.0f) return "0"; // Normalize permitted positive/negative zero.
    // Float32's fixed shortest decimal fits this buffer even at either extreme.
    std::array<char, 128> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                         value, std::chars_format::fixed);
    if (converted.ec != std::errc{}) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    }
    return std::string(buffer.data(), converted.ptr);
}

void add_weapon_properties(nlohmann::json& row, const WeaponBalance& weapon) {
    row.update({
        {"HeatPerShot", decimal(weapon.heat_per_shot)},
        {"Spread", decimal(weapon.spread)},
        {"RefireRate", decimal(weapon.refire_rate)},
        {"WeaponRange", decimal(weapon.weapon_range)},
        {"InstantHitDamage", decimal(weapon.instant_hit_damage)},
        {"AimHelpdist", decimal(weapon.aim_help_distance)},
        {"DamageForUI", decimal(weapon.damage_for_ui)},
        {"RefireRateForUI", decimal(weapon.refire_rate_for_ui)},
        {"HeatForUI", decimal(weapon.heat_for_ui)},
        {"EffectiveRangeForUI", decimal(weapon.effective_range_for_ui)},
        {"AccuracyForUI", decimal(weapon.accuracy_for_ui)}});
}
}

StarterCatalogueError::StarterCatalogueError(StarterCatalogueErrorCode code)
    : std::runtime_error("Starter catalogue operation failed."), code_(code) {}

StarterCatalogueErrorCode StarterCatalogueError::code() const noexcept {
    return code_;
}

FtuxTest parse_ftux_test(const nlohmann::json& section) {
    if (!section.is_object() || section.size() != 4 ||
        !section.contains("GlobalsGuid") || !section.at("GlobalsGuid").is_string() ||
        !section.contains("Version") || !section.at("Version").is_string() ||
        !section.contains("MatchTimeMinutes") || !section.contains("MatchGoal")) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    }
    const auto bounded_integer = [](const nlohmann::json& value, std::int32_t maximum) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number >= 1 && number <= static_cast<std::uint64_t>(maximum))
                return static_cast<std::int32_t>(number);
        } else if (value.is_number_integer()) {
            const auto number = value.get<std::int64_t>();
            if (number >= 1 && number <= maximum) return static_cast<std::int32_t>(number);
        }
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    };
    FtuxTest trial{section.at("GlobalsGuid").get<std::string>(),
        section.at("Version").get<std::string>(),
        bounded_integer(section.at("MatchTimeMinutes"), 60),
        bounded_integer(section.at("MatchGoal"), 1000)};
    validate_ftux(trial);
    return trial;
}

nlohmann::json starter_catalogue_result(
    const StarterCatalogueIds& ids,
    const std::optional<StarterBalanceProfile>& balance) {
    const std::array<std::string_view, 9> type_ids = {
        ids.mech, ids.primary, ids.secondary, ids.upper, ids.middle,
        ids.arms, ids.lower, ids.drone, ids.ability};
    for (const auto id : type_ids) {
        if (!valid_id(id)) throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_id);
    }
    for (std::size_t i = 0; i < type_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < type_ids.size(); ++j) {
            if (type_ids[i] == type_ids[j]) {
                throw StarterCatalogueError(StarterCatalogueErrorCode::duplicate_id);
            }
        }
    }
    if (!balance) throw StarterCatalogueError(StarterCatalogueErrorCode::missing_balance);
    if (!valid_version(balance->version)) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_version);
    }
    validate_weapon(balance->primary);
    validate_weapon(balance->secondary);
    require_number(balance->coolant.regeneration_seconds);
    require_number(balance->coolant.mode_active_time);
    require_number(balance->coolant.heat_reduction_rate);
    const auto& projectile = balance->secondary_projectile;
    for (const float value : {projectile.speed, projectile.max_speed,
             projectile.damage, projectile.damage_radius}) require_number(value);
    for (const float value : {projectile.acceleration, projectile.inner_damage_radius,
             projectile.min_damage}) require_number(value, true);
    if (projectile.max_speed < projectile.speed ||
        projectile.inner_damage_radius > projectile.damage_radius ||
        projectile.min_damage > projectile.damage) {
        throw StarterCatalogueError(StarterCatalogueErrorCode::invalid_balance);
    }

    if (balance->ftux_test) {
        validate_ftux(*balance->ftux_test);
        for (const auto id : type_ids) {
            if (id == balance->ftux_test->globals_guid) {
                throw StarterCatalogueError(StarterCatalogueErrorCode::duplicate_id);
            }
        }
    }

    const std::array<std::string_view, 9> names = {
        "Mech B - Rookie", "Weap Prim - Point-D Vulcan", "Weap Sec - TOW Rocket",
        "FRED UPPER", "FRED MIDDLE", "FRED ARMS", "FRED LOWER", "RD-TR1", "Ability - Coolant"};
    const std::array<std::string_view, 9> inventory_types = {
        "0", "20", "21", "8", "9", "10", "11", "33", "34"};
    const std::array<std::string_view, 9> indices = {
        "15", "0", "1", "10", "10", "10", "10", "0", "3"};
    auto result = nlohmann::json::array();
    for (std::size_t i = 0; i < type_ids.size(); ++i) {
        result.push_back({
            {"GameItemsGuid", std::string(type_ids[i])},
            {"ItemName", std::string(names[i])},
            {"Version", balance->version},
            {"invType", std::string(inventory_types[i])},
            {"index", std::string(indices[i])}});
        if (i != 0) result.back()["supportedMechClasses"] = "AllMechB";
    }
    add_weapon_properties(result.at(1), balance->primary);
    add_weapon_properties(result.at(2), balance->secondary);
    result.at(2).update({
        {"Proj.Speed", decimal(projectile.speed)},
        {"Proj.MaxSpeed", decimal(projectile.max_speed)},
        {"Proj.AccelRate", decimal(projectile.acceleration)},
        {"Proj.Damage", decimal(projectile.damage)},
        {"Proj.DamageRadius", decimal(projectile.damage_radius)},
        {"Proj.InnerDamageRadius", decimal(projectile.inner_damage_radius)},
        {"Proj.MinDamage", decimal(projectile.min_damage)}});
    result.at(8).update({
        {"RegenerationSeconds", decimal(balance->coolant.regeneration_seconds)},
        {"ModeActiveTime", decimal(balance->coolant.mode_active_time)},
        {"HeatReductionRate", decimal(balance->coolant.heat_reduction_rate)}});
    if (balance->ftux_test) {
        const auto& trial = *balance->ftux_test;
        result.push_back({{"GameItemsGuid", trial.globals_guid}, {"ItemName", "Globals"},
            {"Version", trial.version}, {"invType", "41"},
            {"FTUXMatchTime", std::to_string(trial.match_time_minutes)},
            {"FTUXMatchGoal", std::to_string(trial.match_goal)}});
    }
    return result;
}
}
