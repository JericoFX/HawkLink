#include <hawken/local_store.hpp>
#include <sqlite3.h>
#include <array>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

namespace hawken::store {
StoreError::StoreError(StoreErrorCode code)
    : std::runtime_error("Local store operation failed."), code_(code) {}
StoreErrorCode StoreError::code() const noexcept { return code_; }

namespace {
[[noreturn]] void fail(StoreErrorCode code) { throw StoreError(code); }
void require(bool valid, StoreErrorCode code = StoreErrorCode::invalid_schema) {
    if (!valid) fail(code);
}
void checked(int result) {
    if (result == SQLITE_OK) return;
    switch (result & 0xff) {
    case SQLITE_BUSY: case SQLITE_LOCKED: fail(StoreErrorCode::busy);
    case SQLITE_CORRUPT: case SQLITE_NOTADB: case SQLITE_SCHEMA:
    case SQLITE_ERROR: case SQLITE_CONSTRAINT: fail(StoreErrorCode::invalid_schema);
    default: fail(StoreErrorCode::io_error);
    }
}
bool safe_id(std::string_view value) {
    if (value.empty() || value.size() > 64 || value == "." || value == "..") return false;
    for (const unsigned char byte : value) {
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-')) return false;
    }
    return true;
}
bool steam_id(std::string_view value) {
    if (value.empty() || value.size() > 20 || value.front() < '1' || value.front() > '9') return false;
    for (const char byte : value) if (byte < '0' || byte > '9') return false;
    return value.size() < 20 || value <= "18446744073709551615";
}
bool inventory_id(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}
void validate_starter(const StarterItems& items, StoreErrorCode code) {
    std::array<std::string_view, 18> ids;
    for (std::size_t i = 0; i < items.size(); ++i) {
        ids[2 * i] = items[i].type_id;
        ids[2 * i + 1] = items[i].instance_id;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(inventory_id(ids[i]), code);
        for (std::size_t j = 0; j < i; ++j) require(ids[i] != ids[j], code);
    }
}
bool text_valid(std::string_view value, std::size_t minimum, std::size_t maximum, bool reject_controls = true) {
    if (value.size() < minimum || value.size() > maximum) return false;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index++]);
        std::uint32_t code = first;
        std::uint32_t lowest = 0;
        std::size_t remaining = 0;
        if (first <= 0x7f) {}
        else if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1fU; remaining = 1; lowest = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0fU; remaining = 2; lowest = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07U; remaining = 3; lowest = 0x10000; }
        else return false;
        if (remaining > value.size() - index) return false;
        while (remaining > 0) {
            const auto next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0U) != 0x80U) return false;
            code = (code << 6U) | (next & 0x3fU);
            --remaining;
        }
        if (code < lowest || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
            code == 0 || (reject_controls && (code <= 0x1f || (code >= 0x7f && code <= 0x9f)))) return false;
    }
    return true;
}
void validate_ban(const Ban& ban, StoreErrorCode code) {
    require(ban.starts_at >= 0 && (!ban.expires_at || *ban.expires_at > ban.starts_at) &&
        text_valid(ban.reason, 0, 256), code);
}
class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        const int result = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
        if (result != SQLITE_OK) { sqlite3_finalize(stmt_); stmt_ = nullptr; checked(result); }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view value) {
        require(value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()), StoreErrorCode::invalid_input);
        checked(sqlite3_bind_text(stmt_, index, value.empty() ? "" : value.data(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void bind(int index, std::int64_t value) { checked(sqlite3_bind_int64(stmt_, index, value)); }
    void bind_null(int index) { checked(sqlite3_bind_null(stmt_, index)); }
    bool row() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        checked(result);
        return false;
    }
    void done() { require(!row()); }
    bool null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }
    std::int64_t integer(int column) const {
        require(sqlite3_column_type(stmt_, column) == SQLITE_INTEGER);
        return sqlite3_column_int64(stmt_, column);
    }
    std::string text(int column, std::size_t maximum) const {
        require(sqlite3_column_type(stmt_, column) == SQLITE_TEXT);
        const int bytes = sqlite3_column_bytes(stmt_, column);
        require(bytes >= 0 && static_cast<std::size_t>(bytes) <= maximum);
        const auto* data = sqlite3_column_text(stmt_, column);
        require(data != nullptr);
        return std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(bytes));
    }
