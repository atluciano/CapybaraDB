#pragma once

#include "core/config/query_options.hpp"
#include "core/result/result_set_storage.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace capybaradb::core {

/// Inclusive rectangular range in view row indices and column ordinals.
struct CellRange {
    int64_t row_start = 0;
    int64_t row_end   = -1;
    int32_t col_start = 0;
    int32_t col_end   = -1;

    [[nodiscard]] constexpr bool contains(int64_t row, int32_t col_ordinal) const noexcept {
        return row >= row_start && row <= row_end && col_ordinal >= col_start && col_ordinal <= col_end;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return row_start <= row_end && col_start <= col_end;
    }
};

struct ClipboardCopyOptions {
    std::span<const CellRange> ranges;
    std::span<const uint32_t>  display_order;      // Column ordinals in display order (empty = 0..col_count-1)
    std::span<const uint32_t>  row_order;          // View row -> physical row indirection (empty = identity)
    bool                       include_headers    = false;
    bool                       retain_line_breaks = true;
};

inline constexpr uint64_t kMaxClipboardCopyCells = 250000ull;
inline constexpr size_t   kMaxClipboardCopyBytes = 4ull * 1024 * 1024; // 4 MiB

/// Two-pass exact-size TSV clipboard builder directly reading ResultSetStorage.
/// Pass 1 validates cell count (<= 250,000) and computes exact UTF-8 byte length (<= 4 MiB)
/// BEFORE allocating any output buffer.
/// Pass 2 performs a single reservation and copies directly from Arena string_views.
/// Single-cell selection without headers produces pure value with no trailing CRLF or quotes.
[[nodiscard]] std::expected<std::string, std::string> build_clipboard_tsv(
    const ResultSetStorage& rs,
    const ClipboardCopyOptions& opts);

/// Formats a QueryExecutionResult directly from its Arena storage into Results-to-Text output
/// using full CapybaraDB TextResultsFormat options (ColumnAligned, Comma, Tab, Space, Custom).
[[nodiscard]] std::string format_results_as_text(
    const QueryExecutionResult& result,
    double elapsed_seconds,
    const TextResultsFormat& format,
    const std::optional<std::string>& error_text = std::nullopt,
    std::string_view query_sql = {},
    uint64_t max_rows_per_result = 100000ull);

/// Convenience overload for simple Results-to-Text calls.
[[nodiscard]] std::string format_results_as_text(
    const QueryExecutionResult& result,
    double elapsed_seconds,
    bool show_completion_time,
    bool nocount,
    const std::optional<std::string>& error_text);

/// Truncates a UTF-8 string to at most `max_chars` Unicode codepoints (never splitting UTF-8 sequences).
[[nodiscard]] std::string truncate_utf8_codepoints(
    std::string_view text,
    size_t max_chars,
    bool flatten_line_breaks = true);

/// Counts UTF-8 Unicode codepoints in a string_view (for column alignment widths).
[[nodiscard]] size_t count_utf8_codepoints(std::string_view text) noexcept;

/// Returns true if the column logical type / type_name is numeric for right-alignment in Results to Text.
[[nodiscard]] bool is_numeric_column(const ColumnMeta& col) noexcept;

/// Returns true if trimmed text starts with '{' or '[' and parses as a valid JSON object/array.
[[nodiscard]] bool is_valid_json_document(std::string_view text) noexcept;

/// Pretty-prints a valid JSON object/array with 2-space indentation.
[[nodiscard]] std::optional<std::string> pretty_print_json(std::string_view text);

/// Compares two physical rows of a ResultSetStorage on column `col_ordinal` for local sorting.
/// NULL always sorts first in ascending order (last in descending order).
/// Int / Float / Numeric columns compare numerically before falling back to lexical order.
[[nodiscard]] int compare_result_cells(
    const ResultSetStorage& rs,
    uint64_t phys_row_a,
    uint64_t phys_row_b,
    uint32_t col_ordinal,
    bool descending) noexcept;

/// Returns current local timestamp formatted as "Completion time: YYYY-MM-DDTHH:MM:SS...".
[[nodiscard]] std::string format_completion_time_line();

} // namespace capybaradb::core
