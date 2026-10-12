#pragma once
#include <nlohmann/json.hpp>
#include <limits>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace hawken::inventory {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

struct StarterCatalogueIds {
    std::string mech;
    std::string primary;
    std::string secondary;
    std::string upper;
    std::string middle;
    std::string arms;
    std::string lower;
    std::string drone;
    std::string ability;
};

struct WeaponBalance {
    float heat_per_shot{};
    float spread{};
    float refire_rate{};
    float weapon_range{};
    float instant_hit_damage{};
    float aim_help_distance{};
    float damage_for_ui{};
    float refire_rate_for_ui{};
    float heat_for_ui{};
    float effective_range_for_ui{};
    float accuracy_for_ui{};
};

struct CoolantBalance {
    float regeneration_seconds{};
    float mode_active_time{};
    float heat_reduction_rate{};
};

struct ProjectileBalance {
    float speed{};
    float max_speed{};
    float acceleration{};
    float damage{};
    float damage_radius{};
    float inner_damage_radius{};
    float min_damage{};
};

// Explicit NEW trial policy; original FTUX reads minutes/team kill threshold.
// No zero/unlimited ending mode and no consent, reward or stats completion claim.
struct FtuxTest {
    std::string globals_guid;
    std::string version;
    std::int32_t match_time_minutes{};
    std::int32_t match_goal{};
};

struct StarterBalanceProfile {
    std::string version;
    WeaponBalance primary;
    WeaponBalance secondary;
    CoolantBalance coolant;
    ProjectileBalance secondary_projectile;
    std::optional<FtuxTest> ftux_test = std::nullopt;
};

enum class StarterCatalogueErrorCode {
    invalid_id, duplicate_id, missing_balance, invalid_version, invalid_balance
};

class StarterCatalogueError : public std::runtime_error {
public:
    explicit StarterCatalogueError(StarterCatalogueErrorCode code);
    StarterCatalogueErrorCode code() const noexcept;
private:
    StarterCatalogueErrorCode code_;
};

// Parse a strict FtuxTest DOM section with exactly GlobalsGuid, Version,
// MatchTimeMinutes and MatchGoal. A DOM cannot retain duplicate raw keys;
// this helper adds no raw-JSON duplicate-key rejection policy.
// New policy: minutes integer1..60, goal integer1..1000; canonical nonzero GUID
// and explicit Version token. Errors are fixed StarterCatalogueError messages.
FtuxTest parse_ftux_test(const nlohmann::json& section);

// Pure builder of nine TYPE records, optionally followed by one Globals row,
// for the Meteor envelope Result array. Without FtuxTest the nine are identical.
// IDs are supplied/persisted by the caller: canonical lower-case UUID36,
// nonzero and all distinct. Selectors/order agree with the separate Starter
// instance graph. This function does not establish ownership or entitlements.
// Balance/version are NEW per-server policy, never recovered historical values.
// Version is an ASCII [A-Za-z0-9_.-] token of 1..64 bytes; profile is mandatory.
// Every float must be finite. Spread/AimHelpdist and projectile acceleration,
// inner radius/minimum damage allow zero. Other values must be >0. Projectile
// max speed >= speed, inner radius <= radius, minimum damage <= damage.
// These restrictions are our rules, not SDK parser limits.
// Refire/coolant timers use chosen seconds, distances use engine units; the
// five UI metrics have caller-chosen scales, not inferred historical ratios.
// Output numeric attributes are plain decimal base strings, locale-independent
// and round-trip float32, without exponents, levels, percentages or metadata.
// Four exact class sheet names and original indices are reused. Parts/drone
// labels, Version and children's AllMechB compatibility are NEW policy.
// No IID/Parent/Children/Preset/ReadOnly fields, optional type links, global
// definition rows other than the opt-in Globals, auth, grants, SQL,
// auto-selection or generation of any ID. Globals uses its own unique caller
// GUID/version and string invType41/FTUXMatchTime/FTUXMatchGoal; its GUID must
// differ from all nine type IDs. It is never an owned item or preset child.
// Secondary projectile fields use the original Proj.* keys, independently of
// instant-hit and UI damage. Other missing weapon properties use setter fallbacks;
// mech uses original class defaults. Neither implies gameplay acceptance.
nlohmann::json starter_catalogue_result(
    const StarterCatalogueIds& ids,
    const std::optional<StarterBalanceProfile>& balance);
}