private:
    sqlite3_stmt* stmt_ = nullptr;
};
void execute(sqlite3* db, const char* sql) { Statement statement(db, sql); statement.done(); }
std::int64_t scalar(sqlite3* db, const char* sql) {
    Statement statement(db, sql);
    require(statement.row());
    const auto value = statement.integer(0);
    statement.done();
    return value;
}
class Transaction {
public:
    Transaction(sqlite3* db, bool write) : db_(db) { execute(db_, write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() { if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { execute(db_, "COMMIT"); active_ = false; }
private:
    sqlite3* db_;
    bool active_ = true;
};
constexpr int current_schema_version = 5;
constexpr std::array<const char*, 11> names{
    "server_metadata", "accounts", "aliases", "visits", "roles", "bans", "wallets", "starter_grants", "starter_items", "game_settings",
    "owned_item_attributes"};
constexpr std::array<const char*, 11> ddl{
R"(CREATE TABLE server_metadata (
    singleton INTEGER PRIMARY KEY NOT NULL CHECK(singleton = 1),
    server_id TEXT NOT NULL CHECK(
        length(CAST(server_id AS BLOB)) BETWEEN 1 AND 64
        AND instr(CAST(server_id AS BLOB), x'00') = 0
        AND server_id NOT GLOB '*[^A-Za-z0-9_.-]*'
        AND server_id NOT IN ('.', '..'))
) STRICT)",
R"(CREATE TABLE accounts (
    id INTEGER PRIMARY KEY NOT NULL CHECK(id > 0),
    steam_id64 TEXT NOT NULL UNIQUE CHECK(
        length(steam_id64) BETWEEN 1 AND 20
        AND instr(CAST(steam_id64 AS BLOB), x'00') = 0
        AND steam_id64 NOT GLOB '*[^0-9]*'
        AND substr(steam_id64, 1, 1) BETWEEN '1' AND '9'
        AND (length(steam_id64) < 20
             OR steam_id64 COLLATE BINARY <= '18446744073709551615')),
    created_at INTEGER NOT NULL CHECK(created_at >= 0)
) STRICT)",
R"(CREATE TABLE aliases (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    value TEXT NOT NULL CHECK(
        length(CAST(value AS BLOB)) BETWEEN 1 AND 128
        AND instr(CAST(value AS BLOB), x'00') = 0),
    first_seen INTEGER NOT NULL CHECK(first_seen >= 0),
    last_seen INTEGER NOT NULL CHECK(last_seen >= first_seen),
    PRIMARY KEY(account_id, value)
) STRICT)",
R"(CREATE TABLE visits (
    visit_id TEXT PRIMARY KEY NOT NULL CHECK(
        length(CAST(visit_id AS BLOB)) BETWEEN 1 AND 64
        AND instr(CAST(visit_id AS BLOB), x'00') = 0
        AND visit_id NOT GLOB '*[^A-Za-z0-9_.-]*'
        AND visit_id NOT IN ('.', '..')),
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    alias TEXT NOT NULL CHECK(
        length(CAST(alias AS BLOB)) BETWEEN 1 AND 128
        AND instr(CAST(alias AS BLOB), x'00') = 0),
    occurred_at INTEGER NOT NULL CHECK(occurred_at >= 0)
) STRICT)",
R"(CREATE TABLE roles (
    account_id INTEGER PRIMARY KEY NOT NULL
        REFERENCES accounts(id) ON DELETE RESTRICT,
    role TEXT NOT NULL CHECK(role IN ('owner', 'moderator', 'analyst'))
) STRICT)",
R"(CREATE TABLE bans (
    account_id INTEGER PRIMARY KEY NOT NULL
        REFERENCES accounts(id) ON DELETE RESTRICT,
    starts_at INTEGER NOT NULL CHECK(starts_at >= 0),
    expires_at INTEGER CHECK(expires_at IS NULL OR expires_at > starts_at),
    reason TEXT NOT NULL CHECK(
        length(CAST(reason AS BLOB)) <= 256
        AND instr(CAST(reason AS BLOB), x'00') = 0)
) STRICT)",
R"(CREATE TABLE wallets (
    account_id INTEGER PRIMARY KEY NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    hawken_points INTEGER NOT NULL CHECK(hawken_points BETWEEN 0 AND 2147483647),
    meteor_points INTEGER NOT NULL CHECK(meteor_points BETWEEN 0 AND 2147483647)
) STRICT)",
R"(CREATE TABLE starter_grants (
    account_id INTEGER PRIMARY KEY NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT
) STRICT)",
R"(CREATE TABLE starter_items (
    account_id INTEGER NOT NULL REFERENCES starter_grants(account_id) ON DELETE RESTRICT,
    slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 8),
    type_id TEXT NOT NULL CHECK(
        length(CAST(type_id AS BLOB)) = 36 AND instr(CAST(type_id AS BLOB), x'00') = 0
        AND substr(type_id, 9, 1) = '-' AND substr(type_id, 14, 1) = '-'
        AND substr(type_id, 19, 1) = '-' AND substr(type_id, 24, 1) = '-'
        AND length(replace(type_id, '-', '')) = 32
        AND replace(type_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND type_id != '00000000-0000-0000-0000-000000000000'),
    instance_id TEXT NOT NULL UNIQUE CHECK(
        length(CAST(instance_id AS BLOB)) = 36 AND instr(CAST(instance_id AS BLOB), x'00') = 0
        AND substr(instance_id, 9, 1) = '-' AND substr(instance_id, 14, 1) = '-'
        AND substr(instance_id, 19, 1) = '-' AND substr(instance_id, 24, 1) = '-'
        AND length(replace(instance_id, '-', '')) = 32
        AND replace(instance_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND instance_id != '00000000-0000-0000-0000-000000000000'),
    PRIMARY KEY(account_id, slot)
) STRICT)",
R"(CREATE TABLE game_settings (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    key TEXT NOT NULL CHECK(
        length(CAST(key AS BLOB)) BETWEEN 1 AND 256
        AND instr(CAST(key AS BLOB), x'00') = 0),
    value TEXT NOT NULL CHECK(
        length(CAST(value AS BLOB)) <= 16384
        AND instr(CAST(value AS BLOB), x'00') = 0),
    PRIMARY KEY(account_id, key)
) STRICT)",
R"(CREATE TABLE owned_item_attributes (
    account_id INTEGER NOT NULL,
    slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 8),
    key TEXT NOT NULL CHECK(
        length(CAST(key AS BLOB)) BETWEEN 1 AND 256
        AND instr(CAST(key AS BLOB), x'00') = 0
        AND substr(key, 1, 7) = 'Preset.'),
    value TEXT NOT NULL CHECK(
        length(CAST(value AS BLOB)) <= 16384
        AND instr(CAST(value AS BLOB), x'00') = 0),
    PRIMARY KEY(account_id, slot, key),
    FOREIGN KEY(account_id, slot) REFERENCES starter_items(account_id, slot) ON DELETE RESTRICT
) STRICT)"};
struct Column { const char* name; const char* type; int not_null; int primary; };
constexpr std::array metadata_columns{Column{"singleton", "INTEGER", 1, 1}, Column{"server_id", "TEXT", 1, 0}};
constexpr std::array account_columns{Column{"id", "INTEGER", 1, 1}, Column{"steam_id64", "TEXT", 1, 0}, Column{"created_at", "INTEGER", 1, 0}};
constexpr std::array alias_columns{Column{"account_id", "INTEGER", 1, 1}, Column{"value", "TEXT", 1, 2},
    Column{"first_seen", "INTEGER", 1, 0}, Column{"last_seen", "INTEGER", 1, 0}};
