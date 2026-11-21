#pragma once

#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "ui/grid/column_metrics.hpp"
#include "ui/grid/grid_selection_model.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace capybaradb::ui::grid {

/// Lightweight per-ResultSet view state so switching Result 1 / Result 2 / ... in a single
/// ResultGridViewport preserves scroll, selection, column widths, and sort order in O(1).
struct GridViewState {
    ColumnMetrics       columns;
    GridSelectionModel  selection;
    int64_t             vscroll_row        = 0;
    double              hscroll_px         = 0.0;
    uint64_t            auto_sized_rows    = 0;
    bool                columns_initialized = false;
    bool                first_row_focused  = false;
    int32_t             sorted_col_ordinal = -1;
    SortDirection       sort_dir           = SortDirection::None;
    std::vector<uint32_t> row_order; // Empty = identity order; otherwise view_row -> phys_row
    std::unordered_map<uint64_t, bool> json_doc_cache;

    void reset() {
        columns.clear();
        selection.clear(true);
        vscroll_row         = 0;
        hscroll_px          = 0.0;
        auto_sized_rows     = 0;
        columns_initialized = false;
        first_row_focused   = false;
        sorted_col_ordinal  = -1;
        sort_dir            = SortDirection::None;
        row_order.clear();
        json_doc_cache.clear();
    }

    [[nodiscard]] uint64_t resolve_physical_row(uint64_t view_row) const noexcept {
        if (!row_order.empty() && view_row < row_order.size()) {
            return row_order[static_cast<size_t>(view_row)];
        }
        return view_row;
    }

    /// Toggles local sort on `col_ordinal` (ASC <-> DESC) via indirection vector without mutating the Arena.
    void toggle_sort(const core::ResultSetStorage& rs, uint32_t col_ordinal) {
        if (col_ordinal >= rs.col_count) {
            return;
        }
        const uint64_t nrows = rs.published_rows.load(std::memory_order_acquire);
        if (sorted_col_ordinal == static_cast<int32_t>(col_ordinal) && sort_dir == SortDirection::Ascending) {
            sort_dir = SortDirection::Descending;
        } else {
            sorted_col_ordinal = static_cast<int32_t>(col_ordinal);
            sort_dir = SortDirection::Ascending;
        }
        columns.set_sort(sorted_col_ordinal, sort_dir);

        if (nrows == 0) {
            row_order.clear();
            return;
        }

        const size_t n = static_cast<size_t>(nrows);
        row_order.resize(n);
        std::iota(row_order.begin(), row_order.end(), 0u);
        const bool desc = (sort_dir == SortDirection::Descending);
        std::stable_sort(row_order.begin(), row_order.end(), [&](uint32_t a, uint32_t b) {
            return core::compare_result_cells(rs, a, b, col_ordinal, desc) < 0;
        });
    }

    /// Cached JSON document check keyed by (phys_row << 16) | col_ordinal.
    [[nodiscard]] bool is_cached_json_doc(uint64_t phys_row, uint32_t col_ordinal, std::string_view text) {
        const uint64_t key = (phys_row << 16u) | (col_ordinal & 0xFFFFu);
        if (const auto it = json_doc_cache.find(key); it != json_doc_cache.end()) {
            return it->second;
        }
        if (json_doc_cache.size() >= 4096u) {
            json_doc_cache.clear();
        }
        const bool valid = core::is_valid_json_document(text);
        json_doc_cache.emplace(key, valid);
        return valid;
    }
};

} // namespace capybaradb::ui::grid
