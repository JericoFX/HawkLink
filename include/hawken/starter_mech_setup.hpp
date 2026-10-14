#pragma once
#include "hawken/local_store.hpp"
#include "hawken/mech_setup_wire.hpp"
#include "hawken/starter_catalogue.hpp"
#include <cstdint>
#include <optional>

namespace hawken::inventory {
struct StarterMechSelection {
    std::int32_t requested_primary_index;
    std::uint8_t holo_taunt_index;
    std::uint8_t previous_setup_id;
    std::optional<wire::object::PackageExportReference> prior_template_reference;
    bool loaded_completion;
};

// Pure finite nine-role materializer. The caller must first establish account,
// complete grant, ownership and catalogue identity through the store/service.
// effective.attributes must already contain the HTTP equipment baseline plus
// persisted overrides, not only raw SQL attributes. Required eight role indices
// and instance references are validated here; this function never grants items
// or verifies cosmetic entitlements. Cosmetic integrity is a caller premise.
//
// Only the configured B/Rookie15 starter is supported: primary0, secondary1,
// ability3, four parts10, drone0, no owned internals. Missing optional numeric
// style fields use original defaults. Present empty values convert to zero.
// Known ASCII keys compare without case; conflicting aliases, nonfinite values,
// int32 overflow and invalid role/lifecycle data fail closed as local policy.
// Completed nonlocal allocation/upgrade normalization yields zero flags/points.
// Primary, Holo, prior revision and human TemplateNone are explicit caller state.
// Output follows Loaded/SetReady formulas; it proves no native callback, fieldID,
// PackageMap eligibility, actor reception, admission, possession or gameplay.
// Throws std::invalid_argument with fixed message "Invalid starter mech setup."
// No partial result or input/storage mutation is returned on failure.
wire::mech::SMechSetupStruct materialize_starter_mech(
    const store::StarterMechSnapshot& effective,
    const StarterCatalogueIds& expected,
    const StarterMechSelection& selection);
}