constexpr std::array visit_columns{Column{"visit_id", "TEXT", 1, 1}, Column{"account_id", "INTEGER", 1, 0},
    Column{"alias", "TEXT", 1, 0}, Column{"occurred_at", "INTEGER", 1, 0}};
constexpr std::array role_columns{Column{"account_id", "INTEGER", 1, 1}, Column{"role", "TEXT", 1, 0}};
constexpr std::array ban_columns{Column{"account_id", "INTEGER", 1, 1}, Column{"starts_at", "INTEGER", 1, 0},
    Column{"expires_at", "INTEGER", 0, 0}, Column{"reason", "TEXT", 1, 0}};
constexpr std::array wallet_columns{Column{"account_id", "INTEGER", 1, 1},
    Column{"hawken_points", "INTEGER", 1, 0}, Column{"meteor_points", "INTEGER", 1, 0}};
constexpr std::array starter_grant_columns{Column{"account_id", "INTEGER", 1, 1}};
constexpr std::array starter_item_columns{Column{"account_id", "INTEGER", 1, 1}, Column{"slot", "INTEGER", 1, 2},
    Column{"type_id", "TEXT", 1, 0}, Column{"instance_id", "TEXT", 1, 0}};
constexpr std::array settings_columns{Column{"account_id", "INTEGER", 1, 1},
    Column{"key", "TEXT", 1, 2}, Column{"value", "TEXT", 1, 0}};
constexpr std::array item_attribute_columns{Column{"account_id", "INTEGER", 1, 1}, Column{"slot", "INTEGER", 1, 2},
    Column{"key", "TEXT", 1, 3}, Column{"value", "TEXT", 1, 0}};
const std::array<std::span<const Column>, 11> columns{
    metadata_columns, account_columns, alias_columns, visit_columns, role_columns, ban_columns, wallet_columns,
    starter_grant_columns, starter_item_columns, settings_columns, item_attribute_columns};
constexpr std::array<const char*, 7> index_names{
    "sqlite_autoindex_accounts_1", "sqlite_autoindex_aliases_1", "sqlite_autoindex_visits_1",
    "sqlite_autoindex_starter_items_1", "sqlite_autoindex_starter_items_2", "sqlite_autoindex_game_settings_1",
    "sqlite_autoindex_owned_item_attributes_1"};
constexpr std::array<std::size_t, 7> index_tables{1, 2, 3, 8, 8, 9, 10};

