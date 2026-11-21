#pragma once

#include "core/result/result_set_storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace capybaradb::ui::grid {

enum class SortDirection : uint8_t {
    None = 0,
    Ascending,
    Descending,
};

struct GridColumnInfo {
    uint32_t              ordinal         = 0;
    uint32_t              display_index   = UINT32_MAX;
    std::string           caption;
    core::LogicalTypeKind logical_type    = core::LogicalTypeKind::Unknown;
    bool                  may_hold_json   = false;
    float                 width           = 64.0f;
    bool                  visible         = true;
    bool                  user_resized    = false;
    bool                  header_measured = false;
    SortDirection         sort_dir        = SortDirection::None;
};

/// Maintains column widths, display order, visibility, and prefix-sum offsets
/// for O(log M) visible range, column hit-test, and divider hit-test queries.
class ColumnMetrics final {
public:
    static constexpr float kDefaultMinAutoWidth = 42.0f;
    static constexpr float kDefaultMaxAutoWidth = 320.0f;
    static constexpr float kAutoFitMinWidth     = 28.0f;
    static constexpr float kAutoFitMaxWidth     = 640.0f;
    static constexpr float kMinResizeWidth      = 24.0f;
    static constexpr float kMinInternalWidth    = 20.0f;
    static constexpr float kDividerHitTolerance = 4.0f;

    void clear() {
        columns_.clear();
        visible_ordinals_.clear();
        ordinal_to_visible_.clear();
        offsets_.assign(1, 0.0f);
        dirty_ = false;
    }

    void init_from_storage(const core::ResultSetStorage& rs) {
        clear();
        columns_.reserve(rs.col_count);
        for (uint32_t c = 0; c < rs.col_count; ++c) {
            GridColumnInfo info;
            info.ordinal       = c;
            info.display_index = c;
            if (rs.columns != nullptr) {
                info.caption       = std::string(rs.columns[c].name);
                info.logical_type  = rs.columns[c].logical_type;
                info.may_hold_json = rs.columns[c].may_hold_json;
            }
            columns_.push_back(std::move(info));
        }
        dirty_ = true;
        ensure_built();
    }

    void add_column(GridColumnInfo col) {
        const uint32_t ord = static_cast<uint32_t>(columns_.size());
        col.ordinal = ord;
        if (col.display_index >= columns_.size() + 1) {
            col.display_index = ord;
        }
        columns_.push_back(std::move(col));
        dirty_ = true;
    }

    [[nodiscard]] size_t column_count() const noexcept { return columns_.size(); }
    [[nodiscard]] GridColumnInfo& column(uint32_t ordinal) noexcept { return columns_[ordinal]; }
    [[nodiscard]] const GridColumnInfo& column(uint32_t ordinal) const noexcept { return columns_[ordinal]; }

    void set_column_width(uint32_t ordinal, float width, bool by_user) noexcept {
        if (ordinal >= columns_.size()) {
            return;
        }
        const float min_w = by_user ? kMinResizeWidth : kMinInternalWidth;
        const float clamped = (std::max)(min_w, std::round(width));
        GridColumnInfo& col = columns_[ordinal];
        if (by_user) {
            col.user_resized = true;
        }
        if (std::abs(col.width - clamped) < 0.01f) {
            return;
        }
        col.width = clamped;
        dirty_ = true;
    }

    void set_column_visible(uint32_t ordinal, bool visible) noexcept {
        if (ordinal >= columns_.size() || columns_[ordinal].visible == visible) {
            return;
        }
        columns_[ordinal].visible = visible;
        dirty_ = true;
    }

    void set_display_index(uint32_t ordinal, uint32_t new_display_index) {
        if (ordinal >= columns_.size() || columns_.empty()) {
            return;
        }
        const uint32_t clamped = (std::min)(new_display_index, static_cast<uint32_t>(columns_.size() - 1));
        const uint32_t old_idx = columns_[ordinal].display_index;
        if (old_idx == clamped) {
            return;
        }
        for (auto& c : columns_) {
            if (c.ordinal == ordinal) {
                continue;
            }
            if (old_idx < clamped) {
                if (c.display_index > old_idx && c.display_index <= clamped) {
                    --c.display_index;
                }
            } else {
                if (c.display_index >= clamped && c.display_index < old_idx) {
                    ++c.display_index;
                }
            }
        }
        columns_[ordinal].display_index = clamped;
        dirty_ = true;
    }

    void set_sort(int32_t ordinal, SortDirection dir) noexcept {
        for (auto& c : columns_) {
            c.sort_dir = (static_cast<int32_t>(c.ordinal) == ordinal) ? dir : SortDirection::None;
        }
    }

    void invalidate() noexcept { dirty_ = true; }

