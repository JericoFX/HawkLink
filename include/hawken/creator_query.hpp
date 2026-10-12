#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hawken::creator {
using QueryBind = std::variant<std::monostate, std::int64_t, std::string>;
using QueryCell = std::variant<std::monostate, std::int64_t, double, std::string>;

struct ReadColumn {
    std::string table;
    std::string column;
};

struct QueryLimits {
    std::size_t max_sql_bytes = 16384;
    std::size_t max_rows = 100;
    std::size_t max_result_bytes = 262144;
    std::uint32_t progress_callbacks = 1000;
    std::chrono::milliseconds timeout{100};
};

struct QueryPolicy {
    std::vector<ReadColumn> columns;
    QueryLimits limits;
};

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<QueryCell>> rows;
};

enum class QueryErrorCode {
    not_implemented,
    invalid_limits,
    invalid_policy,
    invalid_sql,
    unauthorized,
    bind_mismatch,
    bind_too_large,
    database_unavailable,
    execution_failed,
    resource_limit,
    deadline_exceeded
};

class QueryError : public std::runtime_error {
public:
    explicit QueryError(QueryErrorCode error_code);
    QueryErrorCode code;
};

// The caller supplies a trusted path and policy after checking query.read.
// The trusted schema must have no empty column names: SQLite cannot distinguish
// an actual column named "" from its synthetic row-only READ event.
// Grants cover every row of main and must include base and view columns.
// An empty column grants row-only/count access without a real column already
// authorized for that same table in this query. A permitted column predicate
// can authorize COUNT(*) too: this is column access, not SQL-shape permission.
// Synthetic empty-column READ may omit main; real columns always require main.
// Context/view/alias names grant nothing, nor does an unused column grant.
// At most 256 grants; ASCII identifier names <=128 bytes (empty column allowed).
// sqlite_* tables stay forbidden even if an erroneous policy names them.
// Exactly one effective non-EXPLAIN statement; SQLite may ignore empty ';'
// statements in the tail, which is fully checked before stepping the first one.
// Positive limits may not exceed 1MiB SQL, 10000 rows, 8MiB result,
// 100000 progress callbacks or 5000ms. Total bound text <= max_sql_bytes.
// Result bytes charge column names and text verbatim, integers/doubles 8, null 0.
// This is a cooperative execution deadline, not a hard I/O or heap ceiling.
QueryResult read_creator_query(const std::filesystem::path& trusted_database,
                               const QueryPolicy& trusted_policy,
                               std::string_view sql,
                               std::span<const QueryBind> binds = {});
}
