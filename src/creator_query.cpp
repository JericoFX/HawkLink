#include "hawken/creator_query.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <utility>

namespace hawken::creator {
namespace {
constexpr int progress_interval = 1000;
constexpr int internal_value_bytes = 16 * 1024 * 1024;
constexpr int internal_sql_floor = 64 * 1024;
constexpr int internal_columns = 256;
constexpr int internal_parameters = 1024;

[[noreturn]] void fail(QueryErrorCode code) { throw QueryError(code); }

bool ascii_letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool identifier(std::string_view name, bool allow_empty) noexcept {
    if (name.empty()) return allow_empty;
    if (name.size() > 128 || (!ascii_letter(name.front()) && name.front() != '_')) return false;
    for (const auto value : name) {
        if (!ascii_letter(value) && !(value >= '0' && value <= '9') && value != '_') return false;
    }
    return true;
}

char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool reserved_table(const char* table) noexcept {
    if (table == nullptr) return true;
    constexpr std::string_view prefix = "sqlite_";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (table[i] == '\0' || ascii_lower(table[i]) != prefix[i]) return false;
    }
    return true;
}

void validate(const QueryPolicy& policy) {
    const auto& limits = policy.limits;
    if (limits.max_sql_bytes == 0 || limits.max_sql_bytes > 1048576 ||
        limits.max_rows == 0 || limits.max_rows > 10000 ||
        limits.max_result_bytes == 0 || limits.max_result_bytes > 8388608 ||
        limits.progress_callbacks == 0 || limits.progress_callbacks > 100000 ||
        limits.timeout.count() <= 0 || limits.timeout.count() > 5000)
        fail(QueryErrorCode::invalid_limits);
    if (policy.columns.size() > 256) fail(QueryErrorCode::invalid_policy);
    for (const auto& grant : policy.columns) {
        if (!identifier(grant.table, false) || !identifier(grant.column, true))
            fail(QueryErrorCode::invalid_policy);
    }
}

struct QueryContext {
    const QueryPolicy& policy;
    std::chrono::steady_clock::time_point deadline;
    std::uint32_t callbacks = 0;
    bool denied = false;
    bool quota_exceeded = false;
    bool clock_exceeded = false;
    std::array<bool, 256> observed_columns{};
};

int authorize(void* opaque, int action, const char* first, const char* second,
              const char* database, const char*) noexcept {
    if (opaque == nullptr) return SQLITE_DENY;
    auto& context = *static_cast<QueryContext*>(opaque);
    try {
        if (action == SQLITE_SELECT) return SQLITE_OK;
        if (action == SQLITE_READ && first != nullptr && second != nullptr && !reserved_table(first)) {
            const auto synthetic = second[0] == '\0';
            const auto main = database != nullptr && sqlite3_stricmp(database, "main") == 0;
            // SQLite3.54 also emits READ(table,"",NULL) for unqualified
            // row-only/IPK sources. Only that empty-column event may omit main.
            // This private connection admits no other database or source grant.
            if (main || (synthetic && database == nullptr)) {
                for (std::size_t i = 0; i < context.policy.columns.size(); ++i) {
                    const auto& grant = context.policy.columns[i];
                    if (sqlite3_stricmp(first, grant.table.c_str()) != 0) continue;
                    if (synthetic) {
                        if (grant.column.empty() || context.observed_columns[i]) return SQLITE_OK;
                    } else if (sqlite3_stricmp(second, grant.column.c_str()) == 0) {
                        context.observed_columns[i] = true;
                        return SQLITE_OK;
                    }
                }
            }
        } else if (action == SQLITE_FUNCTION && second != nullptr) {
            for (const auto* function : {"count", "sum", "min", "max", "avg", "coalesce",
                "lower", "upper", "length", "abs"}) {
                if (sqlite3_stricmp(second, function) == 0) return SQLITE_OK;
            }
        }
    } catch (...) {
        // No C++ exception may cross SQLite's C callback boundary.
    }
    context.denied = true;
    return SQLITE_DENY;
}

int progress(void* opaque) noexcept {
    if (opaque == nullptr) return 1;
    auto& context = *static_cast<QueryContext*>(opaque);
    try {
        if (std::chrono::steady_clock::now() >= context.deadline) {
            context.clock_exceeded = true;
            return 1;
        }
        if (context.callbacks >= context.policy.limits.progress_callbacks) {
            context.quota_exceeded = true;
            return 1;
        }
        ++context.callbacks;
        return 0;
    } catch (...) {
        context.quota_exceeded = true;
        return 1;
    }
}

void checkpoint(QueryContext& context) {
    if (context.clock_exceeded || std::chrono::steady_clock::now() >= context.deadline)
        fail(QueryErrorCode::deadline_exceeded);
    if (context.quota_exceeded) fail(QueryErrorCode::resource_limit);
}

struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept { sqlite3_close_v2(database); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const noexcept { sqlite3_finalize(statement); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

void sqlite_failure(int code, QueryContext& context, QueryErrorCode fallback) {
    checkpoint(context);
    if (context.denied || (code & 0xff) == SQLITE_AUTH) fail(QueryErrorCode::unauthorized);
    switch (code & 0xff) {
    case SQLITE_NOMEM:
    case SQLITE_TOOBIG:
    case SQLITE_INTERRUPT:
        fail(QueryErrorCode::resource_limit);
    case SQLITE_CANTOPEN:
    case SQLITE_NOTADB:
    case SQLITE_CORRUPT:
    case SQLITE_IOERR:
        fail(QueryErrorCode::database_unavailable);
    default:
        fail(fallback);
    }
}

std::string database_filename(const std::filesystem::path& path) {
    try {
        if (path.empty()) fail(QueryErrorCode::database_unavailable);
        // An absolute filesystem path cannot be interpreted as a file: URI.
        const auto encoded = std::filesystem::absolute(path).u8string();
        std::string result(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        if (result.find('\0') != std::string::npos) fail(QueryErrorCode::database_unavailable);
        return result;
    } catch (const QueryError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail(QueryErrorCode::database_unavailable);
    }
}

void configure(sqlite3* database) {
    if (sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) != SQLITE_OK ||
        sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK ||
        sqlite3_busy_timeout(database, 0) != SQLITE_OK)
        fail(QueryErrorCode::execution_failed);
    // Engine limits complement application quotas. Schema loading must not
    // inherit a result budget of e.g. 13 bytes or a short query's byte limit.
    sqlite3_limit(database, SQLITE_LIMIT_LENGTH, internal_value_bytes);
    sqlite3_limit(database, SQLITE_LIMIT_COLUMN, internal_columns);
    sqlite3_limit(database, SQLITE_LIMIT_EXPR_DEPTH, 100);
    sqlite3_limit(database, SQLITE_LIMIT_COMPOUND_SELECT, 32);
    sqlite3_limit(database, SQLITE_LIMIT_VDBE_OP, 100000);
    sqlite3_limit(database, SQLITE_LIMIT_FUNCTION_ARG, 32);
    sqlite3_limit(database, SQLITE_LIMIT_VARIABLE_NUMBER, internal_parameters);
    sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_limit(database, SQLITE_LIMIT_TRIGGER_DEPTH, 0);
    sqlite3_limit(database, SQLITE_LIMIT_WORKER_THREADS, 0);
}

Statement prepare(sqlite3* database, QueryContext& context, const char* begin, int length,
                  const char** tail) {
    checkpoint(context);
    sqlite3_stmt* raw = nullptr;
    const auto code = sqlite3_prepare_v3(database, begin, length,
        SQLITE_PREPARE_NO_VTAB | SQLITE_PREPARE_DONT_LOG, &raw, tail);
    Statement statement(raw);
    if (code != SQLITE_OK) sqlite_failure(code, context, QueryErrorCode::invalid_sql);
    checkpoint(context);
    return statement;
}

void charge(std::size_t bytes, std::size_t& remaining) {
    if (bytes > remaining) fail(QueryErrorCode::resource_limit);
    remaining -= bytes;
}

std::size_t charge_name(const char* name, std::size_t& remaining) {
    if (name == nullptr) fail(QueryErrorCode::resource_limit);
    std::size_t size = 0;
    while (name[size] != '\0') {
        if (size == remaining) fail(QueryErrorCode::resource_limit);
        ++size;
    }
    charge(size, remaining);
    return size;
}

QueryResult run(const std::filesystem::path& path, const QueryPolicy& policy,
                std::string_view sql, std::span<const QueryBind> binds) {
    validate(policy);
    if (sql.size() > policy.limits.max_sql_bytes) fail(QueryErrorCode::resource_limit);
    if (sql.empty() || sql.find('\0') != std::string_view::npos) fail(QueryErrorCode::invalid_sql);
    std::size_t bind_remaining = policy.limits.max_sql_bytes;
    for (const auto& bind : binds) {
        if (const auto* text = std::get_if<std::string>(&bind)) {
            if (text->size() > bind_remaining) fail(QueryErrorCode::bind_too_large);
            bind_remaining -= text->size();
        }
    }
    if (binds.size() > static_cast<std::size_t>(internal_parameters)) fail(QueryErrorCode::resource_limit);
    const std::string owned_sql(sql);
    const auto filename = database_filename(path);
    QueryContext context{policy, std::chrono::steady_clock::now() + policy.limits.timeout};
    sqlite3* raw_database = nullptr;
    const auto opened = sqlite3_open_v2(filename.c_str(), &raw_database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, nullptr);
    Database database(raw_database);
    if (opened != SQLITE_OK || sqlite3_db_readonly(database.get(), "main") != 1)
        fail(QueryErrorCode::database_unavailable);
    checkpoint(context);
    configure(database.get());
    sqlite3_limit(database.get(), SQLITE_LIMIT_SQL_LENGTH,
        std::max(internal_sql_floor, static_cast<int>(policy.limits.max_sql_bytes)));
    if (sqlite3_set_authorizer(database.get(), authorize, &context) != SQLITE_OK)
        fail(QueryErrorCode::execution_failed);
    sqlite3_progress_handler(database.get(), progress_interval, progress, &context);

    const char* tail = nullptr;
    auto statement = prepare(database.get(), context, owned_sql.data(), static_cast<int>(owned_sql.size()), &tail);
    if (!statement || sqlite3_stmt_isexplain(statement.get()) != 0) fail(QueryErrorCode::invalid_sql);
    if (sqlite3_stmt_readonly(statement.get()) == 0) fail(QueryErrorCode::unauthorized);
    const auto* end = owned_sql.data() + owned_sql.size();
    while (tail != nullptr && tail < end) {
        const char* next = nullptr;
        auto extra = prepare(database.get(), context, tail, static_cast<int>(end - tail), &next);
        if (extra || next == nullptr || next <= tail) fail(QueryErrorCode::invalid_sql);
        tail = next;
    }

    const auto count = sqlite3_bind_parameter_count(statement.get());
    for (int i = 1; i <= count; ++i) {
        if (sqlite3_bind_parameter_name(statement.get(), i) != nullptr) fail(QueryErrorCode::invalid_sql);
    }
    if (static_cast<std::size_t>(count) != binds.size()) fail(QueryErrorCode::bind_mismatch);
    for (std::size_t i = 0; i < binds.size(); ++i) {
        int code = SQLITE_OK;
        const auto index = static_cast<int>(i) + 1;
        if (const auto* text = std::get_if<std::string>(&binds[i])) {
            code = sqlite3_bind_text(statement.get(), index, text->data(), static_cast<int>(text->size()), SQLITE_TRANSIENT);
        } else if (const auto* integer = std::get_if<std::int64_t>(&binds[i])) {
            code = sqlite3_bind_int64(statement.get(), index, *integer);
        } else {
            code = sqlite3_bind_null(statement.get(), index);
        }
        if (code != SQLITE_OK) sqlite_failure(code, context, QueryErrorCode::execution_failed);
    }

    QueryResult result;
    auto remaining = policy.limits.max_result_bytes;
    int column_count = 0;
    bool metadata_ready = false;
    while (true) {
        checkpoint(context);
        const auto code = sqlite3_step(statement.get());
        if (code != SQLITE_ROW && code != SQLITE_DONE)
            sqlite_failure(code, context, QueryErrorCode::execution_failed);
        checkpoint(context);
        // step may have automatically reprepared after a schema change.
        // Obtain actual metadata after the first step, including empty results.
        if (!metadata_ready) {
            column_count = sqlite3_column_count(statement.get());
            for (int i = 0; i < column_count; ++i) {
                const auto* name = sqlite3_column_name(statement.get(), i);
                const auto size = charge_name(name, remaining);
                result.columns.emplace_back(name, size);
            }
            metadata_ready = true;
        }
        if (code == SQLITE_DONE) return result;
        if (result.rows.size() >= policy.limits.max_rows) fail(QueryErrorCode::resource_limit);
        std::vector<QueryCell> row;
        row.reserve(static_cast<std::size_t>(column_count));
        for (int i = 0; i < column_count; ++i) {
            switch (sqlite3_column_type(statement.get(), i)) {
            case SQLITE_NULL:
                row.emplace_back(std::monostate{});
                break;
            case SQLITE_INTEGER:
                charge(8, remaining);
                row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), i)));
                break;
            case SQLITE_FLOAT:
                charge(8, remaining);
                row.emplace_back(sqlite3_column_double(statement.get(), i));
                break;
            case SQLITE_TEXT: {
                const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), i));
                charge(size, remaining);
                const auto* text = sqlite3_column_text(statement.get(), i);
                if (sqlite3_errcode(database.get()) == SQLITE_NOMEM) fail(QueryErrorCode::resource_limit);
                if (text == nullptr && size != 0) fail(QueryErrorCode::execution_failed);
                row.emplace_back(text == nullptr ? std::string{} :
                    std::string(reinterpret_cast<const char*>(text), size));
                break;
            }
            default:
                fail(QueryErrorCode::execution_failed);
            }
        }
        checkpoint(context);
        result.rows.push_back(std::move(row));
    }
}
}

QueryError::QueryError(QueryErrorCode error_code)
    : std::runtime_error("Creator query could not be executed."), code(error_code) {}

QueryResult read_creator_query(const std::filesystem::path& path, const QueryPolicy& policy,
                               std::string_view sql, std::span<const QueryBind> binds) {
    try {
        return run(path, policy, sql, binds);
    } catch (const QueryError&) {
        throw;
    } catch (const std::bad_alloc&) {
        fail(QueryErrorCode::resource_limit);
    } catch (...) {
        fail(QueryErrorCode::execution_failed);
    }
}
}
