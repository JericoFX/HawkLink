#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace hawken::inventory {
struct ItemIds {
    std::string type_id;
    std::string instance_id;
};

struct StarterInventoryIds {
    ItemIds mech;
    ItemIds primary;
    ItemIds secondary;
    ItemIds upper;
    ItemIds middle;
    ItemIds arms;
    ItemIds lower;
    ItemIds drone;
    ItemIds ability;
};

enum class StarterInventoryErrorCode { invalid_id, duplicate_id };

class StarterInventoryError : public std::runtime_error {
public:
    explicit StarterInventoryError(StarterInventoryErrorCode code);
    StarterInventoryErrorCode code() const noexcept;
private:
    StarterInventoryErrorCode code_;
};

// A NEW per-server starter policy, not a recovered historical Recruit bundle.
// The trusted caller supplies 18 real, persisted IDs and binds the named type
// IDs to the original selectors: mech inv0/index15; primary20/0; secondary21/1;
// B upper8/middle9/arms10/lower11 at index10; drone33/0; ability34/3.
// This builder cannot establish catalogue membership, entitlement or account
// ownership. It never creates IDs, persists/grants items or selects the preset.
// ID policy: nonzero canonical lower-case 8-4-4-4-12 hex strings, all 18 distinct.
// This is our input policy; the historical SDK parser accepts general strings.
// Returns only the nine GameItem instance objects for the envelope Result array.
// Parent/default/equipped references use INSTANCE IDs, not catalogue type IDs.
// Optional item/internal slots are intentionally empty. GarageOrder="0" is
// initial server policy using a confirmed original key. No names, versions,
// property-sheet values, supportedMechClasses, Steam identities or grants.
nlohmann::json starter_inventory_result(const StarterInventoryIds& ids);
}
