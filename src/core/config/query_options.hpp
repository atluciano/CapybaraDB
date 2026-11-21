#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace capybaradb::core {

/// Stored by numeric value in environment.json (0 = ServerDefault, 1 = ReadCommitted, 2 = RepeatableRead, 3 = Serializable).
enum class QueryIsolationLevel : int32_t {
    ServerDefault  = 0,
    ReadCommitted  = 1,
    RepeatableRead = 2,
    Serializable   = 3,
};

/// Query Options -> Results -> Text -> Output format. Stored by numeric value.
enum class TextOutputFormat : int32_t {
    ColumnAligned   = 0,
    CommaDelimited  = 1,
    TabDelimited    = 2,
    SpaceDelimited  = 3,
    CustomDelimited = 4,
};

/// How a Results-to-Text / Results-to-File block is laid out (100% POD).
struct TextResultsFormat {
    static constexpr uint32_t kDefaultMaxCharacters = 256u;
    static constexpr uint32_t kMinMaxCharacters     = 1u;
    static constexpr uint32_t kMaxMaxCharacters     = 8192u;

    TextOutputFormat output               = TextOutputFormat::ColumnAligned;
    char             custom_delimiter     = ';';
    bool             include_headers      = true;
    bool             include_query        = false;
    bool             right_align_numeric  = false;
    uint32_t         max_characters       = kDefaultMaxCharacters;
    bool             nocount              = false;
    bool             show_completion_time = true;

    [[nodiscard]] constexpr char delimiter_char() const noexcept {
        switch (output) {
        case TextOutputFormat::CommaDelimited:  return ',';
        case TextOutputFormat::TabDelimited:    return '\t';
        case TextOutputFormat::SpaceDelimited:  return ' ';
        case TextOutputFormat::CustomDelimited: return custom_delimiter != '\0' ? custom_delimiter : ';';
        case TextOutputFormat::ColumnAligned:
        default:                                return '\0';
        }
    }

    [[nodiscard]] constexpr bool is_delimited() const noexcept {
        return output != TextOutputFormat::ColumnAligned;
    }

    constexpr void normalize_in_place() noexcept {
        switch (output) {
        case TextOutputFormat::ColumnAligned:
        case TextOutputFormat::CommaDelimited:
        case TextOutputFormat::TabDelimited:
        case TextOutputFormat::SpaceDelimited:
        case TextOutputFormat::CustomDelimited:
            break;
        default:
            output = TextOutputFormat::ColumnAligned;
            break;
        }
        if (custom_delimiter == '\0' || custom_delimiter == '\r' || custom_delimiter == '\n') {
            custom_delimiter = ';';
        }
        if (max_characters < kMinMaxCharacters || max_characters > kMaxMaxCharacters) {
            max_characters = kDefaultMaxCharacters;
        }
    }

    [[nodiscard]] constexpr TextResultsFormat normalized() const noexcept {
        TextResultsFormat out = *this;
        out.normalize_in_place();
        return out;
    }
};
static_assert(std::is_trivially_copyable_v<TextResultsFormat>);

/// Thresholds for graphical execution plan discarded-rows warning badge (100% POD).
struct ExecutionPlanWarningOptions {
    static constexpr int32_t kDefaultRatio   = 10;
    static constexpr int32_t kMinRatio       = 2;
    static constexpr int32_t kMaxRatio       = 1000000;
    static constexpr int32_t kDefaultMinimum = 1000;
    static constexpr int32_t kMaxMinimum     = 1000000000;

    int32_t removed_rows_ratio   = kDefaultRatio;
    int32_t removed_rows_minimum = kDefaultMinimum;

    constexpr void normalize_in_place() noexcept {
        if (removed_rows_ratio < kMinRatio || removed_rows_ratio > kMaxRatio) {
            removed_rows_ratio = kDefaultRatio;
        }
        if (removed_rows_minimum < 0 || removed_rows_minimum > kMaxMinimum) {
            removed_rows_minimum = kDefaultMinimum;
        }
    }

    [[nodiscard]] constexpr ExecutionPlanWarningOptions normalized() const noexcept {
        ExecutionPlanWarningOptions out = *this;
        out.normalize_in_place();
        return out;
    }
};
static_assert(std::is_trivially_copyable_v<ExecutionPlanWarningOptions>);

/// Per-tab Query Options (and global defaults stored in environment.json).
/// 100% trivially copyable POD struct (no std::string for 1-char custom_delimiter).
struct QueryOptions {
    static constexpr int32_t  kMaxTimeoutSeconds   = 86400;
    static constexpr int32_t  kMaxLockTimeoutMs    = 86400000;
    static constexpr int32_t  kDefaultGridPageRows = 100000;
    static constexpr int32_t  kMinGridPageRows     = 100;
    static constexpr int32_t  kMaxGridPageRows     = 1000000;
    static constexpr uint32_t kDefaultMaxArenaMiB  = 1024u;
    static constexpr uint32_t kMinMaxArenaMiB      = 64u;
    static constexpr uint32_t kMaxMaxArenaMiB      = 16384u;

    // Execution -> General
    int32_t             execution_timeout_seconds = 0;
    bool                check_open_transactions   = true;

    // Execution -> Advanced
    bool                nocount                   = false;
    bool                show_completion_time      = true;
    QueryIsolationLevel isolation                 = QueryIsolationLevel::ServerDefault;
    int32_t             lock_timeout_ms           = -1; // -1 = server default, 0 = no limit
    int32_t             plan_warning_ratio        = ExecutionPlanWarningOptions::kDefaultRatio;
    int32_t             plan_warning_min_rows     = ExecutionPlanWarningOptions::kDefaultMinimum;

