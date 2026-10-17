#pragma once
#include "hawken/object_reference_wire.hpp"

namespace hawken::wire::rpc {
struct InventoryRpcPayload {
    std::vector<std::uint8_t> payload;
    std::size_t logical_bits;
};

// One original Inventory.ClientGivenTo(Pawn NewOwner, bool bDoNotActivate)
// RPC: bounded fieldID, eligible non-Bool ObjectProperty presence, PRIMARY
// actor reference only when present, then one eligible BoolNET value bit.
// BoolNET has no presence bit, including false. Both parms are ArrayDim1;
// no parameter index byte, alignment, terminator or integer archive order.
// Caller proves both parameter eligibilities, exact effective field/cache,
// PRIMARY map and Pawn class resolution. No actor creation, lookup, inventory
// activation, ownership authorization or successful gameplay is inferred.
//
// pawn_channel0 represents a null NewOwner: native pointer comparator emits
// presence0 and omits the reference. Channels1..2047 emit presence1+12bit ref;
// no package/CDO reference is accepted through this channel-only interface.
// Null encoding does not claim that inventory activation with null succeeds.
// Positive bound/global budget and fieldID<bound required; exact aggregate
// bitcount must fit. Owned output, zero unused high bits, no partial result.
// Native common sender688262..688338 and ObjectProperty f34d0/f8fd0 proofs
// retained in controller-rpc-sender/contract.json; exact ClientGivenTo flags80
// NewOwner/BoolNET metadata in native-followup/gameplay-diagnosis.json.
// Fixed WireError: invalid_limit, invalid_value, field_too_large, invalid_length.
InventoryRpcPayload encode_client_given_to(std::uint32_t field_id,std::uint32_t exclusive_bound,
    std::uint16_t pawn_channel,bool do_not_activate,std::size_t max_rpc_bits);

struct ServerSetCurrentWeaponArguments {
    object::Reference desired_weapon;
    std::size_t consumed_bits;
};
// ONE eligible ObjectProperty80/ArrayDim1 ExpectedClass Engine.Weapon argument.
// Caller already consumed the configured Manager RPC tag (conditional23/b24)
// and proves exact cache/PRIMARY map/eligibility and authority. No lookup or
// activation. Presence0 leaves native-zero storage: ActorChannelReference0;
// presence1 preserves PRIMARY package/actor variant, including package index0.
// No Bool, array-index byte, archive order, alignment or suffix scan.
// Positive budget includes presence and complete reference. Copy cursor and
// commit only on success; all errors preserve caller position, tail untouched.
// Fixed errors invalid_limit, field_too_large, truncated.
ServerSetCurrentWeaponArguments read_server_set_current_weapon_arguments(
    BitReader& reader,std::size_t max_argument_bits);
}