void check_table(sqlite3* db, std::size_t table) {
    Statement attributes(db, "SELECT type,ncol,wr,strict FROM pragma_table_list WHERE schema='main' AND name=?");
    attributes.bind(1, names[table]);
    require(attributes.row());
    require(attributes.text(0, 16) == "table" && attributes.integer(1) == static_cast<std::int64_t>(columns[table].size()) &&
        attributes.integer(2) == 0 && attributes.integer(3) == 1);
    attributes.done();
    Statement description(db, "SELECT cid,name,type,\"notnull\",dflt_value,pk,hidden FROM pragma_table_xinfo(?, 'main') ORDER BY cid");
    description.bind(1, names[table]);
    std::size_t position = 0;
    while (description.row()) {
        require(position < columns[table].size());
        const auto& expected = columns[table][position];
        require(description.integer(0) == static_cast<std::int64_t>(position) && description.text(1, 64) == expected.name &&
            description.text(2, 16) == expected.type && description.integer(3) == expected.not_null && description.null(4) &&
            description.integer(5) == expected.primary && description.integer(6) == 0);
        ++position;
    }
    require(position == columns[table].size());
    Statement foreign(db, "SELECT id,seq,\"table\",\"from\",\"to\",on_update,on_delete,\"match\" FROM pragma_foreign_key_list(?, 'main') ORDER BY id,seq");
    foreign.bind(1, names[table]);
    if (table == 10) {
        constexpr std::array<const char*, 2> keys{"account_id", "slot"};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            require(foreign.row());
            require(foreign.integer(0) == 0 && foreign.integer(1) == static_cast<std::int64_t>(index) &&
                foreign.text(2, 64) == "starter_items" && foreign.text(3, 64) == keys[index] &&
                foreign.text(4, 64) == keys[index] && foreign.text(5, 16) == "NO ACTION" &&
                foreign.text(6, 16) == "RESTRICT" && foreign.text(7, 16) == "NONE");
        }
    } else if (table >= 2) {
        require(foreign.row());
        require(foreign.integer(0) == 0 && foreign.integer(1) == 0 && foreign.text(2, 64) == (table == 8 ? "starter_grants" : "accounts") &&
            foreign.text(3, 64) == "account_id" && foreign.text(4, 64) == (table == 8 ? "account_id" : "id") && foreign.text(5, 16) == "NO ACTION" &&
            foreign.text(6, 16) == "RESTRICT" && foreign.text(7, 16) == "NONE");
    }
    foreign.done();
    Statement indexes(db, "SELECT seq,name,\"unique\",origin,partial FROM pragma_index_list(?, 'main')");
    indexes.bind(1, names[table]);
    if (table >= 1 && table <= 3) {
        require(indexes.row());
        require(indexes.integer(0) == 0 && indexes.text(1, 64) == index_names[table - 1] && indexes.integer(2) == 1 &&
            indexes.text(3, 8) == (table == 1 ? "u" : "pk") && indexes.integer(4) == 0);
    } else if (table == 9 || table == 10) {
        require(indexes.row());
        require(indexes.integer(0) == 0 && indexes.text(1, 64) == index_names[table == 9 ? 5 : 6] && indexes.integer(2) == 1 &&
            indexes.text(3, 8) == "pk" && indexes.integer(4) == 0);
    } else if (table == 8) {
        require(indexes.row());
        require(indexes.integer(0) == 0 && indexes.text(1, 64) == index_names[4] && indexes.integer(2) == 1 &&
            indexes.text(3, 8) == "pk" && indexes.integer(4) == 0);
        require(indexes.row());
        require(indexes.integer(0) == 1 && indexes.text(1, 64) == index_names[3] && indexes.integer(2) == 1 &&
            indexes.text(3, 8) == "u" && indexes.integer(4) == 0);
    }
    indexes.done();
}
void check_index(sqlite3* db, std::size_t index) {
    Statement statement(db, "SELECT seqno,cid,name,\"desc\",coll,\"key\" FROM pragma_index_xinfo(?, 'main') ORDER BY seqno");
    statement.bind(1, index_names[index]);
    const std::size_t keys = index == 6 ? 3 : index == 1 || index == 4 || index == 5 ? 2 : 1;
    for (std::size_t position = 0; position < keys; ++position) {
        const std::size_t column = index == 0 ? 1 : index == 3 ? 3 : position;
        require(statement.row());
        require(statement.integer(0) == static_cast<std::int64_t>(position) && statement.integer(1) == static_cast<std::int64_t>(column) &&
            statement.text(2, 64) == columns[index_tables[index]][column].name && statement.integer(3) == 0 &&
            statement.text(4, 16) == "BINARY" && statement.integer(5) == 1);
    }
    require(statement.row());
    require(statement.integer(0) == static_cast<std::int64_t>(keys) && statement.integer(1) == -1 && statement.null(2) &&
        statement.integer(3) == 0 && statement.text(4, 16) == "BINARY" && statement.integer(5) == 0);
    statement.done();
}
void check_identity(sqlite3* db, std::string_view server, int expected_version = current_schema_version) {
    const auto version = scalar(db, "PRAGMA main.user_version");
    const auto application = scalar(db, "PRAGMA main.application_id");
    if (version > current_schema_version || application != 0x4841574B) fail(StoreErrorCode::unsupported_schema);
    require(version == expected_version);
    Statement metadata(db, "SELECT singleton,server_id FROM main.server_metadata");
    require(metadata.row() && metadata.integer(0) == 1);
    const auto stored_server = metadata.text(1, 64);
    require(safe_id(stored_server));
    require(stored_server == server, StoreErrorCode::invalid_scope);
    metadata.done();
}
void validate_schema(sqlite3* db, std::string_view server, int expected_version = current_schema_version) {
    const auto version = scalar(db, "PRAGMA main.user_version");
    const auto application = scalar(db, "PRAGMA main.application_id");
    if (version > current_schema_version || application != 0x4841574B) fail(StoreErrorCode::unsupported_schema);
    require(version == expected_version);
    const std::size_t table_count = expected_version == 1 ? 6 : expected_version == 2 ? 7 : expected_version == 3 ? 9 :
        expected_version == 4 ? 10 : names.size();
    const std::size_t index_count = expected_version < 3 ? 3 : expected_version == 3 ? 5 :
        expected_version == 4 ? 6 : index_names.size();
    std::array<bool, names.size()> tables{};
    std::array<bool, index_names.size()> indexes{};
    Statement objects(db, "SELECT type,name,tbl_name,sql FROM main.sqlite_schema");
    while (objects.row()) {
        const auto type = objects.text(0, 16);
        const auto name = objects.text(1, 64);
        const auto owner = objects.text(2, 64);
        bool matched = false;
        if (type == "table") {
            for (std::size_t index = 0; index < table_count; ++index) {
                if (name != names[index]) continue;
                require(!tables[index] && owner == name && objects.text(3, 8192) == ddl[index]);
                tables[index] = true;
                matched = true;
                break;
            }
        } else if (type == "index") {
            for (std::size_t index = 0; index < index_count; ++index) {
                if (name != index_names[index]) continue;
                require(!indexes[index] && owner == names[index_tables[index]] && objects.null(3));
                indexes[index] = true;
                matched = true;
                break;
            }
        }
        require(matched);
    }
    for (std::size_t index = 0; index < table_count; ++index) require(tables[index]);
    for (std::size_t index = 0; index < index_count; ++index) require(indexes[index]);
    for (std::size_t index = 0; index < table_count; ++index) check_table(db, index);
    for (std::size_t index = 0; index < index_count; ++index) check_index(db, index);
    check_identity(db, server, expected_version);
    Statement foreign(db, "PRAGMA main.foreign_key_check");
    foreign.done();
}
Account read_account(Statement& statement, std::string_view server) {
    Account account{{std::string(server), statement.integer(0)}, statement.text(1, 20), statement.integer(2)};
    require(account.ref.local_id > 0 && steam_id(account.steam_id64) && account.created_at >= 0);
    return account;
}
std::optional<Account> find_account_row(sqlite3* db, std::string_view server, std::string_view steam) {
    Statement statement(db, "SELECT id,steam_id64,created_at FROM main.accounts WHERE steam_id64=?");
    statement.bind(1, steam);
    if (!statement.row()) return std::nullopt;
    auto account = read_account(statement, server);
    require(account.steam_id64 == steam);
    statement.done();
    return account;
}
void require_account(sqlite3* db, std::string_view server, const AccountRef& ref) {
    require(safe_id(ref.server_id) && ref.local_id > 0, StoreErrorCode::invalid_input);
    require(ref.server_id == server, StoreErrorCode::invalid_scope);
    Statement statement(db, "SELECT id,steam_id64,created_at FROM main.accounts WHERE id=?");
    statement.bind(1, ref.local_id);
    require(statement.row(), StoreErrorCode::not_found);
    const auto account = read_account(statement, server);
    require(account.ref == ref);
    statement.done();
}
std::optional<StarterItems> read_starter_items(sqlite3* db, std::int64_t account_id) {
    Statement grant(db, "SELECT account_id FROM main.starter_grants WHERE account_id=?");
    grant.bind(1, account_id);
    const bool recorded = grant.row();
    if (recorded) require(grant.integer(0) == account_id);
    grant.done();
    Statement rows(db, "SELECT slot,type_id,instance_id FROM main.starter_items WHERE account_id=? ORDER BY slot");
    rows.bind(1, account_id);
    StarterItems result;
    std::size_t count = 0;
    while (rows.row()) {
        require(recorded && count < result.size() && rows.integer(0) == static_cast<std::int64_t>(count));
        result[count] = {rows.text(1, 36), rows.text(2, 36)};
        ++count;
    }
    if (!recorded) return std::nullopt;
    require(count == result.size());
    validate_starter(result, StoreErrorCode::invalid_schema);
    return result;
}
std::int64_t require_owned_slot(sqlite3* db, std::int64_t account_id, std::string_view instance_id) {
    require(inventory_id(instance_id), StoreErrorCode::invalid_input);
    const auto items = read_starter_items(db, account_id);
    require(items.has_value(), StoreErrorCode::not_found);
    for (std::size_t slot = 0; slot < items->size(); ++slot) {
        if ((*items)[slot].instance_id == instance_id) return static_cast<std::int64_t>(slot);
    }
    fail(StoreErrorCode::not_found);
}
void validate_settings_pair(std::string_view key, std::string_view value, StoreErrorCode code) {
    require(text_valid(key, 1, 256) && text_valid(value, 0, 16384, false), code);
}
void validate_attribute_pair(std::string_view key, std::string_view value, StoreErrorCode code) {
    require(key.starts_with("Preset."), code);
    validate_settings_pair(key, value, code);
}
void validate_settings_total(const GameSettings& settings, StoreErrorCode code) {
    require(settings.size() <= 1024, code);
    std::size_t bytes = 0;
    for (const auto& [key, value] : settings) {
        validate_settings_pair(key, value, code);
        const auto pair_bytes = key.size() + value.size();
        require(pair_bytes <= 262144 - bytes, code);
        bytes += pair_bytes;
    }
}
GameSettings read_settings(sqlite3* db, std::int64_t account_id) {
    Statement rows(db, "SELECT key,value FROM main.game_settings WHERE account_id=? ORDER BY key");
    rows.bind(1, account_id);
    GameSettings result;
    std::size_t bytes = 0;
    while (rows.row()) {
        auto key = rows.text(0, 256), value = rows.text(1, 16384);
        validate_settings_pair(key, value, StoreErrorCode::invalid_schema);
        require(result.size() < 1024 && key.size() + value.size() <= 262144 - bytes);
        bytes += key.size() + value.size();
        require(result.emplace(std::move(key), std::move(value)).second);
    }
    return result;
}
ItemAttributes read_item_attributes(sqlite3* db, std::int64_t account_id, std::int64_t slot) {
    Statement rows(db, "SELECT key,value FROM main.owned_item_attributes WHERE account_id=? AND slot=? ORDER BY key");
    rows.bind(1, account_id);
    rows.bind(2, slot);
    ItemAttributes result;
    std::size_t bytes = 0;
    while (rows.row()) {
        auto key = rows.text(0, 256), value = rows.text(1, 16384);
        validate_attribute_pair(key, value, StoreErrorCode::invalid_schema);
        require(result.size() < 1024 && key.size() + value.size() <= 262144 - bytes);
        bytes += key.size() + value.size();
        require(result.emplace(std::move(key), std::move(value)).second);
    }
    return result;
}
Visit read_visit(Statement& statement, std::string_view server) {
    Visit visit{statement.text(0, 64), {std::string(server), statement.integer(1)}, statement.text(2, 128), statement.integer(3)};
    require(safe_id(visit.visit_id) && visit.account.local_id > 0 && text_valid(visit.alias, 1, 128) && visit.occurred_at >= 0);
    return visit;
}
const char* role_name(Role role) {
    switch (role) {
    case Role::owner: return "owner";
    case Role::moderator: return "moderator";
    case Role::analyst: return "analyst";
    }
    fail(StoreErrorCode::invalid_input);
}
Role role_value(std::string_view value) {
    if (value == "owner") return Role::owner;
    if (value == "moderator") return Role::moderator;
    if (value == "analyst") return Role::analyst;
    fail(StoreErrorCode::invalid_schema);
}
template<class T> T& state_of(const std::unique_ptr<T>& state) {
    require(state != nullptr, StoreErrorCode::invalid_input);
    return *state;
}
}

