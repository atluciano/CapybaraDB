#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace capybaradb::core {

enum class SqlCompletionKind : uint8_t {
    Keyword = 0,
    Schema,
    Table,
    View,
    Column,
    Function,
    Join,
};

struct SqlCompletionItem {
    std::string       text;        // Inserted into editor
    SqlCompletionKind kind = SqlCompletionKind::Keyword;
    std::string       description; // Side tooltip text
    std::string       filter;      // Optional alternate filter key (empty -> use text)
};

struct SqlCompletionResult {
    size_t                         replace_start = 0; // UTF-8 byte offset in text
    std::string                    prefix;
    std::vector<SqlCompletionItem> items;
};

struct SqlCatalogRelation {
    std::string schema;
    std::string name;
    bool        is_view      = false;
    uint32_t    first_column = 0;
    uint32_t    column_count = 0;
};

struct SqlCatalogColumn {
    std::string name;
    std::string type;
    bool        nullable = true;
};

struct SqlCatalogFunction {
    std::string schema;
    std::string name;
    std::string arguments;
    std::string result;
    bool        returns_rows = false;
};

struct SqlCatalogForeignKey {
    std::string              name;
    std::string              schema;
    std::string              table;
    std::vector<std::string> columns;
    std::string              target_schema;
    std::string              target_table;
    std::vector<std::string> target_columns;
};

/// Flat data-oriented database catalog snapshot read once per connection (refreshed via Ctrl+Shift+R).
/// Stores all table/view columns contiguously in `all_columns` and indexes them via `(first_column, column_count)`
/// slices inside `SqlCatalogRelation`, eliminating per-table `std::map` nodes and `std::vector` heap allocations.
struct SqlCompletionCatalog {
    static constexpr size_t kMaxRelations   = 20000u;
    static constexpr size_t kMaxColumns     = 200000u;
    static constexpr size_t kMaxFunctions   = 20000u;
    static constexpr size_t kMaxForeignKeys = 50000u;

    bool                                 is_tsql = false;
    std::string                          database;
    std::vector<std::string>             search_path;
    std::vector<std::string>             schemas;
    std::vector<SqlCatalogRelation>      relations;
    std::vector<SqlCatalogColumn>        all_columns;
    std::vector<SqlCatalogFunction>      functions;
    std::vector<SqlCatalogForeignKey>    foreign_keys;
    uint32_t                             hint_relation_index = 0;

    [[nodiscard]] std::span<const SqlCatalogColumn> relation_columns(size_t rel_index) const noexcept {
        if (rel_index >= relations.size()) {
            return {};
        }
        const SqlCatalogRelation& rel = relations[rel_index];
        if (rel.column_count == 0 ||
            static_cast<size_t>(rel.first_column) + rel.column_count > all_columns.size()) {
            return {};
        }
        return std::span<const SqlCatalogColumn>(
            all_columns.data() + rel.first_column,
            rel.column_count);
    }

    size_t find_or_add_relation(std::string_view schema, std::string_view table, bool is_view = false);

    void append_column(std::string_view schema, std::string_view table, SqlCatalogColumn col);

    void set_relation_columns(
        std::string_view schema,
        std::string_view table,
        std::initializer_list<SqlCatalogColumn> cols);
};

inline constexpr size_t kMaxSqlCompletionTextBytes = 4ull * 1024u * 1024u;

/// Quotes an identifier according to PostgreSQL ("...") or T-SQL ([...]) rules when needed.
[[nodiscard]] std::string quote_sql_identifier(std::string_view name, bool is_tsql = false);

/// Computes context-aware completion candidates at `caret_utf8` byte offset.
/// Returns std::nullopt when caret is inside a string, comment, or dollar-quoted literal.
[[nodiscard]] std::optional<SqlCompletionResult> complete_sql(
    std::string_view text,
    size_t caret_utf8,
    const SqlCompletionCatalog& catalog);

/// Filters and ranks completion items accent-insensitively by `prefix` (prefix matches first, then substring matches).
[[nodiscard]] std::vector<SqlCompletionItem> filter_sql_completion_items(
    const std::vector<SqlCompletionItem>& items,
    std::string_view prefix);

} // namespace capybaradb::core
