#pragma once

#include "core/result/result_formatter.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace capybaradb::ui::grid {

struct GridCellPos {
    int64_t row         = 0;
    int32_t col_ordinal = 0;

    constexpr bool operator==(const GridCellPos&) const noexcept = default;
};

/// Inclusive rectangular selection range in view row indices and column ordinals.
using GridSelectionRange = core::CellRange;

[[nodiscard]] constexpr GridSelectionRange normalize_range(
    int64_t r1, int64_t r2, int32_t c1, int32_t c2) noexcept {
    return GridSelectionRange{
        .row_start = (r1 < r2) ? r1 : r2,
        .row_end   = (r1 > r2) ? r1 : r2,
        .col_start = (c1 < c2) ? c1 : c2,
        .col_end   = (c1 > c2) ? c1 : c2,
    };
}

/// Mathematical multi-range selection model with O(1) SelectAll and O(1) adjacent strip merging.
class GridSelectionModel final {
public:
    [[nodiscard]] const std::vector<GridSelectionRange>& ranges() const noexcept { return ranges_; }
    [[nodiscard]] bool has_selection() const noexcept { return !ranges_.empty(); }

    [[nodiscard]] const std::optional<GridCellPos>& current_cell() const noexcept { return current_cell_; }
    void set_current_cell(std::optional<GridCellPos> pos) noexcept { current_cell_ = pos; }

    [[nodiscard]] const std::optional<GridCellPos>& anchor_cell() const noexcept { return anchor_cell_; }
    void set_anchor_cell(std::optional<GridCellPos> pos) noexcept { anchor_cell_ = pos; }

    void clear(bool clear_current_cell = false) noexcept {
        ranges_.clear();
        if (clear_current_cell) {
            current_cell_.reset();
            anchor_cell_.reset();
        }
    }

    void select_single_cell(int64_t row, int32_t col_ordinal) {
        ranges_.clear();
        ranges_.push_back(GridSelectionRange{row, row, col_ordinal, col_ordinal});
        current_cell_ = GridCellPos{row, col_ordinal};
        anchor_cell_  = GridCellPos{row, col_ordinal};
    }

    void select_range(int64_t r1, int32_t c1, int64_t r2, int32_t c2, bool keep_existing = false) {
        if (!keep_existing) {
            ranges_.clear();
        }
        const GridSelectionRange norm = normalize_range(r1, r2, c1, c2);
        if (keep_existing && !ranges_.empty()) {
            ranges_.back() = norm;
        } else {
            ranges_.push_back(norm);
        }
        current_cell_ = GridCellPos{r2, c2};
    }

    void start_additive_range(int64_t row, int32_t col_ordinal) {
        ranges_.push_back(GridSelectionRange{row, row, col_ordinal, col_ordinal});
        anchor_cell_  = GridCellPos{row, col_ordinal};
        current_cell_ = GridCellPos{row, col_ordinal};
    }

    /// O(1) Select All represented as a single inclusive rectangle.
    void select_all(int64_t row_count, int32_t col_count) {
        ranges_.clear();
        if (row_count > 0 && col_count > 0) {
            ranges_.push_back(GridSelectionRange{0, row_count - 1, 0, col_count - 1});
            if (!current_cell_.has_value()) {
                current_cell_ = GridCellPos{0, 0};
            }
            if (!anchor_cell_.has_value()) {
                anchor_cell_ = GridCellPos{0, 0};
            }
        }
    }

    /// Adds a single cell and merges adjacent strips in O(1).
    void add_cell(int64_t row, int32_t col_ordinal) {
        if (row < 0 || col_ordinal < 0) {
            return;
        }
        if (!ranges_.empty()) {
            GridSelectionRange& last = ranges_.back();
            if (last.contains(row, col_ordinal)) {
                return;
            }
            if (last.col_start == col_ordinal && last.col_end == col_ordinal && row == last.row_end + 1) {
                last.row_end = row;
                try_merge_last_two_by_column();
                return;
            }
            if (last.row_start == row && last.row_end == row && col_ordinal == last.col_end + 1) {
                last.col_end = col_ordinal;
                try_merge_last_two_by_row();
                return;
            }
        }
        ranges_.push_back(GridSelectionRange{row, row, col_ordinal, col_ordinal});
        try_merge_last_two_by_column();
        try_merge_last_two_by_row();
    }