struct LocalStore::Impl {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db{nullptr, sqlite3_close_v2};
    std::string server;
    std::mutex mutex;
    std::int64_t schema_cookie = 0;
    Impl(const std::filesystem::path& path, std::string id) : server(std::move(id)) {
        require(safe_id(server) && !path.empty(), StoreErrorCode::invalid_input);
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        require(!error, StoreErrorCode::io_error);
        std::string filename;
        try {
            const auto utf8 = absolute.u8string();
            filename.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        } catch (const std::filesystem::filesystem_error&) { fail(StoreErrorCode::io_error); }
        require(filename.find('\0') == std::string::npos, StoreErrorCode::invalid_input);
        sqlite3* opened = nullptr;
        const int result = sqlite3_open_v2(filename.c_str(), &opened,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
        db.reset(opened);
        checked(result);
        checked(sqlite3_extended_result_codes(db.get(), 1));
        checked(sqlite3_busy_timeout(db.get(), 250));
        int configured = 0;
        checked(sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, &configured));
        require(configured == 1);
        checked(sqlite3_db_config(db.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &configured));
        require(configured == 0);
        execute(db.get(), "PRAGMA foreign_keys=ON");
        require(scalar(db.get(), "PRAGMA foreign_keys") == 1 && scalar(db.get(), "PRAGMA trusted_schema") == 0);
        require(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 1);
        Transaction transaction(db.get(), true);
        const auto version = scalar(db.get(), "PRAGMA main.user_version");
        const auto application = scalar(db.get(), "PRAGMA main.application_id");
        if (version == 0) {
            require(application == 0, StoreErrorCode::unsupported_schema);
            require(scalar(db.get(), "SELECT count(*) FROM main.sqlite_schema") == 0);
            for (const auto* sql : ddl) execute(db.get(), sql);
            Statement metadata(db.get(), "INSERT INTO main.server_metadata(singleton,server_id) VALUES(1,?)");
            metadata.bind(1, server);
            metadata.done();
            execute(db.get(), "PRAGMA main.application_id=0x4841574B");
            execute(db.get(), "PRAGMA main.user_version=5");
        } else if (version == 1 || version == 2 || version == 3 || version == 4) {
            // Validate old scope/schema before DDL or data changes. One write
            // transaction preserves existing identities, balances and grants.
            validate_schema(db.get(), server, static_cast<int>(version));
            if (version == 1) {
                execute(db.get(), ddl[6]);
                execute(db.get(), "INSERT INTO main.wallets(account_id,hawken_points,meteor_points) "
                    "SELECT id,0,0 FROM main.accounts");
            }
            if (version < 3) {
                execute(db.get(), ddl[7]);
                execute(db.get(), ddl[8]);
            }
            if (version < 4) execute(db.get(), ddl[9]);
            execute(db.get(), ddl[10]);
            execute(db.get(), "PRAGMA main.user_version=5");
        }
        validate_schema(db.get(), server);
        schema_cookie = scalar(db.get(), "PRAGMA main.schema_version");
        transaction.commit();
    }
    // Called only with mutex held and an active transaction. The cookie is a
    // local optimization, not a defense against an owner rewriting DB bytes.
    void check() {
        const auto current = scalar(db.get(), "PRAGMA main.schema_version");
        if (current != schema_cookie) {
            validate_schema(db.get(), server);
            schema_cookie = current;
        } else check_identity(db.get(), server);
    }
};

