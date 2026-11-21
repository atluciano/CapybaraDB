#pragma once

#include "core/config/query_options.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::core {

enum class QueryHistoryOutcome : int32_t {
    Succeeded = 0,
    Failed    = 1,
    Canceled  = 2,
};

enum class QueryHistoryOrigin : int32_t {
    Typed         = 0,
    QueryDesigner = 1,
    Ai            = 2,
    Other         = 3,
};

struct QueryHistoryKey {
    std::string server;
    int32_t     port = 5432;
    std::string database;
    std::string hash;

    bool operator==(const QueryHistoryKey&) const noexcept = default;
};

struct QueryHistoryEntry {
    static constexpr size_t kMaxErrorChars = 4096u;
    static constexpr size_t kMaxNameChars  = 1024u;

    std::string                id;
    int64_t                    started_utc_ms = 0; // Unix epoch ms (UTC)
    int64_t                    duration_ms    = 0;
    std::string                engine         = "PostgreSql";
    std::string                server;
    int32_t                    port           = 5432;
    std::string                database;
    std::string                user;
    std::string                sql;
    size_t                     original_length = 0;
    std::string                hash; // 64-char uppercase hex SHA-256 of normalized SQL
    QueryHistoryOrigin         origin  = QueryHistoryOrigin::Typed;
    QueryHistoryOutcome        outcome = QueryHistoryOutcome::Succeeded;
    std::optional<int64_t>     rows_returned;
    std::optional<int64_t>     rows_affected;
    std::optional<std::string> sql_state;
    std::optional<std::string> error;

    [[nodiscard]] bool truncated() const noexcept { return sql.size() < original_length; }
    [[nodiscard]] QueryHistoryKey key() const { return QueryHistoryKey{server, port, database, hash}; }
    [[nodiscard]] std::string reopen_text() const;
};

struct QueryHistoryCapture {
    std::string                sql;
    std::string                engine = "PostgreSql";
    std::string                server;
    int32_t                    port = 5432;
    std::string                database;
    std::string                user;
    int64_t                    started_utc_ms = 0;
    int64_t                    duration_ms    = 0;
    QueryHistoryOrigin         origin         = QueryHistoryOrigin::Typed;
    QueryHistoryOutcome        outcome        = QueryHistoryOutcome::Succeeded;
    std::optional<int64_t>     rows_returned;
    std::optional<int64_t>     rows_affected;
    std::optional<std::string> sql_state;
    std::optional<std::string> error;
};

struct QueryHistorySnapshot {
    std::vector<QueryHistoryEntry> entries;
    std::vector<QueryHistoryKey>   pins;
};

struct QueryHistoryGroup {
    QueryHistoryKey                key;
    std::vector<QueryHistoryEntry> executions; // Newest first
    bool                           pinned = false;

    [[nodiscard]] const QueryHistoryEntry& latest() const noexcept { return executions.front(); }
};

// ---------------------------------------------------------------------------
// Query History text normalization, hashing & UTF-8 slicing (free functions)
// ---------------------------------------------------------------------------

/// Collapses whitespace runs into single spaces and trims both ends; preserves case.
[[nodiscard]] std::string normalize_query_history_sql(std::string_view sql);

/// Uppercase 64-character hex SHA-256 digest of normalize_query_history_sql(sql).
[[nodiscard]] std::string hash_query_history_sql(std::string_view sql);

/// Truncates UTF-8 string to at most max_bytes without splitting a multi-byte sequence.
[[nodiscard]] std::string cut_utf8_bytes(std::string_view text, size_t max_bytes);

/// Single-line preview for list/grid display.
[[nodiscard]] std::string preview_query_history_sql(std::string_view sql, size_t max_bytes = 200u);

// ---------------------------------------------------------------------------
// Local, user-encrypted (DPAPI / libsecret) Query History persistence
// ---------------------------------------------------------------------------

[[nodiscard]] int64_t current_utc_ms() noexcept;

[[nodiscard]] QueryHistorySnapshot load_query_history(const std::filesystem::path& path);
[[nodiscard]] std::vector<QueryHistoryGroup> build_query_history_groups(const QueryHistorySnapshot& snapshot);
[[nodiscard]] std::vector<QueryHistoryGroup> load_query_history_groups(const std::filesystem::path& path);

bool record_query_history_entry(
    const std::filesystem::path& path,
    const QueryHistoryCapture& capture,
    const QueryHistorySettings& settings,
    int64_t now_utc_ms = 0);

bool prune_query_history(
    const std::filesystem::path& path,
    const QueryHistorySettings& settings,
    int64_t now_utc_ms = 0);

bool toggle_query_history_pin(
    const std::filesystem::path& path,
    const QueryHistoryKey& key);

bool delete_query_history_entry(
    const std::filesystem::path& path,
    std::string_view entry_id);

bool delete_query_history_group(
    const std::filesystem::path& path,
    const QueryHistoryKey& key);

bool clear_query_history(const std::filesystem::path& path);

} // namespace capybaradb::core