    [[nodiscard]] bool is_cell_selected(int64_t row, int32_t col_ordinal) const noexcept {
        for (const auto& r : ranges_) {
            if (r.contains(row, col_ordinal)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_row_selected(int64_t row) const noexcept {
        for (const auto& r : ranges_) {
            if (row >= r.row_start && row <= r.row_end) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] uint64_t selected_cell_count(int64_t max_rows, int32_t max_cols) const {
        if (ranges_.empty() || max_rows <= 0 || max_cols <= 0) {
            return 0;
        }
        auto clamp_range = [&](const GridSelectionRange& r) -> std::optional<GridSelectionRange> {
            const int64_t r0 = (std::max<int64_t>)(0, r.row_start);
            const int64_t r1 = (std::min<int64_t>)(max_rows - 1, r.row_end);
            const int32_t c0 = (std::max<int32_t>)(0, r.col_start);
            const int32_t c1 = (std::min<int32_t>)(max_cols - 1, r.col_end);
            if (r0 > r1 || c0 > c1) {
                return std::nullopt;
            }
            return GridSelectionRange{r0, r1, c0, c1};
        };

        if (ranges_.size() == 1) {
            if (const auto c = clamp_range(ranges_[0])) {
                return static_cast<uint64_t>(c->row_end - c->row_start + 1) *
                       static_cast<uint64_t>(c->col_end - c->col_start + 1);
            }
            return 0;
        }

        uint64_t sum = 0;
        bool overlaps = false;
        for (size_t i = 0; i < ranges_.size(); ++i) {
            const auto ci = clamp_range(ranges_[i]);
            if (!ci) continue;
            sum += static_cast<uint64_t>(ci->row_end - ci->row_start + 1) *
                   static_cast<uint64_t>(ci->col_end - ci->col_start + 1);
            if (!overlaps && ranges_.size() <= 64) {
                for (size_t j = 0; j < i; ++j) {
                    const auto cj = clamp_range(ranges_[j]);
                    if (!cj) continue;
                    if (ci->row_start <= cj->row_end && ci->row_end >= cj->row_start &&
                        ci->col_start <= cj->col_end && ci->col_end >= cj->col_start) {
                        overlaps = true;
                        break;
                    }
                }
            }
        }
        if (!overlaps) {
            return sum;
        }

        std::unordered_set<uint64_t> unique;
        for (const auto& raw : ranges_) {
            const auto ci = clamp_range(raw);
            if (!ci) continue;
            for (int64_t r = ci->row_start; r <= ci->row_end; ++r) {
                for (int32_t c = ci->col_start; c <= ci->col_end; ++c) {
                    unique.insert((static_cast<uint64_t>(r) << 16u) | static_cast<uint32_t>(c));
                }
            }
        }
        return static_cast<uint64_t>(unique.size());
    }

private:
    void try_merge_last_two_by_column() noexcept {
        if (ranges_.size() < 2) {
            return;
        }
        GridSelectionRange& prev = ranges_[ranges_.size() - 2];
        const GridSelectionRange& curr = ranges_.back();
        if (prev.row_start == curr.row_start && prev.row_end == curr.row_end &&
            curr.col_start == prev.col_end + 1) {
            prev.col_end = curr.col_end;
            ranges_.pop_back();
        }
    }

    void try_merge_last_two_by_row() noexcept {
        if (ranges_.size() < 2) {
            return;
        }
        GridSelectionRange& prev = ranges_[ranges_.size() - 2];
        const GridSelectionRange& curr = ranges_.back();
        if (prev.col_start == curr.col_start && prev.col_end == curr.col_end &&
            curr.row_start == prev.row_end + 1) {
            prev.row_end = curr.row_end;
            ranges_.pop_back();
        }
    }

    std::vector<GridSelectionRange> ranges_;
    std::optional<GridCellPos>      current_cell_;
    std::optional<GridCellPos>      anchor_cell_;
};

} // namespace capybaradb::ui::grid