LocalStore::LocalStore(const std::filesystem::path& path, std::string server_id)
    : impl_(std::make_unique<Impl>(path, std::move(server_id))) {}
LocalStore::~LocalStore() = default;
LocalStore::LocalStore(LocalStore&&) noexcept = default;
LocalStore& LocalStore::operator=(LocalStore&&) noexcept = default;

StarterMechSnapshot LocalStore::owned_starter_mech(const AccountRef& account, std::string_view instance_id) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    require(inventory_id(instance_id), StoreErrorCode::invalid_input);
    const auto items = read_starter_items(state.db.get(), account.local_id);
    require(items.has_value() && items->at(0).instance_id == instance_id, StoreErrorCode::not_found);
    StarterMechSnapshot result{*items, read_item_attributes(state.db.get(), account.local_id, 0)};
    transaction.commit();
    return result;
}
ItemAttributes LocalStore::owned_item_attributes(const AccountRef& account, std::string_view instance_id) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    const auto slot = require_owned_slot(state.db.get(), account.local_id, instance_id);
    auto result = read_item_attributes(state.db.get(), account.local_id, slot);
    transaction.commit();
    return result;
}
bool LocalStore::merge_owned_item_attributes(const AccountRef& account, std::string_view instance_id, const ItemAttributes& update) {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, account);
    const auto slot = require_owned_slot(state.db.get(), account.local_id, instance_id);
    require(update.size() <= 256, StoreErrorCode::invalid_input);
    for (const auto& [key, value] : update) validate_attribute_pair(key, value, StoreErrorCode::invalid_input);
    const auto previous = read_item_attributes(state.db.get(), account.local_id, slot);
    auto merged = previous;
    for (const auto& [key, value] : update) merged.insert_or_assign(key, value);
    validate_settings_total(merged, StoreErrorCode::invalid_input);
    bool changed = false;
    for (const auto& [key, value] : update) {
        const auto existing = previous.find(key);
        if (existing != previous.end() && existing->second == value) continue;
        Statement row(state.db.get(), "INSERT INTO main.owned_item_attributes(account_id,slot,key,value) VALUES(?,?,?,?) "
            "ON CONFLICT(account_id,slot,key) DO UPDATE SET value=excluded.value");
        row.bind(1, account.local_id);
        row.bind(2, slot);
        row.bind(3, key);
        row.bind(4, value);
        row.done();
        changed = true;
    }
    transaction.commit();
    return changed;
}

