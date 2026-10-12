#pragma once
#include <string_view>

namespace hawken::http {
struct RequestDiagnostic {
    std::string_view category;
    std::string_view method;
};

// Returned views refer exclusively to constant labels, never input storage.
// Supply the HTTP method and parsed path (without query). Exact, case-sensitive
// matches only. A single missing/extra final slash on a known route gets its
// own label; this does not normalize the request or change its response.
// Known sync resource prefixes get fixed family labels; dynamic suffixes never
// appear in returned views. Families do not validate IDs or enable API routes.
// Every other path/method maps to "other"; raw query/fragment/encoded/NUL paths
// are not classified. The caller already supplies a parsed path without query.
RequestDiagnostic classify_request(std::string_view method, std::string_view path) noexcept;
}
