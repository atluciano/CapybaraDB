#pragma once

#include "core/config/query_options.hpp"
#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/cancel_token.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::core {

enum class ExportFormat : uint8_t {
    Csv  = 0,
    Json = 1,
};

enum class ExportScope : uint8_t {
    AllDisplayedRows = 0,
    SelectedRows     = 1,
};

struct ExportOptions {
    ExportFormat            format             = ExportFormat::Csv;
    ExportScope             scope              = ExportScope::AllDisplayedRows;
    char                    csv_delimiter      = ',';  // ',', ';', or '\t'
    bool                    include_headers    = true;
    bool                    utf8_bom           = false;
    bool                    retain_line_breaks = true;
    bool                    overwrite          = true;
    std::vector<CellRange>  selected_ranges;           // Used when scope == SelectedRows
    std::vector<uint32_t>   display_order;             // Empty = 0..col_count-1
    std::vector<uint32_t>   row_order;                 // Empty = identity view->physical order
};

struct ExportSummary {
    uint64_t rows_written  = 0;
    uint64_t bytes_written = 0;
};

inline constexpr size_t kExportStreamBufferBytes = 64u * 1024u;

using ExportProgressFn = void (*)(uint64_t rows_written, uint64_t total_rows, void* user_data);

/// Zero-snapshot streaming file exporter reading directly from ResultSetStorage / QueryExecutionResult.
/// Uses a 64 KiB output buffer and atomic `<dest>.<id>.partial` + rename semantics.
[[nodiscard]] std::expected<ExportSummary, std::string> export_result_set(
    const ResultSetStorage& rs,
    const std::filesystem::path& destination,
    const ExportOptions& opts,
    CancelToken& cancel,
    ExportProgressFn on_progress = nullptr,
    void* progress_user_data = nullptr);

/// Streams a QueryExecutionResult into a CapybaraDB `.rpt` file (UTF-8 with BOM + CRLF).
[[nodiscard]] std::expected<ExportSummary, std::string> write_results_to_rpt_file(
    const QueryExecutionResult& result,
    const std::filesystem::path& destination,
    const TextResultsFormat& format,
    double elapsed_seconds,
    const std::optional<std::string>& error_text,
    std::string_view query_sql,
    CancelToken& cancel,
    ExportProgressFn on_progress = nullptr,
    void* progress_user_data = nullptr);

} // namespace capybaradb::core