GameSettings LocalStore::game_settings(const AccountRef& account) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    auto result = read_settings(state.db.get(), account.local_id);
    transaction.commit();
    return result;
}
bool LocalStore::merge_game_settings(const AccountRef& account, const GameSettings& update) {
    require(update.size() <= 256, StoreErrorCode::invalid_input);
    for (const auto& [key, value] : update) validate_settings_pair(key, value, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, account);
    const auto previous = read_settings(state.db.get(), account.local_id);
    auto merged = previous;
    for (const auto& [key, value] : update) merged.insert_or_assign(key, value);
    validate_settings_total(merged, StoreErrorCode::invalid_input);
    bool changed = false;
    for (const auto& [key, value] : update) {
        const auto existing = previous.find(key);
        if (existing != previous.end() && existing->second == value) continue;
        Statement row(state.db.get(), "INSERT INTO main.game_settings(account_id,key,value) VALUES(?,?,?) "
            "ON CONFLICT(account_id,key) DO UPDATE SET value=excluded.value");
        row.bind(1, account.local_id);
        row.bind(2, key);
        row.bind(3, value);
        row.done();
        changed = true;
    }
    transaction.commit();
    return changed;
}

int LocalStore::schema_version() const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    transaction.commit();
    return current_schema_version;
}
Account LocalStore::get_or_create_account(std::string_view steam, std::int64_t created_at) {
    require(steam_id(steam) && created_at >= 0, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    if (auto existing = find_account_row(state.db.get(), state.server, steam)) {
        transaction.commit();
        return *existing;
    }
    Statement insert(state.db.get(), "INSERT INTO main.accounts(steam_id64,created_at) VALUES(?,?)");
    insert.bind(1, steam);
    insert.bind(2, created_at);
    insert.done();
    auto created = find_account_row(state.db.get(), state.server, steam);
    require(created.has_value());
    Statement wallet(state.db.get(), "INSERT INTO main.wallets(account_id,hawken_points,meteor_points) VALUES(?,0,0)");
    wallet.bind(1, created->ref.local_id);
    wallet.done();
    transaction.commit();
    return *created;
}
std::optional<Account> LocalStore::find_account(std::string_view steam) const {
    require(steam_id(steam), StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    auto account = find_account_row(state.db.get(), state.server, steam);
    transaction.commit();
    return account;
}
WalletBalances LocalStore::wallet(const AccountRef& account) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement row(state.db.get(), "SELECT hawken_points,meteor_points FROM main.wallets WHERE account_id=?");
    row.bind(1, account.local_id);
    require(row.row());
    const auto hp = row.integer(0), mp = row.integer(1);
    require(hp >= 0 && hp <= std::numeric_limits<std::int32_t>::max()
        && mp >= 0 && mp <= std::numeric_limits<std::int32_t>::max());
    row.done();
    transaction.commit();
    return {static_cast<std::int32_t>(hp), static_cast<std::int32_t>(mp)};
}
std::optional<StarterItems> LocalStore::starter_items(const AccountRef& account) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    auto result = read_starter_items(state.db.get(), account.local_id);
    transaction.commit();
    return result;
}
bool LocalStore::grant_starter_items(const AccountRef& account, const StarterItems& items) {
    validate_starter(items, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, account);
    if (const auto existing = read_starter_items(state.db.get(), account.local_id)) {
        require(*existing == items, StoreErrorCode::conflict);
        transaction.commit();
        return false;
    }
    for (const auto& item : items) {
        Statement collision(state.db.get(), "SELECT EXISTS(SELECT 1 FROM main.starter_items "
            "WHERE instance_id=? OR type_id=? OR instance_id=?)");
        collision.bind(1, item.instance_id);
        collision.bind(2, item.instance_id);
        collision.bind(3, item.type_id);
        require(collision.row());
        require(collision.integer(0) == 0, StoreErrorCode::conflict);
        collision.done();
    }
    Statement grant(state.db.get(), "INSERT INTO main.starter_grants(account_id) VALUES(?)");
    grant.bind(1, account.local_id);
    grant.done();
    for (std::size_t slot = 0; slot < items.size(); ++slot) {
        Statement row(state.db.get(), "INSERT INTO main.starter_items(account_id,slot,type_id,instance_id) VALUES(?,?,?,?)");
        row.bind(1, account.local_id);
        row.bind(2, static_cast<std::int64_t>(slot));
        row.bind(3, items[slot].type_id);
        row.bind(4, items[slot].instance_id);
        row.done();
    }
    transaction.commit();
    return true;
}
bool LocalStore::record_visit(const Visit& visit) {
    require(safe_id(visit.visit_id) && text_valid(visit.alias, 1, 128) && visit.occurred_at >= 0, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, visit.account);
    {
        Statement existing(state.db.get(), "SELECT visit_id,account_id,alias,occurred_at FROM main.visits WHERE visit_id=?");
        existing.bind(1, visit.visit_id);
        if (existing.row()) {
            const auto saved = read_visit(existing, state.server);
            existing.done();
            require(saved == visit, StoreErrorCode::conflict);
            transaction.commit();
            return false;
        }
    }
    Statement insert(state.db.get(), "INSERT INTO main.visits(visit_id,account_id,alias,occurred_at) VALUES(?,?,?,?)");
    insert.bind(1, visit.visit_id);
    insert.bind(2, visit.account.local_id);
    insert.bind(3, visit.alias);
    insert.bind(4, visit.occurred_at);
    insert.done();
    Statement alias(state.db.get(), "INSERT INTO main.aliases(account_id,value,first_seen,last_seen) VALUES(?,?,?,?) "
        "ON CONFLICT(account_id,value) DO UPDATE SET first_seen=min(first_seen,excluded.first_seen),last_seen=max(last_seen,excluded.last_seen)");
    alias.bind(1, visit.account.local_id);
    alias.bind(2, visit.alias);
    alias.bind(3, visit.occurred_at);
    alias.bind(4, visit.occurred_at);
    alias.done();
    transaction.commit();
    return true;
}
std::vector<Alias> LocalStore::aliases(const AccountRef& account, std::size_t limit) const {
    require(limit >= 1 && limit <= 256, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement rows(state.db.get(), "SELECT value,first_seen,last_seen FROM main.aliases WHERE account_id=? ORDER BY last_seen DESC,value ASC LIMIT ?");
    rows.bind(1, account.local_id);
    rows.bind(2, static_cast<std::int64_t>(limit));
    std::vector<Alias> result;
    while (rows.row()) {
        require(result.size() < limit);
        Alias alias{rows.text(0, 128), rows.integer(1), rows.integer(2)};
        require(text_valid(alias.value, 1, 128) && alias.first_seen >= 0 && alias.last_seen >= alias.first_seen);
        result.push_back(std::move(alias));
    }
    transaction.commit();
    return result;
}
std::vector<Visit> LocalStore::visits(const AccountRef& account, std::size_t limit) const {
    require(limit >= 1 && limit <= 256, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement rows(state.db.get(), "SELECT visit_id,account_id,alias,occurred_at FROM main.visits WHERE account_id=? ORDER BY occurred_at DESC,visit_id ASC LIMIT ?");
    rows.bind(1, account.local_id);
    rows.bind(2, static_cast<std::int64_t>(limit));
    std::vector<Visit> result;
    while (rows.row()) {
        require(result.size() < limit);
        auto visit = read_visit(rows, state.server);
        require(visit.account == account);
        result.push_back(std::move(visit));
    }
    transaction.commit();
    return result;
}
void LocalStore::set_role(const AccountRef& account, std::optional<Role> role) {
    const char* name = role ? role_name(*role) : nullptr;
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, account);
    if (role) {
        Statement statement(state.db.get(), "INSERT INTO main.roles(account_id,role) VALUES(?,?) ON CONFLICT(account_id) DO UPDATE SET role=excluded.role");
        statement.bind(1, account.local_id);
        statement.bind(2, name);
        statement.done();
    } else {
        Statement statement(state.db.get(), "DELETE FROM main.roles WHERE account_id=?");
        statement.bind(1, account.local_id);
        statement.done();
    }
    transaction.commit();
}
std::optional<Role> LocalStore::role(const AccountRef& account) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement statement(state.db.get(), "SELECT role FROM main.roles WHERE account_id=?");
    statement.bind(1, account.local_id);
    std::optional<Role> result;
    if (statement.row()) { result = role_value(statement.text(0, 16)); statement.done(); }
    transaction.commit();
    return result;
}
void LocalStore::set_ban(const AccountRef& account, const Ban& ban) {
    validate_ban(ban, StoreErrorCode::invalid_input);
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), true);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement statement(state.db.get(), "INSERT INTO main.bans(account_id,starts_at,expires_at,reason) VALUES(?,?,?,?) "
        "ON CONFLICT(account_id) DO UPDATE SET starts_at=excluded.starts_at,expires_at=excluded.expires_at,reason=excluded.reason");
    statement.bind(1, account.local_id);
    statement.bind(2, ban.starts_at);
    if (ban.expires_at) statement.bind(3, *ban.expires_at); else statement.bind_null(3);
    statement.bind(4, ban.reason);
    statement.done();
    transaction.commit();
}
std::optional<Ban> LocalStore::ban(const AccountRef& account) const {
    auto& state = state_of(impl_);
    const std::lock_guard lock(state.mutex);
    Transaction transaction(state.db.get(), false);
    state.check();
    require_account(state.db.get(), state.server, account);
    Statement statement(state.db.get(), "SELECT starts_at,expires_at,reason FROM main.bans WHERE account_id=?");
    statement.bind(1, account.local_id);
    std::optional<Ban> result;
    if (statement.row()) {
        Ban saved{statement.integer(0), std::nullopt, statement.text(2, 256)};
        if (!statement.null(1)) saved.expires_at = statement.integer(1);
        validate_ban(saved, StoreErrorCode::invalid_schema);
        result = std::move(saved);
        statement.done();
    }
    transaction.commit();
    return result;
}
bool LocalStore::is_banned(const AccountRef& account, std::int64_t now) const {
    require(now >= 0, StoreErrorCode::invalid_input);
    const auto saved = ban(account);
    return saved && saved->starts_at <= now && (!saved->expires_at || now < *saved->expires_at);
}
}