    void ensure_built() const {
        if (!dirty_) {
            return;
        }
        visible_ordinals_.clear();
        visible_ordinals_.reserve(columns_.size());
        for (const auto& c : columns_) {
            if (c.visible) {
                visible_ordinals_.push_back(c.ordinal);
            }
        }
        std::sort(visible_ordinals_.begin(), visible_ordinals_.end(), [this](uint32_t a, uint32_t b) {
            if (columns_[a].display_index != columns_[b].display_index) {
                return columns_[a].display_index < columns_[b].display_index;
            }
            return a < b;
        });

        ordinal_to_visible_.assign(columns_.size(), -1);
        offsets_.resize(visible_ordinals_.size() + 1);
        offsets_[0] = 0.0f;
        for (size_t i = 0; i < visible_ordinals_.size(); ++i) {
            const uint32_t ord = visible_ordinals_[i];
            ordinal_to_visible_[ord] = static_cast<int32_t>(i);
            offsets_[i + 1] = offsets_[i] + columns_[ord].width;
        }
        dirty_ = false;
    }

    [[nodiscard]] const std::vector<uint32_t>& visible_ordinals() const {
        ensure_built();
        return visible_ordinals_;
    }

    [[nodiscard]] uint32_t visible_count() const {
        ensure_built();
        return static_cast<uint32_t>(visible_ordinals_.size());
    }

    [[nodiscard]] float total_width() const {
        ensure_built();
        return offsets_.empty() ? 0.0f : offsets_.back();
    }

    [[nodiscard]] float column_left(uint32_t vis_idx) const {
        ensure_built();
        return (vis_idx < visible_ordinals_.size()) ? offsets_[vis_idx] : total_width();
    }

    [[nodiscard]] float column_right(uint32_t vis_idx) const {
        ensure_built();
        return (vis_idx < visible_ordinals_.size()) ? offsets_[vis_idx + 1] : total_width();
    }

    [[nodiscard]] int32_t visible_index_from_ordinal(uint32_t ordinal) const {
        ensure_built();
        return (ordinal < ordinal_to_visible_.size()) ? ordinal_to_visible_[ordinal] : -1;
    }

    /// Returns inclusive [first_vis_col, last_vis_col] intersecting [h_offset, h_offset + viewport_w).
    /// Returns {1, 0} (empty) if no column is visible. Complexity: O(log M).
    [[nodiscard]] std::pair<int32_t, int32_t> visible_range(float h_offset, float viewport_w) const {
        ensure_built();
        const int32_t n = static_cast<int32_t>(visible_ordinals_.size());
        if (n == 0 || viewport_w <= 0.0f) {
            return {1, 0};
        }
        const float start_x = (std::max)(0.0f, h_offset);
        const float end_x   = start_x + viewport_w;

        // First column whose right edge (offsets_[i+1]) > start_x
        auto it_first = std::upper_bound(offsets_.begin() + 1, offsets_.end(), start_x);
        if (it_first == offsets_.end()) {
            return {1, 0};
        }
        const int32_t first = static_cast<int32_t>(std::distance(offsets_.begin() + 1, it_first));

        // First column whose left edge (offsets_[i]) >= end_x
        auto it_last = std::lower_bound(offsets_.begin() + first, offsets_.begin() + n, end_x);
        int32_t last = static_cast<int32_t>(std::distance(offsets_.begin(), it_last)) - 1;
        if (last < first) {
            last = first;
        }
        if (last >= n) {
            last = n - 1;
        }
        return {first, last};
    }

    /// Hit-tests content_x (relative to data area left + h_offset) in O(log M).
    /// Returns visible column index, or -1 if outside all columns.
    [[nodiscard]] int32_t hit_test_column(float content_x) const {
        ensure_built();
        const int32_t n = static_cast<int32_t>(visible_ordinals_.size());
        if (n == 0 || content_x < 0.0f || content_x >= offsets_.back()) {
            return -1;
        }
        auto it = std::upper_bound(offsets_.begin() + 1, offsets_.end(), content_x);
        if (it == offsets_.end()) {
            return -1;
        }
        return static_cast<int32_t>(std::distance(offsets_.begin() + 1, it));
    }

    /// Hit-tests column right-edge dividers within +/- tolerance px in O(log M).
    /// Returns visible column index whose right divider was hit, or -1.
    [[nodiscard]] int32_t hit_test_divider(float content_x, float tolerance = kDividerHitTolerance) const {
        ensure_built();
        const int32_t n = static_cast<int32_t>(visible_ordinals_.size());
        if (n == 0 || content_x < 0.0f) {
            return -1;
        }
        // Binary search nearest right edge in offsets_[1..n]
        auto it = std::lower_bound(offsets_.begin() + 1, offsets_.end(), content_x);
        int32_t best = -1;
        float best_dist = tolerance + 1.0f;

        if (it != offsets_.end()) {
            const float d = std::abs(*it - content_x);
            if (d <= tolerance && d < best_dist) {
                best_dist = d;
                best = static_cast<int32_t>(std::distance(offsets_.begin() + 1, it));
            }
        }
        if (it != offsets_.begin() + 1) {
            auto prev = it - 1;
            const float d = std::abs(*prev - content_x);
            if (d <= tolerance && d < best_dist) {
                best = static_cast<int32_t>(std::distance(offsets_.begin() + 1, prev));
            }
        }
        return best;
    }

private:
    std::vector<GridColumnInfo>    columns_;
    mutable std::vector<uint32_t>  visible_ordinals_;
    mutable std::vector<int32_t>   ordinal_to_visible_;
    mutable std::vector<float>     offsets_{0.0f};
    mutable bool                   dirty_ = true;
};

} // namespace capybaradb::ui::grid