    // Results -> Grid
    bool                retain_line_breaks        = true;
    bool                grid_discard_results      = false;
    bool                display_json_as_link      = true;
    int32_t             grid_page_rows            = kDefaultGridPageRows;
    uint32_t            max_arena_mib             = kDefaultMaxArenaMiB;

    // Results -> Text
    TextOutputFormat    text_format               = TextOutputFormat::ColumnAligned;
    char                custom_delimiter          = ';';
    bool                text_include_headers      = true;
    bool                text_include_query        = false;
    bool                text_right_align_numeric  = false;
    bool                text_discard_results      = false;
    uint32_t            text_max_characters       = TextResultsFormat::kDefaultMaxCharacters;

    constexpr void normalize_in_place() noexcept {
        if (execution_timeout_seconds < 0 || execution_timeout_seconds > kMaxTimeoutSeconds) {
            execution_timeout_seconds = 0;
        }
        switch (isolation) {
        case QueryIsolationLevel::ServerDefault:
        case QueryIsolationLevel::ReadCommitted:
        case QueryIsolationLevel::RepeatableRead:
        case QueryIsolationLevel::Serializable:
            break;
        default:
            isolation = QueryIsolationLevel::ServerDefault;
            break;
        }
        if (lock_timeout_ms < -1 || lock_timeout_ms > kMaxLockTimeoutMs) {
            lock_timeout_ms = -1;
        }
        const auto pw = ExecutionPlanWarningOptions{plan_warning_ratio, plan_warning_min_rows}.normalized();
        plan_warning_ratio    = pw.removed_rows_ratio;
        plan_warning_min_rows = pw.removed_rows_minimum;
        if (grid_page_rows < kMinGridPageRows || grid_page_rows > kMaxGridPageRows) {
            grid_page_rows = kDefaultGridPageRows;
        }
        if (max_arena_mib < kMinMaxArenaMiB || max_arena_mib > kMaxMaxArenaMiB) {
            max_arena_mib = kDefaultMaxArenaMiB;
        }
        switch (text_format) {
        case TextOutputFormat::ColumnAligned:
        case TextOutputFormat::CommaDelimited:
        case TextOutputFormat::TabDelimited:
        case TextOutputFormat::SpaceDelimited:
        case TextOutputFormat::CustomDelimited:
            break;
        default:
            text_format = TextOutputFormat::ColumnAligned;
            break;
        }
        if (custom_delimiter == '\0' || custom_delimiter == '\r' || custom_delimiter == '\n') {
            custom_delimiter = ';';
        }
        if (text_max_characters < TextResultsFormat::kMinMaxCharacters ||
            text_max_characters > TextResultsFormat::kMaxMaxCharacters) {
            text_max_characters = TextResultsFormat::kDefaultMaxCharacters;
        }
    }

    [[nodiscard]] constexpr QueryOptions normalized() const noexcept {
        QueryOptions out = *this;
        out.normalize_in_place();
        return out;
    }

    [[nodiscard]] constexpr ExecutionPlanWarningOptions plan_warnings() const noexcept {
        return ExecutionPlanWarningOptions{plan_warning_ratio, plan_warning_min_rows}.normalized();
    }

    [[nodiscard]] constexpr TextResultsFormat text_layout() const noexcept {
        return TextResultsFormat{
            .output               = text_format,
            .custom_delimiter     = custom_delimiter,
            .include_headers      = text_include_headers,
            .include_query        = text_include_query,
            .right_align_numeric  = text_right_align_numeric,
            .max_characters       = text_max_characters,
            .nocount              = nocount,
            .show_completion_time = show_completion_time,
        }.normalized();
    }
};
static_assert(std::is_trivially_copyable_v<QueryOptions>);

/// Tools -> Options -> Query Execution -> Query History settings (100% POD).
struct QueryHistorySettings {
    static constexpr int32_t kDefaultRetentionDays = 90;
    static constexpr int32_t kMinRetentionDays     = 1;
    static constexpr int32_t kMaxRetentionDays     = 3650;
    static constexpr int32_t kDefaultMaxExecutions = 10000;
    static constexpr int32_t kMinExecutions        = 100;
    static constexpr int32_t kMaxExecutionsLimit   = 1000000;
    static constexpr int32_t kDefaultMaxSqlKiB     = 64;
    static constexpr int32_t kMinSqlKiB            = 1;
    static constexpr int32_t kMaxSqlKiBLimit       = 256;

    bool    enabled        = true;
    int32_t retention_days = kDefaultRetentionDays;
    int32_t max_executions = kDefaultMaxExecutions;
    int32_t max_sql_kib    = kDefaultMaxSqlKiB;

    [[nodiscard]] constexpr size_t max_sql_bytes() const noexcept {
        const int32_t kib = (max_sql_kib >= kMinSqlKiB && max_sql_kib <= kMaxSqlKiBLimit)
            ? max_sql_kib
            : kDefaultMaxSqlKiB;
        return static_cast<size_t>(kib) * 1024u;
    }

    constexpr void normalize_in_place() noexcept {
        if (retention_days < kMinRetentionDays || retention_days > kMaxRetentionDays) {
            retention_days = kDefaultRetentionDays;
        }
        if (max_executions < kMinExecutions || max_executions > kMaxExecutionsLimit) {
            max_executions = kDefaultMaxExecutions;
        }
        if (max_sql_kib < kMinSqlKiB || max_sql_kib > kMaxSqlKiBLimit) {
            max_sql_kib = kDefaultMaxSqlKiB;
        }
    }

    [[nodiscard]] constexpr QueryHistorySettings normalized() const noexcept {
        QueryHistorySettings out = *this;
        out.normalize_in_place();
        return out;
    }
};
static_assert(std::is_trivially_copyable_v<QueryHistorySettings>);

} // namespace capybaradb::core
