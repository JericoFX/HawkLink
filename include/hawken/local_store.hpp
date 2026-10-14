#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hawken::store {
enum class StoreErrorCode {
    invalid_input, invalid_scope, not_found, conflict,
    unsupported_schema, invalid_schema, busy, io_error
};

class StoreError : public std::runtime_error {
public:
    explicit StoreError(StoreErrorCode code);
    StoreErrorCode code() const noexcept;
private:
    StoreErrorCode code_;
};

// Logical database scope, not an authentication credential. Clones sharing the
// same server_id cannot be distinguished by this reference.
struct AccountRef {
    std::string server_id;
    std::int64_t local_id = 0;
    bool operator==(const AccountRef&) const = default;
};
struct Account {
    AccountRef ref;
    std::string steam_id64;
    std::int64_t created_at = 0;
    bool operator==(const Account&) const = default;
};
struct WalletBalances {
    std::int32_t hawken_points = 0;
    std::int32_t meteor_points = 0;
    bool operator==(const WalletBalances&) const = default;
};
struct StarterItem {
    std::string type_id;
    std::string instance_id;
    bool operator==(const StarterItem&) const = default;
};
// New server policy. Slots are mech, primary, secondary, upper, middle, arms,
// lower, drone, ability. The caller binds type IDs to the verified catalogue.
using StarterItems = std::array<StarterItem, 9>;
using GameSettings = std::map<std::string, std::string, std::less<>>;
using ItemAttributes = std::map<std::string, std::string, std::less<>>;
struct StarterMechSnapshot {
    StarterItems items;
    ItemAttributes attributes;
};
struct Alias {
    std::string value;
    std::int64_t first_seen = 0;
    std::int64_t last_seen = 0;
    bool operator==(const Alias&) const = default;
};
struct Visit {
    std::string visit_id;
    AccountRef account;
    std::string alias;
    std::int64_t occurred_at = 0;
    bool operator==(const Visit&) const = default;
};
enum class Role { owner, moderator, analyst };
struct Ban {
    std::int64_t starts_at = 0;
    std::optional<std::int64_t> expires_at;
    std::string reason;
    bool operator==(const Ban&) const = default;
};

// Trusted internal storage API: callers must authenticate Steam identities and
// authorize administrative actions before invoking it. No public endpoint.
// IDs: [A-Za-z0-9_.-], 1..64 bytes, excluding "." and "..". Steam IDs:
// canonical nonzero uint64 decimal strings, format only. Aliases: valid UTF-8,
// 1..128 bytes. Reasons: valid UTF-8, at most 256 bytes. Both reject Unicode
// C0/DEL/C1 controls, including NUL. All timestamps are nonnegative.
// A private mutex serializes complete operations on the connection. Move or
// destruction requires callers to have stopped using the instance.
class LocalStore {
public:
    LocalStore(const std::filesystem::path& database_path, std::string server_id);
    ~LocalStore();
    LocalStore(LocalStore&&) noexcept;
    LocalStore& operator=(LocalStore&&) noexcept;
    LocalStore(const LocalStore&) = delete;
    LocalStore& operator=(const LocalStore&) = delete;

    int schema_version() const;
    Account get_or_create_account(std::string_view steam_id64, std::int64_t created_at);
    std::optional<Account> find_account(std::string_view steam_id64) const;
    // SQL balances belong to this account/server. New and migrated accounts
    // start at zero; reads do not grant credits or enable payment services.
    // Both wire values are nonnegative int32. Gameplay ledger writes are separate.
    WalletBalances wallet(const AccountRef& account) const;
    // Opaque client-received settings only: no consent/progress interpretation,
    // credits, stats or inventory grant. Reads never insert defaults.
    // New bounded server policy: 256 pairs/update; UTF8 keys1..256 bytes without
    // C0/DEL/C1; values0..16384 bytes with escaped controls preserved. NUL is
    // forbidden in both. Per account<=1024 keys and<=262144 key+value bytes.
    GameSettings game_settings(const AccountRef& account) const;
    // Merge atomically, preserving keys absent from a SDK batch. False for
    // identical replay or empty update. All validation precedes SQL commit.
    bool merge_game_settings(const AccountRef& account, const GameSettings& update);
    // Reads never grant or repair inventory. A recorded grant with missing or
    // invalid rows fails closed. IDs are canonical nonzero lower-case GUIDs;
    // all 18 IDs in a grant are distinct. Instance IDs are unique in this DB
    // and cannot collide with any stored type ID (or vice versa).
    std::optional<StarterItems> starter_items(const AccountRef& account) const;
    // Trusted read-only snapshot of the existing starter mech (slot0), all nine
    // owned types/instances and this mech's raw saved attributes in ONE SQL read
    // transaction. No grant/repair/defaults/rendering or gameplay interpretation.
    // Missing grant, foreign/unknown instance or another owned slot: not_found;
    // malformed instance: invalid_input; existing scope/schema checks remain.
    StarterMechSnapshot owned_starter_mech(const AccountRef& account, std::string_view instance_id) const;
    // Trusted internal policy only, not a public grant/purchase endpoint.
    // Grants all nine rows atomically. False means an identical replay; any
    // changed payload conflicts. Does not alter wallets or existing grants.
    bool grant_starter_items(const AccountRef& account, const StarterItems& items);
    // Mutable opaque strings for an ALREADY owned instance, never a grant.
    // Both operations verify account, complete starter grant and canonical
    // nonzero instance GUID inside the store transaction. Unknown or foreign
    // instances fail not_found; a corrupt/incomplete grant fails invalid_schema.
    // Read with no attributes returns empty. Keys must start exactly "Preset."
    // and follow settings UTF8/control/byte rules. Values may be empty; native
    // SDK omission of empty strings is a producer rule, not an invented delete.
    // Per instance: 256 pairs/update, 1024 stored keys, 262144 key+value bytes.
    // Merge preserves omitted keys; empty/identical replay returns false. Never
    // changes catalogue/instance IDs, graph, wallets or progression authority.
    // Preset.ProgressionUnlockFlags/BonusXpMultiplier remain opaque text: saving
    // them grants no entitlements and changes no wallet or XP ledger. Caller
    // validates semantic equipment choices/references; no value interpretation.
    ItemAttributes owned_item_attributes(const AccountRef& account, std::string_view instance_id) const;
    bool merge_owned_item_attributes(const AccountRef& account, std::string_view instance_id,
                                     const ItemAttributes& update);
    // Returns false only for an identical replay. A changed payload conflicts;
    // both the visit and alias observation are committed atomically.
    bool record_visit(const Visit& visit);
    // Limits 1..256. Aliases: last_seen descending, value ascending. Visits:
    // occurred_at descending, visit_id ascending (ties are deterministic).
    std::vector<Alias> aliases(const AccountRef& account, std::size_t limit = 64) const;
    std::vector<Visit> visits(const AccountRef& account, std::size_t limit = 64) const;
    void set_role(const AccountRef& account, std::optional<Role> role);
    std::optional<Role> role(const AccountRef& account) const;
    void set_ban(const AccountRef& account, const Ban& ban);
    std::optional<Ban> ban(const AccountRef& account) const;
    // Starts inclusively; expires exclusively. No implicit system clock.
    bool is_banned(const AccountRef& account, std::int64_t now) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
