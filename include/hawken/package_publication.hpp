#pragma once
#include "hawken/posthello_control.hpp"

namespace hawken::control::packages {
struct RootMetadata {
    std::string name;
    std::array<std::uint32_t,4> identity;
    std::vector<std::uint32_t> generation_net_counts;
};
struct ContainedMetadata {
    RootMetadata package;
    std::string container_name;
};

// Materialize ONE explicitly supplied root-local Package7, not a package list.
// Caller provides the real root name/GUID/generation metadata and is responsible
// for local resolution/identity; this pure function does no I/O or verification.
// No contained/ForcedExport inference, map/world selection, download or admission.
//
// Own policy: name nonempty ASCII A-Z/a-z/0-9/_/-, not None (case insensitive);
// identity not all zero; generation count1..INT32_MAX. Netcounts are retained in
// caller metadata exactly, without monotonicity assumptions; value1 is their
// COUNT, never a netcount or export_count. Nothing mutates the metadata.
// name0 narrow name+NUL; name1/name3 narrow ONE NUL; text2 physical zero FString;
// value0/flags0. The explicit None representation is proved by9ea730->a4590
// ->c5f70->cec50 empty branch, see evidence/package-root-policy/contract.json.
// These choices are supported on the locally resolvable root/GUID path only.
//
// Existing posthello::Limits bound this message's string units/archive bytes;
// returned Package can be encoded in either scalar endian. Fixed posthello::Error
// codes invalid_limits, invalid_message, resource_limit; no input in diagnostics.
// not_implemented is reserved for this module's RED scaffold.
posthello::Package make_root_package(const RootMetadata& metadata,
                                     posthello::Limits limits);

// Explicit NEW local-only trial policy, not a historical host descriptor or
// verified runtime linker association. The caller supplies the contained
// export's name/GUID/generations, not the physical container header identity.
// Root validation applies to package and container names; they must differ
// ASCII-case-insensitively. Check combined archive/string quotas before allocation.
// name0 is package.name+NUL, name3 is container_name+NUL, both narrow; name1
// narrow ONE NUL, text2 zero, value0=0 (no download), flags=0, value1 generation
// COUNT. Unknown historical name1/text2/flags and export package-flags word
// 0x20200001 are not copied or claimed required. Client local resolution and
// ACK/Join remain unverified. No I/O, aliases, downloads, world or admission.
// Uses the same fixed posthello::Error codes and explicit Limits as above.
posthello::Package make_contained_package(const ContainedMetadata& metadata,
                                          posthello::Limits limits);
}
